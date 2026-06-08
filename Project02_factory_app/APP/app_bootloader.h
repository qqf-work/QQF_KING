/*
 * 出厂程序 —— 串口下载交互状态机
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
 */

#ifndef __APP_BOOTLOADER_H__
#define __APP_BOOTLOADER_H__

#include "flash_download.h"

typedef enum {
    APPBL_IDLE,
    APPBL_WAIT_START,
    APPBL_RECV_SIZE,
    APPBL_TRANSFERRING,
    APPBL_VERIFY,
    APPBL_JUMP,
    APPBL_ERROR
} AppBL_State_t;

typedef struct {
    AppBL_State_t   state;
    uint32_t        expected_size;
    uint32_t        last_frame_tick;
    FlashDownload_t dl_ctx;
} AppBootloader_t;

void AppBootloader_Init(AppBootloader_t *ctx);
void AppBootloader_Process(AppBootloader_t *ctx);

#endif
