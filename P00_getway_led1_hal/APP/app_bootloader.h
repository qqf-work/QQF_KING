/**
 * @file    app_bootloader.h
 * @brief   串口下载交互状态机（上位机侧）
 *
 * 通过串口文本命令控制固件下载流程：
 *   1. 用户发送 START 命令触发传输模式
 *   2. 用户发送 SIZE:<字节数> 声明固件大小
 *   3. 用户发送 bin 文件原始数据（每帧 <=256 字节）
 *   4. 2 秒无新帧触发校验：已写入字节数 vs 声明字节数
 *   5. 校验通过后标记固件就绪，等待 CAN 转发
 *
 * 状态流转：
 *   IDLE → WAIT_START → RECV_SIZE → TRANSFERRING → VERIFY → READY / ERROR
 *
 * READY 状态下 CAN 状态机可从 Flash 读取固件并发送给 A 设备
 */

#ifndef __APP_BOOTLOADER_H__
#define __APP_BOOTLOADER_H__

#include "flash_download.h"
#include <stdint.h>

typedef enum {
    APPBL_IDLE,
    APPBL_WAIT_START,
    APPBL_RECV_SIZE,
    APPBL_TRANSFERRING,
    APPBL_VERIFY,
    APPBL_READY,
    APPBL_ERROR
} AppBL_State_t;

typedef struct {
    AppBL_State_t   state;
    uint32_t        expected_size;    /* 声明的固件大小（字节） */
    uint32_t        last_frame_tick;  /* 最后一次收到数据的 tick */
    FlashDownload_t dl_ctx;           /* Flash 下载上下文 */
} AppBootloader_t;

/**
 * @brief  初始化串口下载状态机
 * @param  ctx  状态机上下文
 */
void AppBootloader_Init(AppBootloader_t *ctx);

/**
 * @brief  状态机轮询，在 main while(1) 中调用
 *         处理 UART 帧接收、超时检测、校验
 * @param  ctx  状态机上下文
 */
void AppBootloader_Process(AppBootloader_t *ctx);

/**
 * @brief  查询固件是否已下载就绪
 * @return 已写入的字节数，0 表示未就绪
 */
uint32_t AppBootloader_GetFwSize(AppBootloader_t *ctx);

#endif
