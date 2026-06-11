#ifndef __LORA_PROTO_H__
#define __LORA_PROTO_H__

#include <stdint.h>

/*
 * LoRa OTA 更新协议定义
 *
 * 本文件定义了通过 LoRa 无线链路进行固件 OTA 更新的通信协议。
 * 替代原 CAN 通信方案（can_proto.h），用于 E32-433T20D LoRa 模块的透明传输模式。
 *
 * 帧格式（字节序：大端）：
 *   [0xAA HEADER] [CMD 1B] [LEN 1B] [PAYLOAD 0~55B]
 *   - 0xAA 帧头：固定标识，用于接收端检测帧起始
 *   - CMD：命令码，1 字节，区分不同类型的控制/数据帧
 *   - LEN：载荷长度，1 字节，表示后续 PAYLOAD 的字节数（0~55）
 *   - PAYLOAD：可变长度载荷，具体格式取决于 CMD 类型
 *
 * 总帧最大 58 字节 = E32-433T20D LoRa 模块单包传输上限
 *   （LoRa 在透明传输模式下，单次 UART 发送超过 58 字节会被自动分包，
 *    导致帧边界不可控，因此将单帧总长限制在 58 字节以内）
 *
 * 命令码按方向分为两组：
 *   App -> Gateway（0x01, 0x04, 0x83, 0x84）：App 主动发起的请求/通知
 *   Gateway -> App（0x81, 0x02, 0x03）：网关响应/推送的命令
 *   其中高位置 1（0x80 位）表示 ACK/响应类型帧，便于区分请求与应答
 *
 * OTA 交互时序：
 *   1. App 发 UPDATE_REQ -> Gateway 回 UPDATE_ACK（含固件大小）
 *   2. App 擦除 W25Q16 -> 发 UPDATE_READY
 *   3. Gateway 逐帧发 UPDATE_DATA（每帧间隔 LORA_DATA_FRAME_DELAY ms）
 *   4. Gateway 发 UPDATE_END（含 CRC32）
 *   5. App 校验通过后发 UPDATE_DONE -> 延时 -> SystemReset
 *
 * 注意：错误码定义与 CAN 版协议保持一致（0x01~0x06），
 *       确保 Bootloader 端和 App 端的错误处理逻辑可复用
 */

/* 帧头标识字节，接收端通过此值检测帧起始位置 */
#define LORA_FRAME_HEADER       0xAA

/* 最大载荷字节数 = 58（LoRa 单包限制）- 3（帧头 0xAA + CMD + LEN）= 55 */
#define LORA_MAX_PAYLOAD        55

/*
 * DATA 帧中固件数据的最大字节数
 * 计算：帧头 3B + SEQ 2B（小端序号）+ DATA 净载荷 = 总帧长
 *       3 + 2 + 50 = 55 <= 55（LORA_MAX_PAYLOAD），满足帧长限制
 * 实际 DATA 帧载荷结构：[seq_lo][seq_hi][data_0][data_1]...[data_49]
 * 前 2 字节是帧序号（用于丢帧检测），剩余 50 字节才是真正的固件数据
 */
#define LORA_MAX_DATA_PER_FRAME 50

/*
 * DATA 帧发送间隔（毫秒）
 *
 * 设计考量：
 *   E32-433T20D 模块配置为 9.6kbps 空中速率时，发送 58 字节约需 48ms。
 *   如果网关不等待就连续发送，模块内部 FIFO 缓冲会溢出导致数据丢失。
 *   50ms 间隔 >= 48ms 空中传输时间，留有 2ms 余量，确保上一帧完全发送
 *   完毕后再发下一帧。
 *
 * 与 CAN 版的对比：CAN 版帧间隔仅 2ms（因为 CAN 总线速率 200kbps 且硬件仲裁），
 * LoRa 版因为空中速率低（9.6kbps）需要 50ms 级别间隔
 */
#define LORA_DATA_FRAME_DELAY   50

/* ---- 命令码定义 ---- */
/* 命令码编码规则：
 *   - 0x01~0x04：App -> Gateway 方向的请求/通知帧
 *   - 0x81~0x84：与 0x01~0x04 对应的响应帧（高位置 1 = ACK 类型）
 *   - 0x02, 0x03：Gateway -> App 方向的数据/控制帧
 */

