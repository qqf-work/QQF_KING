/*
 * AT24C02 EEPROM 驱动（256 字节，I2C 接口）
 *
 * 通过硬件 I2C 与 AT24C02 通信
 * 写入时自动处理 8 字节页边界拆分，读取无页限制
 */

#ifndef __AT24C02_H__
#define __AT24C02_H__

#include "stm32f1xx_hal.h"
#include <stdint.h>

/* A0=A1=A2=GND 时的 7 位地址左移后值 */
#define AT24C02_ADDR        0xA0
/* 页大小：8 字节，跨页写入会回绕覆盖，需自动拆分 */
#define AT24C02_PAGE_SIZE   8
/* 总容量：256 字节（地址范围 0x00~0xFF） */
#define AT24C02_TOTAL_SIZE  256

/* AT24C02 设备实例：持有 HAL I2C 句柄 + 设备地址 */
typedef struct {
    I2C_HandleTypeDef *hi2c;  /* HAL I2C 句柄，由 CubeMX 初始化 */
    uint8_t            addr;  /* 设备 I2C 地址（已左移，如 0xA0） */
} AT24C02_t;

/* 初始化设备句柄，绑定 HAL I2C 句柄和设备地址 */
void     AT24C02_Init(AT24C02_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr);
/* 写入单个字节，写入后等待 5ms */
int      AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data);
/* 读取单个字节 */
int      AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data);
/* 多字节写入（自动拆分跨页数据，每页写入后等待 5ms） */
int      AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len);
/* 多字节读取（顺序读，无页边界限制） */
int      AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len);

#endif
