/*
 * app_bootloader.c —— Bootloader 主流程：检查更新 → 搬运固件 → 跳转 App
 *
 * 三个核心流程：
 *   1. check_update  —— 读 EEPROM，看有没有人通知我们要更新
 *   2. update        —— 把 W25Q16 里的固件搬到 STM32 内部 Flash A 区
 *   3. jump_app      —— 跳转到 A 区运行新程序
 *
 * 设备句柄（EEPROM、W25Q16）在 main.c 里初始化，这里用 extern 直接用
 */

#include "app_bootloader.h"
#include "bootloader_conf.h"
#include "flash.h"
#include <stdio.h>
#include <string.h>

/* ---- 全局状态变量 ---- */
/* main.c 会读取 app_boot_update_status 来决定走哪个分支 */
uint8_t  app_boot_update_status = BOOT_NO_UPDATE;  /* 0x00=不更新, 0x01=需要更新, 0x02=强制更新 */
uint32_t app_boot_fw_size = 0;                      /* 固件总字节数，从 EEPROM 读出 */

/*
 * verify_w25q_firmware —— 搬运前先看看 W25Q16 里的固件是不是合法的
 *
 * STM32 程序的 bin 文件开头 8 字节固定是：
 *   前 4 字节 = 初始栈指针（MSP），必须指向 RAM 区域（0x20000000~0x20004FFF）
 *   后 4 字节 = ResetHandler 地址，必须在 A 区 Flash 范围内（0x08008000~0x08010000）
 *
 * 如果这两个值不对，说明 W25Q16 里的数据是坏的，
 * 直接返回失败，避免把 A 区好的程序给覆盖了
 *
 * 返回: 0=合法, -1=有问题
 */
static int verify_w25q_firmware(void)
{
    uint8_t hdr[8];

    /* 从 W25Q16 起始地址读 8 字节（即 bin 文件头） */
    if (W25Q16_Read(&w25q_dev, W25Q16_FW_ADDR, hdr, 8) != 0)
    {
        printf("[OTA] W25Q16 read header failed\r\n");
        return -1;
    }

    /* 小端序拼出 32 位值：低字节在前 */
    uint32_t sp    = hdr[0] | ((uint32_t)hdr[1] << 8)
                   | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint32_t entry = hdr[4] | ((uint32_t)hdr[5] << 8)
                   | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

    /* 检查栈指针：必须在 STM32 的 RAM 范围内 */
    if (sp < RAM_START || sp >= RAM_END)
    {
        printf("[OTA] Invalid MSP 0x%08lX in W25Q16\r\n", sp);
        return -1;
    }

    /* 检查 ResetHandler：必须落在 A 区 Flash 里 */
    uint32_t a_region_end = A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE;
    if (entry < A_REGION_ADDR || entry >= a_region_end)
    {
        printf("[OTA] Invalid ResetHandler 0x%08lX in W25Q16\r\n", entry);
        return -1;
    }

    printf("[OTA] Firmware header OK: MSP=0x%08lX Entry=0x%08lX\r\n", sp, entry);
    return 0;
}

/*
 * App_bootloader_check_update —— 读 EEPROM 看要不要更新
 *
 * EEPROM 里存了 3 个字节（地址 0x10 起）：
 *   [0] 更新状态：0x00=不用更新, 0x01=要更新, 0x02=强制更新
 *   [1] 密钥高字节
 *   [2] 密钥低字节
 *
 * 密钥必须是 0xA5A5 才算有效数据，否则说明 EEPROM 没被正确写入过
 * 读完后会把密钥清零，这样即使板子突然复位也不会重复触发更新（一次性触发）
 */
void App_bootloader_check_update(void)
{
    uint8_t data[3];

    /* 从 EEPROM 读 3 字节 */
    if (AT24C02_Read(&eeprom_dev, CHECK_UPDATE_ADDR, data, 3) != 0)
    {
        printf("[BL] EEPROM read failed\r\n");
        app_boot_update_status = BOOT_NO_UPDATE;
        return;
    }

    /* 拼出 16 位密钥（大端序：高字节在前） */
    uint16_t key = (uint16_t)((data[1] << 8) | data[2]);

    if (key != CHECK_KEY)
    {
        /* 密钥不对：EEPROM 可能没初始化过或数据坏了，写回默认值并清零密钥 */
        data[0] = BOOT_NO_UPDATE;
        data[1] = 0x00;
        data[2] = 0x00;
        AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR, data, 3);

        app_boot_update_status = BOOT_NO_UPDATE;
        printf("[BL] EEPROM key invalid, reset to default\r\n");
        return;
    }

    /* 密钥正确，取出更新状态 */
    app_boot_update_status = data[0];

    /* 立即清零密钥和状态，防止复位后再次触发（一次性触发） */
    data[0] = BOOT_NO_UPDATE;
    data[1] = 0x00;
    data[2] = 0x00;
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR, data, 3);

    printf("[BL] Update status: 0x%02X\r\n", app_boot_update_status);
}

