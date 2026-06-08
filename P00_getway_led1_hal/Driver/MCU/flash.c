/**
 * @file    flash.c
 * @brief   内部 Flash 操作实现 —— STM32F103C8 (页大小 1KB)
 *
 * 封装 HAL 库，提供 Unlock/ErasePage/Write/Lock 接口
 */

#include "flash.h"
#include "stm32f1xx_hal.h"

int Flash_Unlock(void)
{
    return HAL_FLASH_Unlock();
}

int Flash_Lock(void)
{
    return HAL_FLASH_Lock();
}

/**
 * @brief  擦除指定地址所在的 1 页（1KB），擦除后全 0xFF
 * @param  addr  页内任意地址（建议页起始地址）
 * @return 0=成功, -1=失败
 */
int Flash_ErasePage(uint32_t addr)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t err = 0;

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = addr;
    erase.NbPages     = 1;

    if (HAL_FLASHEx_Erase(&erase, &err) != HAL_OK)
        return -1;

    return (err == 0xFFFFFFFF) ? 0 : -1;
}

/**
 * @brief  向 Flash 写入数据（半字编程）
 * @param  addr  写入起始地址（必须半字对齐）
 * @param  data  数据指针
 * @param  len   字节长度
 * @return 0=成功, -1=失败
 *
 * STM32F103 最小编程单位为 16 位
 * 奇数长度时高字节补 0xFF
 */
int Flash_Write(uint32_t addr, uint8_t *data, uint16_t len)
{
    if (addr & 1) return -1;  /* 地址必须半字对齐 */
    for (uint16_t i = 0; i < len; i += 2)
    {
        uint16_t half;
        if (i + 1 < len)
            half = data[i] | (data[i + 1] << 8);
        else
            half = data[i] | (0xFF << 8);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, half) != HAL_OK)
            return -1;
    }
    return 0;
}

/**
 * @brief  检查目标区域是否需要擦除（是否含非 0xFF）
 * @return 1=需要擦除, 0=已擦除
 */
int Flash_NeedsErase(uint32_t addr, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        if (*(volatile uint8_t *)(addr + i) != 0xFF)
            return 1;
    }
    return 0;
}
