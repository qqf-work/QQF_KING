#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>

/*
 * STM32 hardware CRC32 driver
 *
 * Uses STM32F1 built-in CRC unit (polynomial 0x04C11DB7, no input/output bit reflection)
 * CubeMX must enable CRC peripheral, main.c will generate MX_CRC_Init()
 *
 * Usage (one-shot):
 *   uint32_t crc = CRC32_Calculate(data, len);
 *
 * Usage (streaming, for chunked reads):
 *   CRC32_Ctx ctx;
 *   CRC32_Init(&ctx);
 *   CRC32_Update(&ctx, chunk1, len1);
 *   CRC32_Update(&ctx, chunk2, len2);
 *   uint32_t crc = CRC32_Final(&ctx);
 */

/* Streaming context: holds leftover bytes between chunks */
typedef struct {
    uint8_t  tail[4];    /* leftover bytes not yet fed to CRC (0-3, max 3 used) */
    uint8_t  tail_len;   /* number of bytes in tail */
} CRC32_Ctx;

/* Streaming API */
void     CRC32_Init(CRC32_Ctx *ctx);
void     CRC32_Update(CRC32_Ctx *ctx, const uint8_t *data, uint32_t len);
uint32_t CRC32_Final(CRC32_Ctx *ctx);

/* One-shot convenience wrapper */
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t len);

#endif
