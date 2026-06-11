/**
 * @file    lora_buf.c
 * @brief   LoRa USART3 DMA+IDLE 帧收发驱动实现
 *
 * 本文件实现 lora_buf.h 中声明的四个函数，提供基于 USART3 DMA Normal 模式
 * + IDLE 线路空闲检测的帧级收发能力。
 *
 * 核心设计决策：
 *   1. 使用 DMA Normal 模式（非 Circular）：每次 IDLE 中断后手动重启 DMA，
 *      配合单帧缓冲实现"收到一帧 -> 主循环处理 -> 再收下一帧"的节奏控制。
 *   2. 全局指针 g_lora_ctx：IDLE 中断服务函数无参数，通过全局指针访问上下文。
 *      这限制了只能有一个 LoRa 通道实例，但对于单设备网关场景足够。
 *   3. 假中断过滤：USART 外设初始化后 IDLE 标志默认置位，首次进入中断时
 *      DMA CNDTR == LORA_RX_BUF_SIZE（recv_len == 0），需要忽略。
 */

#include "lora_buf.h"
#include "lora_proto.h"
#include <string.h>

/**
 * 全局 LoRa 上下文指针
 *
 * LORA_Buf_IdleHandler() 是无参数的中断回调，无法直接传入上下文。
 * 通过此全局指针间接访问。在 LORA_Buf_Init() 中设置。
 *
 * 设计权衡：使用全局指针牺牲了多实例能力，但简化了中断处理函数的调用方式。
 * 对于只有一个 LoRa 模块的网关设备，这是合理的折中。
 */
static LORA_Buf_t *g_lora_ctx = NULL;

/**
 * @brief  初始化 LoRa USART3 传输层
 *
 * 清零上下文结构体，保存 UART 句柄，启用 IDLE 中断并启动 DMA 接收。
 * 调用后 USART3 将持续通过 DMA 接收数据，每帧结束后触发 IDLE 中断。
 *
 * @param  ctx    LoRa 收发上下文指针，调用者需保证生命周期覆盖整个运行期
 *                （通常定义为 main.c 中的全局变量或静态局部变量）
 * @param  huart  USART3 句柄指针，由 CubeMX 生成的 MX_USART3_UART_Init() 初始化，
 *                通常传入 &huart3
 *
 * 执行步骤：
 *   1. memset 清零整个上下文（rx_ready=0, rx_len=0 等）
 *   2. 保存 huart 句柄和全局指针
 *   3. 启用 USART3 的 IDLE 中断（线路空闲检测）
 *   4. 启动 DMA Normal 模式接收，DMA 将 USART3 数据持续写入 rx_buf
 */
void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart)
{
    /* 清零所有字段：rx_ready=0, rx_len=0, huart=NULL 等 */
    memset(ctx, 0, sizeof(*ctx));

    /* 保存 UART 句柄，后续 Send/Recv/IdleHandler 都通过它操作 USART3 */
    ctx->huart = huart;

    /* 设置全局指针，供 IDLE 中断回调使用 */
    g_lora_ctx = ctx;

    /* 启用 IDLE 中断：当 USART3 检测到总线空闲（一个字节时间无新数据）时触发中断 */
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);

    /* 启动 DMA Normal 模式接收：
     * - DMA 将 USART3 接收到的数据自动搬入 rx_buf
     * - Normal 模式下 DMA 传输 LORA_RX_BUF_SIZE 字节后自动停止
     * - 配合 IDLE 中断，每收到一帧就手动重启 DMA */
    HAL_UART_Receive_DMA(huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}

/**
 * @brief  发送一帧 LoRa 数据
 *
 * 在 tx_buf 中按照协议格式组装帧，然后通过 USART3 阻塞发送。
 *
 * @param  ctx      LoRa 收发上下文指针
 * @param  cmd      命令码（如 LORA_CMD_UPDATE_DATA 等，见 lora_proto.h）
 * @param  payload  载荷数据指针，当 len>0 时不能为 NULL；len=0 时可传 NULL
 * @param  len      载荷字节数，范围 0~55（LORA_MAX_PAYLOAD），超过返回 -1
 *
 * @retval  0   发送成功（HAL_UART_Transmit 返回 HAL_OK）
 * @retval -1   参数错误（len 超过 LORA_MAX_PAYLOAD）
 * @retval  其他  HAL 错误码（HAL_TIMEOUT/HAL_ERROR）
 *
 * 帧组装过程：
 *   tx_buf[0] = 0xAA        帧头标识
 *   tx_buf[1] = cmd         命令码
 *   tx_buf[2] = len         载荷长度
 *   tx_buf[3..3+len-1] = payload  载荷数据（如果有）
 *
 * 然后通过 HAL_UART_Transmit 阻塞发送 3+len 字节，超时 100ms。
 */
