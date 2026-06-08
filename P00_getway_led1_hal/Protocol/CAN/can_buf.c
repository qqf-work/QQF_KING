#include "can_buf.h"
#include "can.h"
#include <string.h>

/**
 * @brief  初始化 CAN：配置硬件过滤器 + 启动 CAN 外设
 * @param  ctx   CAN 收发上下文，内部持有 hcan 句柄
 * @param  hcan  CubeMX 生成的 CAN 句柄（已由 MX_CAN_Init 初始化）
 *
 * 过滤器配置为只接收标准 ID=0x000 的帧（即 A 设备发送的帧），
 * 其他 ID 的帧在硬件层直接丢弃，不产生中断
 */
void CAN_Buf_Init(CAN_Buf_t *ctx, CAN_HandleTypeDef *hcan)
{
    ctx->hcan = hcan;

    /* 配置过滤器：掩码模式，32 位宽，精确匹配标准 ID = 0x000 */
    CAN_FilterTypeDef filterConfig = {0};
    filterConfig.FilterBank = 0;                          /* 使用过滤器组 0（共 14 组：0-13） */
    filterConfig.FilterMode = CAN_FILTERMODE_IDMASK;      /* 掩码模式：ID + MASK 联合匹配 */
    filterConfig.FilterScale = CAN_FILTERSCALE_32BIT;     /* 32 位宽：提供更精确的过滤 */
    filterConfig.FilterIdHigh = (0x000 << 5);             /* 期望 ID：0x000 左移 5 位（寄存器格式） */
    filterConfig.FilterIdLow = 0x0000;
    filterConfig.FilterMaskIdHigh = (0x7FF << 5);         /* 掩码：0x7FF = 全部 11 位 ID 都要匹配 */
    filterConfig.FilterMaskIdLow = 0x0000;                /* 只接受 ID 完全等于 0x000 的帧 */
    filterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;     /* 匹配的帧存入接收 FIFO0 */
    filterConfig.FilterActivation = ENABLE;               /* 使能该过滤器 */

    HAL_CAN_ConfigFilter(ctx->hcan, &filterConfig);

    /* 启动 CAN 外设，进入正常收发模式 */
    HAL_CAN_Start(ctx->hcan);
}

/**
 * @brief  发送一帧 CAN 数据（标准格式，数据帧）
 * @param  ctx   CAN 收发上下文
 * @param  id    标准 ID（11 位，范围 0x000-0x7FF）
 *               Host 发送用 0x001，A 设备发送用 0x000
 * @param  data  发送数据缓冲区指针
 * @param  len   数据长度（1-8 字节）
 *
 * 内部阻塞等待发送邮箱空闲后投递，由硬件自动发送到 CAN 总线
 */
void CAN_Buf_Send(CAN_Buf_t *ctx, uint32_t id,
                  const uint8_t *data, uint8_t len)
{
    /* 参数校验：CAN 数据帧长度范围 1~8 字节 */
    if (len == 0 || len > 8)
        return;

    /* 等待至少一个发送邮箱空闲（STM32 有 3 个发送邮箱），带 100ms 超时 */
    uint32_t tick_start = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(ctx->hcan) == 0)
    {
        if (HAL_GetTick() - tick_start > 100) return;
    }

    /* 组装发送帧头 */
    CAN_TxHeaderTypeDef txHeader = {0};
    txHeader.StdId = id;                /* 标准 ID（11 位，范围 0x000-0x7FF） */
    txHeader.IDE   = CAN_ID_STD;        /* 标准格式（非扩展 29 位） */
    txHeader.RTR   = CAN_RTR_DATA;      /* 数据帧（非远程帧） */
    txHeader.DLC   = len;               /* 数据长度（1-8 字节） */

    /* 投入空闲发送邮箱，硬件自动将帧发送到 CAN 总线 */
    uint32_t mailbox = 0;
    HAL_CAN_AddTxMessage(ctx->hcan, &txHeader,
                         (uint8_t *)data, &mailbox);
}

/**
 * @brief  接收 FIFO0 中所有 CAN 帧（轮询方式，一次性读空）
 * @param  ctx        CAN 收发上下文
 * @param  rx_msg     接收缓冲区数组（建议大小 3，FIFO0 最多缓存 3 帧）
 * @param  msg_count  输出参数，实际接收到的帧数
 *
 * 每次调用将 FIFO0 中所有帧读出，清空缓冲区
 */
void CAN_Buf_Recv(CAN_Buf_t *ctx, CAN_RxMsg_t *rx_msg,
                  uint8_t *msg_count)
{
    /* 查询 FIFO0 中有多少帧待读取（硬件最多缓存 3 帧） */
    *msg_count = HAL_CAN_GetRxFifoFillLevel(ctx->hcan, CAN_RX_FIFO0);

    for (uint8_t i = 0; i < *msg_count; i++)
    {
        CAN_RxMsg_t *msg = &rx_msg[i];
        memset(msg, 0, sizeof(CAN_RxMsg_t));              /* 清空当前消息缓存 */
        /* 从 FIFO0 取出一帧：帧头写入 rxHeader，数据写入 data 数组 */
        HAL_CAN_GetRxMessage(ctx->hcan, CAN_RX_FIFO0,
                             &(msg->rxHeader), msg->data);
    }
}
