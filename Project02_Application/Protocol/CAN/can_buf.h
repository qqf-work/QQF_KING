#ifndef __CAN_BUF_H__
#define __CAN_BUF_H__

#include "main.h"
#include <stdint.h>

/*
 * CAN 轮询收发封装
 *
 * 对 CubeMX 生成的 hcan 句柄做二次封装：
 *   CAN_Buf_Init  — 配置过滤器 + 启动 CAN
 *   CAN_Buf_Send  — 发送一帧（标准 ID，最多 8 字节）
 *   CAN_Buf_Recv  — 一次性读取 FIFO0 中所有帧（最多 3 帧）
 *
 * 调用流程：
 *   1. CubeMX 的 MX_CAN_Init() 完成硬件初始化
 *   2. CAN_Buf_Init() 配置过滤器并启动 CAN
 *   3. CAN_Buf_Send() / CAN_Buf_Recv() 进行收发
 */

#define CAN_BUF_MAX_DLC  8

/* CAN 接收消息结构体：包含帧头信息和数据 */
typedef struct {
    CAN_RxHeaderTypeDef rxHeader;     /* 接收帧头：ID、DLC、IDE 等 */
    uint8_t data[CAN_BUF_MAX_DLC];   /* 帧数据，最多 8 字节 */
} CAN_RxMsg_t;

/* CAN 收发上下文：持有 hcan 句柄，所有函数通过 ctx 操作 */
typedef struct {
    CAN_HandleTypeDef *hcan;          /* 指向 CubeMX 生成的 CAN 句柄 */
} CAN_Buf_t;

/*
 * 初始化 CAN：配置过滤器（接收所有帧）+ 启动 CAN 外设
 * 必须在 MX_CAN_Init() 之后调用
 */
void CAN_Buf_Init(CAN_Buf_t *ctx, CAN_HandleTypeDef *hcan);

/*
 * 发送一帧 CAN 数据（标准格式，数据帧）
 * id: 标准 ID（11位），data: 数据指针，len: 数据长度（1-8）
 * 内部会等待发送邮箱空闲后发送
 */
void CAN_Buf_Send(CAN_Buf_t *ctx, uint32_t id,
                  const uint8_t *data, uint8_t len);

/*
 * 接收 FIFO0 中所有帧
 * rx_msg: 接收缓冲区数组（建议大小 3，FIFO0 最多缓存 3 帧）
 * msg_count: 输出实际接收到的帧数
 */
void CAN_Buf_Recv(CAN_Buf_t *ctx, CAN_RxMsg_t *rx_msg,
                  uint8_t *msg_count);

#endif
