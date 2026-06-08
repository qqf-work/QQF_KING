#ifndef __UART_RINGBUF_H__
#define __UART_RINGBUF_H__

#include <stdint.h>

/* 环形缓冲区大小（必须为 2 的幂以优化取模运算） */
#define UART_RINGBUF_SIZE       512

/*
 * 环形缓冲区（DMA Circular 模式直接写入此 buffer）
 *
 * 数据流：DMA 硬件自动写入 buffer → IDLE 中断更新 head → 主循环从 tail 读取
 * head: 由 IDLE 中断更新（通过 DMA 剩余计数器 CNDTR 计算）
 * tail: 由主循环消费数据时更新
 * 两者都用 volatile 修饰，确保跨中断/主循环的可见性
 */
typedef struct {
    uint8_t          buffer[UART_RINGBUF_SIZE];  /* DMA 直接写入的底层数据区 */
    volatile uint16_t head;    /* 写入位置，IDLE 中断中通过 CNDTR 更新 */
    volatile uint16_t tail;    /* 读取位置，主循环消费时更新 */
} UART_RingBuf_t;

extern UART_RingBuf_t uart_rx_ringbuf;

void     UART_RingBuf_Init(UART_RingBuf_t *rb);
uint16_t UART_RingBuf_Read(UART_RingBuf_t *rb, uint8_t *buf, uint16_t len);
uint16_t UART_RingBuf_Available(UART_RingBuf_t *rb);

void UART_RingBuf_DMA_Init(void);
void UART_RingBuf_DMA_IdleHandler(void);

#endif
