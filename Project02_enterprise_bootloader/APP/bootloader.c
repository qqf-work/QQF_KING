/*
 * Bootloader 核心模块 —— App 有效性校验与跳转
 *
 * ARM Cortex-M 向量表布局（Flash 起始处）：
 *   [+0x00] 初始栈指针 MSP  → 必须指向 SRAM 范围（RAM_START ~ RAM_END）
 *   [+0x04] ResetHandler    → 必须落在目标 Flash 区域内
 *   [+0x08] NMIHandler
 *   [+0x0C] HardFaultHandler
 *   ...
 *
 * 跳转前的清理工作（归还 Bootloader 占用的硬件资源）：
 *   1. 关闭全局中断（__disable_irq）—— 防止跳转过程中被中断打断
 *   2. 停止 SysTick（CTRL=0, LOAD=0, VAL=0）—— HAL 的时基，App 会重新初始化
 *   3. HAL_DeInit() —— 反初始化所有 HAL 外设，恢复寄存器默认值
 *   4. NVIC 全量清理 —— 禁用所有中断通道 + 清除所有挂起标志
 *   5. 重设 VTOR —— 告诉 CPU 新的向量表位置
 *   6. 重设 MSP —— 切换到 App 的栈
 *   7. 跳转到 ResetHandler —— 开始执行 App
 */

#include "bootloader.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

/* 函数指针类型，用于跳转到 App 的 ResetHandler */
typedef void (*pFunction)(void);

/* ---------- 校验 ---------- */

/**
 * @brief 校验栈指针 MSP 是否在 SRAM 范围内
 * @param sp  待校验的栈指针值
 * @return 1 有效, 0 无效
 *
 * STM32F103C8 SRAM 范围：0x20000000 ~ 0x20004FFF（20KB）
 * 合法的 MSP 必须指向这个范围，否则说明向量表数据无效（如 Flash 被擦除后为 0xFFFFFFFF）
 */
static int validate_msp(uint32_t sp)
{
    return (sp >= RAM_START && sp < RAM_END);
}

/**
 * @brief 检查 A 区 App 是否有效
 * @return 1 有效, 0 无效
 *
 * 校验两个条件：
 *   1. MSP（向量表 [0]）在 RAM 范围内
 *   2. ResetHandler（向量表 [4]）在 A 区 Flash 范围内
 *
 * 当 A 区 Flash 全为 0xFF（未烧录）时：
 *   MSP = 0xFFFFFFFF（不在 RAM 范围）→ 校验失败
 */
int Bootloader_IsAppValid(void)
{
    uint32_t sp    = *(volatile uint32_t *)A_REGION_ADDR;
    uint32_t entry = *(volatile uint32_t *)(A_REGION_ADDR + 4);
    return validate_msp(sp) && entry >= A_REGION_ADDR
        && entry < A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE;
}

/* ---------- 跳转 ---------- */

/**
 * @brief 跳转到指定 Flash 区域的程序（出厂区或 A 区）
 * @param addr  目标区域的起始地址（FACTORY_REGION_ADDR 或 A_REGION_ADDR）
 *
 * 统一的跳转函数，既可用于跳转出厂程序也可用于跳转 A 区 App：
 *   1. 从目标地址读取向量表获取 MSP 和 ResetHandler
 *   2. 校验 MSP 在 RAM 范围内
 *   3. 根据 addr 判断目标区域，校验 ResetHandler 在区域内
 *   4. 清理 Bootloader 资源（中断、SysTick、HAL 外设、NVIC 残留）
 *   5. 设置 VTOR 指向新向量表
 *   6. 切换 MSP 并跳转
 *
 * 成功不返回（直接执行目标程序），失败则 return
 */
void Bootloader_JumpToApp(uint32_t addr)
{
    /* 从目标地址的向量表读取 MSP 和 ResetHandler */
    uint32_t sp    = *(volatile uint32_t *)addr;
    uint32_t entry = *(volatile uint32_t *)(addr + 4);

    /* 校验 MSP：必须在 SRAM 范围内 */
    if (!validate_msp(sp))
    {
        printf("[BL] Invalid MSP 0x%08lX\r\n", sp);
        return;
    }

    /* 根据 addr 确定区域边界，校验 ResetHandler 在区域内 */
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
    HAL_Delay(50);  /* 等待串口数据发送完毕 */

    /* ---- 以下为不可逆操作：清理 Bootloader 并跳转 ---- */

    __disable_irq();    /* 关闭全局中断 */
    SysTick->CTRL = 0;  /* 停止 SysTick 计数器 */
    SysTick->LOAD = 0;  /* 清除重载值 */
    SysTick->VAL  = 0;  /* 清除当前值 */
    HAL_DeInit();       /* 反初始化所有 HAL 外设 */

    /* NVIC 全量清理：禁用所有中断通道 + 清除所有挂起标志
     * 防止 Bootloader 残留的中断使能位在 App 开中断后误触发 */
    for (uint32_t i = 0; i < 2; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;  /* 禁用所有中断通道 */
        NVIC->ICPR[i] = 0xFFFFFFFF;  /* 清除所有挂起标志 */
    }

    SCB->VTOR = addr;   /* 重定向向量表到目标区域 */

    __set_MSP(sp);              /* 切换到 App 的栈指针 */
    ((pFunction)entry)();       /* 跳转到 ResetHandler */
}
