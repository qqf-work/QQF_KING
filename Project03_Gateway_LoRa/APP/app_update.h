/**
 * @file    app_update.h
 * @brief   LoRa 固件更新状态机（网关侧）
 *
 * 本模块实现了网关设备作为"固件分发服务器"的核心逻辑。网关在 Flash 缓存区
 * (0x08004000) 中保存一份待分发的固件 .bin 数据，当远端 App 设备通过 LoRa
 * 发送 UPDATE_REQ 请求后，网关按状态机流程逐步将固件发送给 App。
 *
 * 状态机共 3 个状态，转换图如下：
 *
 *   ┌──────────────────────┐
 *   │ APP_WAIT_UPDATE_CMD  │ ◄─────── 初始状态 / 更新完成后回到此状态
 *   │ 等待 App 发送 REQ    │
 *   └──────┬───────────────┘
 *          │ 收到 LORA_CMD_UPDATE_REQ
 *          │ → 预计算 CRC32
 *          │ → 发送 ACK（含固件大小）
 *          ▼
 *   ┌──────────────────────┐
 *   │ APP_WAIT_READY       │ ──超时 60s──→ 回到 APP_WAIT_UPDATE_CMD
 *   │ 等待 App 擦除完成    │
 *   └──────┬───────────────┘
 *          │ 收到 LORA_CMD_UPDATE_READY
 *          ▼
 *   ┌──────────────────────┐
 *   │ APP_UPDATE_SEND      │
 *   │ 逐帧发送固件数据     │ ──每帧 50ms 间隔──→ 循环发送
 *   │ 全部发完 → 发 END    │
 *   └──────┬───────────────┘
 *          │ 发送 END 帧完成
 *          ▼
 *   回到 APP_WAIT_UPDATE_CMD（等待下一次更新请求或 DONE/ERR 响应）
 *
 * 使用方法（在 main.c 中）：
 *   @code
 *   AppUpdate_t update_ctx;
 *   AppUpdate_Init(&update_ctx, &lora_ctx, (const uint8_t *)0x08004000, fw_size);
 *   while (1) {
 *       AppUpdate_Poll(&update_ctx);
 *       // ... 其他任务
 *   }
 *   @endcode
 */

#ifndef __APP_UPDATE_H__
#define __APP_UPDATE_H__

#include "lora_buf.h"
#include "crc32.h"
#include "lora_proto.h"
#include <stdint.h>

/**
 * @brief  状态机状态枚举
 *
 * 定义网关 OTA 更新流程的三个阶段。状态转换由 AppUpdate_Poll() 内部
 * 根据事件（收到命令、超时、发送完成）驱动。
 */
typedef enum {
    /**
     * 初始状态 / 空闲状态
     * - 等待远端 App 设备发送 LORA_CMD_UPDATE_REQ 命令
     * - 收到 REQ 后：预计算整份固件的 CRC32、发送 ACK（含固件大小）、
     *   重置发送偏移和序号，然后转入 APP_WAIT_READY
     */
    APP_WAIT_UPDATE_CMD = 0,

    /**
     * 等待 App 就绪状态
     * - 已发送 ACK，等待 App 擦除 W25Q16 SPI Flash 完成后回复 READY
     * - App 擦除 W25Q16 需要较长时间（~100ms/4KB 扇区），期间无法接收数据
     * - 收到 READY 后转入 APP_UPDATE_SEND 开始发送数据
     * - 如果 60 秒内未收到 READY，超时回到 APP_WAIT_UPDATE_CMD
     */
    APP_WAIT_READY,

    /**
     * 数据发送状态
     * - 每次 Poll 发送一帧 DATA（最多 50 字节固件数据），然后延时 50ms
     * - 发送完所有固件数据后发送 END 帧（含 CRC32），然后回到 APP_WAIT_UPDATE_CMD
     * - 此状态下不接收 LoRa 命令，专心发送数据
     */
    APP_UPDATE_SEND
} AppUpdate_State_t;

/**
 * @brief  更新状态机上下文结构体
 *
 * 封装了整个 OTA 更新流程所需的全部状态。由 AppUpdate_Init() 初始化，
 * AppUpdate_Poll() 每次调用时读取和更新。调用者需保证此对象的生命周期
 * 覆盖整个 OTA 过程（通常定义为全局变量或 main 函数内的静态变量）。
 */
