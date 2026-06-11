#ifndef __SOFT_SPI_H__
#define __SOFT_SPI_H__

#include "main.h"
#include <stdint.h>

/* 软件 SPI 总线句柄，不同设备可共用同一个总线实例（各自管理 CS） */
typedef struct {
    GPIO_TypeDef *sck_port;   /* SCK 时钟引脚端口 */
    uint16_t      sck_pin;    /* SCK 时钟引脚编号 */
    GPIO_TypeDef *mosi_port;  /* MOSI 主出从入引脚端口 */
    uint16_t      mosi_pin;   /* MOSI 引脚编号 */
    GPIO_TypeDef *miso_port;  /* MISO 主入从出引脚端口 */
    uint16_t      miso_pin;   /* MISO 引脚编号 */
} SoftSPI_Bus_t;

/* 初始化总线句柄，填入引脚信息，SCK 空闲低电平（Mode 0） */
void     SoftSPI_Init(SoftSPI_Bus_t *bus,
                      GPIO_TypeDef *sck_port, uint16_t sck_pin,
                      GPIO_TypeDef *mosi_port, uint16_t mosi_pin,
                      GPIO_TypeDef *miso_port, uint16_t miso_pin);
/* 只发不收：发送一个字节，忽略 MISO（适用于写命令/写数据） */
void     SoftSPI_WriteByte(SoftSPI_Bus_t *bus, uint8_t byte);
/* 全双工收发：同时发送和接收一个字节（适用于读寄存器，发 dummy byte 读数据） */
uint8_t  SoftSPI_TransferByte(SoftSPI_Bus_t *bus, uint8_t byte);
/* 连续写入多个字节（内部循环调用 WriteByte） */
void     SoftSPI_Write(SoftSPI_Bus_t *bus, uint8_t *data, uint16_t len);

#endif
