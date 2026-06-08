#ifndef __FLASH_DOWNLOAD_H__
#define __FLASH_DOWNLOAD_H__

#include <stdint.h>

/*
 * UART 串口 Flash 下载模块
 *
 * 配合 uart_buf 帧队列使用：
 *   主循环中取到一帧数据 → 调用 FlashDownload_WriteFrame()
 *   模块内部处理：智能擦页 + 跨帧奇数字节缓冲 + 半字写入
 */

typedef struct {
    uint32_t write_addr;      /* 当前写入地址（A 区内递增） */
    uint32_t total_written;   /* 已写入总字节 */
    uint8_t  last_byte_flag;  /* 跨帧奇数字节标记 */
    uint8_t  last_byte;       /* 缓存的奇数字节 */
    uint32_t next_erase_addr; /* 下一个需要擦除的页地址（单调递增，不回退） */
} FlashDownload_t;

/* 初始化下载上下文，写入地址设为 A 区起始 */
void     FlashDownload_Init(FlashDownload_t *ctx);

/*
 * 处理一帧 UART 数据，写入 Flash
 * 流程：智能擦页 → 跨帧奇数字节拼接 → 半字写入
 * 返回 0 成功, -1 失败
 */
int      FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len);

/* 返回已写入总字节数 */
uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx);

#endif
