#ifndef __BSP_SOFT_SPI_H__
#define __BSP_SOFT_SPI_H__

#include "soft_spi.h"

/*
 * SPI1 总线引脚定义
 *
 * PA5 —— SCK  时钟线，由主机驱动，决定数据传输节拍
 * PA6 —— MISO 主入从出，W25Q16 → STM32 的数据通道
 * PA7 —— MOSI 主出从入，STM32 → W25Q16 的数据通道
 * 使用 SPI Mode 0（CPOL=0, CPHA=0，空闲低电平，上升沿采样）
 */
#define SPI1_SCK_PORT    GPIOA
#define SPI1_SCK_PIN     GPIO_PIN_5
#define SPI1_MISO_PORT   GPIOA
#define SPI1_MISO_PIN    GPIO_PIN_6
#define SPI1_MOSI_PORT   GPIOA
#define SPI1_MOSI_PIN    GPIO_PIN_7

/* W25Q16 片选引脚：低电平有效，每次操作前拉低选中，操作后拉高释放总线 */
#define W25Q_CS_PORT    GPIOA
#define W25Q_CS_PIN     GPIO_PIN_4

/* 板级 SPI1 总线实例，全局唯一 */
extern SoftSPI_Bus_t spi1_bus;

void BSP_SoftSPI_Init(void);

#endif
