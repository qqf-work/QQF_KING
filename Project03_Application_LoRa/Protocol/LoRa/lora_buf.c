#include "lora_buf.h"
#include "lora_proto.h"
#include <string.h>

/*
 * 全局上下文指针 —— 供 IDLE 中断服务程序访问收发上下文
 *
 * 设计原因：LORA_Buf_IdleHandler() 由 USART3_IRQHandler() 在中断上下文中
 * 调用，无法传递参数。因此在 LORA_Buf_Init() 中将 ctx 指针保存到此全局变量，
 * ISR 通过它间接访问缓冲区和 UART 句柄。
 *
 * static 修饰确保作用域仅限本文件（lora_buf.c），不污染全局命名空间。
 */
static LORA_Buf_t *g_lora_ctx = NULL;

/*
 * LORA_Buf_Init - 初始化 LoRa 传输层
 *
 * 执行三个关键步骤：
 *   1. 清零上下文结构体（所有字段初始化为 0/NULL）
 *   2. 启用 USART3 的 IDLE 线空闲检测中断
 *   3. 启动 DMA Normal 模式接收，DMA 持续将 UART 数据写入 rx_buf
 *
 * 调用前提：
 *   - MX_DMA_Init() 已执行（DMA 通道已配置）
 *   - MX_USART3_UART_Init() 已执行（USART3 已配置波特率等参数）
 *   调用后 USART3 即开始接收数据，IDLE 中断开始工作
 *
 * 参数：
 *   ctx   - 调用者分配的 LORA_Buf_t 实例指针，初始化后持续使用
 *   huart - USART3 的 HAL 句柄，通常为 CubeMX 生成的 &huart3
 */
void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart)
{
    /* 清零整个结构体：huart=NULL, rx_buf全0, rx_len=0, rx_ready=0, tx_buf全0 */
    memset(ctx, 0, sizeof(*ctx));

    /* 保存 UART 句柄，后续发送/接收操作都需要通过它调用 HAL API */
    ctx->huart = huart;

    /* 保存到全局指针，使 IDLE ISR 能通过 g_lora_ctx 访问此上下文 */
    g_lora_ctx = ctx;

    /* 启用 IDLE 中断：当 UART 总线空闲超过一个字符时间时触发中断 */
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);

    /*
     * 启动 DMA Normal 模式接收
     *
     * DMA 配置为将 USART3 接收到的数据持续写入 ctx->rx_buf[]，
     * 最多接收 LORA_RX_BUF_SIZE(64) 字节。
     * Normal 模式下 DMA 传输计数器(CNDTR)递减到 0 后自动停止。
     * 当 IDLE 中断触发时，通过读取 CNDTR 可计算实际接收了多少字节。
     */
    HAL_UART_Receive_DMA(huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}

/*
 * LORA_Buf_Send - 构建并发送一帧 LoRa 协议数据
 *
 * 组帧过程：
 *   tx_buf[0] = 0xAA          （帧头标识，LORA_FRAME_HEADER）
 *   tx_buf[1] = cmd           （命令码，如 0x01=REQ, 0x83=DONE）
 *   tx_buf[2] = len           （载荷长度，0~55）
 *   tx_buf[3..3+len-1] = payload  （载荷数据，如果有的话）
 *
 * 参数：
 *   ctx     - LoRa 收发上下文
 *   cmd     - 命令码，使用 lora_proto.h 中定义的 LORA_CMD_xxx 宏
 *   payload - 载荷数据指针，无载荷帧传 NULL（如 REQ、READY、DONE）
 *   len     - 载荷字节数，0~55；超过 55 返回 -1
 *
 * 返回值：
 *   0    - 发送成功
 *   -1   - len > 55（超过 LORA_MAX_PAYLOAD 限制）
 *   其他 - HAL_UART_Transmit 的返回值（HAL_TIMEOUT 等）
 */
