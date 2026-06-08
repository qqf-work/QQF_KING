#include "w25q16.h"
#include "main.h"

/* CS 操作宏 */
#define CS_LOW(dev)   HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_RESET)
#define CS_HIGH(dev)  HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_SET)

static void W25Q_WriteEnable(W25Q16_t *dev)
{
    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_WRITE_ENABLE);
    CS_HIGH(dev);
}

static uint8_t W25Q_ReadStatus(W25Q16_t *dev)
{
    uint8_t status;
    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_READ_STATUS1);
    status = SoftSPI_TransferByte(dev->bus, 0xFF);
    CS_HIGH(dev);
    return status;
}



static int W25Q_WaitBusy(W25Q16_t *dev)
{
    uint32_t timeout = 0xFFFFF;
    while ((W25Q_ReadStatus(dev) & W25Q_SR_BUSY) && --timeout);
    return timeout ? 0 : -1;
}

/**
 * @brief 初始化 W25Q16 设备句柄
 *
 * 记录 SPI 总线和 CS 引脚，CS 默认拉高（不选中）
 */
void W25Q16_Init(W25Q16_t *dev, SoftSPI_Bus_t *bus,
                 GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    dev->bus     = bus;
    dev->cs_port = cs_port;
    dev->cs_pin  = cs_pin;
    CS_HIGH(dev);
}

/**
 * @brief 读取 JEDEC ID
 *
 * 时序：CS→ 发 0x9F → 读 3 字节（厂商标识 + 内存类型 + 容量）
 * W25Q16 应返回 0xEF4015
 */
uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev)
{
    uint32_t id = 0;

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_JEDEC_ID);
    id  = (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF) << 16;
    id |= (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF) << 8;
    id |= (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF);
    CS_HIGH(dev);

    return id;
}

/**
 * @brief 读取数据
 *
 * 时序：CS→ 发 0x03 → 发 24 位地址 → 读数据 → CS↑
 * 无页边界限制，可连续读取到芯片末尾
 */
int W25Q16_Read(W25Q16_t *dev, uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_READ_DATA);
    SoftSPI_WriteByte(dev->bus, (addr >> 16) & 0xFF);
    SoftSPI_WriteByte(dev->bus, (addr >> 8)  & 0xFF);
    SoftSPI_WriteByte(dev->bus,  addr        & 0xFF);

    for (uint16_t i = 0; i < len; i++)
        buf[i] = SoftSPI_TransferByte(dev->bus, 0xFF);

    CS_HIGH(dev);
    return 0;
}

/**
 * @brief 写入数据（自动处理页边界）
 *
 * W25Q16 页编程（Page Program）每次最多写 256 字节，且不能跨页。
 * 超过页边界的部分会回绕到页首覆盖，因此需要自动拆分。
 *
 * 注意：Flash 只能将 1 写为 0，写入前必须先擦除（擦除将所有位置 1）
 */
int W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len)
{
    if (addr + len > W25Q16_TOTAL_SIZE) return -1;

    uint16_t offset = 0;
    while (offset < len)
    {
        /* 当前地址到页末尾的剩余空间 */
        uint16_t page_remain = W25Q16_PAGE_SIZE - (addr % W25Q16_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        W25Q_WriteEnable(dev);

        CS_LOW(dev);
        SoftSPI_WriteByte(dev->bus, W25Q_CMD_PAGE_PROGRAM);
        SoftSPI_WriteByte(dev->bus, ((addr + offset) >> 16) & 0xFF);
        SoftSPI_WriteByte(dev->bus, ((addr + offset) >> 8)  & 0xFF);
        SoftSPI_WriteByte(dev->bus,  (addr + offset)        & 0xFF);
        SoftSPI_Write(dev->bus, data + offset, write_len);
        CS_HIGH(dev);

        if (W25Q_WaitBusy(dev)) return -1;
        offset += write_len;
    }
    return 0;
}

/**
 * @brief 擦除一个扇区（4KB）
 *
 * 将指定地址所在的扇区全部置为 0xFF
 * addr 会对齐到扇区起始地址（4KB 边界）
 */
int W25Q16_EraseSector(W25Q16_t *dev, uint32_t addr)
{
    if (addr >= W25Q16_TOTAL_SIZE) return -1;

    W25Q_WriteEnable(dev);

    CS_LOW(dev);
    SoftSPI_WriteByte(dev->bus, W25Q_CMD_SECTOR_ERASE);
    SoftSPI_WriteByte(dev->bus, (addr >> 16) & 0xFF);
    SoftSPI_WriteByte(dev->bus, (addr >> 8)  & 0xFF);
    SoftSPI_WriteByte(dev->bus,  addr        & 0xFF);
    CS_HIGH(dev);

    if (W25Q_WaitBusy(dev)) return -1;
    return 0;
}

/**
 * @brief 整片擦除
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