/* ===== App -> Gateway 方向 ===== */

/*
 * UPDATE_REQ (0x01)：App 向网关请求固件更新
 *   - 无载荷（payload 长度为 0）
 *   - 网关收到后回复 UPDATE_ACK，携带固件大小信息
 *   - 如果网关没有缓存固件，可选择不回复（App 会超时重发 REQ）
 */
#define LORA_CMD_UPDATE_REQ     0x01

/*
 * UPDATE_READY (0x04)：App 通知网关 W25Q16 已擦除完毕，可以开始发送数据
 *   - 无载荷
 *   - 类似 CAN 版的流控机制：W25Q16 扇区擦除耗时约 100ms/4KB，
 *     必须等擦除完成后再接收数据，否则写入会失败
 */
#define LORA_CMD_UPDATE_READY   0x04

/*
 * UPDATE_DONE (0x83)：App 通知网关固件校验通过，更新完成
 *   - 无载荷
 *   - 发送后 App 会延时 100ms（确保 LoRa 帧传输完毕），
 *     然后调用 NVIC_SystemReset() 复位，Bootloader 接管更新流程
 */
#define LORA_CMD_UPDATE_DONE    0x83

/*
 * UPDATE_ERR (0x84)：App 通知网关更新过程中发生错误
 *   - 载荷：1 字节错误码（见下方 OTA_ERR_xxx 定义）
 *   - 网关收到后可记录日志或通知上位机
 */
#define LORA_CMD_UPDATE_ERR     0x84

/* ===== Gateway -> App 方向 ===== */

/*
 * UPDATE_ACK (0x81)：网关响应 App 的更新请求，确认有固件可发
 *   - 载荷：4 字节小端序固件大小（fw_size）
 *     例如 32KB 固件 = 0x00008000，载荷为 [0x00, 0x80, 0x00, 0x00]
 *   - App 收到后校验 fw_size 合法性，然后擦除 W25Q16 并发 READY
 */
#define LORA_CMD_UPDATE_ACK     0x81

/*
 * UPDATE_DATA (0x02)：网关向 App 发送固件数据帧
 *   - 载荷：2 字节小端序号（seq）+ <=50 字节固件数据
 *     载荷结构：[seq_lo][seq_hi][data_byte_0]...[data_byte_N]
 *   - 序号从 0 开始，每帧递增 1，App 通过序号连续性检测丢帧
 *   - 每帧最多 50 字节固件数据（LORA_MAX_DATA_PER_FRAME）
 *   - 32KB 固件需要 655 帧（32768 / 50），以 50ms/帧计约需 33 秒
 */
#define LORA_CMD_UPDATE_DATA    0x02

/*
 * UPDATE_END (0x03)：网关通知 App 固件数据已全部发送完毕
 *   - 载荷：4 字节小端序 CRC32 校验值
 *     网关在发送前对完整固件二进制预计算 CRC32，用于 App 端校验
 *   - App 收到后：刷新页缓冲 -> 回读 W25Q16 计算 CRC32 -> 与此值比对
 *   - CRC32 使用 STM32 硬件 CRC 外设（多项式 0x04C11DB7）
 */
#define LORA_CMD_UPDATE_END     0x03

/* ---- 错误码定义 ---- */
/* 错误码与 CAN 版协议完全一致，便于上层 OTA 逻辑复用 */
#define OTA_ERR_SEQ_MISMATCH    0x01   /* 序号不连续：收到的 seq != 期望值，可能是丢帧或乱序 */
#define OTA_ERR_FLASH_WRITE     0x02   /* W25Q16 写入失败：SPI Flash 页编程失败 */
#define OTA_ERR_EEPROM_WRITE    0x03   /* EEPROM 写入失败：AT24C02 I2C 写入异常 */
#define OTA_ERR_TIMEOUT         0x04   /* 接收超时：在指定时间内未收到预期的帧 */
#define OTA_ERR_SIZE_MISMATCH   0x05   /* 接收量与声明大小不匹配：total_recv != fw_size */
#define OTA_ERR_CRC_MISMATCH    0x06   /* CRC32 校验不匹配：回读 W25Q16 计算的 CRC 与网关提供的不一致 */

#endif
