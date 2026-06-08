#ifndef __BSP_SOFT_I2C_H__
#define __BSP_SOFT_I2C_H__

#include "soft_i2c.h"

/*
 * I2C1 总线引脚定义（换板子改这里）
 *
 * PB8 —— I2C 时钟线 SCL，控制通信节拍
 * PB9 —— I2C 数据线 SDA，开漏输出，双向传输
 * 两线均需外接上拉电阻（板载或内部上拉），用于连接 AT24C02、OLED 等
 */
#define I2C1_SCL_PORT   GPIOB
#define I2C1_SCL_PIN    GPIO_PIN_8
#define I2C1_SDA_PORT   GPIOB
#define I2C1_SDA_PIN    GPIO_PIN_9

extern SoftI2C_Bus_t i2c1_bus;

void BSP_SoftI2C_Init(void);

#endif
