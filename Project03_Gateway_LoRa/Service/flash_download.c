/**
 * @file    flash_download.c
 * @brief   UART 串口 Flash 下载实现
 *
 * 移植自 Project02_factory_app，改用 fw_cache_conf.h 地址定义
 * 智能擦页（单调递增）+ 跨帧奇数字节缓冲 + 半字写入
 */

#include "flash_download.h"
#include "flash.h"
#include "fw_cache_conf.h"
#include <stdio.h>

/* ---------- 智能擦除（单调递增，每页只擦一次） ---------- */

static void smart_erase(FlashDownload_t *ctx, uint16_t len)
{
    uint32_t end = ctx->write_addr + len;
    uint32_t page_end = (end + FW_CACHE_PAGE_SIZE - 1) & ~(FW_CACHE_PAGE_SIZE - 1);

    while (ctx->next_erase_addr < page_end)
    {
        if (Flash_NeedsErase(ctx->next_erase_addr, FW_CACHE_PAGE_SIZE))
        {
            if (Flash_ErasePage(ctx->next_erase_addr) != 0)
            {
                printf("[DL] Erase failed at 0x%08lX\r\n", ctx->next_erase_addr);
                return;
            }
        }
        ctx->next_erase_addr += FW_CACHE_PAGE_SIZE;
    }
}

/* ---------- 跨帧奇数字节写入 ---------- */

/**
 * @brief  带跨帧奇数字节处理的半字写入
 *
 * 四种情况：
 *   1. 无 last_byte 且 len 偶数 → 直接写入
 *   2. 无 last_byte 且 len 奇数 → 写前 len-1，缓存末字节
 *   3. 有 last_byte 且 (1+len) 偶数 → 拼接 + 写剩余
 *   4. 有 last_byte 且 (1+len) 奇数 → 拼接 + 写前 n-1 + 缓存末字节
 */
static int write_with_last_byte(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    uint32_t addr = ctx->write_addr;
    uint16_t pos = 0;

    if (ctx->last_byte_flag)
    {
        /* 拼接上一帧缓存的奇数字节和本帧第一个字节 */
        uint8_t buf[2] = { ctx->last_byte, data[0] };
        if (Flash_Write(addr, buf, 2) != 0)
            return -1;
        addr += 2;
        pos = 1;
        ctx->total_written += 1;
        ctx->last_byte_flag = 0;
    }

    /* 剩余可写字节数（向下取偶） */
    uint16_t remaining = len - pos;
    uint16_t write_count = remaining & ~1;

    if (write_count > 0)
    {
        if (Flash_Write(addr, data + pos, write_count) != 0)
            return -1;
        addr += write_count;
    }

    ctx->total_written += write_count;
    ctx->write_addr = addr;

    /* 处理剩余奇数字节 */
    if (remaining & 1)
    {
        ctx->last_byte = data[pos + write_count];
        ctx->last_byte_flag = 1;
    }

    return 0;
}

/* ---------- 公开 API ---------- */

void FlashDownload_Init(FlashDownload_t *ctx)
{
    ctx->write_addr      = FW_CACHE_ADDR;
    ctx->total_written   = 0;
    ctx->last_byte_flag  = 0;
    ctx->last_byte       = 0;
    ctx->next_erase_addr = FW_CACHE_ADDR;
}

int FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    if (len == 0)
        return 0;

    /* 溢出保护（考虑 last_byte 缓存的额外 1 字节） */
    uint32_t total_need = ctx->write_addr + len + ctx->last_byte_flag;
    if (total_need > FW_CACHE_ADDR + FW_CACHE_SIZE)
    {
        printf("[DL] Error: write overflow\r\n");
        return -1;
    }

    Flash_Unlock();

    /* 智能擦除 */
    smart_erase(ctx, len);

    /* 写入（含跨帧奇数字节处理） */
    int ret = write_with_last_byte(ctx, data, len);

    Flash_Lock();

    if (ret != 0)
    {
        printf("[DL] Error: write failed at 0x%08lX\r\n", ctx->write_addr);
        return -1;
    }

    return 0;
}

uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx)
{
    return ctx->total_written;
}