int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len)
{
    /* 载荷长度合法性检查：超过 LoRa 单帧最大载荷则拒绝 */
    if (len > LORA_MAX_PAYLOAD) return -1;

    /* ---- 组装协议帧 ---- */
    ctx->tx_buf[0] = LORA_FRAME_HEADER;  /* 帧头 0xAA */
    ctx->tx_buf[1] = cmd;                 /* 命令码 */
    ctx->tx_buf[2] = len;                 /* 载荷长度 */

    /* 仅当有载荷且指针非空时才拷贝数据 */
    if (len > 0 && payload != NULL) {
        memcpy(&ctx->tx_buf[3], payload, len);
    }

    /*
     * 阻塞式 UART 发送
     *
     * 超时设为 100ms：发送 58 字节在 115200 波特率下约需 5ms，
     * 100ms 超时留有充足余量。如果发送超时，可能是硬件连线问题。
     *
     * 注意：此处使用 HAL_UART_Transmit（阻塞轮询）而非 DMA 发送，
     * 因为 LoRa 帧短（<=58B）且发送频率低（50ms+ 间隔），
     * 阻塞发送的 CPU 开销可忽略，且实现更简单。
     */
    return HAL_UART_Transmit(ctx->huart, ctx->tx_buf, 3 + len, 100);
}

/*
 * LORA_Buf_Recv - 非阻塞接收并解析一帧
 *
 * 处理流程：
 *   1. 检查 rx_ready 标志，无新帧则立即返回 0
 *   2. 清除 rx_ready（标记此帧已被消费）
 *   3. 帧格式合法性校验：最小长度、帧头、载荷长度、总长一致性
 *   4. 通过输出参数返回解析后的 cmd、payload、len
 *
 * 参数：
 *   ctx     - LoRa 收发上下文
 *   cmd     - [输出] 命令码
 *   payload - [输出] 载荷数据缓冲区（调用者保证 >=55 字节空间）
 *   len     - [输出] 载荷字节数
 *
 * 返回值：
 *   1 - 成功解析一帧
 *   0 - 无新帧或帧格式错误（丢弃）
 */
int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len)
{
    /* 没有新的完整帧可读，立即返回 */
    if (!ctx->rx_ready) return 0;

    /* 标记此帧已被消费，允许 ISR 写入下一帧 */
    ctx->rx_ready = 0;

    /*
     * ---- 帧格式合法性校验 ----
     * 任何一项校验失败都丢弃整帧（返回 0），不做部分恢复尝试
     */

    /* 校验 1：最小帧长度检查 —— 协议帧至少需要 3 字节（HEADER + CMD + LEN） */
    if (ctx->rx_len < 3) return 0;

    /* 校验 2：帧头检查 —— 第一字节必须是 0xAA（LORA_FRAME_HEADER） */
    if (ctx->rx_buf[0] != LORA_FRAME_HEADER) return 0;

    /* 提取帧中的命令码和声明载荷长度 */
    uint8_t frame_cmd = ctx->rx_buf[1];
    uint8_t frame_len = ctx->rx_buf[2];

    /* 校验 3：载荷长度上限检查 —— 不能超过 55（LORA_MAX_PAYLOAD） */
    if (frame_len > LORA_MAX_PAYLOAD) return 0;

    /* 校验 4：实际接收长度 >= 帧头 + 声明载荷长度，防止读越界 */
    if (ctx->rx_len < (uint16_t)(3 + frame_len)) return 0;

    /* ---- 校验通过，输出解析结果 ---- */
    *cmd = frame_cmd;      /* 命令码 */
    *len = frame_len;      /* 载荷长度 */

    /* 仅当有载荷且输出缓冲区非空时才拷贝 */
    if (frame_len > 0 && payload != NULL) {
        memcpy(payload, &ctx->rx_buf[3], frame_len);
    }

    return 1;  /* 成功解析 */
}

