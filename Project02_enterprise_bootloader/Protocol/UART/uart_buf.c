#include "uart_buf.h"
#include "usart.h"
#include <string.h>

extern DMA_HandleTypeDef hdma_usart1_rx;

/* DMA 接收缓冲区，所有帧数据都落在这块内存中 */
static uint8_t uart_dma_rx_buf[UART_READ_BUF_SIZE];

/* 全局缓冲区队列实例 */
UART_BufQueue_t uart_rx_queue;

/**
 * @brief 初始化缓冲区队列
 * @param queue 队列指针
 *
 * IN  和 OUT 都指向数组起始，表示队列为空
 * END 指向数组最后一个元素，用于回绕判断
 * IN->start 指向 DMA 缓冲区起始，准备接收第一帧
 */
void UART_BufQueue_Init(UART_BufQueue_t *queue)
{
    memset(queue, 0, sizeof(UART_BufQueue_t));
    queue->URxDataIN  = &queue->URxDataPtr[0];
    queue->URxDataOUT = &queue->URxDataPtr[0];
    queue->URxDataEND = &queue->URxDataPtr[UART_BUF_QUEUE_SIZE - 1];
    queue->URxDataIN->start = uart_dma_rx_buf;
}

/**
 * @brief 初始化 DMA 接收并使能串口空闲中断
 *
 * 调用顺序：队列初始化 → 使能 IDLE 中断 → 启动 DMA
 */
void UART_DMA_Rx_Init(void)
{
    UART_BufQueue_Init(&uart_rx_queue);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
    HAL_UART_Receive_DMA(&huart1, uart_dma_rx_buf, UART_READ_MAX_SIZE + 1);
}

/**
 * @brief 串口空闲中断处理函数
 *
 * 当串口总线空闲时触发，表示一帧数据接收完成
 * 工作流程：
 *   1. 清除 IDLE 标志
 *   2. 通过 DMA 剩余计数器计算本帧长度
 *   3. 记录本帧结束位置 (end)
 *   4. IN 指针前移，准备下一个描述符
 *   5. 判断缓冲区剩余空间是否够放下一帧，不够则回绕
 *   6. 停止 DMA → 重新启动 DMA 接收下一帧
 */
void UART_DMA_RxIdleHandler(void)
{
    /* 清除空闲中断标志（读 SR 再读 DR） */
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    /* CNDTR 未变说明没收到数据（如上电后首次 IDLE），直接返回 */
    if (__HAL_DMA_GET_COUNTER(&hdma_usart1_rx) == (UART_READ_MAX_SIZE + 1)) return;

    /* 累加写入位置：已传输量 = 总长度 - DMA 剩余计数 */
    uart_rx_queue.URxCounter += (UART_READ_MAX_SIZE + 1) - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);

    /* end 指向本帧最后一个字节 */
    uart_rx_queue.URxDataIN->end = &uart_dma_rx_buf[uart_rx_queue.URxCounter - 1];

    /* 计算下一个可用的描述符位置（环形回绕） */
    UART_Buffptr *next = uart_rx_queue.URxDataIN + 1;
    if (next > uart_rx_queue.URxDataEND)
        next = &uart_rx_queue.URxDataPtr[0];

    /* 队列满判断：next 追上 OUT 说明所有描述符已被占用
     * 此时丢弃本帧并直接返回，避免 IN 覆盖未消费的描述符 */
    if (next == uart_rx_queue.URxDataOUT)
        return;

    uart_rx_queue.URxDataIN = next;

    /* 判断缓冲区剩余空间能否容纳下一帧（需要 > MAX_SIZE，不是 >=） */
    if (UART_READ_BUF_SIZE - uart_rx_queue.URxCounter > UART_READ_MAX_SIZE)
    {
        /* 空间足够，紧接着上一帧继续写 */
        uart_rx_queue.URxDataIN->start = &uart_dma_rx_buf[uart_rx_queue.URxCounter];
    }
    else
    {
        /* 空间不足，回绕到缓冲区起始，计数器归零 */
        uart_rx_queue.URxDataIN->start = uart_dma_rx_buf;
        uart_rx_queue.URxCounter = 0;
    }

    /* 停止当前 DMA，从新 start 位置重新启动接收 */
    HAL_UART_DMAStop(&huart1);
    HAL_UART_Receive_DMA(&huart1, uart_rx_queue.URxDataIN->start, UART_READ_MAX_SIZE + 1);
}
