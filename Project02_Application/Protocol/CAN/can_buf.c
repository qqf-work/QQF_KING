#include "can_buf.h"
#include "can.h"
#include <string.h>

void CAN_Buf_Init(CAN_Buf_t *ctx, CAN_HandleTypeDef *hcan)
{
    ctx->hcan = hcan;

    /* 1. 配置过滤器 —— 只接收标准 ID=0x001 的帧 */
    CAN_FilterTypeDef filterConfig = {0};
    filterConfig.FilterBank = 0;                          /* 使用过滤器 0（共 0-13） */
    filterConfig.FilterMode = CAN_FILTERMODE_IDMASK;      /* 掩码模式 */
    filterConfig.FilterScale = CAN_FILTERSCALE_32BIT;     /* 32位宽 */
    /* 标准ID在32位寄存器中位于 [31:21]，即 FilterIdHigh = ID << 5 */
    filterConfig.FilterIdHigh = 0x0001 << 5;              /* 期望 ID = 0x001 */
    filterConfig.FilterIdLow = 0x0000;
    /* 掩码全1：所有11位ID位都必须匹配 */
    filterConfig.FilterMaskIdHigh = 0x7FF << 5;           /* 0x7FF 覆盖全部11位标准ID */
    filterConfig.FilterMaskIdLow = 0x0000;
    filterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;     /* 匹配的帧存入 FIFO0 */
    filterConfig.FilterActivation = ENABLE;               /* 使能该过滤器 */

    HAL_CAN_ConfigFilter(ctx->hcan, &filterConfig);

    /* 2. 启动 CAN 外设，开始正常收发 */
    HAL_CAN_Start(ctx->hcan);
}

void CAN_Buf_Send(CAN_Buf_t *ctx, uint32_t id,
                  const uint8_t *data, uint8_t len)
{
    /* 等待至少一个发送邮箱空闲（共 3 个邮箱），带 100ms 超时 */
    uint32_t tick_start = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(ctx->hcan) == 0)
    {
        if (HAL_GetTick() - tick_start > 100) return;
    }

    CAN_TxHeaderTypeDef txHeader = {0};
    txHeader.StdId = id;                /* 标准 ID（11 位，范围 0x000-0x7FF） */
    txHeader.IDE   = CAN_ID_STD;        /* 使用标准格式（非扩展 29 位） */
    txHeader.RTR   = CAN_RTR_DATA;      /* 数据帧（非远程帧） */
    txHeader.DLC   = len;               /* 数据长度（1-8 字节） */

    uint32_t mailbox = 0;
    /* 将消息投入发送邮箱：句柄、帧头、数据、邮箱编号 */
    HAL_CAN_AddTxMessage(ctx->hcan, &txHeader,
                         (uint8_t *)data, &mailbox);
}

void CAN_Buf_Recv(CAN_Buf_t *ctx, CAN_RxMsg_t *rx_msg,
                  uint8_t *msg_count)
{
    /* 查询 FIFO0 中有多少帧待读取（最多 3 帧） */
    *msg_count = HAL_CAN_GetRxFifoFillLevel(ctx->hcan, CAN_RX_FIFO0);

    for (uint8_t i = 0; i < *msg_count; i++)
    {
        CAN_RxMsg_t *msg = &rx_msg[i];
        memset(msg, 0, sizeof(CAN_RxMsg_t));              /* 清空当前消息缓存 */
        /* 从 FIFO0 读取一帧：帧头写入 rxHeader，数据写入 data */
        HAL_CAN_GetRxMessage(ctx->hcan, CAN_RX_FIFO0,
                             &(msg->rxHeader), msg->data);
    }
}
