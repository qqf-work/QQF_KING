#ifndef __FLASH_H__
#define __FLASH_H__

#include <stdint.h>

/*
 * 内部 Flash 操作抽象 —— 换芯片只改 flash.c 实现
 * 当前实现：STM32F103C8 (页大小 1KB)
 *
 * 使用流程：Flash_Unlock() → Flash_ErasePage()/Flash_Write() → Flash_Lock()
 * 注意：写入前必须先擦除（Flash 只能 1→0，擦除将所有位置 1）
 */

/* 解锁 Flash，允许擦除/写入操作（操作前必须调用） */
int  Flash_Unlock(void);
/* 锁定 Flash，禁止擦除/写入（操作完成后必须调用） */
int  Flash_Lock(void);
/* 擦除指定地址所在的 1 页（1KB），addr 必须页对齐 */
int  Flash_ErasePage(uint32_t addr);
/* 向 Flash 写入数据，按半字（2 字节）编程，addr 必须半字对齐 */
int  Flash_Write(uint32_t addr, uint8_t *data, uint16_t len);
/* 检查 [addr, addr+len) 是否全为 0xFF（已擦除），返回 1=需要擦除, 0=已擦除 */
int  Flash_NeedsErase(uint32_t addr, uint16_t len);

#endif
