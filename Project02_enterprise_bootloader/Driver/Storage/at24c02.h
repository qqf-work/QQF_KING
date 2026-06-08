/*
 * AT24C02 EEPROM 驱动 —— 软件 I2C 实现
 */

#ifndef __AT24C02_H__
#define __AT24C02_H__

#include "soft_i2c.h"
#include <stdint.h>

#define AT24C02_PAGE_SIZE    8
#define AT24C02_TOTAL_SIZE   256
#define AT24C02_ADDR         0xA0

typedef struct {
    SoftI2C_Bus_t *bus;
    uint8_t        addr;
} AT24C02_t;

void     AT24C02_Init(AT24C02_t *dev, SoftI2C_Bus_t *bus, uint8_t addr);
int      AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data);
int      AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data);
int      AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len);
int      AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len);

#endif
