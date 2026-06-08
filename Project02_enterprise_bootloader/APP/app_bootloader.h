/*
 * Bootloader 用户交互 —— 公开接口与类型定义
 *
 * 通过 EEPROM 标志判断是否需要固件更新
 * EEPROM 布局（地址 0x10 起）:
 *   [0x10] status      更新状态 (BOOT_xxx)
 *   [0x11] key_high    密钥高字节
 *   [0x12] key_low     密钥低字节
 *   [0x13~0x16] fw_size 固件大小（小端序，4 字节）
 */

#ifndef __APP_BOOTLOADER_H__
#define __APP_BOOTLOADER_H__

#include "at24c02.h"
#include "w25q16.h"
#include "flash_download.h"
#include "flash.h"
#include "bootloader.h"

/* EEPROM 存储地址（预留 0x00~0x0F） */
#define CHECK_UPDATE_ADDR   0x10
#define FW_SIZE_ADDR        (CHECK_UPDATE_ADDR + 3)   /* 0x13 */

/* W25Q16 中固件存放起始地址 */
#define W25Q16_FW_ADDR      0x000000

/* 每次搬运的数据块大小（等于内部 Flash 页大小，减少擦写次数） */
#define TRANSFER_BUF_SIZE   FLASH__PAGE_SIZE

/* 更新状态定义 */
#define BOOT_NO_UPDATE      0x00
#define BOOT_NEED_UPDATE    0x01
#define BOOT_FORCE_UPDATE   0x02

/* EEPROM 校验密钥（大端序存储: 高字节在前） */
#define CHECK_KEY           0xA5A5

/* 设备句柄（由 main.c 定义和初始化） */
extern AT24C02_t eeprom_dev;
extern W25Q16_t  w25q_dev;

/* 全局更新状态，由 App_bootloader_check_update() 设置 */
extern uint8_t  app_boot_update_status;

/* 全局固件大小，由 update 流程从 EEPROM 读取 */
extern uint32_t app_boot_fw_size;

/* 从 EEPROM 读取并判断是否需要更新 */
void App_bootloader_check_update(void);

/* 从 W25Q16 读取固件写入内部 Flash（W25Q16 → Flash A 区） */
int App_bootloader_update(void);

/* 跳转到 A 区 App */
int App_bootloader_jump_app(void);

/* 恢复出厂设置：出厂区(0x08004000) → A区(0x08008000) 复制后跳转 */
int App_bootloader_factory_reset(void);

#endif
