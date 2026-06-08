#ifndef INIT_BOOTLOADER_H_
#define INIT_BOOTLOADER_H_

#include "usart.h"
#include "string.h"

#define BOOTLOADER_UART_REC_BUFF_LEN 512

/* Flash 分区定义：B 区 16KB (0x0000-0x3FFF), A 区 48KB (0x4000-0xFFFF) */
#define FLASH_PAGE_SIZE  1024
#define APP_START_ADDR   0x08004000
#define APP_END_ADDR     0x08010000  /* STM32F103C8 Flash 末端 */

extern uint16_t uart_rec_full_len;

/**
 * @brief  串口通信->准备接收A程序
 */
void Init_bootloader(void);

#endif
