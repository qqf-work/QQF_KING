/*
 * W25Q16 SPI Flash 驱动 —— 软件 SPI 实现
 */

#ifndef __W25Q16_H__
#define __W25Q16_H__

#include "soft_spi.h"
#include <stdint.h>

#define W25Q16_PAGE_SIZE    256
#define W25Q16_SECTOR_SIZE  4096
#define W25Q16_TOTAL_SIZE   (2 * 1024 * 1024)

#define W25Q_CMD_WRITE_ENABLE      0x06
#define W25Q_CMD_READ_STATUS1      0x05
#define W25Q_CMD_PAGE_PROGRAM      0x02
#define W25Q_CMD_READ_DATA         0x03
#define W25Q_CMD_SECTOR_ERASE      0x20
#define W25Q_CMD_CHIP_ERASE        0xC7
#define W25Q_CMD_JEDEC_ID          0x9F

#define W25Q_SR_BUSY               0x01

typedef struct {
    SoftSPI_Bus_t *bus;
    GPIO_TypeDef  *cs_port;
    uint16_t       cs_pin;
} W25Q16_t;

void     W25Q16_Init(W25Q16_t *dev, SoftSPI_Bus_t *bus,
                     GPIO_TypeDef *cs_port, uint16_t cs_pin);
uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev);
int      W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len);
int      W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len);
int      W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr);
int      W25Q16_EraseChip(W25Q16_t *dev);

#endif
