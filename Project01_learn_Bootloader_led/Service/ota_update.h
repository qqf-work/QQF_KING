#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

#include "bootloader.h"
#include <stdint.h>

/*
 * OTA 固件搬运状态机 —— 从 W25Q16 读取固件写入 A区 内部 Flash
 * 由主循环每次调用 OTA_Process() 驱动一步
 */

/*
 * OTA 状态机定义
 *
 * 状态流转：
 *   IDLE → READ_INFO → ERASE → TRANSFER → FINISH → 系统重启
 *                       ↑         ↑
 *                     ERROR ←─────┘ (出错回到 IDLE)
 *
 * 每次主循环调用 OTA_Process() 执行一步，不会阻塞
 */
typedef enum {
    OTA_STATE_IDLE,       /* 空闲：等待 OTA 触发，主循环空转 */
    OTA_STATE_READ_INFO,  /* 读 EEPROM：校验 OTA_flag 和固件大小 */
    OTA_STATE_ERASE,      /* 擦除 A区：每循环擦 1 页，逐页推进 */
    OTA_STATE_TRANSFER,   /* 搬运固件：每循环从 W25Q16 读 256B 写入 Flash */
    OTA_STATE_FINISH,     /* 完成：清除 OTA_flag 并软复位 */
    OTA_STATE_ERROR       /* 出错：打印错误信息后回到 IDLE */
} OTA_State_t;

/* OTA 运行时上下文，保存状态机全部运行状态，无全局依赖 */
typedef struct {
    OTA_State_t state;      /* 当前状态 */
    uint32_t    fw_size;    /* 固件总大小（字节），从 EEPROM Firelen[0] 读取 */
    uint32_t    page_count; /* 需擦除的 Flash 页数 = (fw_size + PAGE_SIZE - 1) / PAGE_SIZE */
    uint32_t    erase_index;/* 当前擦除到第几页（0 ~ page_count-1） */
    uint32_t    offset;     /* 当前搬运偏移量（字节），从 W25Q16 读写的地址 */
    uint8_t     buf[256];   /* 搬运缓冲区，每次搬运 256 字节 */
} OTA_Context_t;

/* 接口 */
void OTA_Process(OTA_Context_t *ctx);

#endif
