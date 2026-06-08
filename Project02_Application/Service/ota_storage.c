#include "ota_storage.h"
#include "w25q16.h"
#include "at24c02.h"
#include <string.h>

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
    /* 校验实际接收量是否等于声明大小 */
    if (ctx->total_recv != ctx->fw_size) return OTA_ERR_SIZE_MISMATCH;

    /* 1. 刷剩余缓冲到 W25Q16 */
    int ret = ota_storage_flush(ctx);
    if (ret != 0) return OTA_ERR_FLASH_WRITE;

    /* 2. 写 EEPROM 标志位（与 Bootloader 定义一致）
     *    地址 0x10: status = 0x01 (BOOT_NEED_UPDATE)
     *    地址 0x11-0x12: key = 0xA5, 0xA5
     *    地址 0x13-0x16: fw_size (4B 小端) */
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

    return 0;
}

void OTA_Storage_Reset(OTA_Storage_t *ctx)
{
    ctx->flash_addr = 0;
    ctx->fw_size    = 0;
    ctx->buf_pos    = 0;
    ctx->total_recv = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
}
