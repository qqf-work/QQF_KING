/*
 * W25Q16 SPI Flash 驱动 —— 硬件 SPI 实现
 */

#include "w25q16.h"
#include "main.h"

/* CS 操作宏 */
#define CS_LOW(dev)   HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_RESET)
#define CS_HIGH(dev)  HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_SET)

static void W25Q_WriteEnable(W25Q16_t *dev)
{
    uint8_t cmd = W25Q_CMD_WRITE_ENABLE;
    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, &cmd, 1, 100);
    CS_HIGH(dev);
}

static uint8_t W25Q_ReadStatus(W25Q16_t *dev)
{
    uint8_t cmd = W25Q_CMD_READ_STATUS1;
    uint8_t status = 0;
    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, &cmd, 1, 100);
    HAL_SPI_Receive(dev->hspi, &status, 1, 100);
    CS_HIGH(dev);
    return status;
}

static int W25Q_WaitBusy(W25Q16_t *dev)
{
    uint32_t timeout = 0xFFFFF;
    while ((W25Q_ReadStatus(dev) & W25Q_SR_BUSY) && --timeout);
    return timeout ? 0 : -1;
}

void W25Q16_Init(W25Q16_t *dev, SPI_HandleTypeDef *hspi,
                 GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    dev->hspi    = hspi;
    dev->cs_port = cs_port;
    dev->cs_pin  = cs_pin;
    CS_HIGH(dev);
}

uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev)
{
    uint8_t cmd = W25Q_CMD_JEDEC_ID;
    uint8_t id_buf[3] = {0};

    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, &cmd, 1, 100);
    HAL_SPI_Receive(dev->hspi, id_buf, 3, 100);
    CS_HIGH(dev);

    return ((uint32_t)id_buf[0] << 16) | ((uint32_t)id_buf[1] << 8) | id_buf[2];
}

int W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    uint8_t cmd[4] = {
        W25Q_CMD_READ_DATA,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)(addr & 0xFF)
    };

    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, cmd, 4, 100);
    HAL_SPI_Receive(dev->hspi, buf, len, 100);
    CS_HIGH(dev);
    return 0;
}

int W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        uint16_t page_remain = W25Q16_PAGE_SIZE - (addr % W25Q16_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        W25Q_WriteEnable(dev);

        uint8_t cmd[4] = {
            W25Q_CMD_PAGE_PROGRAM,
            (uint8_t)(((addr + offset) >> 16) & 0xFF),
            (uint8_t)(((addr + offset) >> 8) & 0xFF),
            (uint8_t)((addr + offset) & 0xFF)
        };

        CS_LOW(dev);
        HAL_SPI_Transmit(dev->hspi, cmd, 4, 100);
        HAL_SPI_Transmit(dev->hspi, data + offset, write_len, 100);
        CS_HIGH(dev);

        if (W25Q_WaitBusy(dev)) return -1;
        offset += write_len;
    }
    return 0;
}

int W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr)
{
    if (addr >= W25Q16_TOTAL_SIZE) return -1;

    W25Q_WriteEnable(dev);

    uint8_t cmd[4] = {
        W25Q_CMD_SECTOR_ERASE,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)(addr & 0xFF)
    };

    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, cmd, 4, 100);
    CS_HIGH(dev);

    if (W25Q_WaitBusy(dev)) return -1;
    return 0;
}

int W25Q16_EraseChip(W25Q16_t *dev)
{
    W25Q_WriteEnable(dev);

    uint8_t cmd = W25Q_CMD_CHIP_ERASE;
    CS_LOW(dev);
    HAL_SPI_Transmit(dev->hspi, &cmd, 1, 100);
    CS_HIGH(dev);

    if (W25Q_WaitBusy(dev)) return -1;
    return 0;
}
