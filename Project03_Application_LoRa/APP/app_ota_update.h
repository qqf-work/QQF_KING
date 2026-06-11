#ifndef __APP_OTA_UPDATE_H__
#define __APP_OTA_UPDATE_H__

#include "lora_buf.h"
#include "ota_storage.h"
#include <stdint.h>

/*
 * LoRa OTA 固件更新状态机 —— App 侧核心模块
 *
 * 本模块实现了 App 端的 OTA 更新完整状态机，通过 LoRa 无线链路
 * 从网关接收固件数据，写入 W25Q16 SPI Flash，校验 CRC32，
 * 最后写 EEPROM 标志位触发系统复位，由 Bootloader 将固件从
 * W25Q16 搬运到内部 Flash（A 区）完成升级。
 *
 * 状态机设计（5 个状态）：
 *
 *   IDLE ──REQ──> WAIT_ACK ──ACK──> RECV_DATA ──END+校验OK──> [DONE+Reset]
 *     ^              |                    |
 *     |              |超时/错误           |超时/错误/seq错误
 *     |              v                    v
 *     +---------- ERROR <──CRC失败3次────+
 *                     |
 *                     |退避5s
 *                     +──> IDLE
 *
 *   - IDLE:      空闲/初始态，发送 UPDATE_REQ 后转入 WAIT_ACK
 *   - WAIT_ACK:  等待网关回复 UPDATE_ACK（含固件大小），超时 5s
 *   - RECV_DATA: 接收固件数据帧 + END 帧，校验通过后复位
 *   - DONE:      预留状态（当前流程在 RECV_DATA 中直接 SystemReset）
 *   - ERROR:     错误处理，发送 ERR 帧，退避后回到 IDLE 重试
 *
 * 关键设计决策：
 *   1. App 主动发起更新（发 REQ），而非被动等待网关推送。
 *      这样 App 可以在上电后、按键触发、或定时检查时发起更新。
 *   2. CRC 校验失败时自动重试，最多 3 次（OTA_MAX_CRC_RETRY）。
 *      每次重试会重新从 IDLE 发起 REQ，全量重新接收固件。
 *      超过重试次数则放弃更新，继续运行旧固件。
 *   3. ERROR 后有 5s 退避等待，避免错误时频繁发送 REQ 干扰 LoRa 信道。
 *   4. 掉电安全保证：EEPROM 标志在 W25Q16 写完并回读 CRC 校验通过后才写入，
 *      中途掉电不会触发 Bootloader 更新（详见 ota_storage.c）。
 *
 * 使用方式：
 *   APP_OTA_Init()    — 初始化，绑定 LoRa 上下文和存储驱动
 *   APP_OTA_Process() — 主循环中持续轮询调用，驱动状态机运行
 *
 * 典型 main() 用法：
 *   APP_OTA_t ota;
 *   APP_OTA_Init(&ota, &lora_buf, &w25q, &eeprom);
 *   while (1) {
 *       APP_OTA_Process(&ota);
 *   }
 */

/*
 * OTA_State_t - 状态机状态枚举
 *
 * OTA_STATE_IDLE      (0): 空闲态，等待发送 UPDATE_REQ 或退避等待
 * OTA_STATE_WAIT_ACK  (1): 已发 REQ，等待网关回复 UPDATE_ACK
 * OTA_STATE_RECV_DATA (2): 已收 ACK 且 W25Q16 已擦除，正在接收数据帧
 * OTA_STATE_DONE      (3): 预留完成态（当前流程在 RECV_DATA 中直接复位）
 * OTA_STATE_ERROR     (4): 错误态，发送 ERR 帧后退避回到 IDLE
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_WAIT_ACK,
    OTA_STATE_RECV_DATA,
    OTA_STATE_DONE,
    OTA_STATE_ERROR
} OTA_State_t;

/*
 * 等待 UPDATE_ACK 的超时时间（毫秒）
 *
 * 网关收到 REQ 后需要读取 Flash 缓存区中的固件信息再回复 ACK，
 * 处理时间较短。5 秒超时考虑了 LoRa 传输延迟（~50ms/帧）和
 * 网关处理延迟。超时后进入 ERROR 态。
 */
#define OTA_WAIT_ACK_TIMEOUT    5000

/*
 * RECV_DATA 状态下无数据接收的超时时间（毫秒）
 *
 * LoRa 帧间隔 50ms（LORA_DATA_FRAME_DELAY），但网关可能因为处理
 * 其他任务、LoRa 信道拥塞等原因暂停发送。30 秒超时容忍了较大的
 * 传输延迟，同时也避免了无限等待。超时后进入 ERROR 态。
 *
 * 与 CAN 版的对比：CAN 版无 DATA 超时（因为 CAN 传输可靠且快速），
 * LoRa 版因为无线信道不确定性，必须有超时保护。
 */
