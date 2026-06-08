#ifndef __OTA_STORAGE_H__
#define __OTA_STORAGE_H__

#include "w25q16.h"
#include "at24c02.h"
#include <stdint.h>

/*
 * OTA 存储管理模块
 *
 * 负责 W25Q16 页缓冲写入 + AT24C02 EEPROM 标志位更新。
 * 不依赖通信协议（CAN/UART），可被不同传输层复用。
 *
 * 写入流程：
 *   OTA_Storage_Init()   — 绑定驱动句柄
 *   OTA_Storage_Start()  — 重置状态 + 擦除扇区
 *   OTA_Storage_Write()  — 多次调用，内部 256B 页缓冲
 *   OTA_Storage_Finish() — 刷剩余缓冲 + 写 EEPROM 标志位
 */

#define OTA_STORAGE_PAGE_BUF_SIZE  256  /* W25Q16 页大小 */

/* EEPROM 写入地址（与 Bootloader app_bootloader.h 定义一致） */
#define OTA_EEPROM_STATUS_ADDR   0x10
#define OTA_EEPROM_KEY_ADDR      0x11
#define OTA_EEPROM_SIZE_ADDR     0x13

/* EEPROM 状态值 */
#define OTA_EEPROM_NEED_UPDATE   0x01
#define OTA_EEPROM_CHECK_KEY     0xA5

/* 错误码（OTA_Storage_Finish 返回值） */
#define OTA_ERR_FLASH_WRITE   0x02  /* W25Q16 写入失败 */
#define OTA_ERR_EEPROM_WRITE  0x03  /* EEPROM 写入失败 */
#define OTA_ERR_SIZE_MISMATCH 0x05  /* 接收量与声明大小不匹配 */

typedef struct {
    W25Q16_t   *w25q;                          /* W25Q16 设备句柄 */
    AT24C02_t  *eeprom;                        /* AT24C02 设备句柄 */
    uint32_t    flash_addr;                    /* 当前 W25Q16 写入偏移 */
    uint32_t    fw_size;                       /* 固件总大小 */
    uint8_t     page_buf[OTA_STORAGE_PAGE_BUF_SIZE]; /* 页缓冲 */
    uint16_t    buf_pos;                       /* 缓冲区填充位置 */
    uint32_t    total_recv;                    /* 已接收字节总数 */
} OTA_Storage_t;

/* 初始化：绑定驱动句柄 */
void OTA_Storage_Init(OTA_Storage_t *ctx, W25Q16_t *w25q, AT24C02_t *eeprom);

/* 开始接收：重置状态，根据 fw_size 擦除 W25Q16 扇区（4KB 对齐） */
int OTA_Storage_Start(OTA_Storage_t *ctx, uint32_t fw_size);

/* 写入数据：填页缓冲，满 256B 自动刷到 W25Q16 */
int OTA_Storage_Write(OTA_Storage_t *ctx, const uint8_t *data, uint16_t len);

/* 完成接收：刷剩余缓冲 + 写 EEPROM 标志位（原子操作） */
int OTA_Storage_Finish(OTA_Storage_t *ctx);

/* 错误复位：清空缓冲，不写 EEPROM */
void OTA_Storage_Reset(OTA_Storage_t *ctx);

#endif
