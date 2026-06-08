/*
 * W25Q16 SPI Flash 驱动 —— 软件 SPI 实现
 *
 * W25Q16 容量 2MB（16Mbit），页大小 256 字节，扇区大小 4KB
 * 写入前必须先擦除（Flash 只能从 1 变 0，擦除将所有位恢复为 1）
 * 写入以页为单位（最多 256 字节），跨页需拆分
 * 擦除以扇区为单位（最小 4KB），或整片擦除
 *
 * SPI 协议：
 *   每次操作前拉低 CS 选中芯片，操作后拉高释放
 *   发送命令码 + 24 位地址（3 字节，高字节在前）
 *   读数据时发 dummy byte (0xFF) 切换为读方向
 */

#include "w25q16.h"
#include "main.h"

/* CS 片选控制宏 */
#define CS_LOW(dev)   HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_RESET)
#define CS_HIGH(dev)  HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_SET)

/**
 * @brief 发送写使能命令
 *
 * W25Q16 每次页写入或擦除前都必须发送写使能（WREN）
 * 写使能是单字节命令，没有数据阶段
 */
static void W25Q_WriteEnable(W25Q16_t *dev)
{
    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_WRITE_ENABLE);
    CS_HIGH(dev);
}

/**
 * @brief 读取状态寄存器 1
 * @return 状态寄存器值，bit0 = BUSY（1=正在写入/擦除）
 */
static uint8_t W25Q_ReadStatus(W25Q16_t *dev)
{
    uint8_t status;
    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_READ_STATUS1);
    status = SoftSPI_TransferByte(dev->bus, 0xFF);  /* 发 dummy 读 1 字节 */
    CS_HIGH(dev);
    return status;
}

/**
 * @brief 等待芯片完成内部写入/擦除操作
 * @return 0 完成, -1 超时
 *
 * 写入或擦除后芯片内部需要时间完成操作（页写入约 3ms，扇区擦除约 100ms）
 * 轮询 BUSY 位直到操作完成
 */
static int W25Q_WaitBusy(W25Q16_t *dev)
{
    uint32_t timeout = 0xFFFFF;
    while ((W25Q_ReadStatus(dev) & W25Q_SR_BUSY) && --timeout);
    return timeout ? 0 : -1;
}

/**
 * @brief 初始化 W25Q16 设备句柄
 * @param bus      软件 SPI 总线句柄（已初始化）
 * @param cs_port  CS 引脚端口
 * @param cs_pin   CS 引脚编号
 */
void W25Q16_Init(W25Q16_t *dev, SoftSPI_Bus_t *bus,
                 GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    dev->bus     = bus;
    dev->cs_port = cs_port;
    dev->cs_pin  = cs_pin;
    CS_HIGH(dev);  /* 默认不选中 */
}

/**
 * @brief 读取 JEDEC ID
 * @return 24 位 ID，W25Q16 应返回 0xEF4015
 *
 * 命令 0x9F，芯片返回 3 字节：厂商 ID + 内存类型 + 容量
 * 用于上电时确认 SPI 通信正常、芯片型号正确
 */
uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev)
{
    uint8_t id_buf[3];

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_JEDEC_ID);
    id_buf[0] = SoftSPI_TransferByte(dev->bus, 0xFF);  /* 厂商 ID (0xEF) */
    id_buf[1] = SoftSPI_TransferByte(dev->bus, 0xFF);  /* 内存类型 (0x40) */
    id_buf[2] = SoftSPI_TransferByte(dev->bus, 0xFF);  /* 容量 (0x15 = 16Mbit) */
    CS_HIGH(dev);

    return ((uint32_t)id_buf[0] << 16) | ((uint32_t)id_buf[1] << 8) | id_buf[2];
}

/**
 * @brief 从 W25Q16 读取数据
 * @param addr  起始地址（24 位，0x000000 ~ 0x1FFFFF）
 * @param buf   存放读出数据的缓冲区
 * @param len   读取长度
 * @return 0 成功, -1 地址越界
 *
 * 命令 0x03 + 24 位地址，然后连续读取 len 个字节
 * 读取没有页边界限制，可以跨页连续读
 */
int W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_READ_DATA);
    SoftSPI_WriteByte(dev->bus, (uint8_t)((addr >> 16) & 0xFF));  /* 地址高字节 */
    SoftSPI_WriteByte(dev->bus, (uint8_t)((addr >> 8) & 0xFF));   /* 地址中字节 */
    SoftSPI_WriteByte(dev->bus, (uint8_t)(addr & 0xFF));           /* 地址低字节 */

    for (uint16_t i = 0; i < len; i++)
        buf[i] = SoftSPI_TransferByte(dev->bus, 0xFF);  /* 发 dummy 读数据 */

    CS_HIGH(dev);
    return 0;
}

/**
 * @brief 向 W25Q16 写入数据（自动跨页拆分）
 * @param addr  起始地址
 * @param data  要写入的数据
 * @param len   数据长度
 * @return 0 成功, -1 地址越界或写入超时
 *
 * W25Q16 页写入（Page Program）一次最多 256 字节，且不能跨页边界
 * 此函数自动处理跨页拆分：
 *   1. 计算当前页剩余空间
 *   2. 若本次写入超出页边界，只写到页末尾
 *   3. 下一轮从新页继续写
 *   4. 每次写入后等待 BUSY 位清零
 */
int W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        /* 当前页剩余可写字节数 */
        uint16_t page_remain = W25Q16_PAGE_SIZE - (addr % W25Q16_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;  /* 截断到页边界 */

        W25Q_WriteEnable(dev);  /* 每次页写入前必须使能 */

        CS_LOW(dev);
        SoftSPI_WriteByte(dev->bus, W25Q_CMD_PAGE_PROGRAM);
        SoftSPI_WriteByte(dev->bus, (uint8_t)(((addr + offset) >> 16) & 0xFF));
        SoftSPI_WriteByte(dev->bus, (uint8_t)(((addr + offset) >> 8) & 0xFF));
        SoftSPI_WriteByte(dev->bus, (uint8_t)((addr + offset) & 0xFF));
        SoftSPI_Write(dev->bus, data + offset, write_len);
        CS_HIGH(dev);

        if (W25Q_WaitBusy(dev)) return -1;  /* 等待写入完成 */
        offset += write_len;
    }
    return 0;
}

/**
 * @brief 擦除一个 4KB 扇区
 * @param addr  扇区内任意地址（会对齐到扇区起始）
 * @return 0 成功, -1 地址越界或超时
 *
 * 擦除将扇区内所有字节恢复为 0xFF
 * 擦除时间约 100ms~400ms
 */
int W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr)
{
    if (addr >= W25Q16_TOTAL_SIZE) return -1;

    W25Q_WriteEnable(dev);

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_SECTOR_ERASE);
    SoftSPI_WriteByte(dev->bus, (uint8_t)((addr >> 16) & 0xFF));
    SoftSPI_WriteByte(dev->bus, (uint8_t)((addr >> 8) & 0xFF));
    SoftSPI_WriteByte(dev->bus, (uint8_t)(addr & 0xFF));
    CS_HIGH(dev);

    if (W25Q_WaitBusy(dev)) return -1;
    return 0;
}

/**
 * @brief 整片擦除
 * @return 0 成功, -1 超时
 *
 * 擦除全部 2MB 空间，耗时较长（数秒）
 */
int W25Q16_EraseChip(W25Q16_t *dev)
{
    W25Q_WriteEnable(dev);

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_CHIP_ERASE);
    CS_HIGH(dev);

    if (W25Q_WaitBusy(dev)) return -1;
    return 0;
}
