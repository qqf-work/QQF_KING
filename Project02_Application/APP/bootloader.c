/*
 * Bootloader 核心模块 —— App 有效性校验与跳转
 *
 * App 向量表布局（ARM Cortex-M 约定）：
 *   [+0] 初始 MSP      → 必须指向 SRAM 范围
 *   [+4] Reset_Handler → 必须落在 App Flash 区域内
 *
 * 跳转前必须归还 Bootloader 占用的所有硬件资源
 */

#include "bootloader.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

typedef void (*pFunction)(void);

/* A 区 Flash 结束地址 */
#define A_REGION_END  (A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE)

/* ---------- 校验 ---------- */

static int validate_msp(uint32_t sp)
{
    return (sp >= RAM_START && sp < RAM_END);
}

static int validate_entry(uint32_t entry)
{
    return (entry >= A_REGION_ADDR && entry < A_REGION_END);
}

int Bootloader_IsAppValid(void)
{
    uint32_t sp    = *(volatile uint32_t *)A_REGION_ADDR;
    uint32_t entry = *(volatile uint32_t *)(A_REGION_ADDR + 4);
    return validate_msp(sp) && validate_entry(entry);
}

/* ---------- 跳转 ---------- */

static void deinit_bootloader(void)
{
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    HAL_DeInit();
    SCB->VTOR = A_REGION_ADDR;
}

int Bootloader_JumpToApp(void)
{
    uint32_t sp    = *(volatile uint32_t *)A_REGION_ADDR;
    uint32_t entry = *(volatile uint32_t *)(A_REGION_ADDR + 4);

    if (!validate_msp(sp))
    {
        printf("[BL] Invalid MSP 0x%08lX\r\n", sp);
        return -1;
    }
    if (!validate_entry(entry))
    {
        printf("[BL] Invalid entry 0x%08lX\r\n", entry);
        return -1;
    }

    printf("[BL] Jump to 0x%08lX\r\n", entry);
    HAL_Delay(50);

    deinit_bootloader();

    __set_MSP(sp);
    ((pFunction)entry)();

    return 0;
}
