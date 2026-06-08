/*
 * Bootloader 核心模块 —— App 有效性校验与跳转
 *
 * App 向量表布局（ARM Cortex-M 约定）：
 *   [+0] 初始 MSP      → 必须指向 SRAM 范围
 *   [+4] Reset_Handler → 必须落在目标 Flash 区域内
 *
 * 跳转前必须归还 Bootloader 占用的所有硬件资源
 */

#include "bootloader.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

typedef void (*pFunction)(void);

/* ---------- 校验 ---------- */

static int validate_msp(uint32_t sp)
{
    return (sp >= RAM_START && sp < RAM_END);
}

int Bootloader_IsAppValid(void)
{
    uint32_t sp    = *(volatile uint32_t *)A_REGION_ADDR;
    uint32_t entry = *(volatile uint32_t *)(A_REGION_ADDR + 4);
    return validate_msp(sp) && entry >= A_REGION_ADDR
        && entry < A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE;
}

/* ---------- 跳转 ---------- */

void Bootloader_JumpToApp(uint32_t addr)
{
    uint32_t sp    = *(volatile uint32_t *)addr;
    uint32_t entry = *(volatile uint32_t *)(addr + 4);

    if (!validate_msp(sp))
    {
        printf("[BL] Invalid MSP 0x%08lX\r\n", sp);
        return;
    }

    uint32_t region_end;
    if (addr == FACTORY_REGION_ADDR)
        region_end = FACTORY_REGION_ADDR + FACTORY_PAGE_NUM * FLASH__PAGE_SIZE;
    else
        region_end = A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE;

    if (entry < addr || entry >= region_end)
    {
        printf("[BL] Invalid entry 0x%08lX\r\n", entry);
        return;
    }

    printf("[BL] Jump to 0x%08lX\r\n", entry);
    HAL_Delay(50);

    /* 停止可能正在运行的 DMA 传输 */
    extern UART_HandleTypeDef huart1;
    HAL_UART_DMAStop(&huart1);

    HAL_DeInit();

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* 清除所有 NVIC 中断使能和挂起标志 */
    for (uint32_t i = 0; i < 2; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    SCB->VTOR = addr;

    __set_MSP(sp);
    ((pFunction)entry)();
}
