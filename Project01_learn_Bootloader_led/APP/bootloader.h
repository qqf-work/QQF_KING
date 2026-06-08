#ifndef __BOOTLOADER_H__
#define __BOOTLOADER_H__

#include "bootloader_conf.h"

/**
 * Bootloader 模块 —— App 跳转与有效性校验
 *
 * 使用流程：
 *   Bootloader_IsAppValid() 检查 → Bootloader_JumpToApp() 跳转
 */

/* 检查 A 区 App 是否有效（MSP 在 RAM 范围内） */
int  Bootloader_IsAppValid(void);

/* 跳转到 A 区 App，成功不返回，失败返回 -1 */
int  Bootloader_JumpToApp(void);

#endif
