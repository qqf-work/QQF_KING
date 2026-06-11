#ifndef __AT24C02_H__
#define __AT24C02_H__

/*
 * AT24C02 EEPROM 驱动（256 字节，I2C 接口）
 *
 * 通过软件 I2C 总线与 AT24C02 通信
 * 写入时自动处理 8 字节页边界拆分，读取无页限制
 */

#include "soft_i2c.h"
#include <stdint.h>

/* A0=A1=A2=GND 时的 7 位地址左移后值，实际地址为 0xA0（写）/ 0xA1（读） */
#define AT24C02_ADDR        0xA0
/* 页大小：8 字节，跨页写入会回绕覆盖，需自动拆分 */
#define AT24C02_PAGE_SIZE   8
/* 总容量：256 字节（地址范围 0x00~0xFF） */
#define AT24C02_TOTAL_SIZE  256

/* AT24C02 设备实例：持有总线句柄 + 设备地址，支持多设备挂同一总线 */
typedef struct {
    SoftI2C_Bus_t *bus;   /* 指向 I2C 总线句柄，由 BSP 层初始化 */
    uint8_t        addr;  /* 设备 I2C 地址（已左移，如 0xA0） */
} AT24C02_t;

/* 初始化设备句柄，绑定总线句柄和设备地址 */
void     AT24C02_Init(AT24C02_t *dev, SoftI2C_Bus_t *bus, uint8_t addr);
/* 写入单个字节，写入后等待 5ms 完成内部写入周期 */
int      AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data);
/* 读取单个字节 */
int      AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data);
/* 多字节写入（自动拆分跨页数据，每页写入后等待 5ms） */
int      AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len);
/* 多字节读取（顺序读，无页边界限制，可一次读完整个芯片） */
int      AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len);

#endif
