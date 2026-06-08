/**
 * @file    flash.h
 * @brief   内部 Flash 操作抽象
 *
 * 换芯片只需改 flash.c 实现
 * 使用流程：Flash_Unlock() → Erase/Write → Flash_Lock()
 */

#ifndef __FLASH_H__
#define __FLASH_H__

#include <stdint.h>

int  Flash_Unlock(void);
int  Flash_Lock(void);
int  Flash_ErasePage(uint32_t addr);
int  Flash_Write(uint32_t addr, uint8_t *data, uint16_t len);
int  Flash_NeedsErase(uint32_t addr, uint16_t len);

#endif
