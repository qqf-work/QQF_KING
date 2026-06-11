/**
 * @file    app_bootloader.c
 * @brief   串口下载交互状态机实现（上位机侧）
 *
 * 移植自 Project02_factory_app，适配 P00 固件缓存区
 * 固件下载到 Flash 后，标记就绪供 CAN 转发使用
 *
 * 命令格式（ASCII，以 \r\n 或 \n 结尾）：
 *   START         - 触发进入传输模式
 *   SIZE:<十进制数> - 声明固件字节数
 *   其他帧内容    - 视为 bin 原始数据（仅 TRANSFERRING 状态）
 *
 * 超时机制：
 *   RECV_SIZE    30 秒无 SIZE 命令 → 回退 WAIT_START
 *   TRANSFERRING  2 秒无新帧      → 触发 VERIFY
 */

#include "app_bootloader.h"
#include "fw_cache_conf.h"
#include "uart_buf.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ---------- 内部辅助函数 ---------- */

static uint16_t match_cmd(uint8_t *data, uint16_t len, const char *cmd)
{
    uint16_t cmd_len = (uint16_t)strlen(cmd);
    if (len >= cmd_len && memcmp(data, cmd, cmd_len) == 0)
        return cmd_len;
    return 0;
}

static int parse_size(uint8_t *data, uint16_t len, uint32_t *out)
{
    uint32_t val = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        if (data[i] >= '0' && data[i] <= '9')
        {
            if (val > (0xFFFFFFFF - 9) / 10) return -1;
            val = val * 10 + (data[i] - '0');
        }
        else if (data[i] == '\r' || data[i] == '\n')
            break;
        else
            return -1;
    }
    *out = val;
    return (val == 0) ? -1 : 0;
}

/* ---------- 初始化 ---------- */

void AppBootloader_Init(AppBootloader_t *ctx)
{
    ctx->state           = APPBL_WAIT_START;
    ctx->expected_size   = 0;
    ctx->last_frame_tick = HAL_GetTick();

    printf("[BL] Host ready\r\n");
}

/* ---------- 各状态处理函数 ---------- */

static void handle_wait_start(AppBootloader_t *ctx, uint8_t *data, uint16_t len)
{
    if (match_cmd(data, len, "START"))
    {
        printf("[BL] Ready. Send SIZE:<bytes>\r\n");
        ctx->state = APPBL_RECV_SIZE;
    }
    else
    {
        /* ignore unknown commands */
    }
}

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

    if (size > FW_CACHE_SIZE)
    {
        printf("[BL] SIZE too large: %lu > %lu\r\n",
               size, (uint32_t)FW_CACHE_SIZE);
        ctx->state = APPBL_ERROR;
        return;
    }

    ctx->expected_size = size;
    FlashDownload_Init(&ctx->dl_ctx);
    printf("[BL] Expecting %lu bytes. Send bin now.\r\n", size);
    ctx->state = APPBL_TRANSFERRING;
}

static void handle_verify(AppBootloader_t *ctx)
{
    uint32_t total = FlashDownload_GetTotal(&ctx->dl_ctx);
    total += ctx->dl_ctx.last_byte_flag;

    if (total == ctx->expected_size)
    {
        printf("[BL] Verify OK: %lu bytes stored in Flash\r\n", total);
        printf("[BL] Firmware ready for CAN transfer.\r\n");
        ctx->state = APPBL_READY;
    }
    else
    {
        printf("[BL] Verify FAIL: expected %lu, got %lu\r\n",
               ctx->expected_size, total);
        ctx->state = APPBL_ERROR;
    }
}

/* ---------- 主轮询函数 ---------- */

void AppBootloader_Process(AppBootloader_t *ctx)
{
    /* 处理 UART 帧队列中的数据 */
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

        /* 前移 OUT 指针 */
        uart_rx_queue.URxDataOUT++;
        if (uart_rx_queue.URxDataOUT > uart_rx_queue.URxDataEND)
            uart_rx_queue.URxDataOUT = &uart_rx_queue.URxDataPtr[0];
    }
    else
    {
        /* 无数据时处理超时和状态推进 */
        uint32_t elapsed = HAL_GetTick() - ctx->last_frame_tick;

        if (ctx->state == APPBL_RECV_SIZE && elapsed > 30000)
        {
            printf("[BL] SIZE timeout, back to WAIT_START\r\n");
            ctx->state = APPBL_WAIT_START;
        }
        else if (ctx->state == APPBL_TRANSFERRING && elapsed > 2000)
        {
            ctx->state = APPBL_VERIFY;
            handle_verify(ctx);
        }
        else if (ctx->state == APPBL_ERROR)
        {
            static uint8_t error_count = 0;
            error_count++;
            if (error_count < 3)
            {
                FlashDownload_Init(&ctx->dl_ctx);
                ctx->state = APPBL_WAIT_START;
            }
            /* 3次重试后停机 */
        }
    }
}

uint32_t AppBootloader_GetFwSize(AppBootloader_t *ctx)
{
    if (ctx->state != APPBL_READY)
        return 0;

    return FlashDownload_GetTotal(&ctx->dl_ctx) + ctx->dl_ctx.last_byte_flag;
}
