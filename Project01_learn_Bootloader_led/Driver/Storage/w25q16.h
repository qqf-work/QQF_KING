#ifndef __W25Q16_H__
#define __W25Q16_H__

/*
 * W25Q16 SPI Flash 驱动（2MB，SPI 接口）
 *
 * 通过软件 SPI 总线与 W25Q16 通信
 * 写入前必须先擦除（Flash 只能 1→0，擦除将所有位置 1）
 * 写入自动处理 256 字节页边界拆分
 */

#include "soft_spi.h"
#include <stdint.h>

/* 页大小：256 字节，Page Program 最大写入长度，且不能跨页 */
#define W25Q16_PAGE_SIZE    256
/* 扇区大小：4KB，最小擦除单位 */
#define W25Q16_SECTOR_SIZE  4096
/* 总容量：2MB（地址范围 0x000000~0x1FFFFF） */
#define W25Q16_TOTAL_SIZE   (2 * 1024 * 1024)

/* ---- W25Q16 命令字 ---- */
#define W25Q_CMD_WRITE_ENABLE      0x06  /* 写使能：每次擦除/写入前必须发送 */
#define W25Q_CMD_WRITE_DISABLE     0x04  /* 写禁止 */
#define W25Q_CMD_READ_STATUS1      0x05  /* 读状态寄存器1：bit0=BUSY, bit1=WEL */
#define W25Q_CMD_WRITE_STATUS1     0x01  /* 写状态寄存器1 */
#define W25Q_CMD_PAGE_PROGRAM      0x02  /* 页编程：写入最多 256 字节到指定页 */
#define W25Q_CMD_READ_DATA         0x03  /* 读数据：从指定地址连续读取，无页限制 */
#define W25Q_CMD_SECTOR_ERASE      0x20  /* 扇区擦除：将 4KB 扇区全部置为 0xFF */
#define W25Q_CMD_BLOCK_ERASE_32K   0x52  /* 32KB 块擦除 */
#define W25Q_CMD_BLOCK_ERASE_64K   0xD8  /* 64KB 块擦除 */
#define W25Q_CMD_CHIP_ERASE        0xC7  /* 整片擦除 */
#define W25Q_CMD_JEDEC_ID          0x9F  /* 读取 JEDEC ID（厂商标识+内存类型+容量） */

/* 状态寄存器位定义 */
#define W25Q_SR_BUSY               0x01  /* bit0: 正在执行内部操作（擦除/写入） */
#define W25Q_SR_WEL                0x02  /* bit1: 写使能锁存器状态 */

/* W25Q16 设备实例：持有 SPI 总线句柄 + CS 引脚，支持多设备挂同一总线 */
typedef struct {
    SoftSPI_Bus_t *bus;     /* 指向 SPI 总线句柄，由 BSP 层初始化 */
    GPIO_TypeDef  *cs_port; /* 片选引脚端口 */
    uint16_t       cs_pin;  /* 片选引脚编号，低电平选中芯片 */
} W25Q16_t;

void     W25Q16_Init(W25Q16_t *dev, SoftSPI_Bus_t *bus,
                     GPIO_TypeDef *cs_port, uint16_t cs_pin);
uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev);
int      W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len);
int      W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len);
int      W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr);
int      W25Q16_EraseChip(W25Q16_t *dev);

#endif
