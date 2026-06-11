#include "at24c02.h"
#include "main.h"

void AT24C02_Init(AT24C02_t *dev, SoftI2C_Bus_t *bus, uint8_t addr)
{
    dev->bus  = bus;
    dev->addr = addr;
}

/**
 * @brief 写入单个字节
 *
 * 时序：START → 设备地址(W) → 内存地址 → 数据 → STOP
 * 写入后需等待 ~5ms 完成内部写入周期
 */
int AT24C02_WriteByte(AT24C02_t *dev, uint8_t addr, uint8_t data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;

    int ret = SoftI2C_WriteReg(dev->bus, dev->addr, addr, &data, 1);
    HAL_Delay(5);
    return ret;
}

/**
 * @brief 读取单个字节
 */
int AT24C02_ReadByte(AT24C02_t *dev, uint8_t addr, uint8_t *data)
{
    if (addr >= AT24C02_TOTAL_SIZE) return -1;

    return SoftI2C_ReadReg(dev->bus, dev->addr, addr, data, 1);
}

/**
 * @brief 多字节写入（自动处理页边界）
 *
 * AT24C02 页大小为 8 字节，写入跨页时地址会在页内回绕覆盖。
 * 本函数自动拆分写入，确保跨页数据不丢失。
 *
 * 示例：从地址 5 写入 10 字节
 *   第 1 段：地址 5~7（3 字节，到页边界）
 *   第 2 段：地址 8~12（7 字节）
 */
int AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        /* 当前地址到页末尾的剩余空间 */
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        if (SoftI2C_WriteReg(dev->bus, dev->addr, addr + offset, data + offset, write_len) != 0)
            return -1;

        HAL_Delay(5);   /* 等待内部写入周期 */
        offset += write_len;
    }
    return 0;
}

/**
 * @brief 多字节读取（顺序读，无页边界限制）
 *
 * EEPROM 读取不会回绕，可以一次性连续读取
 */
int AT24C02_Read(AT24C02_t *dev, uint8_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > AT24C02_TOTAL_SIZE) return -1;

    return SoftI2C_ReadReg(dev->bus, dev->addr, addr, buf, len);
}