int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len)
{
    /* 载荷长度合法性检查：超过协议最大载荷则拒绝发送 */
    if (len > LORA_MAX_PAYLOAD) return -1;

    /* 组装帧头：固定 0xAA 帧头标识 + 命令码 + 载荷长度 */
    ctx->tx_buf[0] = LORA_FRAME_HEADER;  /* 0xAA */
    ctx->tx_buf[1] = cmd;
    ctx->tx_buf[2] = len;

    /* 如果有载荷数据，拷贝到帧头之后的位置 */
    if (len > 0 && payload != NULL) {
        memcpy(&ctx->tx_buf[3], payload, len);
    }

    /* 阻塞式 UART 发送：
     * - 总发送长度 = 3 字节帧头 + len 字节载荷
     * - 超时 100ms：9600 波特率下 58 字节约需 60ms，留有余量
     * - 返回 HAL 状态码（HAL_OK=0, HAL_TIMEOUT, HAL_ERROR 等） */
    return HAL_UART_Transmit(ctx->huart, ctx->tx_buf, 3 + len, 100);
}

/**
 * @brief  非阻塞接收一帧 LoRa 数据
 *
 * 检查是否有已由 IDLE 中断标记的完整帧。如果有，按照协议格式解析
 * 帧头、命令码、载荷长度和载荷数据，通过输出参数返回给调用者。
 *
 * @param  ctx      LoRa 收发上下文指针
 * @param  cmd      [输出] 命令码，如 LORA_CMD_UPDATE_REQ(0x01) 等
 * @param  payload  [输出] 载荷数据缓冲区（调用者分配，>=55 字节），可为 NULL
 * @param  len      [输出] 实际载荷字节数
 *
 * @retval  1  成功解析一帧，输出参数已填充
 * @retval  0  无新帧（rx_ready=0），或帧格式不合法（已丢弃，清除 rx_ready）
 *
 * 解析步骤：
 *   1. 检查 rx_ready 标志，无新帧则立即返回 0
 *   2. 清除 rx_ready（消费标记，允许 IDLE 中断接受下一帧）
 *   3. 检查最小帧长度 >= 3（HEADER + CMD + LEN）
 *   4. 校验帧头 == 0xAA
 *   5. 校验 LEN 字段 <= LORA_MAX_PAYLOAD(55)
 *   6. 校验实际接收长度 >= 3 + LEN（确保载荷完整）
 *   7. 通过所有检查后，拷贝 cmd/len/payload 到输出参数
 */
int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len)
{
    /* 没有新帧就绪，直接返回 */
    if (!ctx->rx_ready) return 0;

    /* 标记已消费：清除后 IDLE 中断才能接受下一帧 */
    ctx->rx_ready = 0;

    /* ---- 帧格式合法性校验 ---- */

    /* 最小帧长度检查：协议帧至少 3 字节（HEADER + CMD + LEN） */
    if (ctx->rx_len < 3) return 0;

    /* 帧头校验：必须为 0xAA，否则视为噪声或格式错误 */
    if (ctx->rx_buf[0] != LORA_FRAME_HEADER) return 0;

    /* 提取命令码和声明载荷长度 */
    uint8_t frame_cmd = ctx->rx_buf[1];  /* 命令码字节 */
    uint8_t frame_len = ctx->rx_buf[2];  /* LEN 字段声明的载荷长度 */

    /* 载荷长度上限检查：LEN 字段不能超过协议最大载荷 55 字节 */
    if (frame_len > LORA_MAX_PAYLOAD) return 0;

    /* 载荷完整性检查：实际接收到的字节数必须 >= 帧头 + 声明载荷长度 */
    if (ctx->rx_len < (uint16_t)(3 + frame_len)) return 0;

    /* ---- 校验通过，输出解析结果 ---- */

    /* 输出命令码 */
    *cmd = frame_cmd;
    /* 输出载荷长度 */
    *len = frame_len;

    /* 如果有载荷且调用者提供了缓冲区，拷贝载荷数据 */
    if (frame_len > 0 && payload != NULL) {
        memcpy(payload, &ctx->rx_buf[3], frame_len);
    }

    return 1;  /* 成功解析一帧 */
}

