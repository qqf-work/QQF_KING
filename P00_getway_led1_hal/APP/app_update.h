/**
 * @file    app_update.h
 * @brief   CAN 固件更新状态机接口（上位机侧）
 *
 * 使用方法:
 *   1. AppUpdate_Init() 初始化上下文
 *   2. 在 main while(1) 中调用 AppUpdate_Poll() 驱动状态机
 */

#ifndef __APP_UPDATE_H__
#define __APP_UPDATE_H__

#include "can_buf.h"
#include "crc32.h"
#include "can_proto.h"
#include <stdint.h>

/**
 * @brief  状态机状态枚举
 */
typedef enum {
    APP_WAIT_UPDATE_CMD = 0,  /* 等待 A 程序发送 UPDATE_REQ */
    APP_WAIT_READY,           /* 等待 A 程序擦除完成后发 READY */
    APP_UPDATE_SEND           /* 逐帧发送固件数据 */
} AppUpdate_State_t;

/**
 * @brief  更新状态机上下文结构体
 */
typedef struct {
    AppUpdate_State_t state;  /* 当前状态 */
    CAN_Buf_t *can_ctx;       /* CAN 缓冲句柄 */
    const uint8_t *fw_data;   /* 固件数据指针 */
    uint32_t fw_size;         /* 固件总大小（字节） */
    uint32_t fw_offset;       /* 当前发送偏移量 */
    uint16_t fw_seq;          /* 当前帧序号 */
    uint32_t wait_ready_tick; /* WAIT_READY 状态超时计时 */
    uint32_t fw_crc;          /* Firmware CRC32 (pre-calculated before sending) */
} AppUpdate_t;

/**
 * @brief  初始化更新上下文
 * @param  ctx      状态机上下文
 * @param  can_ctx  CAN 缓冲句柄
 * @param  fw_data  固件数据指针
 * @param  fw_size  固件大小（字节）
 */
void AppUpdate_Init(AppUpdate_t *ctx, CAN_Buf_t *can_ctx,
                    const uint8_t *fw_data, uint32_t fw_size);

/**
 * @brief  等待更新命令处理（APP_WAIT_UPDATE_CMD 状态）
 *         轮询 CAN 接收，收到 UPDATE_REQ 后发送 ACK 并切换状态
 * @param  ctx  状态机上下文
 */
void AppUpdate_WaitCmd(AppUpdate_t *ctx);

/**
 * @brief  发送固件数据处理（APP_UPDATE_SEND 状态）
 *         每次调用发送一帧 DATA，全部发完后发送 END
 * @param  ctx  状态机上下文
 */
void AppUpdate_Send(AppUpdate_t *ctx);

/**
 * @brief  状态机轮询入口，根据当前状态分发到对应处理函数
 * @param  ctx  状态机上下文
 */
void AppUpdate_Poll(AppUpdate_t *ctx);

#endif
