/**
 * @file    lora_buf.h
 * @brief   LoRa USART3 DMA+IDLE 帧收发驱动
 *
 * 本模块为 E32-433T20D LoRa 模块提供 USART3 上的帧级收发封装。
 * 设计思路与 uart_buf（CAN 网关版本）完全一致，但针对 LoRa 场景简化：
 *   - 使用 DMA Normal 模式（非 Circular），每次 IDLE 中断后手动重启 DMA
 *   - 单帧缓冲（非队列），适用于 LoRa 低速率、单设备点对点通信
 *   - 帧格式由 lora_proto.h 定义（[0xAA] [CMD] [LEN] [PAYLOAD]）
 *
 * 工作原理：
 *   发送路径（主循环调用）：
 *     LORA_Buf_Send() -> 构建 tx_buf -> HAL_UART_Transmit() 阻塞发送
 *
 *   接收路径（中断 + 主循环配合）：
 *     DMA 持续将 USART3 接收数据写入 rx_buf
 *     -> 总线空闲触发 IDLE 中断
 *     -> LORA_Buf_IdleHandler() 记录帧长度，设置 rx_ready 标志，重启 DMA
 *     -> 主循环中 LORA_Buf_Recv() 查询 rx_ready，解析帧内容
 *
 * 调用顺序（在 main.c 中）：
 *   1. HAL_Init() / SystemClock_Config() / MX_DMA_Init() / MX_USART3_UART_Init()
 *   2. LORA_Buf_Init(&lora_ctx, &huart3)    -- 启动 DMA + IDLE
 *   3. while(1) { LORA_Buf_Recv(...) ... }   -- 主循环轮询
 *
 * 注意事项：
 *   - MX_DMA_Init() 必须在 MX_USART3_UART_Init() 之前调用（STM32 HAL 要求）
 *   - IDLE 中断需要在 stm32f1xx_it.c 的 USART3_IRQHandler 中调用 LORA_Buf_IdleHandler()
 *   - 接收为单帧缓冲，前一帧未被消费（rx_ready==1）时新帧会被丢弃
 */

#ifndef __LORA_BUF_H__
#define __LORA_BUF_H__

#include "main.h"
#include <stdint.h>

/* ======================== 缓冲区大小常量 ======================== */

/**
 * DMA 接收缓冲区大小（字节）
 * - 设为 64 字节，大于 LoRa 单包最大帧长 58 字节，留有 6 字节余量
 * - 不能设得太小：如果帧数据超过缓冲区大小，DMA 写入会越界导致内存损坏
 * - 不能设得太大：STM32F103C8 仅 20KB RAM，需节约使用
 */
#define LORA_RX_BUF_SIZE     64

/**
 * 发送帧构建缓冲区大小（字节）
 * - 计算：帧头 3 字节（HEADER + CMD + LEN）+ 最大载荷 55 字节 = 58 字节
 * - 发送时先在 tx_buf 中组装完整帧，再一次性通过 UART 发出
 */
#define LORA_TX_BUF_SIZE     (3 + 55)

/* ======================== 收发上下文结构体 ======================== */

/**
 * @brief  LoRa 收发上下文结构体
 *
 * 封装了 USART3 DMA 收发所需的全部状态，由 LORA_Buf_Init() 初始化，
 * 后续所有 LORA_Buf_xxx 函数均通过此上下文操作。
 *
 * @note  同一时刻只支持一个 LORA_Buf_t 实例（内部使用全局指针 g_lora_ctx
 *        供 IDLE 中断访问），不支持多实例并发。
 */
typedef struct {
    UART_HandleTypeDef *huart;              /**< USART3 句柄，由 LORA_Buf_Init() 设置，
                                                 指向 CubeMX 生成的 huart3 全局变量 */
    uint8_t  rx_buf[LORA_RX_BUF_SIZE];     /**< DMA 接收缓冲区：DMA 直接将 USART3 接收
                                                 的字节写入此数组，IDLE 中断时通过
                                                 CNDTR 寄存器计算实际接收长度 */
    uint16_t rx_len;                        /**< 接收帧长度：IDLE 中断中计算并保存，
                                                 表示 rx_buf 中当前帧的有效字节数 */
    uint8_t  rx_ready;                      /**< 帧就绪标志：
                                                 0 = 无新帧（主循环可忽略 rx_buf）
                                                 1 = 有完整帧待消费（主循环应调用
                                                     LORA_Buf_Recv() 读取） */
    uint8_t  tx_buf[LORA_TX_BUF_SIZE];     /**< 发送帧构建缓冲：LORA_Buf_Send() 在此
                                                 数组中组装 [0xAA][CMD][LEN][PAYLOAD...]，
                                                 然后通过 HAL_UART_Transmit() 发出 */
} LORA_Buf_t;

/* ======================== 公共函数声明 ======================== */

