/**
 * @file    fw_cache_conf.h
 * @brief   P00 固件缓存分区配置
 *
 * Flash 分区:
 *   0x08000000 ~ 0x08003FFF  P00 应用 (16KB, page 0~15)
 *   0x08004000 ~ 0x0800FFFF  固件缓存 (48KB, page 16~63)
 *
 * 换芯片时修改本文件前 3 个宏即可
 */

#ifndef __FW_CACHE_CONF_H__
#define __FW_CACHE_CONF_H__

#include <stdint.h>

/* Flash 基础参数 */
#define FW_CACHE_FLASH_BASE      0x08000000    /* Flash 起始地址 */
#define FW_CACHE_PAGE_SIZE       1024          /* 页大小 (字节) */

/* P00 应用区 */
#define FW_CACHE_APP_PAGE_NUM    16            /* P00 应用占 16 页 (16KB) */

/* 固件缓存区（P00 应用之后） */
#define FW_CACHE_START_PAGE      FW_CACHE_APP_PAGE_NUM  /* 起始页号 = 16 */
#define FW_CACHE_ADDR            (FW_CACHE_FLASH_BASE + FW_CACHE_START_PAGE * FW_CACHE_PAGE_SIZE)  /* 0x08004000 */
#define FW_CACHE_SIZE            (48 * FW_CACHE_PAGE_SIZE)  /* 48KB */

#endif
