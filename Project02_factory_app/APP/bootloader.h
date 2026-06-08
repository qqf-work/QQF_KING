#ifndef __BOOTLOADER_H__
#define __BOOTLOADER_H__

#include "bootloader_conf.h"
#include "stm32f1xx.h"

/* 检查 A 区 App 是否有效（MSP 在 RAM 范围内，ResetHandler 在 A 区内） */
int  Bootloader_IsAppValid(void);

/* 跳转到指定区域（出厂区或 A 区），成功不返回，失败直接 return */
void Bootloader_JumpToApp(uint32_t addr);

#endif
