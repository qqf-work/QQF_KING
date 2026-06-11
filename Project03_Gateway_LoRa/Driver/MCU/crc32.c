#include "crc32.h"
#include "stm32f1xx_hal.h"

extern CRC_HandleTypeDef hcrc;

/* ---- Streaming API ---- */

void CRC32_Init(CRC32_Ctx *ctx)
{
    __HAL_CRC_DR_RESET(&hcrc);
    ctx->tail_len = 0;
}

void CRC32_Update(CRC32_Ctx *ctx, const uint8_t *data, uint32_t len)
{
    if (data == 0 || len == 0) return;

    uint32_t i = 0;

    /* If there are leftover bytes from previous chunk, combine with new data */
    if (ctx->tail_len > 0)
    {
        while (ctx->tail_len < 4 && i < len)
        {
            ctx->tail[ctx->tail_len] = data[i];
            ctx->tail_len++;
            i++;
        }

        /* Only feed when we have a complete word */
        if (ctx->tail_len == 4)
        {
            uint32_t word = (uint32_t)ctx->tail[0]
                          | ((uint32_t)ctx->tail[1] << 8)
                          | ((uint32_t)ctx->tail[2] << 16)
                          | ((uint32_t)ctx->tail[3] << 24);
            CRC->DR = word;
            ctx->tail_len = 0;
        }
        /* else: still less than 4 bytes, saved in tail, return */
        if (i >= len) return;
    }

    /* Feed aligned 4-byte words directly */
    while (i + 4 <= len)
    {
        uint32_t word = (uint32_t)data[i]
                      | ((uint32_t)data[i + 1] << 8)
                      | ((uint32_t)data[i + 2] << 16)
                      | ((uint32_t)data[i + 3] << 24);
        CRC->DR = word;
        i += 4;
    }

    /* Save remaining bytes (0-3) for next chunk or Final */
    while (i < len)
    {
        ctx->tail[ctx->tail_len] = data[i];
        ctx->tail_len++;
        i++;
    }
}

uint32_t CRC32_Final(CRC32_Ctx *ctx)
{
    /* Pad leftover bytes with 0x00 and feed last word */
    if (ctx->tail_len > 0)
    {
        uint32_t word = 0;
        for (uint8_t j = 0; j < ctx->tail_len; j++)
            word |= (uint32_t)ctx->tail[j] << (j * 8);
        CRC->DR = word;
        ctx->tail_len = 0;
    }

    return CRC->DR;
}

/* ---- One-shot wrapper ---- */

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t len)
{
    if (data == 0 || len == 0) return 0;

    CRC32_Ctx ctx;
    CRC32_Init(&ctx);
    CRC32_Update(&ctx, data, len);
    return CRC32_Final(&ctx);
}
