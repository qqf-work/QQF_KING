#ifndef __LORA_BUF_H__
#define __LORA_BUF_H__

#include "main.h"
#include <stdint.h>

/*
 * LoRa USART3 DMA + IDLE 帧收发封装
 *
 * 本模块对 E32-433T20D LoRa 模块的 UART 透明传输进行二次封装，
 * 实现"发送自动组帧、接收自动拆帧"的收发机制。
 *
 * 核心设计思路 —— DMA Normal 模式 + IDLE 线空闲检测：
 *   1. DMA 配置为 Normal 模式（非 Circular），持续将 USART3 接收到的
 *      字节写入 rx_buf[] 缓冲区
 *   2. 当 UART 总线空闲超过一个字符时间（即一帧接收完毕），硬件触发
 *      IDLE 中断
 *   3. IDLE 中断处理函数中，通过 DMA CNDTR 寄存器（剩余传输计数）反算
 *      本帧接收到的字节数，记录 rx_len 并置位 rx_ready
 *   4. 主循环中通过 LORA_Buf_Recv() 轮询 rx_ready 标志，非阻塞地取走帧
 *
 * 与 DMA Circular 模式的区别：
 *   Normal 模式下 DMA 传输完成后自动停止，需要手动重启。
 *   这恰好配合 IDLE 中断使用：每收完一帧 -> ISR 中重启 DMA -> 等待下一帧。
 *   优点是不需要环形缓冲区的读写指针管理，实现简单可靠。
 *
 * 线程安全说明：
 *   - rx_ready / rx_len 由 ISR 写入、主循环读取，因为都是单字节/原子操作，
 *     且主循环读取前会检查标志，所以不需要关中断保护
 *   - 发送使用阻塞式 HAL_UART_Transmit()，不与 DMA 接收冲突
 *     （USART3 全双工，收发可同时进行）
 *
 * 对外接口：
 *   LORA_Buf_Init()       — 初始化，启动 DMA + IDLE 中断
 *   LORA_Buf_Send()       — 构建协议帧并通过 USART3 阻塞发送
 *   LORA_Buf_Recv()       — 非阻塞查询，解析已收到的完整帧
 *   LORA_Buf_IdleHandler() — IDLE 中断回调（需在 stm32f1xx_it.c 中手动调用）
 *
 * 初始化调用顺序（在 main.c 中）：
 *   1. HAL_Init() + SystemClock_Config()
 *   2. MX_GPIO_Init()
 *   3. MX_DMA_Init()          <-- DMA 必须在 USART3 之前初始化！
 *   4. MX_USART3_UART_Init()  <-- CubeMX 生成的 UART 初始化
 *   5. LORA_Buf_Init()        <-- 本模块初始化，启动 DMA 接收
 */

/*
 * DMA 接收缓冲区大小
 *
 * 设为 64 字节，大于 LoRa 单包最大帧长 58 字节（E32-433T20D 限制），
 * 留有 6 字节余量。DMA 配置为接收 64 字节，但实际收到一帧后 IDLE
 * 中断就会触发，不会真的等到 64 字节收满。
 */
#define LORA_RX_BUF_SIZE     64

/*
 * 发送帧缓冲区大小
 *
 * = 3（帧头开销：0xAA + CMD + LEN）+ 55（最大载荷）= 58 字节
 * 这也是 LoRa 单包的最大传输上限
 */
#define LORA_TX_BUF_SIZE     (3 + 55)

/*
 * LORA_Buf_t —— LoRa 收发上下文结构体
 *
 * 封装了一次 DMA+IDLE 帧收发所需的全部状态，设计为值语义：
 * 调用者在栈上或静态区分配一个实例，将其地址传给各 API。
 *
 * 成员说明：
 *   huart    - USART3 的 HAL 句柄指针，由 LORA_Buf_Init() 保存，
 *              后续所有 HAL UART/DMA API 调用都需要此句柄
 *   rx_buf   - DMA 接收目标缓冲区，DMA 硬件直接将 UART 接收字节写入此数组
 *   rx_len   - 最近一次 IDLE 中断记录的帧长度（字节数），
 *              由 LORA_Buf_IdleHandler() 写入，由 LORA_Buf_Recv() 读取
 *   rx_ready - 完整帧就绪标志：
 *              0 = 缓冲区中没有未处理的新帧（或已被消费）
 *              1 = 收到了新的完整帧，等待 LORA_Buf_Recv() 消费
 *              ISR 中仅当 rx_ready==0 时才写入新帧（防止覆盖未消费帧）
 *   tx_buf   - 发送帧构建缓冲区，LORA_Buf_Send() 在此数组中组装完整协议帧
 *              后调用 HAL_UART_Transmit() 发送
 */
typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t  rx_buf[LORA_RX_BUF_SIZE];
    uint16_t rx_len;
    uint8_t  rx_ready;
    uint8_t  tx_buf[LORA_TX_BUF_SIZE];
} LORA_Buf_t;

