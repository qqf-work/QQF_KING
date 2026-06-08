#include "ota_update.h"
#include "bootloader_conf.h"
#include "flash.h"
#include "w25q16.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

extern W25Q16_t w25q;

/**
 * @brief READ_INFO 状态处理：从 EEPROM 读取 OTA 信息并校验
 *
 * 校验流程：
 *   1. 读取 EEPROM 中的 OTA_InfoCB 结构体
 *   2. 验证 OTA_flag 是否为魔数 0xA5A5A5A5
 *   3. 验证固件大小是否合法（非零且不超过 A 区容量）
 *   4. 计算需擦除的页数，初始化上下文
 *   5. 解锁 Flash，进入 ERASE 状态
 *
 * 任何一步失败都进入 ERROR 状态
 */
static void OTA_HandleReadInfo(OTA_Context_t *ctx)
{
    OTA_InfoCB info;

    /* 从 EEPROM 读取 OTA 信息（OTA_flag + 固件大小） */
    if (Bootloader_ReadOTAInfo(&info) != 0)
    {
        printf("[OTA] Read EEPROM failed\r\n");
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    /* 校验 OTA 标志位 */
    if (info.OTA_flag != OTA_SET_FLAG)
    {
        printf("[OTA] Flag mismatch\r\n");
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    /* 校验固件大小：必须非零且不超过 A 区总容量 */
    ctx->fw_size = info.Firelen[0];
    if (ctx->fw_size == 0 || ctx->fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[OTA] Invalid fw_size=%lu\r\n", ctx->fw_size);
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    /* 计算需要擦除的 Flash 页数（向上取整） */
    ctx->page_count = (ctx->fw_size + FLASH__PAGE_SIZE - 1) / FLASH__PAGE_SIZE;
    ctx->erase_index = 0;
    ctx->offset = 0;

    printf("[OTA] fw_size=%lu bytes, pages=%lu\r\n", ctx->fw_size, ctx->page_count);

    /* 解锁 Flash，准备擦除（Flash 操作前必须解锁） */
    Flash_Unlock();
    ctx->state = OTA_STATE_ERASE;
}

/**
 * @brief ERASE 状态处理：逐页擦除 A区 Flash
 *
 * 每次调用只擦除 1 页，通过 erase_index 追踪进度
 * 全部页擦除完成后进入 TRANSFER 状态
 * 擦除失败时锁定 Flash 并进入 ERROR 状态
 */
static void OTA_HandleErase(OTA_Context_t *ctx)
{
    /* 计算当前要擦除的页起始地址 */
    uint32_t page_addr = A_REGION_ADDR + ctx->erase_index * FLASH__PAGE_SIZE;

    if (Flash_ErasePage(page_addr) != 0)
    {
        printf("[OTA] Erase page %lu failed\r\n", ctx->erase_index);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    printf("[OTA] Erased page %lu/%lu\r\n", ctx->erase_index + 1, ctx->page_count);
    ctx->erase_index++;

    /* 所有页擦除完成，进入搬运阶段 */
    if (ctx->erase_index >= ctx->page_count)
    {
        printf("[OTA] Erase done, start transfer\r\n");
        ctx->state = OTA_STATE_TRANSFER;
    }
}

/**
 * @brief TRANSFER 状态处理：从 W25Q16 读取固件写入 A区 Flash
 *
 * 每次搬运 256 字节（最后一帧可能不足 256）
 * 流程：W25Q16[offset] → buf → Flash[A_REGION_ADDR + offset]
 * 全部搬运完成后锁定 Flash 并进入 FINISH 状态
 */
static void OTA_HandleTransfer(OTA_Context_t *ctx)
{
    /* 计算本次搬运长度，最后一帧可能不足 256 字节 */
    uint16_t len = 256;
    if (ctx->offset + len > ctx->fw_size)
        len = (uint16_t)(ctx->fw_size - ctx->offset);

    /* 从外部 SPI Flash 读取固件数据到缓冲区 */
    if (W25Q16_Read(&w25q, ctx->offset, ctx->buf, len) != 0)
    {
        printf("[OTA] W25Q read failed at offset=%lu\r\n", ctx->offset);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    /* 将缓冲区数据写入内部 Flash 的 A区 */
    if (Flash_Write(A_REGION_ADDR + ctx->offset, ctx->buf, len) != 0)
    {
        printf("[OTA] Flash write failed at offset=%lu\r\n", ctx->offset);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    ctx->offset += len;
    printf("[OTA] Transfer %lu/%lu bytes\r\n", ctx->offset, ctx->fw_size);

    /* 全部搬运完成，锁定 Flash 并进入 FINISH */
    if (ctx->offset >= ctx->fw_size)
    {
        Flash_Lock();
        ctx->state = OTA_STATE_FINISH;
    }
}

/**
 * @brief FINISH 状态处理：清除 OTA 标志并重启系统
 *
 * 1. 清除 EEPROM 中的 OTA_flag（避免重启后再次进入 OTA）
 * 2. 延时 100ms 确保串口输出完成
 * 3. 执行软复位，重启后 Bootloader 将检测到有效 App 并跳转
 */
static void OTA_HandleFinish(OTA_Context_t *ctx)
{
    Bootloader_ClearOTAFlag();
    printf("[OTA] Update done, resetting...\r\n");
    HAL_Delay(100);
    NVIC_SystemReset();
}

/**
 * @brief OTA 状态机主处理函数，由主循环每次调用执行一步
 *
 * 根据当前状态分发到对应处理函数：
 *   IDLE       → 空转，不执行任何操作
 *   READ_INFO  → 从 EEPROM 校验 OTA 信息
 *   ERASE      → 擦除 A区 Flash 当前页
 *   TRANSFER   → 搬运一段固件数据
 *   FINISH     → 清除标志并重启
 *   ERROR      → 打印错误后回到 IDLE
 */
void OTA_Process(OTA_Context_t *ctx)
{
    switch (ctx->state)
    {
    case OTA_STATE_IDLE:
        break;

    case OTA_STATE_READ_INFO:
        OTA_HandleReadInfo(ctx);
        break;

    case OTA_STATE_ERASE:
        OTA_HandleErase(ctx);
        break;

    case OTA_STATE_TRANSFER:
        OTA_HandleTransfer(ctx);
        break;

    case OTA_STATE_FINISH:
        OTA_HandleFinish(ctx);
        break;

    case OTA_STATE_ERROR:
        printf("[OTA] Error occurred, staying in Bootloader\r\n");
        ctx->state = OTA_STATE_IDLE;
        break;

    default:
        ctx->state = OTA_STATE_IDLE;
        break;
    }
}
