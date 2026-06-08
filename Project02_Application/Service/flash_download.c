#include "flash_download.h"
#include "bootloader_conf.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

/* ---------- 内部 Flash 操作（原 flash.c 内联） ---------- */

/* 检查 [addr, addr+len) 是否全为 0xFF，返回 1=需要擦除 */
static int needs_erase(uint32_t addr, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        if (*(volatile uint8_t *)(addr + i) != 0xFF)
            return 1;
    }
    return 0;
}

/* 擦除 addr 所在的 1 页（1KB），返回 0 成功 */
static int erase_page(uint32_t addr)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase   = FLASH_TYPEERASE_PAGES,
        .PageAddress = addr,
        .NbPages     = 1,
    };
    uint32_t err = 0;
    if (HAL_FLASHEx_Erase(&erase, &err) != HAL_OK)
        return -1;
    return (err == 0xFFFFFFFF) ? 0 : -1;
}

/* ---------- 擦除辅助 ---------- */

static void smart_erase(FlashDownload_t *ctx, uint16_t len)
{
    uint32_t end = ctx->write_addr + len;
    uint32_t page_end = (end + FLASH__PAGE_SIZE - 1) & ~(FLASH__PAGE_SIZE - 1);

    while (ctx->next_erase_addr < page_end)
    {
        if (needs_erase(ctx->next_erase_addr, FLASH__PAGE_SIZE))
        {
            erase_page(ctx->next_erase_addr);
            printf("[DL] Erased page at 0x%08lX\r\n", ctx->next_erase_addr);
        }
        ctx->next_erase_addr += FLASH__PAGE_SIZE;
    }
}

/* ---------- 跨帧奇数字节写入 ---------- */

/**
 * @brief 带跨帧奇数字节处理的半字写入
 *
 * 四种情况（与 Project02 Init_flash_write_halfworf 一致）：
 *   1. 无 last_byte 且 len 为偶数 → 直接写入
 *   2. 无 last_byte 且 len 为奇数 → 写前 len-1 字节，缓存末字节
 *   3. 有 last_byte 且 (1+len) 为偶数 → last_byte 拼首字节 + 写剩余
 *   4. 有 last_byte 且 (1+len) 为奇数 → last_byte 拼首字节 + 写前 n-1 + 缓存末字节
 */
static int write_with_last_byte(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    uint32_t addr = ctx->write_addr;
    uint16_t pos = 0;

    if (ctx->last_byte_flag)
    {
        /* 拼接上一帧缓存的奇数字节和本帧第一个字节 */
        uint16_t half = ctx->last_byte | (data[0] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
            return -1;
        addr += 2;
        pos = 1;
        ctx->total_written += 1;  /* 加上上一帧缓存的 1 字节 */
        ctx->last_byte_flag = 0;
    }

    /* 计算剩余可写字节数（必须是偶数） */
    uint16_t remaining = len - pos;
    uint16_t write_count = remaining & ~1;  /* 向下取偶 */

    /* 逐半字写入 */
    for (uint16_t i = 0; i < write_count; i += 2)
    {
        uint16_t half = data[pos + i] | (data[pos + i + 1] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
            return -1;
        addr += 2;
    }

    ctx->total_written += write_count;
    ctx->write_addr = addr;

    /* 处理剩余奇数字节 */
    if (remaining & 1)
    {
        ctx->last_byte = data[pos + write_count];
        ctx->last_byte_flag = 1;
    }

    return 0;
}

/* ---------- 公开 API ---------- */

void FlashDownload_Init(FlashDownload_t *ctx)
{
    ctx->write_addr = A_REGION_ADDR;
    ctx->total_written = 0;
    ctx->last_byte_flag = 0;
    ctx->last_byte = 0;
    ctx->next_erase_addr = A_REGION_ADDR;
}

int FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    if (len == 0)
        return 0;

    /* 溢出保护 */
    if (ctx->write_addr + len > A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[DL] Error: write overflow\r\n");
        return -1;
    }

    HAL_FLASH_Unlock();

    /* 智能擦除 */
    smart_erase(ctx, len);

    /* 写入（含跨帧奇数字节处理） */
    int ret = write_with_last_byte(ctx, data, len);

    HAL_FLASH_Lock();

    if (ret != 0)
    {
        printf("[DL] Error: write failed at 0x%08lX\r\n", ctx->write_addr);
        return -1;
    }

    printf("[DL] Written %lu bytes\r\n", ctx->total_written);
    return 0;
}

uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx)
{
    return ctx->total_written;
}
