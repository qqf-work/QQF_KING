/**
 * @file    lora_proto.h
 * @brief   LoRa OTA 无线固件更新协议定义
 *
 * 本文件定义了网关（Gateway）与远端 App 设备之间通过 E32-433T20D LoRa 模块
 * 进行固件无线分发时所使用的应用层协议。
 *
 * 与 CAN 协议（can_proto.h）的设计理念类似，但由于 LoRa 传输有以下特点：
 *   - 单包载荷有限（E32-433T20D 透明模式下建议不超过 58 字节）
 *   - 空中速率较低（9.6kbps），帧间隔需要更长（50ms vs CAN 的 2ms）
 *   - 透明传输模式，无硬件帧分割，需要自定义帧头标识
 *
 * 因此协议在帧格式、载荷大小、时序参数上做了针对性调整。
 *
 * 帧格式（总帧最大 58 字节）：
 *   [0xAA 帧头] [CMD 1字节] [LEN 1字节] [PAYLOAD 0~55字节]
 *   - 0xAA 帧头：用于接收端识别帧起始，区分有效数据与总线噪声
 *   - CMD：命令码，标识本帧的语义（REQ/ACK/DATA/END 等）
 *   - LEN：载荷长度，0 表示无载荷（如 REQ、READY、DONE 帧）
 *   - PAYLOAD：变长载荷，内容取决于 CMD 类型
 *
 * 完整的 OTA 更新交互流程：
 *   1. App  -> Gateway : UPDATE_REQ (0x01)       请求固件更新
 *   2. Gateway -> App  : UPDATE_ACK (0x81)       回复固件大小，准备发送
 *   3. App  -> Gateway : UPDATE_READY (0x04)     App 擦除 W25Q16 完成，准备接收
 *   4. Gateway -> App  : UPDATE_DATA (0x02) x N  逐帧发送固件数据（每帧 50ms 间隔）
 *   5. Gateway -> App  : UPDATE_END (0x03)       发送完成，附带 CRC32 校验值
 *   6. App  -> Gateway : UPDATE_DONE (0x83)      App 校验通过，更新完成
 *      或 UPDATE_ERR (0x84)                      App 校验失败或出错
 */

#ifndef __LORA_PROTO_H__
#define __LORA_PROTO_H__

#include <stdint.h>

/* ======================== 帧格式常量 ======================== */

/**
 * 帧头标识字节
 * - 固定值 0xAA，用于接收端在 USART 数据流中定位帧起始位置
 * - 选择 0xAA（10101010b）的原因：交替的 0/1 模式在串口通信中不易与
 *   常见噪声或对齐错误混淆，且不是 ASCII 可打印字符
 */
#define LORA_FRAME_HEADER       0xAA

/**
 * 最大载荷字节数
 * - 计算：单包总限制 58 字节 - 帧头 3 字节（HEADER + CMD + LEN）= 55 字节
 * - 所有帧类型的载荷长度均不能超过此值
 * - 这个限制来源于 E32-433T20D 在透明传输模式下单包的可靠传输能力
 */
#define LORA_MAX_PAYLOAD        55

/**
 * DATA 帧中纯固件数据的最大字节数
 * - DATA 帧载荷结构：[seq 低字节] [seq 高字节] [固件数据 ...]
 * - 所以固件数据部分 = 最大载荷 55 - 序号 2 = 50 字节
 * - 每帧携带 50 字节固件数据，32KB 固件需要约 655 帧传输完毕
 */
#define LORA_MAX_DATA_PER_FRAME 50

/**
 * DATA 帧之间的发送间隔（毫秒）
 * - 设为 50ms，匹配 E32-433T20D 在 9.6kbps 空中速率下的实际吞吐能力
 * - 计算依据：58 字节帧在 9.6kbps 下传输时间约 48ms，加上模块内部处理时间，
 *   50ms 间隔可以确保前一帧完全发送完毕后再发下一帧
 * - 过短：E32 模块内部 FIFO 溢出，导致数据丢失
 * - 过长：传输效率低，32KB 固件传输时间 = 655 帧 × 50ms ≈ 33 秒
 */
#define LORA_DATA_FRAME_DELAY   50

/* ======================== 命令码定义 ======================== */
/*
 * 命令码编码规则：
 *   - 0x01~0x0F：App -> Gateway 方向（低位范围）
 *   - 0x81~0x8F：Gateway -> App 方向（最高位为 1，方便快速判断帧方向）
 *   - 0x02~0x03：特殊情况（DATA 和 END 虽是 Gateway -> App，但与 CAN 协议保持兼容编号）
 */

