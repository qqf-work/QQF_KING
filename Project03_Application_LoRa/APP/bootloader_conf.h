#ifndef __BOOTLOADER_CONF_H__
#define __BOOTLOADER_CONF_H__

/*
 * 分区配置文件 —— 换芯片时只需修改此文件
 *
 * 当前配置：STM32F103C8 (64KB Flash, 页大小 1KB, 64 页)
 * 适配 GD32 / 其他 Cortex-M 芯片时，修改前 4 个参数即可
 */

#define FLASH_SADDR         0x08000000      // Flash 起始地址
#define FLASH__PAGE_SIZE    1024            // Flash 页大小（字节）
#define FLASH_PAGE_NUM      64              // Flash 总页数
#define B_PAGE_NUM          20              // B区（Bootloader）页数

/* ---- 以下参数自动推导，无需修改 ---- */

#define A_PAGE_NUM          (FLASH_PAGE_NUM - B_PAGE_NUM)           // A区页数
#define A_START_PAGE        B_PAGE_NUM                               // A区起始页编号
#define A_REGION_ADDR       (FLASH_SADDR + A_START_PAGE * FLASH__PAGE_SIZE)  // A区起始地址

/* RAM 范围（App 有效性校验用） */
#define RAM_START           0x20000000
#define RAM_END             0x20005000

#endif
