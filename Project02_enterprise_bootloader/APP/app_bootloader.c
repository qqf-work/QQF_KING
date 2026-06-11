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
 * clear_eeprom_flag_safe -- 安全清除 EEPROM 更新标志
 * 顺序：先废密钥（0x11-0x12 写 0x00），再清状态（0x10 写 0x00）
 * 断电安全：即使步骤 1 后掉电，密钥已失效，Bootloader 不会重复触发更新
 */
static void clear_eeprom_flag_safe(void)
{
    /* 步骤 1：废密钥（地址 0x11-0x12 写 0x00） */
    uint8_t zero_key[2] = { 0x00, 0x00 };
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR + 1, zero_key, 2);

    /* 步骤 2：清状态（地址 0x10 写 0x00） */
    uint8_t zero_status = BOOT_NO_UPDATE;
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR, &zero_status, 1);
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
 * 密钥校验通过后保留不清除，留给后续 update 流程使用
 * 搬运成功后由 clear_eeprom_flag_safe() 统一安全清除
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
    printf("[BL] Update status: 0x%02X\r\n", app_boot_update_status);
}

/*
 * App_bootloader_update —— 从 W25Q16 搬运固件到 A 区 Flash
 *
 * 流程：
 *   1. 校验 W25Q16 芯片 ID
 *   2. 从 EEPROM 读取固件大小
 *   3. 搬运 W25Q16 → Flash A 区
 *   4. 成功则安全清除 EEPROM 标志
 *
 * 返回值：0=成功, -1=失败（已清 EEPROM 标志，应跳出厂区）
 */
int App_bootloader_update(void)
{
    uint8_t buf[TRANSFER_BUF_SIZE];

    /* 步骤 1：校验 W25Q16 芯片是否在线且型号正确 */
    uint32_t id = W25Q16_ReadJEDECID(&w25q_dev);
    if (id != 0xEF4015)
    {
        printf("[BL] W25Q16 ID error: 0x%06lX\r\n", id);
        return -1;
    }

    /* 步骤 2：从 EEPROM 读取固件大小（4 字节小端序） */
    uint8_t size_buf[4];
    if (AT24C02_Read(&eeprom_dev, FW_SIZE_ADDR, size_buf, 4) != 0)
    {
        printf("[BL] EEPROM read fw_size failed\r\n");
        return -1;
    }
    app_boot_fw_size = size_buf[0] | ((uint32_t)size_buf[1] << 8)
                     | ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);

    if (app_boot_fw_size == 0 || app_boot_fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[BL] Invalid fw_size: %lu\r\n", app_boot_fw_size);
        return -1;
    }

    printf("[BL] Start update: %lu bytes\r\n", app_boot_fw_size);

    /* 步骤 3：搬运 W25Q16 → A 区 Flash */
    printf("[BL] Copying firmware %lu bytes...\r\n", app_boot_fw_size);

    FlashDownload_t dl_ctx;
    FlashDownload_Init(&dl_ctx);

    uint32_t offset = 0;
    while (offset < app_boot_fw_size)
    {
        uint16_t chunk = TRANSFER_BUF_SIZE;
        if (offset + chunk > app_boot_fw_size)
            chunk = (uint16_t)(app_boot_fw_size - offset);

        if (W25Q16_Read(&w25q_dev, W25Q16_FW_ADDR + offset, buf, chunk) != 0)
        {
            printf("[BL] W25Q16 read failed at offset %lu\r\n", offset);
            clear_eeprom_flag_safe();
            return -1;
        }

        if (FlashDownload_WriteFrame(&dl_ctx, buf, chunk) != 0)
        {
            printf("[BL] Flash write failed at offset %lu\r\n", offset);
            clear_eeprom_flag_safe();
            return -1;
        }

        offset += chunk;
    }

    /* 步骤 4：安全清除 EEPROM 标志 */
    clear_eeprom_flag_safe();
    printf("[BL] Update success, jump app\r\n");
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
 * App_bootloader_factory_reset —— 恢复出厂：直接跳转到出厂区程序
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
