/*
 * AT24C02 EEPROM 驱动 —— 硬件 I2C 实现
 */

#include "at24c02.h"
#include "main.h"

void AT24C02_Init(AT24C02_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr)
{
    dev->hi2c = hi2c;
    dev->addr = addr;
}

int AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;

    HAL_StatusTypeDef ret = HAL_I2C_Mem_Write(dev->hi2c, dev->addr, addr,
                                               I2C_MEMADD_SIZE_8BIT,
                                               &data, 1, 100);
    HAL_Delay(5);
    return (ret == HAL_OK) ? 0 : -1;
}

int AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;

    return (HAL_I2C_Mem_Read(dev->hi2c, dev->addr, addr,
                              I2C_MEMADD_SIZE_8BIT,
                              data, 1, 100) == HAL_OK) ? 0 : -1;
}

int AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        if (HAL_I2C_Mem_Write(dev->hi2c, dev->addr, addr + offset,
                               I2C_MEMADD_SIZE_8BIT,
                               data + offset, write_len, 100) != HAL_OK)
            return -1;

        HAL_Delay(5);
        offset += write_len;
    }
    return 0;
}

int AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;

    return (HAL_I2C_Mem_Read(dev->hi2c, dev->addr, addr,
                              I2C_MEMADD_SIZE_8BIT,
                              buf, len, 100) == HAL_OK) ? 0 : -1;
}
