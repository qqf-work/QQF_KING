#include "uart_ringbuf.h"
#include "usart.h"
#include <string.h>

UART_RingBuf_t uart_rx_ringbuf;

/**
 * @brief 初始化环形缓冲区
 *
 * head 和 tail 都为 0，表示缓冲区为空
 * DMA Circular 模式启动后会自动往 buffer[0] 开始写入
 */
void UART_RingBuf_Init(UART_RingBuf_t *rb)
{
    memset(rb, 0, sizeof(UART_RingBuf_t));
}

/**
 * @brief 从环形缓冲区读取数据
 * @param rb   环形缓冲区指针
 * @param buf  存放读出数据的数组
 * @param len  期望读取的长度
 * @return 实际读取的长度（可能小于 len）
 *
 * 主循环调用，更新 tail
 */
uint16_t UART_RingBuf_Read(UART_RingBuf_t *rb, uint8_t *buf, uint16_t len)
{
    uint16_t read_count = 0;

    for (uint16_t i = 0; i < len; i++)
    {
        if (rb->head == rb->tail) break;    /* 空，无数据可读 */
        buf[i] = rb->buffer[rb->tail];
        rb->tail = (rb->tail + 1) % UART_RINGBUF_SIZE;
        read_count++;
    }
    return read_count;
}

/**
 * @brief 查询缓冲区中可读数据量
 */
uint16_t UART_RingBuf_Available(UART_RingBuf_t *rb)
{
    return (UART_RINGBUF_SIZE + rb->head - rb->tail) % UART_RINGBUF_SIZE;
}

/**
 * @brief 初始化 DMA Circular 接收 + IDLE 中断
 *
 * 与方案 A（Normal + 重启）的区别：
 *   DMA 配置为 Circular 模式，永不停歇地循环写入 buffer
 *   不需要每帧重启 DMA，不存在重启间隙丢字节的问题
 *
 * 注意：需要在 CubeMX 中将 DMA 模式改为 Circular
 */
void UART_RingBuf_DMA_Init(void)
{
    UART_RingBuf_Init(&uart_rx_ringbuf);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
    HAL_UART_Receive_DMA(&huart1, uart_rx_ringbuf.buffer, UART_RINGBUF_SIZE);
}

/**
 * @brief IDLE 中断处理（DMA Circular 版本）
 *
 * 工作原理：
 *   DMA 以 Circular 模式不断往 buffer 写入数据
 *   CNDTR 寄存器持续递减，到 0 后自动重载为 UART_RINGBUF_SIZE
 *   每次收到一个字节，DMA 自动写入 buffer[当前写位置]
 *
 *   IDLE 中断触发时：
 *     当前写位置 = UART_RINGBUF_SIZE - CNDTR
 *     更新 head 即可，不需要停止或重启 DMA
 *
 * 对比方案 A：
 *   方案 A：停止 DMA → 记录位置 → 重启 DMA（有丢字节风险）
 *   方案 B：只更新 head（DMA 一直在跑，零丢字节风险）
 */
void UART_RingBuf_DMA_IdleHandler(void)
{
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    /* 通过 CNDTR 计算 DMA 当前写到了哪里 */
    uart_rx_ringbuf.head = UART_RINGBUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
}