/*
 * LORA_Buf_Init - 初始化 LoRa 传输层
 *
 * 功能：保存 UART 句柄，启用 IDLE 线空闲中断，启动 DMA Normal 模式接收。
 *       必须在 MX_USART3_UART_Init() 之后调用（确保 UART 外设已配置好）。
 *       必须在 MX_DMA_Init() 之后调用（确保 DMA 通道已配置好）。
 *
 * 参数：
 *   ctx   - LoRa 收发上下文指针，调用者分配的 LORA_Buf_t 实例地址
 *           函数会将此指针保存为全局变量 g_lora_ctx，供 IDLE ISR 访问
 *   huart - USART3 的 HAL 句柄指针，通常传 &huart3（CubeMX 生成的全局变量）
 *
 * 返回值：无
 *
 * 使用示例：
 *   LORA_Buf_t lora_buf;
 *   LORA_Buf_Init(&lora_buf, &huart3);
 */
void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart);

/*
 * LORA_Buf_Send - 发送一帧 LoRa 协议数据
 *
 * 功能：按照协议帧格式 [0xAA][CMD][LEN][PAYLOAD...] 组装完整帧，
 *       然后通过 HAL_UART_Transmit() 阻塞发送到 USART3。
 *
 * 参数：
 *   ctx     - LoRa 收发上下文指针
 *   cmd     - 命令码，取值见 lora_proto.h 中的 LORA_CMD_xxx 宏定义
 *             例如：LORA_CMD_UPDATE_REQ(0x01), LORA_CMD_UPDATE_DONE(0x83) 等
 *   payload - 载荷数据指针，指向要发送的数据内容
 *             无载荷的帧（如 REQ、READY、DONE）传 NULL 即可
 *   len     - 载荷字节数，范围 0~55（LORA_MAX_PAYLOAD）
 *             无载荷时传 0；超过 55 会返回 -1 错误
 *
 * 返回值：
 *   0       - 发送成功（HAL_OK）
 *   -1      - 参数错误（len > 55，即超过最大载荷限制）
 *   其他    - HAL 库错误码（HAL_ERROR、HAL_BUSY、HAL_TIMEOUT）
 *
 * 注意：此函数是阻塞式的，会等待 UART 发送完成或超时（100ms）。
 *       在 LoRa OTA 场景中，发送操作发生在帧间隙或状态转换时，
 *       不在高速接收路径中，因此阻塞发送不影响实时性。
 */
int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len);

/*
 * LORA_Buf_Recv - 非阻塞接收并解析一帧 LoRa 协议数据
 *
 * 功能：检查是否有新的完整帧（rx_ready 标志），如果有则解析帧头、
 *       命令码和载荷，将结果写入输出参数。
 *
 * 参数：
 *   ctx     - LoRa 收发上下文指针
 *   cmd     - [输出] 接收到的命令码，取值见 LORA_CMD_xxx 定义
 *   payload - [输出] 载荷数据缓冲区，调用者需保证至少 55 字节空间
 *             无载荷的帧（如 REQ、READY）不会写入此缓冲区
 *   len     - [输出] 载荷字节数（0~55），无载荷时为 0
 *
 * 返回值：
 *   1 - 成功接收到一帧并完成解析
 *   0 - 没有新帧，或帧格式错误（帧头不对、长度异常等）时丢弃并返回 0
 *
 * 调用方式：在主循环中持续轮询，返回 1 时处理命令，返回 0 时跳过：
 *   uint8_t cmd, payload[55], len;
 *   if (LORA_Buf_Recv(&lora_buf, &cmd, payload, &len)) {
 *       // 处理收到的帧...
 *   }
 */
int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len);

/*
 * LORA_Buf_IdleHandler - USART3 IDLE 线空闲中断处理函数
 *
 * 功能：在 USART3 总线空闲（一帧接收完毕）时被调用，
 *       计算本帧长度、记录到 rx_len、置位 rx_ready，然后重启 DMA 接收。
 *
 * 参数：无（通过全局指针 g_lora_ctx 访问上下文）
 * 返回值：无
 *
 * 调用位置：必须在 stm32f1xx_it.c 的 USART3_IRQHandler() 中手动调用：
 *   void USART3_IRQHandler(void) {
 *       HAL_UART_IRQHandler(&huart3);
 *       LORA_Buf_IdleHandler();  // <-- 添加此行
 *   }
 *
 * 中断处理流程：
 *   1. 检查 IDLE 标志是否置位（可能被其他中断源触发）
 *   2. 清除 IDLE 标志（读 SR + 读 DR）
 *   3. 通过 DMA CNDTR 计算实际接收字节数
 *   4. 过滤假中断（初始化时 IDLE 标志已置位，recv_len == 0 的情况）
 *   5. 仅当上一帧已被消费（rx_ready == 0）时才接受新帧（防覆盖）
 *   6. 停止 DMA -> 重启 DMA（Normal 模式需要手动重启）
 */
void LORA_Buf_IdleHandler(void);

#endif
