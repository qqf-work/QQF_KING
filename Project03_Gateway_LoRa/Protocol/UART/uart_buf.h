#ifndef __UART_BUF_H__
#define __UART_BUF_H__

#include <stdint.h>

/* DMA 接收缓冲区大小（所有帧数据都落在这块连续内存中） */
#define UART_READ_BUF_SIZE      1024

/* 单帧最大长度（DMA 每次接收的最大字节数 + 1） */
#define UART_READ_MAX_SIZE      256

/* 帧描述符队列长度（最多缓存多少帧的起止信息） */
#define UART_BUF_QUEUE_SIZE     8

/* 帧描述符：记录一帧数据在 DMA 缓冲区中的起止位置 */
typedef struct {
    uint8_t *start;  /* 帧数据起始地址 */
    uint8_t *end;    /* 帧数据结束地址（最后一个字节的地址） */
} UART_Buffptr;

/* 缓冲区队列管理：环形队列，IN 写入（中断），OUT 读取（主循环） */
typedef struct {
    uint16_t        URxCounter;                    /* DMA 缓冲区累计写入位置（字节偏移） */
    UART_Buffptr    URxDataPtr[UART_BUF_QUEUE_SIZE]; /* 帧描述符数组 */
    UART_Buffptr   *URxDataIN;                     /* 写入指针（IDLE 中断中前移） */
    UART_Buffptr   *URxDataOUT;                    /* 读取指针（主循环消费时前移） */
    UART_Buffptr   *URxDataEND;                    /* 数组末尾（用于环形回绕判断） */
} UART_BufQueue_t;

extern UART_BufQueue_t uart_rx_queue;

void UART_BufQueue_Init(UART_BufQueue_t *queue);
void UART_DMA_Rx_Init(void);
void UART_DMA_RxIdleHandler(void);

#endif