typedef struct {
    AppUpdate_State_t state;        /**< 当前状态机状态 */

    LORA_Buf_t *lora_ctx;           /**< LoRa 收发上下文指针（由调用者初始化），
                                         用于发送 ACK/DATA/END 帧和接收 REQ/READY */

    const uint8_t *fw_data;         /**< 固件数据起始地址指针（指向 Flash 缓存区，
                                         通常为 0x08004000），STM32 Flash 可直接
                                         按字节读取，无需特殊处理 */

    uint32_t fw_size;               /**< 固件总大小（字节），由 AppUpdate_Init() 设置，
                                         0 表示无固件可用（收到 REQ 时 ACK 发送 size=0） */

    uint32_t fw_offset;             /**< 当前发送偏移量（字节）：
                                         - 范围 [0, fw_size]
                                         - 每次 send_data_frame() 递增
                                         - fw_offset == fw_size 表示全部发送完毕 */

    uint16_t fw_seq;                /**< 当前帧序号（从 0 开始递增）：
                                         - DATA 帧载荷的前 2 字节，用于 App 检测丢帧
                                         - 每次 send_data_frame() 递增 1
                                         - 范围 [0, 65535]，32KB/50B ≈ 655 帧不会溢出 */

    uint32_t wait_ready_tick;       /**< 进入 APP_WAIT_READY 状态时的系统滴答计数：
                                         - 用于计算等待 READY 命令的已用时间
                                         - 超过 60 秒未收到 READY 则超时退出 */

    uint32_t fw_crc;                /**< 固件 CRC32 校验值：
                                         - 在收到 REQ 时通过 CRC32_Calculate() 预计算
                                         - 在发送 END 帧时作为载荷传给 App
                                         - App 用此值与回读 W25Q16 计算的 CRC32 比对 */
} AppUpdate_t;

/* ======================== 公共函数声明 ======================== */

/**
 * @brief  初始化网关 OTA 更新状态机
 *
 * 清零上下文、设置初始状态和固件参数。初始化后状态机处于
 * APP_WAIT_UPDATE_CMD 状态，等待 App 设备发送更新请求。
 *
 * @param  ctx        更新状态机上下文指针（调用者分配内存）
 * @param  lora_ctx   LoRa 收发上下文指针（需已通过 LORA_Buf_Init 初始化）
 * @param  fw_data    固件数据起始地址（Flash 缓存区首地址，通常为 0x08004000）
 *                    STM32 Flash 可以像 RAM 一样直接按字节读取
 * @param  fw_size    固件总大小（字节），
 *                    传入 0 表示缓存区无有效固件（ACK 将发送 size=0）
 *
 * @note   调用前需确保：
 *         - LORA_Buf_Init() 已执行（LoRa 通信就绪）
 *         - CRC32 硬件外设已初始化（CRC32_Calculate 依赖 STM32 CRC 外设）
 *         - fw_data 指向的 Flash 缓存区中已有有效的固件 .bin 数据
 */
void AppUpdate_Init(AppUpdate_t *ctx, LORA_Buf_t *lora_ctx,
                    const uint8_t *fw_data, uint32_t fw_size);

/**
 * @brief  驱动 OTA 更新状态机（主循环轮询）
 *
 * 根据当前状态执行对应的处理逻辑。此函数应在 main() 的 while(1) 循环中
 * 高频调用（不阻塞），由内部状态决定每轮执行的动作。
 *
 * @param  ctx  更新状态机上下文指针（由 AppUpdate_Init 初始化）
 *
 * 各状态下的行为：
 *   - APP_WAIT_UPDATE_CMD：调用 AppUpdate_WaitCmd()，查询是否收到 REQ
 *   - APP_WAIT_READY：调用 AppUpdate_WaitReady()，等待 READY 或超时
 *   - APP_UPDATE_SEND：调用 AppUpdate_Send()，发送一帧 DATA 或 END
 *   - 其他/非法状态：自动回退到 APP_WAIT_UPDATE_CMD
 *
 * @note   在 APP_UPDATE_SEND 状态下，每次 Poll 内部会调用 HAL_Delay(50ms)
 *         控制 DATA 帧发送间隔。这意味着主循环在发送期间会有 50ms/帧的阻塞。
 *         对于网关设备（单任务，仅做 OTA 分发），这是可接受的。
 */
void AppUpdate_Poll(AppUpdate_t *ctx);

#endif