/*
 * LORA_Buf_IdleHandler - USART3 IDLE 线空闲中断处理
 *
 * 在 USART3 总线空闲（一帧传输结束）时被调用。
 * 此函数必须在 stm32f1xx_it.c 的 USART3_IRQHandler() 中手动调用。
 *
 * 处理流程：
 *   1. 检查全局上下文是否已初始化
 *   2. 检查 IDLE 标志是否真的置位
 *   3. 清除 IDLE 标志
 *   4. 通过 DMA 剩余计数器计算本帧接收字节数
 *   5. 过滤假中断（初始化后首次触发时 recv_len==0）
 *   6. 仅当上一帧已被消费时才接受新帧（防止数据覆盖）
 *   7. 重启 DMA 接收（Normal 模式需手动重启）
 *
 * 参数：无（通过全局指针 g_lora_ctx 访问上下文）
 * 返回值：无
 */
void LORA_Buf_IdleHandler(void)
{
    /* 通过全局指针获取上下文 */
    LORA_Buf_t *ctx = g_lora_ctx;

    /* 安全检查：LORA_Buf_Init() 未调用前 g_lora_ctx 为 NULL */
    if (ctx == NULL || ctx->huart == NULL) return;

    /* 检查 USART3 的 IDLE 标志是否置位，未置位则不是 IDLE 中断，直接返回 */
    if (__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE) == RESET) return;

    /*
     * 清除 IDLE 标志
     * STM32F1 的清除方式：先读 SR（状态寄存器），再读 DR（数据寄存器）。
     * HAL 宏 __HAL_UART_CLEAR_IDLEFLAG 内部就是这么做的。
     */
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    /*
     * 计算本帧实际接收到的字节数
     *
     * 原理：DMA 启动时 CNDTR = LORA_RX_BUF_SIZE(64)，每收到 1 字节 CNDTR 减 1。
     * 因此：recv_len = 总缓冲区大小 - DMA 剩余计数 = 本次实际接收字节数
     *
     * 例如：收到 10 字节后触发 IDLE，CNDTR = 54，recv_len = 64 - 54 = 10
     */
    uint16_t recv_len = LORA_RX_BUF_SIZE -
                        (uint16_t)__HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);

    /*
     * 过滤假中断（phantom interrupt）
     *
     * STM32 USART 初始化完成后，IDLE 标志可能已经置位（因为总线上还没有数据，
     * 处于空闲状态）。启用 IDLE 中断后会立即触发一次，此时 recv_len == 0
     * （DMA 没有接收到任何数据）。这种情况直接忽略，不做处理。
     */
    if (recv_len == 0) return;

    /*
     * 仅当上一帧已被主循环消费（rx_ready == 0）时才接受新帧
     *
     * 如果 rx_ready 仍为 1（上一帧还没被取走），则丢弃当前帧。
     * 这是"最新帧覆盖"策略的折中：不覆盖未消费帧，宁可丢新帧。
     * 对于 OTA 场景，丢帧会导致序号不匹配错误，进而触发全量重传。
     *
     * 替代方案：使用双缓冲或环形队列，但 OTA 场景中帧间有 50ms 间隔，
     * 主循环处理速度远快于此，所以单缓冲 + 丢弃策略已足够可靠。
     */
    if (!ctx->rx_ready) {
        ctx->rx_len = recv_len;   /* 记录本帧长度 */
        ctx->rx_ready = 1;        /* 标记有新帧可读 */
    }

    /*
     * 重启 DMA 接收
     *
     * DMA Normal 模式不会自动重新开始，必须先 Stop 再重新启动。
     * 如果不重启，后续的 UART 数据将丢失（DMA 已停止工作）。
     *
     * 注意：此处存在极短的时间窗口（DMAStop 到新 ReceiveDMA 之间），
     * 如果此时有新字节到达会丢失。但 LoRa 帧间有 50ms 间隔，
     * 这个窗口（<1us）可忽略不计。
     */
    HAL_UART_DMAStop(ctx->huart);
    HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}