#define OTA_RECV_DATA_TIMEOUT   30000

/*
 * ERROR 状态退避等待时间（毫秒）
 *
 * 发生错误后不立即重试，而是等待 5 秒再发 REQ，原因：
 *   1. 避免 LoRa 信道被错误重试帧占满
 *   2. 给网关时间处理/清除异常状态
 *   3. 如果是系统性问题（如固件损坏），频繁重试没有意义
 *
 * 退避期间 state_tick != 0，IDLE 态通过检查 tick 判断是否在退避中。
 */
#define OTA_ERROR_BACKOFF       5000

/*
 * CRC32 校验失败后最大重试次数
 *
 * 每次重试 = 从 IDLE 重新发 REQ -> 全量重新接收 -> 重新校验 CRC。
 * 超过此次数后放弃更新，清零 crc_retry_cnt，继续运行旧固件。
 * CRC 失败可能原因：LoRa 信道干扰导致固件数据损坏。
 */
#define OTA_MAX_CRC_RETRY       3

/*
 * APP_OTA_t - OTA 更新上下文结构体
 *
 * 封装了状态机运行所需的全部状态，包括：
 *   - 当前状态和进入时间（用于超时检测）
 *   - 存储层上下文（W25Q16 + EEPROM 操作的封装）
 *   - LoRa 收发上下文
 *   - 帧序号跟踪（用于丢帧检测）
 *   - 错误码和 CRC 重试计数
 *
 * 成员说明：
 *   state         - 当前状态机状态（OTA_State_t 枚举）
 *   storage       - OTA 存储层上下文，管理 W25Q16 写入和 EEPROM 标志
 *                   （包含页缓冲、写入偏移、固件大小、CRC 等）
 *   lora_ctx      - LoRa 收发上下文指针，用于发送/接收协议帧
 *   expect_seq    - 期望的下一个 DATA 帧序号（从 0 开始递增）
 *                   用于检测丢帧：收到的 seq 必须等于此值
 *   state_tick    - 进入当前状态时的 HAL_GetTick() 值
 *                   用于计算超时：当前 tick - state_tick > 超时阈值
 *   error_code    - 当前错误码（OTA_ERR_xxx 定义见 lora_proto.h），
 *                   通过 UPDATE_ERR 帧发送给网关
 *   crc_retry_cnt - CRC 校验失败累计重试次数，
 *                   达到 OTA_MAX_CRC_RETRY 后放弃更新
 */
typedef struct {
    OTA_State_t    state;
    OTA_Storage_t  storage;
    LORA_Buf_t    *lora_ctx;
    uint16_t       expect_seq;
    uint32_t       state_tick;
    uint8_t        error_code;
    uint8_t        crc_retry_cnt;
} APP_OTA_t;

/*
 * APP_OTA_Init - 初始化 OTA 更新模块
 *
 * 功能：清零上下文状态，初始化存储层（绑定 W25Q16 和 EEPROM 驱动），
 *       设置初始状态为 OTA_STATE_IDLE。
 *
 * 参数：
 *   ctx      - OTA 上下文指针，调用者分配的 APP_OTA_t 实例
 *   lora_ctx - LoRa 收发上下文指针，必须已通过 LORA_Buf_Init() 初始化
 *   w25q     - W25Q16 SPI Flash 驱动句柄指针，用于存储接收到的固件数据
 *   eeprom   - AT24C02 EEPROM 驱动句柄指针，用于写入更新标志（触发 Bootloader）
 *
 * 返回值：无
 */
void APP_OTA_Init(APP_OTA_t *ctx, LORA_Buf_t *lora_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom);

/*
 * APP_OTA_Process - OTA 状态机主处理函数
 *
 * 功能：在主循环中持续调用，驱动状态机运行。
 *       每次调用处理：接收 LoRa 帧 -> 根据当前状态进行相应处理 ->
 *       超时检测 -> 状态转换。
 *
 * 参数：
 *   ctx - OTA 上下文指针
 *
 * 返回值：无
 *
 * 调用约定：
 *   - 必须在主循环中高频调用（不要在中间加 HAL_Delay 等阻塞操作）
 *   - 与 LoRa 不同，CAN 版要求主循环无延时（因为 CAN FIFO 浅，3帧溢出），
 *     LoRa 版帧间隔 50ms，对主循环频率要求低得多，但保持非阻塞设计是好习惯
 *
 * 状态处理概述：
 *   IDLE      -> 发 REQ，转 WAIT_ACK
 *   WAIT_ACK  -> 等待 ACK 帧，超时转 ERROR
 *   RECV_DATA -> 收 DATA 帧写 W25Q16，收 END 帧做 CRC 校验
 *   DONE      -> 预留（当前不使用）
 *   ERROR     -> 发 ERR 帧，退避后回 IDLE
 */
void APP_OTA_Process(APP_OTA_t *ctx);

#endif
