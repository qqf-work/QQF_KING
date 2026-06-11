#include "soft_i2c.h"

/* GPIO 操作宏，通过总线句柄访问引脚 */
#define SCL_HIGH(bus)  HAL_GPIO_WritePin((bus)->scl_port, (bus)->scl_pin, GPIO_PIN_SET)
#define SCL_LOW(bus)   HAL_GPIO_WritePin((bus)->scl_port, (bus)->scl_pin, GPIO_PIN_RESET)
#define SDA_HIGH(bus)  HAL_GPIO_WritePin((bus)->sda_port, (bus)->sda_pin, GPIO_PIN_SET)
#define SDA_LOW(bus)   HAL_GPIO_WritePin((bus)->sda_port, (bus)->sda_pin, GPIO_PIN_RESET)
#define SDA_READ(bus)  HAL_GPIO_ReadPin((bus)->sda_port, (bus)->sda_pin)

/* 简易延时，约几百 ns，软件 I2C 不需要精确时序 */
static void I2C_Delay(void)
{
    uint8_t i;
    for (i = 0; i < 10; i++);
}

/**
 * @brief 填充总线句柄并释放总线（SCL/SDA 拉高）
 *
 * GPIO 初始化由 BSP 层完成，这里只记录引脚信息
 */
void SoftI2C_Init(SoftI2C_Bus_t *bus,
                  GPIO_TypeDef *scl_port, uint16_t scl_pin,
                  GPIO_TypeDef *sda_port, uint16_t sda_pin)
{
    bus->scl_port = scl_port;
    bus->scl_pin  = scl_pin;
    bus->sda_port = sda_port;
    bus->sda_pin  = sda_pin;

    SCL_HIGH(bus);
    SDA_HIGH(bus);
}

/**
 * @brief I2C 起始信号
 *
 * 时序：SCL 高电平期间，SDA 从高拉低
 *       ___
 * SCL:    |___
 *       ____
 * SDA:     |___
 */
void SoftI2C_Start(SoftI2C_Bus_t *bus)
{
    SDA_HIGH(bus);
    SCL_HIGH(bus);
    I2C_Delay();
    SDA_LOW(bus);       /* SCL 高时 SDA 下降沿 = START */
    I2C_Delay();
    SCL_LOW(bus);       /* 钳住 SCL，准备发送数据 */
}

/**
 * @brief I2C 停止信号
 *
 * 时序：SCL 高电平期间，SDA 从低拉高
 *       ___
 * SCL:    |___
 *           ____
 * SDA: ____|
 */
void SoftI2C_Stop(SoftI2C_Bus_t *bus)
{
    SDA_LOW(bus);
    SCL_HIGH(bus);
    I2C_Delay();
    SDA_HIGH(bus);      /* SCL 高时 SDA 上升沿 = STOP */
    I2C_Delay();
}

/**
 * @brief I2C 发送一个字节
 * @param byte  要发送的数据
 * @return 0 = 从机应答 ACK, 1 = 从机无应答 NACK
 *
 * MSB 先发，每个 bit 在 SCL 上升沿被从机采样
 */
uint8_t SoftI2C_SendByte(SoftI2C_Bus_t *bus, uint8_t byte)
{
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        /* MSB 先发：取最高位设置 SDA */
        if (byte & (0x80 >> i))
            SDA_HIGH(bus);
        else
            SDA_LOW(bus);
        SCL_HIGH(bus);  /* 从机在上升沿读取 SDA */
        I2C_Delay();
        SCL_LOW(bus);
        I2C_Delay();
    }

    /* 第 9 个时钟：读取从机应答 */
    SDA_HIGH(bus);      /* 释放 SDA，让从机驱动 */
    SCL_HIGH(bus);
    I2C_Delay();
    uint8_t ack = SDA_READ(bus);   /* SDA 低 = ACK, SDA 高 = NACK */
    SCL_LOW(bus);
    I2C_Delay();
    return ack;
}

/**
 * @brief I2C 读取一个字节
 * @param ack  1 = 回复 ACK（继续读）, 0 = 回复 NACK（读最后一个字节）
 * @return 读到的字节数据
 */