/* ---- App -> Gateway 方向 ---- */

/**
 * 请求更新命令（App 发起）
 * - 无载荷
 * - App 在需要固件更新时发送此命令，触发网关开始 OTA 流程
 * - 网关收到后预计算 CRC32 并回复 UPDATE_ACK
 */
#define LORA_CMD_UPDATE_REQ     0x01

/**
 * 擦除就绪命令（App 通知）
 * - 无载荷
 * - App 收到 ACK 后开始擦除 W25Q16 SPI Flash，擦除完成后发送此命令
 * - 类似 CAN 协议中的 READY 流控机制：防止网关在 App 擦除期间发送数据导致丢失
 *   （W25Q16 扇区擦除约 100ms/4KB，期间无法处理接收）
 */
#define LORA_CMD_UPDATE_READY   0x04

/**
 * 更新完成确认（App 确认）
 * - 无载荷
 * - App 完成以下全部步骤后发送：
 *   1. 接收全部固件数据
 *   2. 回读 W25Q16 并计算 CRC32 校验
 *   3. 校验通过，写入 EEPROM 更新标志
 * - 网关收到此命令表示本次 OTA 更新成功
 */
#define LORA_CMD_UPDATE_DONE    0x83

/**
 * 更新错误通知（App 报错）
 * - 载荷：error_code（1 字节），取值见下方 OTA_ERR_xxx 定义
 * - App 在校验失败或发生错误时发送，网关据此决定是否重试
 */
#define LORA_CMD_UPDATE_ERR     0x84

/* ---- Gateway -> App 方向 ---- */

/**
 * 更新应答命令（网关确认）
 * - 载荷：fw_size（4 字节，小端序，即低字节在前）
 * - 网关收到 REQ 后发送，告知 App 即将传输的固件总大小（字节）
 * - App 用此值判断需要接收多少数据、需要擦除多少 W25Q16 空间
 */
#define LORA_CMD_UPDATE_ACK     0x81

/**
 * 固件数据帧（网关发送）
 * - 载荷：seq（2 字节小端序号）+ data（<=50 字节固件数据）
 * - seq 从 0 开始递增，App 用于检测丢帧和乱序
 * - 每帧数据量固定为 50 字节（最后一帧可能不足 50 字节）
 * - 帧间隔 LORA_DATA_FRAME_DELAY(50ms)，防止 LoRa 模块缓冲溢出
 */
#define LORA_CMD_UPDATE_DATA    0x02

/**
 * 传输结束命令（网关通知）
 * - 载荷：crc32（4 字节小端，STM32 硬件 CRC32 计算结果）
 * - 网关发送完所有 DATA 帧后发送此命令，附带整份固件的 CRC32 校验值
 * - App 收到后回读 W25Q16 全部数据计算 CRC32，与本帧携带的 CRC32 比对
 * - 校验通过：写 EEPROM 标志 -> 发 DONE -> SystemReset -> Bootloader 搬运更新
 * - 校验失败：发 ERR(CRC_MISMATCH)，放弃本次更新继续运行旧固件
 */
#define LORA_CMD_UPDATE_END     0x03

/* ======================== 错误码定义 ======================== */
/*
 * 这些错误码由 App 设备在检测到异常时通过 UPDATE_ERR 帧报告给网关。
 * 网关可根据错误码决定重试策略或通知上位机。
 */

/** 0x01 - 序号不连续：App 期望的 seq 与实际收到的 seq 不匹配，说明有帧丢失 */
#define OTA_ERR_SEQ_MISMATCH    0x01

/** 0x02 - W25Q16 写入失败：SPI Flash 页写入失败，可能是硬件故障或地址越界 */
#define OTA_ERR_FLASH_WRITE     0x02

/** 0x03 - EEPROM 写入失败：AT24C02 写入更新标志失败，I2C 通信异常 */
#define OTA_ERR_EEPROM_WRITE    0x03

/** 0x04 - 接收超时：App 在预期时间内未收到下一帧 DATA 或 END */
#define OTA_ERR_TIMEOUT         0x04

/** 0x05 - 大小不匹配：App 接收到的总字节数与 ACK 中声明的 fw_size 不一致 */
#define OTA_ERR_SIZE_MISMATCH   0x05

/** 0x06 - CRC32 不匹配：App 回读 W25Q16 计算的 CRC32 与 END 帧携带的不一致 */
#define OTA_ERR_CRC_MISMATCH    0x06

#endif