/*
 * App_bootloader_update —— 把 W25Q16 里的固件搬到内部 Flash A 区
 *
 * 完整步骤：
 *   1. 读 W25Q16 的 JEDEC ID，确认芯片通信正常（应该是 0xEF4015）
 *   2. 从 EEPROM 读出固件大小（之前由外部工具写入的）
 *   3. 校验 W25Q16 里固件的头部（MSP + ResetHandler），防止坏数据覆盖好程序
 *   4. 循环搬运：每次从 W25Q16 读 1KB → 写入 Flash A 区
 *   5. 搬完后校验总字节数是否匹配
 *
 * 失败时不做任何重试，直接停机。用户可以按住 PB0+复位跳到出厂程序恢复
 *
 * 返回: 0=成功, -1=失败
 */
int App_bootloader_update(void)
{
    uint8_t buf[TRANSFER_BUF_SIZE];

    /* 第 1 步：确认 W25Q16 芯片在线且型号正确 */
    uint32_t id = W25Q16_ReadJEDECID(&w25q_dev);
    if (id != 0xEF4015)
    {
        printf("[BL] W25Q16 ID error: 0x%06lX\r\n", id);
        return -1;
    }

    /* 第 2 步：从 EEPROM 读出固件大小（小端序 4 字节） */
    uint8_t size_buf[4];
    if (AT24C02_Read(&eeprom_dev, FW_SIZE_ADDR, size_buf, 4) != 0)
    {
        printf("[BL] EEPROM read fw_size failed\r\n");
        return -1;
    }
    app_boot_fw_size = size_buf[0] | ((uint32_t)size_buf[1] << 8)
                     | ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);

    /* 固件大小合理性检查：不能为 0，也不能超过 A 区容量（32KB） */
    if (app_boot_fw_size == 0 || app_boot_fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[BL] Invalid fw_size: %lu\r\n", app_boot_fw_size);
        return -1;
    }

    printf("[BL] Start update: %lu bytes from W25Q16\r\n", app_boot_fw_size);

    /* 第 3 步：搬运前先检查 W25Q16 里的固件是不是好的 */
    if (verify_w25q_firmware() != 0)
    {
        printf("[BL] Firmware verification failed, abort update\r\n");
        return -1;
    }

    /* 第 4 步：开始搬运 W25Q16 → Flash A 区 */
    FlashDownload_t dl_ctx;
    FlashDownload_Init(&dl_ctx);

    uint32_t offset = 0;
    while (offset < app_boot_fw_size)
    {
        /* 计算本次要搬运多少字节（最后一批可能不满 1KB） */
        uint16_t chunk = TRANSFER_BUF_SIZE;
        if (offset + chunk > app_boot_fw_size)
            chunk = (uint16_t)(app_boot_fw_size - offset);

        /* 从 W25Q16 读一批数据 */
        if (W25Q16_Read(&w25q_dev, W25Q16_FW_ADDR + offset, buf, chunk) != 0)
        {
            printf("[BL] W25Q16 read failed at offset %lu\r\n", offset);
            return -1;
        }

        /* 写入内部 Flash（内部会自动处理擦除和半字对齐） */
        if (FlashDownload_WriteFrame(&dl_ctx, buf, chunk) != 0)
        {
            printf("[BL] Flash write failed at offset %lu\r\n", offset);
            return -1;
        }

        offset += chunk;
    }

    /* 第 5 步：校验写入总字节数是否和预期一致 */
    uint32_t total = FlashDownload_GetTotal(&dl_ctx);
    if (total != app_boot_fw_size)
    {
        printf("[BL] Verify failed: expected %lu, got %lu\r\n",
               app_boot_fw_size, total);
        return -1;
    }

    printf("[BL] Update complete: %lu bytes\r\n", total);
    return 0;
}

/*
 * App_bootloader_jump_app —— 跳转到 A 区 App
 *
 * 先检查 A 区程序是否有效（通过 MSP 和 ResetHandler 判断），
 * 有效才跳转，无效就停在这里
 *
 * 返回: 0=成功（不会返回）, -1=失败
 */
int App_bootloader_jump_app(void)
{
    if (!Bootloader_IsAppValid())
    {
        printf("[BL] App invalid, cannot jump\r\n");
        return -1;
    }

    printf("[BL] Jumping to App...\r\n");
    Bootloader_JumpToApp(A_REGION_ADDR);
    return -1;
}

/*
 * App_bootloader_factory_reset —— 恢复出厂：跳转到出厂区程序
 *
 * 出厂程序编译地址是 0x08004000，直接跳过去就行
 * Bootloader_JumpToApp 内部会自动设置 VTOR 和 MSP
 *
 * 返回: 0=成功（不会返回）, -1=失败
 */
int App_bootloader_factory_reset(void)
{
    printf("[BL] Factory reset triggered\r\n");
    if (!Bootloader_IsAppValid())
    {
        printf("[BL] Factory region invalid\r\n");
        return -1;
    }
    Bootloader_JumpToApp(FACTORY_REGION_ADDR);
    return -1;
}
