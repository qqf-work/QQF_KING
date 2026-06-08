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
#define B_PAGE_NUM          16              // B区（Bootloader）页数
#define FACTORY_PAGE_NUM    16              // 出厂默认程序页数

/* ---- 以下参数自动推导，无需修改 ---- */

#define FACTORY_START_PAGE  B_PAGE_NUM
#define FACTORY_REGION_ADDR (FLASH_SADDR + FACTORY_START_PAGE * FLASH__PAGE_SIZE)  // 0x08004000
#define A_START_PAGE        (B_PAGE_NUM + FACTORY_PAGE_NUM)                       // 32
#define A_PAGE_NUM          (FLASH_PAGE_NUM - A_START_PAGE)                       // 32
#define A_REGION_ADDR       (FLASH_SADDR + A_START_PAGE * FLASH__PAGE_SIZE)       // 0x08008000

/* RAM 范围（App 有效性校验用） */
#define RAM_START           0x20000000
#define RAM_END             0x20005000

/* ---- 编译期参数校验 ---- */
#if (B_PAGE_NUM + FACTORY_PAGE_NUM >= FLASH_PAGE_NUM)
#error "Partition overflow: Bootloader + Factory pages exceed Flash"
#endif

/* ---- 区域大小宏 ---- */
#define FACTORY_REGION_SIZE  (FACTORY_PAGE_NUM * FLASH__PAGE_SIZE)
#define A_REGION_SIZE        (A_PAGE_NUM * FLASH__PAGE_SIZE)

#endif
