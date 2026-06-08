/*
 * W25Q16 SPI Flash 驱动（2MB，SPI 接口）
 *
 * 通过硬件 SPI 与 W25Q16 通信
 * 写入前必须先擦除（Flash 只能 1→0，擦除将所有位置 1）
 * 写入自动处理 256 字节页边界拆分
 */

#ifndef __W25Q16_H__
#define __W25Q16_H__

#include "stm32f1xx_hal.h"
#include <stdint.h>

/* 页大小：256 字节，Page Program 最大写入长度，且不能跨页 */
#define W25Q16_PAGE_SIZE    256
/* 扇区大小：4KB，最小擦除单位 */
#define W25Q16_SECTOR_SIZE  4096
/* 总容量：2MB（地址范围 0x000000~0x1FFFFF） */
#define W25Q16_TOTAL_SIZE   (2 * 1024 * 1024)

/* ---- W25Q16 命令字 ---- */
#define W25Q_CMD_WRITE_ENABLE      0x06
#define W25Q_CMD_READ_STATUS1      0x05
#define W25Q_CMD_PAGE_PROGRAM      0x02
#define W25Q_CMD_READ_DATA         0x03
#define W25Q_CMD_SECTOR_ERASE      0x20
#define W25Q_CMD_CHIP_ERASE        0xC7
#define W25Q_CMD_JEDEC_ID          0x9F

/* 状态寄存器位定义 */
#define W25Q_SR_BUSY               0x01

/* W25Q16 设备实例：持有 HAL SPI 句柄 + CS 引脚 */
typedef struct {
    SPI_HandleTypeDef *hspi;     /* HAL SPI 句柄，由 CubeMX 初始化 */
    GPIO_TypeDef      *cs_port;  /* 片选引脚端口 */
    uint16_t           cs_pin;   /* 片选引脚编号，低电平选中 */
} W25Q16_t;

void     W25Q16_Init(W25Q16_t *dev, SPI_HandleTypeDef *hspi,
                     GPIO_TypeDef *cs_port, uint16_t cs_pin);
uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev);
int      W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len);
int      W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len);
int      W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr);
int      W25Q16_EraseChip(W25Q16_t *dev);

#endif
