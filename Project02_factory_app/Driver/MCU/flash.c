/*
 * 内部 Flash 操作实现 —— STM32F103C8
 *
 * 封装 HAL 库的 Flash 操作，提供统一的 Unlock/ErasePage/Write/Lock 接口
 * 换芯片时只需修改此文件，上层调用无需改动
 */

#include "flash.h"
#include "bootloader_conf.h"
#include "stm32f1xx_hal.h"

/**
 * @brief 解锁 Flash 控制寄存器
 *
 * STM32 Flash 默认锁定，擦除/写入前必须先解锁
 * 底层操作：KEYR = 0x45670123, KEYR = 0xCDEF89AB
 */
int Flash_Unlock(void)
{
    return HAL_FLASH_Unlock();
}

int Flash_Lock(void)
{
    return HAL_FLASH_Lock();
}

/**
 * @brief 擦除指定地址所在的 1 页 Flash（1KB）
 * @param addr 要擦除的页内任意地址（建议使用页起始地址）
 * @return 0 成功, -1 失败
 *
 * STM32F103C8 页大小为 1KB，擦除后该页所有字节变为 0xFF
 * 操作前必须先调用 Flash_Unlock()
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

    /* err == 0xFFFFFFFF 表示无错误（HAL 库约定） */
    return (err == 0xFFFFFFFF) ? 0 : -1;
}

/**
 * @brief 向 Flash 写入数据（半字编程）
 * @param addr 写入起始地址（必须半字对齐，即偶地址）
 * @param data 要写入的数据
 * @param len  数据长度（字节）
 * @return 0 成功, -1 失败
 *
 * STM32F103 最小编程单位为半字（16 位）
 * 奇数长度时最后一个字节补 0xFF 作为高字节
 */
int Flash_Write(uint32_t addr, uint8_t *data, uint16_t len)
{
    if (addr & 1) return -1;  /* 地址必须半字对齐 */
    for (uint16_t i = 0; i < len; i += 2)
    {
        /* 将两个字节拼成一个半字（小端序：低字节在低地址） */
        uint16_t half;
        if (i + 1 < len)
            half = data[i] | (data[i + 1] << 8);
        else
            half = data[i] | (0xFF << 8); /* 奇数长度：高字节补 0xFF */

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, half) != HAL_OK)
            return -1;
    }
    return 0;
}

/**
 * @brief 检查目标地址区域是否需要擦除
 * @param addr 起始地址
 * @param len  检查长度（字节）
 * @return 1=需要擦除（存在非 0xFF）, 0=已擦除（全 0xFF）
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
