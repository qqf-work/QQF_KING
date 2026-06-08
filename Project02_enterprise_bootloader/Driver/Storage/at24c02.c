/*
 * AT24C02 EEPROM 驱动 —— 软件 I2C 实现
 *
 * AT24C02 容量 256 字节（2Kbit），页大小 8 字节
 * I2C 设备地址：高 4 位固定 1010，低 3 位由 A0/A1/A2 引脚决定，本板全接 GND → 0xA0
 *
 * 写入规则：
 *   - 单次写入不能跨页边界（8 字节），跨页需拆分多次
 *   - 每次页写入后需要等待 5ms（内部写周期），否则下次写入会失败
 *   - 无需擦除，可直接覆盖写入（EEPROM 特性，区别于 Flash）
 *
 * 读取规则：
 *   - 没有页边界限制，可以连续读取任意长度
 */

#include "at24c02.h"
#include "main.h"

/**
 * @brief 初始化 AT24C02 设备句柄
 * @param bus  软件 I2C 总线句柄（已由 BSP_SoftI2C_Init 初始化）
 * @param addr I2C 设备地址（本板为 0xA0）
 */
void AT24C02_Init(AT24C02_t *dev, SoftI2C_Bus_t *bus, uint8_t addr)
{
    dev->bus  = bus;
    dev->addr = addr;
}

/**
 * @brief 写入单个字节
 * @param addr  EEPROM 内部地址（0x00 ~ 0xFF）
 * @param data  要写入的字节
 * @return 0 成功, -1 地址越界或从机无应答
 */
int AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;
    return SoftI2C_WriteReg(dev->bus, dev->addr, addr, &data, 1);
}

/**
 * @brief 读取单个字节
 * @param addr  EEPROM 内部地址
 * @param data  存放读出字节的指针
 * @return 0 成功, -1 地址越界或从机无应答
 */
int AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;
    return SoftI2C_ReadReg(dev->bus, dev->addr, addr, data, 1);
}

/**
 * @brief 写入多个字节（自动跨页拆分）
 * @param addr  起始地址
 * @param data  要写入的数据
 * @param len   数据长度
 * @return 0 成功, -1 地址越界或通信失败
 *
 * AT24C02 页大小 8 字节，单次页写入不能跨页边界
 * 本函数自动处理跨页拆分：
 *   1. 计算当前页剩余空间（页大小 - 地址在页内偏移）
 *   2. 若本次写入超出页边界，只写到页末尾
 *   3. 下一轮从新页继续写
 *   4. 每次页写入后等待 5ms（AT24C02 内部写周期）
 */
int AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        /* 当前页剩余可写字节数 */
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;  /* 截断到页边界 */

        if (SoftI2C_WriteReg(dev->bus, dev->addr,
                             addr + offset, data + offset, write_len) != 0)
            return -1;

        HAL_Delay(5);      /* 等待 AT24C02 内部写周期完成 */
        offset += write_len;
    }
    return 0;
}

/**
 * @brief 读取多个字节
 * @param addr  起始地址
 * @param buf   存放读出数据的缓冲区
 * @param len   读取长度
 * @return 0 成功, -1 地址越界或通信失败
 *
 * 读取没有页边界限制，I2C 连续读即可
 */
int AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;
    return SoftI2C_ReadReg(dev->bus, dev->addr, addr, buf, len);
}
