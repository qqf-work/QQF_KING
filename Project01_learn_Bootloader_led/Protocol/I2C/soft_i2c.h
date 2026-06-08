#ifndef __SOFT_I2C_H__
#define __SOFT_I2C_H__

#include "main.h"
#include <stdint.h>

/* 软件 I2C 总线句柄，不同设备可共用同一个总线实例（各自管理设备地址） */
typedef struct {
    GPIO_TypeDef *scl_port;  /* SCL 引脚所属 GPIO 端口（如 GPIOB） */
    uint16_t      scl_pin;   /* SCL 引脚编号（如 GPIO_PIN_8） */
    GPIO_TypeDef *sda_port;  /* SDA 引脚所属 GPIO 端口 */
    uint16_t      sda_pin;   /* SDA 引脚编号 */
} SoftI2C_Bus_t;

/* 初始化总线句柄，填入引脚信息并释放总线（SCL/SDA 拉高） */
void     SoftI2C_Init(SoftI2C_Bus_t *bus,
                      GPIO_TypeDef *scl_port, uint16_t scl_pin,
                      GPIO_TypeDef *sda_port, uint16_t sda_pin);
/* 发送起始信号（SCL 高时 SDA 下降沿） */
void     SoftI2C_Start(SoftI2C_Bus_t *bus);
/* 发送停止信号（SCL 高时 SDA 上升沿） */
void     SoftI2C_Stop(SoftI2C_Bus_t *bus);
/* 发送一个字节，返回 0=从机ACK, 1=从机NACK */
uint8_t  SoftI2C_SendByte(SoftI2C_Bus_t *bus, uint8_t byte);
/* 读取一个字节，ack=1 回复ACK继续读, ack=0 回复NACK结束读 */
uint8_t  SoftI2C_ReadByte(SoftI2C_Bus_t *bus, uint8_t ack);

/* 写入设备寄存器：START → 设备地址(W) → 寄存器地址 → 数据 → STOP */
int      SoftI2C_WriteReg(SoftI2C_Bus_t *bus, uint8_t dev_addr,
                          uint8_t reg, uint8_t *data, uint16_t len);
/* 读取设备寄存器：START → 设备地址(W) → 寄存器地址 → RESTART → 设备地址(R) → 数据 → STOP */
int      SoftI2C_ReadReg(SoftI2C_Bus_t *bus, uint8_t dev_addr,
                         uint8_t reg, uint8_t *buf, uint16_t len);
/* 总线恢复：发送 9 个 SCL 脉冲 + STOP，用于解除从机锁死 SDA 的情况 */
void     SoftI2C_BusRecovery(SoftI2C_Bus_t *bus);

#endif
