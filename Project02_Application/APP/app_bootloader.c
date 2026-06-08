/*
 * Bootloader 用户交互状态机
 *
 * 通过串口文本命令控制固件下载流程：
 *   1. 用户发送 START 命令触发传输模式
 *   2. 用户发送 SIZE:<字节数> 声明固件大小
 *   3. 用户发送 bin 文件原始数据
 *   4. 2 秒无新帧触发校验：已写入字节数 vs 声明字节数
 *   5. 校验通过则跳转 App，失败则停机等复位
 *
 * 状态流转：
 *   IDLE → WAIT_START → RECV_SIZE → TRANSFERRING → VERIFY → JUMP / ERROR
 *
 * 命令格式（ASCII，以 \r\n 或 \n 结尾）：
 *   START       - 触发进入传输模式
 *   SIZE:<十进制数> - 声明固件字节数
 *   其他帧内容  - 视为 bin 原始数据（仅在 TRANSFERRING 状态）
 *
 * 超时机制：
 *   RECV_SIZE    30 秒无 SIZE 命令 → 回退 WAIT_START
 *   TRANSFERRING  2 秒无新帧      → 触发 VERIFY
 */

#include "app_bootloader.h"
#include "bootloader.h"
#include "uart_buf.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ========================== 命令解析辅助 ========================== */

/**
 * @brief  检查帧数据是否以指定命令前缀开头
 * @param  data  帧数据指针（来自 uart_rx_queue 的 DMA 缓冲区）
 * @param  len   帧数据长度（字节）
 * @param  cmd   期望的命令前缀字符串，如 "START" 或 "SIZE:"
 * @return 匹配的前缀长度（字节），0 表示不匹配
 *
 * 用于在 WAIT_START / RECV_SIZE 状态下区分文本命令和无关数据
 */
static uint16_t match_cmd(uint8_t *data, uint16_t len, const char *cmd)
{
    uint16_t cmd_len = (uint16_t)strlen(cmd);
    if (len >= cmd_len && memcmp(data, cmd, cmd_len) == 0)
        return cmd_len;
    return 0;
}

/**
 * @brief  从数字字符串解析出无符号 32 位整数
 * @param  data  数字字符串起始指针（通常是 "SIZE:" 之后的部分）
 * @param  len   可解析的最大长度
 * @param  out   输出：解析得到的数值
 * @return 0 成功, -1 失败（非法字符或值为 0）
 *
 * 遇到 \r 或 \n 停止解析。纯数字串，不允许前导空格。
 * 值为 0 视为非法（空固件无意义）。
 */
static int parse_size(uint8_t *data, uint16_t len, uint32_t *out)
{
    uint32_t val = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        if (data[i] >= '0' && data[i] <= '9')
            val = val * 10 + (data[i] - '0');
        else if (data[i] == '\r' || data[i] == '\n')
            break;
        else
            return -1;
    }
    *out = val;
    return (val == 0) ? -1 : 0;
}

/* ========================== 公开 API ========================== */

/**
 * @brief  初始化 Bootloader 交互状态机
 * @param  ctx  状态机上下文指针（由调用方分配）
 *
 * 将状态设为 IDLE，清零所有字段，打印欢迎菜单后立即转入 WAIT_START。
 * 必须在 UART_DMA_Rx_Init() 之后调用，确保帧队列已就绪。
 */
void AppBootloader_Init(AppBootloader_t *ctx)
{
    ctx->state          = APPBL_IDLE;
    ctx->expected_size  = 0;
    ctx->last_frame_tick = HAL_GetTick();

    printf("\r\n===== STM32 Bootloader =====\r\n");
    printf("Commands:\r\n");
    printf("  START       - Begin firmware transfer\r\n");
    printf("  SIZE:<n>    - Declare firmware size in bytes\r\n");
    printf("Waiting for START...\r\n");
    printf("============================\r\n\r\n");

    ctx->state = APPBL_WAIT_START;
}

/* ========================== 状态处理函数 ========================== */

/**
 * @brief  WAIT_START 状态处理：等待用户发送 START 命令
 * @param  ctx   状态机上下文
 * @param  data  当前帧数据
 * @param  len   当前帧长度
 *
 * 匹配到 "START" → 打印就绪提示，转入 RECV_SIZE
 * 其他内容 → 提示未知命令
 */
static void handle_wait_start(AppBootloader_t *ctx, uint8_t *data, uint16_t len)
{
    if (match_cmd(data, len, "START"))
    {
        printf("[BL] Ready. Send SIZE:<bytes>\r\n");
        ctx->state = APPBL_RECV_SIZE;
    }
    else
    {
        printf("[BL] Unknown command. Send START to begin.\r\n");
    }
}

/**
 * @brief  RECV_SIZE 状态处理：等待用户发送 SIZE:<字节数> 命令
 * @param  ctx   状态机上下文
 * @param  data  当前帧数据
 * @param  len   当前帧长度
 *
 * 解析 "SIZE:" 后的十进制数字，校验合法性：
 *   - 非 SIZE 命令 → 提示格式错误，留在当前状态
 *   - 值为 0 或含非法字符 → 提示无效
 *   - 超过 A 区容量 → 进入 ERROR
 *   - 合法 → 记录 expected_size，初始化 FlashDownload，转入 TRANSFERRING
 */
