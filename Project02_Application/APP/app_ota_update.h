#ifndef __APP_OTA_UPDATE_H__
#define __APP_OTA_UPDATE_H__

#include "can_buf.h"
#include "ota_storage.h"
#include <stdint.h>

/*
 * CAN OTA 更新状态机
 *
 * 通过 CAN 从网关接收固件数据，调用 ota_storage 写入 W25Q16，
 * 完成后写 EEPROM 标志位触发 Bootloader 更新。
 *
 * 使用方式：
 *   APP_OTA_Init()   — 初始化
 *   APP_OTA_Process() — 主循环中轮询调用
 */

/* 状态机状态 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_WAIT_ACK,
    OTA_STATE_RECV_DATA,
    OTA_STATE_DONE,
    OTA_STATE_ERROR
} OTA_State_t;

/* 等待 ACK 超时（ms） */
#define OTA_WAIT_ACK_TIMEOUT  5000

/* DONE 状态等待时间（ms），之后重新请求 */
#define OTA_DONE_DELAY        2000

/* ERROR 后退避等待（ms） */
#define OTA_ERROR_BACKOFF     3000

typedef struct {
    OTA_State_t    state;
    OTA_Storage_t  storage;
    CAN_Buf_t     *can_ctx;
    uint16_t       expect_seq;    /* 期望的下一个数据帧序号 */
    uint32_t       state_tick;    /* 进入当前状态时的 tick */
    uint8_t        error_code;    /* 错误码 */
} APP_OTA_t;

/* 初始化：绑定 CAN 上下文和存储驱动 */
void APP_OTA_Init(APP_OTA_t *ctx, CAN_Buf_t *can_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom);

/* 主循环轮询调用，处理 CAN 消息并推进状态机 */
void APP_OTA_Process(APP_OTA_t *ctx);

#endif