uint8_t SoftI2C_ReadByte(SoftI2C_Bus_t *bus, uint8_t ack)
{
    uint8_t i, byte = 0;

    SDA_HIGH(bus);      /* 释放 SDA，切换为输入方向 */
    for (i = 0; i < 8; i++)
    {
        SCL_HIGH(bus);
        I2C_Delay();
        byte <<= 1;
        if (SDA_READ(bus)) byte |= 1;  /* SCL 高时采样 SDA */
        SCL_LOW(bus);
        I2C_Delay();
    }

    /* 主机发送应答：ACK 拉低 SDA，NACK 保持高 */
    if (ack)
        SDA_LOW(bus);
    else
        SDA_HIGH(bus);
    SCL_HIGH(bus);
    I2C_Delay();
    SCL_LOW(bus);
    I2C_Delay();
    SDA_HIGH(bus);      /* 恢复 SDA 高电平 */

    return byte;
}

/**
 * @brief 写入设备寄存器（MPU6050 等传感器通用）
 * @param dev_addr  设备地址（已左移，如 0xD0）
 * @param reg       寄存器地址
 * @param data      要写入的数据
 * @param len       数据长度
 * @return 0 成功, -1 从机无应答
 *
 * 时序：START → 设备地址(W) → 寄存器地址 → 数据[0..N] → STOP
 */
int SoftI2C_WriteReg(SoftI2C_Bus_t *bus, uint8_t dev_addr,
                     uint8_t reg, uint8_t *data, uint16_t len)
{
    SoftI2C_Start(bus);
    if (SoftI2C_SendByte(bus, dev_addr))  { SoftI2C_Stop(bus); return -1; }
    if (SoftI2C_SendByte(bus, reg))       { SoftI2C_Stop(bus); return -1; }
    for (uint16_t i = 0; i < len; i++)
    {
        if (SoftI2C_SendByte(bus, data[i])) { SoftI2C_Stop(bus); return -1; }
    }
    SoftI2C_Stop(bus);
    return 0;
}

/**
 * @brief 总线恢复
 *
 * 当从机在通信中途被断电或复位，可能持续拉低 SDA 导致总线锁死。
 * 恢复方法：发送 9 个 SCL 时钟脉冲让从机完成未完成的传输，
 * 然后发送 STOP 释放总线。
 */
void SoftI2C_BusRecovery(SoftI2C_Bus_t *bus)
{
    SDA_HIGH(bus);
    for (uint8_t i = 0; i < 9; i++)
    {
        SCL_HIGH(bus);
        I2C_Delay();
        SCL_LOW(bus);
        I2C_Delay();
    }
    SoftI2C_Stop(bus);
}
/**
 * @brief 读取设备寄存器（MPU6050 等传感器通用）
 * @param dev_addr  设备地址（已左移，如 0xD0）
 * @param reg       寄存器地址
 * @param buf       存放读出数据的缓冲区
 * @param len       读取长度
 * @return 0 成功, -1 从机无应答
 *
 * 时序：START → 设备地址(W) → 寄存器地址 → RESTART → 设备地址(R) → 读数据[0..N] → STOP
 *       最后一个字节回复 NACK，其余回复 ACK
 */
int SoftI2C_ReadReg(SoftI2C_Bus_t *bus, uint8_t dev_addr,
                    uint8_t reg, uint8_t *buf, uint16_t len)
{
    SoftI2C_Start(bus);
    if (SoftI2C_SendByte(bus, dev_addr))     { SoftI2C_Stop(bus); return -1; }
    if (SoftI2C_SendByte(bus, reg))          { SoftI2C_Stop(bus); return -1; }

    SoftI2C_Start(bus);                      /* 重复起始信号，切换读方向 */
    if (SoftI2C_SendByte(bus, dev_addr | 1)) { SoftI2C_Stop(bus); return -1; }
    for (uint16_t i = 0; i < len; i++)
    {
        buf[i] = SoftI2C_ReadByte(bus, (i < len - 1) ? 1 : 0);
    }
    SoftI2C_Stop(bus);
    return 0;
}