/**
 * @brief  USART3 IDLE 线路空闲中断处理函数
 *
 * 当 USART3 检测到总线空闲（一个字符时间内无新数据到达）时被调用。
 * 这意味着一帧数据已经接收完毕。本函数计算帧长度、标记帧就绪、
 * 并重启 DMA 以准备接收下一帧。
 *
 * @param  无（通过全局指针 g_lora_ctx 访问上下文）
 *
 * @note   必须在 stm32f1xx_it.c 的 USART3_IRQHandler() 中调用！
 *         典型用法：
 *         @code
 *         void USART3_IRQHandler(void) {
 *             HAL_UART_IRQHandler(&huart3);
 *             LORA_Buf_IdleHandler();
 *         }
 *         @endcode
 *
 * 处理步骤：
 *   1. 安全检查：全局指针和 UART 句柄非空
 *   2. 检查 IDLE 标志是否确实置位，否则直接返回
 *   3. 清除 IDLE 标志（读 SR 再写 DR）
 *   4. 通过 DMA CNDTR 计算实际接收字节数
 *   5. 过滤假中断（recv_len == 0，初始化时触发）
 *   6. 如果前一帧已被消费（rx_ready==0），保存帧长度并标记就绪
 *      如果前一帧未消费（rx_ready==1），丢弃新帧（保护数据完整性）
 *   7. 停止 DMA 并重新启动，为下一帧接收做准备
 */
void LORA_Buf_IdleHandler(void)
{
    /* 通过全局指针获取上下文 */
    LORA_Buf_t *ctx = g_lora_ctx;

    /* 安全检查：上下文或 UART 句柄未初始化时直接返回 */
    if (ctx == NULL || ctx->huart == NULL) return;

    /* 检查 IDLE 标志是否置位：
     * IDLE 标志在 USART 检测到总线空闲时由硬件置位。
     * 如果不是 IDLE 中断（可能是其他 USART 中断源），直接返回。 */
    if (__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE) == RESET) return;

    /* 清除 IDLE 标志：
     * STM32 清除 IDLE 标志的方法是先读 SR 寄存器再写 DR 寄存器，
     * HAL 宏 __HAL_UART_CLEAR_IDLEFLAG 封装了此操作 */
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    /* 计算本次接收到的字节数：
     * CNDTR 是 DMA 通道的剩余传输计数器，初始化为 LORA_RX_BUF_SIZE(64)，
     * 每接收一个字节 CNDTR 递减。所以已接收字节数 = 总缓冲区大小 - 剩余计数值 */
    uint16_t recv_len = LORA_RX_BUF_SIZE -
                        (uint16_t)__HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);

    /* 假中断过滤：
     * USART 初始化完成后 IDLE 标志就已经置位（因为总线一开始就是空闲的），
     * 此时 DMA 尚未接收到任何数据，CNDTR 仍为 LORA_RX_BUF_SIZE，
     * recv_len 计算为 0。这种情况必须忽略，否则会错误地标记一个空帧。 */
    if (recv_len == 0) return;

    /* 单帧缓冲保护：
     * 只有当前一帧已被主循环消费（rx_ready == 0）时，才接受新帧。
     * 如果 rx_ready == 1（前一帧未消费），说明主循环处理速度跟不上接收速度，
     * 此时新到达的帧只能丢弃，避免覆盖前一帧数据导致数据损坏。
     * 这在 LoRa 低速率场景下很少发生，但作为防御性编程是必要的。 */
    if (!ctx->rx_ready) {
        ctx->rx_len = recv_len;   /* 保存帧长度 */
        ctx->rx_ready = 1;        /* 标记帧就绪，通知主循环 */
    }
    /* else: 前一帧未消费，新帧被丢弃（不做任何处理） */

    /* 重启 DMA 接收：
     * 必须先停止当前 DMA 传输（HAL_UART_DMAStop 会同时停止 DMA 并取消 UART DMA 请求），
     * 然后重新启动。如果不重启，DMA 在 Normal 模式下传输完成后不会再接收新数据。 */
    HAL_UART_DMAStop(ctx->huart);
    HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}
