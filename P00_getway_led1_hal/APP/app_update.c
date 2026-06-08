/**
 * @file    app_update.c
 * @brief   CAN 固件更新状态机（上位机侧）
 *
 * 两种状态:
 *   APP_WAIT_UPDATE_CMD - 等待 A 程序通过 CAN 发送 UPDATE_REQ 请求
 *   APP_UPDATE_SEND     - 逐帧发送固件数据
 *
 * 状态转换:
 *   WAIT_CMD --[收到 REQ]--> SEND --[全部数据发送完成]--> WAIT_CMD
 *
 * 每次 AppUpdate_Poll() 只处理一个动作:
 *   - WAIT_CMD 状态: 轮询一次 CAN 接收
 *   - SEND 状态: 发送一帧 DATA（或全部发完后发送 END）
 */

#include "app_update.h"
#include "can_proto.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief  初始化更新上下文
 * @param  ctx      更新状态机上下文
 * @param  can_ctx  CAN 缓冲句柄（外部已初始化）
 * @param  fw_data  固件二进制数据指针
 * @param  fw_size  固件大小（字节）
 */
void AppUpdate_Init(AppUpdate_t *ctx, CAN_Buf_t *can_ctx,
                    const uint8_t *fw_data, uint32_t fw_size)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = APP_WAIT_UPDATE_CMD;
    ctx->can_ctx = can_ctx;
    ctx->fw_data = fw_data;
    ctx->fw_size = (fw_size == 0) ? 0 : fw_size;  /* fw_size=0 保护 */
}

/**
 * @brief  发送 UPDATE_ACK 帧，携带固件总大小（4 字节小端序）
 */
static void send_ack(AppUpdate_t *ctx)
{
    /* ACK 帧: CMD(1B) + fw_size(4B LE) */
    uint8_t ack[5] = {
        CAN_PROTO_CMD_UPDATE_ACK,
        (uint8_t)(ctx->fw_size),
        (uint8_t)(ctx->fw_size >> 8),
        (uint8_t)(ctx->fw_size >> 16),
        (uint8_t)(ctx->fw_size >> 24)
    };
    CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_HOST, ack, 5);
    printf("[Host] ACK sent, size=%lu\r\n", ctx->fw_size);
}

/**
 * @brief  发送一帧 UPDATE_DATA
 *         帧格式: CMD(1B) + seq(2B LE) + data(<=5B)
 */
static void send_data_frame(AppUpdate_t *ctx)
{
    uint8_t buf[8];
    buf[0] = CAN_PROTO_CMD_UPDATE_DATA;
    buf[1] = (uint8_t)(ctx->fw_seq);
    buf[2] = (uint8_t)(ctx->fw_seq >> 8);

    /* 计算本帧数据长度: 取剩余字节数和最大5字节中的较小值 */
    uint32_t remain = ctx->fw_size - ctx->fw_offset;
    uint8_t chunk = (remain > CAN_PROTO_MAX_DATA_PER_FRAME)
                  ? CAN_PROTO_MAX_DATA_PER_FRAME : (uint8_t)remain;

    memcpy(&buf[3], &ctx->fw_data[ctx->fw_offset], chunk);

    CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_HOST, buf, 3 + chunk);
    ctx->fw_offset += chunk;
    ctx->fw_seq++;

    /* 每 10 帧或全部发完时打印进度 */
    if (ctx->fw_seq % 10 == 0 || ctx->fw_offset >= ctx->fw_size)
        printf("[Host] %lu/%lu\r\n", ctx->fw_offset, ctx->fw_size);
}

/**
 * @brief  发送 UPDATE_END 帧，通知 A 程序固件数据全部发送完成
 */
static void send_end(AppUpdate_t *ctx)
{
    uint8_t end[1] = { CAN_PROTO_CMD_UPDATE_END };
    CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_HOST, end, 1);
    printf("[Host] END sent\r\n");
}

/**
 * @brief  状态 APP_WAIT_UPDATE_CMD 处理函数
 *         轮询 CAN 接收缓冲区，查找 UPDATE_REQ 命令
 *         收到后: 发送 ACK，重置发送进度，切换到 APP_UPDATE_SEND 状态
 */
void AppUpdate_WaitCmd(AppUpdate_t *ctx)
{
    CAN_RxMsg_t rx_msgs[3];
    uint8_t msg_count = 0;
    CAN_Buf_Recv(ctx->can_ctx, rx_msgs, &msg_count);

    for (uint8_t i = 0; i < msg_count; i++)
    {
        if (rx_msgs[i].rxHeader.DLC >= 1 &&
            rx_msgs[i].data[0] == CAN_PROTO_CMD_UPDATE_REQ)
        {
            send_ack(ctx);
            /* 重置发送偏移和序号 */
            ctx->fw_offset = 0;
            ctx->fw_seq = 0;
            /* 等待目标擦除完成后发 READY */
            ctx->state = APP_WAIT_READY;
            ctx->wait_ready_tick = HAL_GetTick();
            return;
        }
    }
}

/**
 * @brief  状态 APP_WAIT_READY 处理函数
 *         等待目标设备擦除 W25Q16 完成后发送 READY 命令
 *         收到后切换到 APP_UPDATE_SEND 开始发送数据
 */
void AppUpdate_WaitReady(AppUpdate_t *ctx)
{
    /* 60 秒超时保护 */
    if (HAL_GetTick() - ctx->wait_ready_tick > 60000)
    {
        printf("[Host] READY timeout\r\n");
        ctx->state = APP_WAIT_UPDATE_CMD;
        return;
    }

    CAN_RxMsg_t rx_msgs[3];
    uint8_t msg_count = 0;
    CAN_Buf_Recv(ctx->can_ctx, rx_msgs, &msg_count);

    for (uint8_t i = 0; i < msg_count; i++)
    {
        if (rx_msgs[i].rxHeader.DLC >= 1 &&
            rx_msgs[i].data[0] == CAN_PROTO_CMD_UPDATE_READY)
        {
            ctx->state = APP_UPDATE_SEND;
            return;
        }
    }
}

/**
 * @brief  状态 APP_UPDATE_SEND 处理函数
 *         每次调用发送一帧 DATA，全部发完后发送 END 并回到 WAIT 状态
 */
void AppUpdate_Send(AppUpdate_t *ctx)
{
    if (ctx->fw_offset < ctx->fw_size)
    {
        /* 还有数据未发送，发送下一帧 */
        send_data_frame(ctx);
        HAL_Delay(2);  /* 帧间延时 2ms，防止 A 设备 CAN FIFO 溢出 */
    }
    else
    {
        /* 全部发送完成，发送 END 帧，回到等待状态 */
        send_end(ctx);
        ctx->state = APP_WAIT_UPDATE_CMD;
    }
}

/**
 * @brief  状态机轮询入口，在 main while(1) 中调用
 *         根据当前状态分发到对应的处理函数
 */
void AppUpdate_Poll(AppUpdate_t *ctx)
{
    /* 空指针保护 */
    if (ctx == NULL || ctx->can_ctx == NULL)
        return;

    switch (ctx->state)
    {
    case APP_WAIT_UPDATE_CMD:
        AppUpdate_WaitCmd(ctx);
        break;
    case APP_WAIT_READY:
        AppUpdate_WaitReady(ctx);
        break;
    case APP_UPDATE_SEND:
        AppUpdate_Send(ctx);
        break;
    default:
        /* 异常状态恢复到等待状态 */
        ctx->state = APP_WAIT_UPDATE_CMD;
        break;
    }
}
