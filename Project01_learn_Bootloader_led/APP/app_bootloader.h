/**
 * @file    app_bootloader.h
 * @brief   Bootloader 用户交互状态机 —— 公开接口与类型定义
 *
 * 提供 Bootloader 的串口命令交互流程：
 *   START → SIZE:<字节数> → bin 传输 → 校验 → 跳转
 *
 * 使用方法：
 *   1. 调用 AppBootloader_Init() 初始化
 *   2. 主循环中反复调用 AppBootloader_Process()
 */

#ifndef __APP_BOOTLOADER_H__
#define __APP_BOOTLOADER_H__

#include "flash_download.h"

/**
 * @brief  交互状态机状态枚举
 *
 * 状态流转：
 *   IDLE → WAIT_START → RECV_SIZE → TRANSFERRING
 *                                       ↓ (2s 超时)
 *                                    VERIFY → JUMP (成功)
 *                                           → ERROR (失败)
 */
typedef enum {
    APPBL_IDLE,          /**< 上电初始态，立即转为 WAIT_START */
    APPBL_WAIT_START,    /**< 等待用户发送 "START" 命令 */
    APPBL_RECV_SIZE,     /**< 等待用户发送 "SIZE:<字节数>" 命令 */
    APPBL_TRANSFERRING,  /**< 正在接收 bin 数据并写入 Flash */
    APPBL_VERIFY,        /**< 传输结束，校验字节数 */
    APPBL_JUMP,          /**< 校验通过，即将跳转 App */
    APPBL_ERROR          /**< 错误态，停机等待硬件复位 */
} AppBL_State_t;

/**
 * @brief  交互状态机上下文结构体
 *
 * 由调用方分配（通常为 main.c 中的静态变量），
 * 通过 AppBootloader_Init() 初始化后传入 Process() 使用。
 */
typedef struct {
    AppBL_State_t   state;            /**< 当前状态 */
    uint32_t        expected_size;    /**< 用户声明的固件字节数（来自 SIZE 命令） */
    uint32_t        last_frame_tick;  /**< 最后一次收到 UART 帧的系统 tick（用于超时判断） */
    FlashDownload_t dl_ctx;           /**< Flash 下载上下文（内嵌，管理写入地址和擦除状态） */
} AppBootloader_t;

/**
 * @brief  初始化交互状态机
 * @param  ctx  状态机上下文指针
 *
 * 打印欢迎菜单，设置初始状态为 WAIT_START。
 * 必须在 UART_DMA_Rx_Init() 之后调用。
 */
void AppBootloader_Init(AppBootloader_t *ctx);

/**
 * @brief  主循环处理函数
 * @param  ctx  状态机上下文指针
 *
 * 每次调用处理一帧 UART 数据或一个超时事件：
 *   - 有帧时：根据当前状态分发处理（命令解析 / Flash 写入）
 *   - 无帧时：检查超时并驱动状态转换
 *
 * 在 main.c 的 while(1) 中反复调用。
 */
void AppBootloader_Process(AppBootloader_t *ctx);

#endif
