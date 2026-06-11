#include "ota_storage.h"
#include "w25q16.h"
#include "at24c02.h"
#include <string.h>
#include <stdio.h>
#include "crc32.h"


void OTA_Storage_Init(OTA_Storage_t *ctx, W25Q16_t *w25q, AT24C02_t *eeprom)
{
    memset(ctx, 0, sizeof(OTA_Storage_t));
    ctx->w25q    = w25q;
    ctx->eeprom  = eeprom;
}

int OTA_Storage_Start(OTA_Storage_t *ctx, uint32_t fw_size)
{
    /* 重置状态 */
    ctx->flash_addr  = 0;
    ctx->fw_size     = fw_size;
    ctx->buf_pos     = 0;
    ctx->total_recv  = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);

    /* 计算需要擦除的扇区数（4KB 对齐向上取整） */
    uint32_t sectors = (fw_size + W25Q16_SECTOR_SIZE - 1) / W25Q16_SECTOR_SIZE;
    for (uint32_t i = 0; i < sectors; i++)
    {
        int ret = W25Q16_EraseSector(ctx->w25q, i * W25Q16_SECTOR_SIZE);
        if (ret != 0) return -1;
    }

    return 0;
}

/* 内部函数：将页缓冲中数据写入 W25Q16 */
static int ota_storage_flush(OTA_Storage_t *ctx)
{
    if (ctx->buf_pos == 0) return 0;

    int ret = W25Q16_Write(ctx->w25q, ctx->flash_addr,
                           ctx->page_buf, ctx->buf_pos);
    if (ret != 0) return -1;

    ctx->flash_addr += ctx->buf_pos;
    ctx->buf_pos = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
    return 0;
}

int OTA_Storage_Write(OTA_Storage_t *ctx, const uint8_t *data, uint16_t len)
{
    /* 越界保护：接收总量不超过声明的固件大小 */
    if (ctx->total_recv + len > ctx->fw_size) return -1;

    uint16_t offset = 0;

    while (offset < len)
    {
        uint16_t space = OTA_STORAGE_PAGE_BUF_SIZE - ctx->buf_pos;
        uint16_t chunk = (len - offset < space) ? (len - offset) : space;

        memcpy(&ctx->page_buf[ctx->buf_pos], &data[offset], chunk);
        ctx->buf_pos += chunk;
        offset += chunk;

        /* 页缓冲满，刷到 W25Q16 */
        if (ctx->buf_pos >= OTA_STORAGE_PAGE_BUF_SIZE)
        {
            int ret = ota_storage_flush(ctx);
            if (ret != 0) return -1;
        }
    }

    ctx->total_recv += len;
    return 0;
}

int OTA_Storage_Finish(OTA_Storage_t *ctx)
{
    /* Verify received count matches declared size */
    if (ctx->total_recv != ctx->fw_size) return OTA_ERR_SIZE_MISMATCH;

    /* 1. Flush remaining buffer to W25Q16 */
    int ret = ota_storage_flush(ctx);
    if (ret != 0) return OTA_ERR_FLASH_WRITE;

    /* 2. Read back W25Q16 and calculate CRC32 */
    uint8_t read_buf[256];
    uint32_t offset = 0;

    CRC32_Ctx crc_ctx;
    CRC32_Init(&crc_ctx);

    while (offset < ctx->fw_size)
    {
        uint16_t chunk = sizeof(read_buf);
        if (offset + chunk > ctx->fw_size)
            chunk = (uint16_t)(ctx->fw_size - offset);

        if (W25Q16_Read(ctx->w25q, offset, read_buf, chunk) != 0)
            return OTA_ERR_FLASH_WRITE;

        CRC32_Update(&crc_ctx, read_buf, chunk);
        offset += chunk;
    }
    uint32_t actual_crc = CRC32_Final(&crc_ctx);

    /* 3. CRC comparison */
    printf("[OTA] CRC expected: 0x%08lX, got: 0x%08lX\r\n",
           ctx->expected_crc, actual_crc);

    if (actual_crc != ctx->expected_crc)
        return OTA_ERR_CRC_MISMATCH;

    printf("[OTA] CRC pass, saving to EEPROM\r\n");

    /* 4. First EEPROM write: status + key + fw_size (7 bytes, addr 0x10-0x16) */
    uint8_t eeprom_data[7];
    eeprom_data[0] = OTA_EEPROM_NEED_UPDATE;
    eeprom_data[1] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[2] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[3] = (uint8_t)(ctx->fw_size);
    eeprom_data[4] = (uint8_t)(ctx->fw_size >> 8);
    eeprom_data[5] = (uint8_t)(ctx->fw_size >> 16);
    eeprom_data[6] = (uint8_t)(ctx->fw_size >> 24);

    ret = AT24C02_Write(ctx->eeprom, OTA_EEPROM_STATUS_ADDR,
                        eeprom_data, sizeof(eeprom_data));
    if (ret != 0) return OTA_ERR_EEPROM_WRITE;

    /* 5. Second EEPROM write: crc32 (4 bytes, addr 0x17-0x1A) */
    uint8_t crc_data[4];
    crc_data[0] = (uint8_t)(ctx->expected_crc);
    crc_data[1] = (uint8_t)(ctx->expected_crc >> 8);
    crc_data[2] = (uint8_t)(ctx->expected_crc >> 16);
    crc_data[3] = (uint8_t)(ctx->expected_crc >> 24);

    ret = AT24C02_Write(ctx->eeprom, OTA_EEPROM_CRC_ADDR,
                        crc_data, sizeof(crc_data));
    if (ret != 0) return OTA_ERR_EEPROM_WRITE;

    return 0;
}

void OTA_Storage_Reset(OTA_Storage_t *ctx)
{
    ctx->flash_addr   = 0;
    ctx->fw_size      = 0;
    ctx->buf_pos      = 0;
    ctx->total_recv   = 0;
    ctx->expected_crc = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
}

void OTA_Storage_SetExpectedCRC(OTA_Storage_t *ctx, uint32_t crc)
{
    ctx->expected_crc = crc;
}