static void handle_recv_size(AppBootloader_t *ctx, uint8_t *data, uint16_t len)
{
    uint16_t prefix_len = match_cmd(data, len, "SIZE:");
    if (prefix_len == 0)
    {
        printf("[BL] Expected SIZE:<bytes>\r\n");
        return;
    }

    uint32_t size;
    if (parse_size(data + prefix_len, len - prefix_len, &size) != 0)
    {
        printf("[BL] Invalid SIZE value\r\n");
        return;
    }

    if (size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[BL] SIZE too large: %lu > %lu\r\n",
               size, (uint32_t)(A_PAGE_NUM * FLASH__PAGE_SIZE));
        ctx->state = APPBL_ERROR;
        return;
    }

    ctx->expected_size = size;
    FlashDownload_Init(&ctx->dl_ctx);
    printf("[BL] Expecting %lu bytes. Send bin file now.\r\n", size);
    ctx->state = APPBL_TRANSFERRING;
}

/**
 * @brief  VERIFY 状态处理：校验已写入字节数与用户声明是否一致
 * @param  ctx  状态机上下文
 *
 * 从 FlashDownload_GetTotal() 获取实际写入字节数，与 expected_size 比较：
 *   - 匹配 → 转入 JUMP
 *   - 不匹配 → 转入 ERROR（停机，需硬件复位恢复）
 */
static void handle_verify(AppBootloader_t *ctx)
{
    uint32_t total = FlashDownload_GetTotal(&ctx->dl_ctx);

    /* 加上跨帧缓存的奇数字节（如果存在） */
    total += ctx->dl_ctx.last_byte_flag;

    if (total == ctx->expected_size)
    {
        printf("[BL] Verify OK: %lu bytes\r\n", total);
        ctx->state = APPBL_JUMP;
    }
    else
    {
        printf("[BL] Verify FAIL: expected %lu, got %lu\r\n",
               ctx->expected_size, total);
        ctx->state = APPBL_ERROR;
    }
}

/**
 * @brief  JUMP 状态处理：跳转到 A 区 App 执行
 * @param  ctx  状态机上下文
 *
 * 调用 Bootloader_JumpToApp()，成功则不返回。
 * 跳转失败（App 无效）→ 转入 ERROR。
 */
static void handle_jump(AppBootloader_t *ctx)
{
    printf("[BL] Jumping to App...\r\n");
    if (Bootloader_JumpToApp() != 0)
    {
        printf("[BL] Jump failed\r\n");
        ctx->state = APPBL_ERROR;
    }
}

/* ========================== 主处理函数 ========================== */

/**
 * @brief  Bootloader 主循环处理函数
 * @param  ctx  状态机上下文指针（由 AppBootloader_Init 初始化）
 *
 * 每个 main 循环周期调用一次。两个分支：
 *
 * 有新帧（URxDataOUT != URxDataIN）：
 *   根据 state 分发到对应处理函数：
 *     WAIT_START   → handle_wait_start()  解析 START 命令
 *     RECV_SIZE    → handle_recv_size()   解析 SIZE 命令
 *     TRANSFERRING → FlashDownload_WriteFrame() 写 bin 数据到 Flash
 *   处理完毕后释放帧（URxDataOUT 指针前移，环形回绕）
 *
 * 无帧（队列为空）：
 *   基于超时驱动状态转换：
 *     RECV_SIZE    超过 30s → 回退 WAIT_START
 *     TRANSFERRING 超过 2s  → 进入 VERIFY 校验
 *     JUMP                   → 执行 handle_jump()
 *     ERROR                  → 空转停机，等待硬件复位
 */
void AppBootloader_Process(AppBootloader_t *ctx)
{
    /* 有新帧：根据状态分发 */
    if (uart_rx_queue.URxDataOUT != uart_rx_queue.URxDataIN)
    {
        uint16_t len = uart_rx_queue.URxDataOUT->end
                      - uart_rx_queue.URxDataOUT->start + 1;
        uint8_t *data = uart_rx_queue.URxDataOUT->start;

        ctx->last_frame_tick = HAL_GetTick();

        switch (ctx->state)
        {
        case APPBL_WAIT_START:
            handle_wait_start(ctx, data, len);
            break;
        case APPBL_RECV_SIZE:
            handle_recv_size(ctx, data, len);
            break;
        case APPBL_TRANSFERRING:
            if (FlashDownload_WriteFrame(&ctx->dl_ctx, data, len) != 0)
            {
                printf("[BL] Flash write error\r\n");
                ctx->state = APPBL_ERROR;
            }
            break;
        default:
            break;
        }

        /* 释放帧：OUT 指针前移，超过数组末尾则回绕到起始 */
        uart_rx_queue.URxDataOUT++;
        if (uart_rx_queue.URxDataOUT > uart_rx_queue.URxDataEND)
            uart_rx_queue.URxDataOUT = &uart_rx_queue.URxDataPtr[0];
    }
    else
    {
        /* 无帧：超时检查 */
        uint32_t elapsed = HAL_GetTick() - ctx->last_frame_tick;

        if (ctx->state == APPBL_RECV_SIZE && elapsed > 30000)
        {
            /* SIZE 命令超时 30s：回退到等待 START */
            printf("[BL] SIZE timeout, back to WAIT_START\r\n");
            ctx->state = APPBL_WAIT_START;
        }
        else if (ctx->state == APPBL_TRANSFERRING && elapsed > 2000)
        {
            /* 传输超时 2s：进入校验 */
            ctx->state = APPBL_VERIFY;
            handle_verify(ctx);
        }
        else if (ctx->state == APPBL_JUMP)
        {
            /* 校验通过，执行跳转 */
            handle_jump(ctx);
        }
        else if (ctx->state == APPBL_ERROR)
        {
            /* 错误态：打印提示并软复位 */
            printf("[BL] System reset...\r\n");
            HAL_Delay(100);
            NVIC_SystemReset();
        }
    }
}
