#include "soft_spi.h"

/* GPIO 操作宏，通过总线句柄访问引脚 */
#define SCK_HIGH(bus)   HAL_GPIO_WritePin((bus)->sck_port,  (bus)->sck_pin,  GPIO_PIN_SET)
#define SCK_LOW(bus)    HAL_GPIO_WritePin((bus)->sck_port,  (bus)->sck_pin,  GPIO_PIN_RESET)
#define MOSI_HIGH(bus)  HAL_GPIO_WritePin((bus)->mosi_port, (bus)->mosi_pin, GPIO_PIN_SET)
#define MOSI_LOW(bus)   HAL_GPIO_WritePin((bus)->mosi_port, (bus)->mosi_pin, GPIO_PIN_RESET)
#define MISO_READ(bus)  HAL_GPIO_ReadPin((bus)->miso_port,  (bus)->miso_pin)

/* 简易延时，与软件 I2C 一致 */
static void SPI_Delay(void)
{
    uint8_t i;
    for (i = 0; i < 10; i++);
}

/**
 * @brief 填充总线句柄并释放总线（SCK/MOSI 拉高）
 *
 * GPIO 初始化由 BSP 层完成，这里只记录引脚信息
 */
void SoftSPI_Init(SoftSPI_Bus_t *bus,
                  GPIO_TypeDef *sck_port, uint16_t sck_pin,
                  GPIO_TypeDef *mosi_port, uint16_t mosi_pin,
                  GPIO_TypeDef *miso_port, uint16_t miso_pin)
{
    bus->sck_port  = sck_port;
    bus->sck_pin   = sck_pin;
    bus->mosi_port = mosi_port;
    bus->mosi_pin  = mosi_pin;
    bus->miso_port = miso_port;
    bus->miso_pin  = miso_pin;

    SCK_LOW(bus);       /* Mode 0：空闲低电平 */
    MOSI_HIGH(bus);
}

/**
 * @brief SPI 发送一个字节（只发不收）
 *
 * SPI Mode 0：CPOL=0（空闲低电平），CPHA=0（上升沿采样）
 * MSB 先发
 */
void SoftSPI_WriteByte(SoftSPI_Bus_t *bus, uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        if (byte & (0x80 >> i))
            MOSI_HIGH(bus);
        else
            MOSI_LOW(bus);

        SCK_HIGH(bus);      /* 上升沿，从机采样 MOSI */
        SPI_Delay();
        SCK_LOW(bus);
        SPI_Delay();
    }
}

/**
 * @brief SPI 全双工收发一个字节（滑动掩码方式）
 *
 * SPI Mode 0：上升沿从机采样 MOSI，下降沿主机采样 MISO
 * MSB 先发，用 0x80 >> i 逐位取发送位，独立 rx 变量存接收
 */
uint8_t SoftSPI_TransferByte(SoftSPI_Bus_t *bus, uint8_t byte)
{
    uint8_t rx = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        if (byte & (0x80 >> i))
            MOSI_HIGH(bus);
        else
            MOSI_LOW(bus);

        SCK_HIGH(bus);      /* 上升沿，从机采样 MOSI */
        SPI_Delay();

        rx <<= 1;
        if (MISO_READ(bus)) rx |= 1;  /* 下降沿前读取 MISO */

        SCK_LOW(bus);
        SPI_Delay();
    }

    return rx;
}

/**
 * @brief 连续写入多个字节
 */
void SoftSPI_Write(SoftSPI_Bus_t *bus, uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        SoftSPI_WriteByte(bus, data[i]);
}