/**
 * @brief  初始化 LoRa USART3 传输层
 *
 * 完成 DMA 接收和 IDLE 中断的启动，必须在 UART 硬件初始化完成后调用。
 *
 * @param  ctx    LoRa 收发上下文指针，调用者需保证此对象的生命周期覆盖整个运行期
 * @param  huart  USART3 句柄指针，通常传入 &huart3（CubeMX 生成）
 *
 * @note   调用前置条件：
 *         1. MX_DMA_Init() 已执行（DMA 时钟已使能）
 *         2. MX_USART3_UART_Init() 已执行（USART3 已配置波特率等参数）
 *         3. NVIC 中 USART3 全局中断已使能（CubeMX 中配置）
 *
 * @note   初始化后 USART3 IDLE 中断将持续触发，需要确保 stm32f1xx_it.c 中
 *         USART3_IRQHandler 调用了 LORA_Buf_IdleHandler()
 */
void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart);

/**
 * @brief  发送一帧 LoRa 数据
 *
 * 在 tx_buf 中组装协议帧并通过 USART3 阻塞式发送。自动添加帧头 0xAA，
 * 调用者只需提供命令码、载荷数据和长度。
 *
 * @param  ctx      LoRa 收发上下文指针（由 LORA_Buf_Init 初始化）
 * @param  cmd      命令码，取值范围见 lora_proto.h 中的 LORA_CMD_xxx 定义
 *                  例如：LORA_CMD_UPDATE_ACK(0x81), LORA_CMD_UPDATE_DATA(0x02) 等
 * @param  payload  载荷数据指针，可为 NULL（当 len=0 时，如 REQ/READY/DONE 帧）
 * @param  len      载荷长度（字节），取值范围 0~LORA_MAX_PAYLOAD(55)，
 *                  超过 55 字节函数会直接返回 -1
 *
 * @retval  0   发送成功
 * @retval -1   参数错误（len > LORA_MAX_PAYLOAD）
 * @retval  其他 HAL 状态码（如 HAL_TIMEOUT 表示 100ms 内未发送完毕，
 *          HAL_ERROR 表示 UART 硬件错误）
 *
 * @note   本函数为阻塞式调用，会等待 UART 发送完成或 100ms 超时。
 *         在 9600 波特率下发送 58 字节约需 60ms，100ms 超时足够。
 *         主循环中调用时不会阻塞太久。
 */
int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len);

/**
 * @brief  非阻塞接收一帧 LoRa 数据
 *
 * 查询是否有新的完整帧到达。如果有，解析帧内容并通过输出参数返回。
 * 此函数设计为在主循环中高频轮询调用。
 *
 * @param  ctx      LoRa 收发上下文指针
 * @param  cmd      [输出] 命令码指针，用于返回接收到的命令码，
 *                  取值见 lora_proto.h（如 LORA_CMD_UPDATE_REQ 等）
 * @param  payload  [输出] 载荷数据缓冲区指针，用于返回帧载荷内容，
 *                  调用者需保证缓冲区 >= LORA_MAX_PAYLOAD(55) 字节；
 *                  可为 NULL（当只需要 cmd 时）
 * @param  len      [输出] 载荷长度指针，用于返回实际载荷字节数（0~55）
 *
 * @retval  1  成功接收到一帧，cmd/payload/len 已填充
 * @retval  0  无新帧可读，或帧格式不合法（已丢弃）
 *
 * @note   帧合法性检查包括：
 *         - 帧长度 >= 3（至少包含 HEADER + CMD + LEN）
 *         - 帧头 == 0xAA
 *         - LEN 字段 <= LORA_MAX_PAYLOAD(55)
 *         - 实际接收长度 >= 3 + LEN
 *         任一条件不满足则丢弃该帧并返回 0
 */
int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len);

/**
 * @brief  USART3 IDLE 线路空闲中断处理函数
 *
 * 在 USART3 总线空闲（一帧接收完成）时被调用，负责：
 *   1. 计算本次接收到的帧长度（通过 DMA CNDTR 寄存器）
 *   2. 过滤假中断（初始化时 IDLE 标志已置位，recv_len == 0）
 *   3. 设置 rx_ready 标志通知主循环有新帧
 *   4. 重启 DMA 接收以准备下一帧
 *
 * @note   此函数必须在中断上下文中调用，通常放在 stm32f1xx_it.c 的
 *         USART3_IRQHandler() 中：
 *         @code
 *         void USART3_IRQHandler(void) {
 *             HAL_UART_IRQHandler(&huart3);
 *             LORA_Buf_IdleHandler();
 *         }
 *         @endcode
 *
 * @note   单帧缓冲设计：如果上一帧尚未被主循环消费（rx_ready == 1），
 *         新到达的帧将被丢弃（不覆盖 rx_buf），确保帧完整性。
 */
void LORA_Buf_IdleHandler(void);

#endif
