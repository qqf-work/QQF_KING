# OTA 固件更新实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 Bootloader 检测到 OTA 事件后，从 W25Q16 读取固件写入 A区 内部 Flash 的完整流程。

**Architecture:** 分层设计 — Driver/MCU/flash 封装内部 Flash 操作，Service/ota_update 实现搬运逻辑，APP/bootloader 定义数据结构。换芯片只改 Driver/MCU/flash 实现和 bootloader_conf.h 参数。

**Tech Stack:** STM32 HAL (stm32f1xx_hal.h), ARM Compiler V5.05, Keil MDK-ARM 5

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `APP/bootloader.h` | Modify | OTA_InfoCB 结构体替换 OTA_Info_t |
| `APP/bootloader.c` | Modify | 适配新结构体名和字段 |
| `APP/bootloader_conf.h` | Modify | 添加 MAX_FW_SLOTS 宏 |
| `Driver/MCU/flash.h` | Create | 内部 Flash 抽象接口 |
| `Driver/MCU/flash.c` | Create | STM32F103 Flash 操作实现 |
| `Service/ota_update.h` | Create | OTA_Update 接口声明 |
| `Service/ota_update.c` | Create | 搬运逻辑实现 |
| `Core/Src/main.c` | Modify | OTA 分支调用 OTA_Update() |

---

### Task 1: 更新 bootloader_conf.h — 添加固件槽数量宏

**Files:**
- Modify: `APP/bootloader_conf.h:25` (在 `#endif` 前插入)

- [ ] **Step 1: 添加 MAX_FW_SLOTS 定义**

在 `APP/bootloader_conf.h` 的 `#endif` 前添加：

```c
/* OTA 固件槽数量（外部 Flash 中最多存几个固件） */
#define MAX_FW_SLOTS         11
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错**

---

### Task 2: 更新 bootloader.h — 替换 OTA_Info_t 为 OTA_InfoCB

**Files:**
- Modify: `APP/bootloader.h`

- [ ] **Step 1: 替换结构体和相关宏**

将 `APP/bootloader.h` 全部内容替换为：

```c
#ifndef __BOOTLOADER_H__
#define __BOOTLOADER_H__

#include "bootloader_conf.h"
#include <stdint.h>

/* ---------- EEPROM 中的 OTA 信息 ---------- */

#define OTA_SET_FLAG       0xA5A5A5A5      /* OTA 标志位魔数值 */
#define OTA_EEPROM_ADDR    0x00            /* OTA 信息在 EEPROM 中的起始地址 */

typedef struct __attribute__((packed)) {
    uint32_t OTA_flag;                     /* OTA 标志：== OTA_SET_FLAG 则进入升级 */
    uint32_t Firelen[MAX_FW_SLOTS];        /* Firelen[0]=OTA固件大小, [1-10]=其他程序大小 */
} OTA_InfoCB;

#define OTA_INFOCB_SIZE    sizeof(OTA_InfoCB)

extern OTA_InfoCB ota_info;
typedef void (*pFunction)(void);

/* ---------- 接口 ---------- */

/* EEPROM OTA 信息读写 */
int  Bootloader_ReadOTAInfo(OTA_InfoCB *info);
int  Bootloader_WriteOTAInfo(const OTA_InfoCB *info);
void Bootloader_ClearOTAFlag(void);

/* 检查 A区是否存在有效 App（通过栈指针合法性判断） */
int  Bootloader_IsAppValid(void);

/* 关闭全局中断，设置 MSP，跳转到 A区 App 的 Reset_Handler */
void Bootloader_JumpToApp(void);

#endif
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错（此时 bootloader.c 会报错，预期中）**

---

### Task 3: 更新 bootloader.c — 适配新结构体

**Files:**
- Modify: `APP/bootloader.c`

- [ ] **Step 1: 替换所有 OTA_Info_t 为 OTA_InfoCB，OTA_INFO_SIZE 为 OTA_INFOCB_SIZE**

将 `APP/bootloader.c` 全部内容替换为：

```c
#include "bootloader.h"
#include "at24c02.h"
#include "usart.h"
#include "dma.h"
#include "bsp_soft_i2c.h"
#include "bsp_soft_spi.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

extern AT24C02_t eeprom;
extern UART_HandleTypeDef huart1;
extern DMA_HandleTypeDef hdma_usart1_rx;

/* ---------- EEPROM OTA 信息读写 ---------- */

int Bootloader_ReadOTAInfo(OTA_InfoCB *info)
{
    memset(info, 0, OTA_INFOCB_SIZE);
    return AT24C02_Read(&eeprom, OTA_EEPROM_ADDR,
                        (uint8_t *)info, OTA_INFOCB_SIZE);
}

int Bootloader_WriteOTAInfo(const OTA_InfoCB *info)
{
    return AT24C02_Write(&eeprom, OTA_EEPROM_ADDR,
                         (uint8_t *)info, OTA_INFOCB_SIZE);
}

void Bootloader_ClearOTAFlag(void)
{
    OTA_InfoCB info = {0};
    Bootloader_WriteOTAInfo(&info);
}

/* ---------- App 有效性校验 ---------- */

int Bootloader_IsAppValid(void)
{
    uint32_t sp = *(volatile uint32_t *)A_REGION_ADDR;
    return (sp >= RAM_START && sp < RAM_END) ? 1 : 0;
}

/* ---------- 跳转前清理外设 ---------- */

static void Bootloader_DeInit(void)
{
    HAL_UART_DeInit(&huart1);
    HAL_DMA_DeInit(&hdma_usart1_rx);

    /* UART TX/RX */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
    /* Soft I2C */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8 | GPIO_PIN_9);
    /* Soft SPI + W25Q CS */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);

    HAL_DeInit();
}

/* ---------- 跳转到 A区 App ---------- */

void Bootloader_JumpToApp(void)
{
    uint32_t app_sp    = *(volatile uint32_t *)A_REGION_ADDR;
    uint32_t app_entry = *(volatile uint32_t *)(A_REGION_ADDR + 4);

    __disable_irq();

    /* 关闭 SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* 清理外设 */
    Bootloader_DeInit();

    /* 设置栈指针 */
    __set_MSP(app_sp);

    /* 跳转 */
    pFunction jump = (pFunction)app_entry;
    jump();
}
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错（此时 main.c 会报错，预期中）**

---

### Task 4: 更新 main.c — 适配新结构体字段名

**Files:**
- Modify: `Core/Src/main.c`

- [ ] **Step 1: 修改全局变量声明和 OTA 分支逻辑**

`main.c` 需要改 3 处：

**1) 第 56 行** — 全局变量类型改为 `OTA_InfoCB`：
```c
OTA_InfoCB ota_info;
```

**2) 第 119 行** — OTA 标志判断改为 `OTA_SET_FLAG`：
```c
if (ota_info.OTA_flag == OTA_SET_FLAG)
```

**3) 第 122 行** — 打印改为 `Firelen[0]`：
```c
printf("[OTA] fw_size=%lu\r\n", ota_info.Firelen[0]);
```

**4) 第 123 行** — 添加 OTA_Update 调用（头文件和实现后续任务添加，此处先预留调用位置）：
```c
/* TODO: 调用 OTA_Update() — Task 7 接入 */
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错**

---

### Task 5: 创建 Driver/MCU/flash.h — 内部 Flash 抽象接口

**Files:**
- Create: `Driver/MCU/flash.h`

- [ ] **Step 1: 创建目录和头文件**

```c
#ifndef __FLASH_H__
#define __FLASH_H__

#include <stdint.h>

/*
 * 内部 Flash 操作抽象 —— 换芯片只改 flash.c 实现
 * 当前实现：STM32F103C8 (页大小 1KB)
 */

int  Flash_Unlock(void);
int  Flash_Lock(void);
int  Flash_ErasePage(uint32_t addr);
int  Flash_Write(uint32_t addr, uint8_t *data, uint16_t len);

#endif
```

- [ ] **Step 2: 在 Keil 工程中添加 Driver/MCU 分组，将 flash.c 加入（Task 6 完成后一起编译）**

---

### Task 6: 创建 Driver/MCU/flash.c — STM32F103 Flash 操作实现

**Files:**
- Create: `Driver/MCU/flash.c`

- [ ] **Step 1: 创建实现文件**

```c
#include "flash.h"
#include "bootloader_conf.h"
#include "stm32f1xx_hal.h"

int Flash_Unlock(void)
{
    return HAL_FLASH_Unlock();
}

int Flash_Lock(void)
{
    return HAL_FLASH_Lock();
}

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

int Flash_Write(uint32_t addr, uint8_t *data, uint16_t len)
{
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
```

注意事项：
- STM32F103 Flash 编程以半字（2 字节）为单位
- 奇数长度时最后一字节补 0xFF
- 擦除返回值检查 `err == 0xFFFFFFFF` 表示无错误

- [ ] **Step 2: 在 Keil 中添加 flash.c 到工程 Driver/MCU 分组，编译确认无报错**

---

### Task 7: 创建 Service/ota_update.h — OTA 搬运接口

**Files:**
- Create: `Service/ota_update.h`

- [ ] **Step 1: 创建头文件**

```c
#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

/*
 * OTA 固件搬运服务 —— 从 W25Q16 读取固件写入 A区 内部 Flash
 * 由 Bootloader 主流程在检测到 OTA 事件时调用
 */

int OTA_Update(void);   /* 返回 0=成功, 负数=失败 */

#endif
```

---

### Task 8: 创建 Service/ota_update.c — 搬运逻辑实现

**Files:**
- Create: `Service/ota_update.c`

- [ ] **Step 1: 创建实现文件**

```c
#include "ota_update.h"
#include "bootloader.h"
#include "bootloader_conf.h"
#include "flash.h"
#include "w25q16.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

extern W25Q16_t w25q;
extern AT24C02_t eeprom;

#define TRANSFER_BUF_SIZE  256

int OTA_Update(void)
{
    OTA_InfoCB info;
    uint8_t    buf[TRANSFER_BUF_SIZE];
    uint32_t   fw_size;
    uint32_t   page_count;
    uint32_t   offset;

    /* 1. 读 EEPROM 获取 OTA 信息 */
    if (Bootloader_ReadOTAInfo(&info) != 0)
    {
        printf("[OTA] Read EEPROM failed\r\n");
        return -1;
    }

    /* 2. 校验 OTA 标志 */
    if (info.OTA_flag != OTA_SET_FLAG)
    {
        printf("[OTA] Flag mismatch\r\n");
        return -2;
    }

    fw_size = info.Firelen[0];
    if (fw_size == 0 || fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[OTA] Invalid fw_size=%lu\r\n", fw_size);
        return -3;
    }
    printf("[OTA] fw_size=%lu bytes\r\n", fw_size);

    /* 3. 计算擦除页数 */
    page_count = (fw_size + FLASH__PAGE_SIZE - 1) / FLASH__PAGE_SIZE;

    /* 4. 解锁 Flash */
    Flash_Unlock();

    /* 5. 逐页擦除 A区 */
    printf("[OTA] Erasing %lu pages...\r\n", page_count);
    for (uint32_t i = 0; i < page_count; i++)
    {
        if (Flash_ErasePage(A_REGION_ADDR + i * FLASH__PAGE_SIZE) != 0)
        {
            printf("[OTA] Erase page %lu failed\r\n", i);
            Flash_Lock();
            return -4;
        }
    }

    /* 6. 分段搬运：W25Q16 → 内部 Flash */
    printf("[OTA] Transferring...\r\n");
    offset = 0;
    while (offset < fw_size)
    {
        uint16_t len = TRANSFER_BUF_SIZE;
        if (offset + len > fw_size)
            len = (uint16_t)(fw_size - offset);

        /* 从 W25Q16 地址 0 开始读 */
        if (W25Q16_Read(&w25q, offset, buf, len) != 0)
        {
            printf("[OTA] W25Q read failed at offset=%lu\r\n", offset);
            Flash_Lock();
            return -5;
        }

        /* 写入 A区 Flash */
        if (Flash_Write(A_REGION_ADDR + offset, buf, len) != 0)
        {
            printf("[OTA] Flash write failed at offset=%lu\r\n", offset);
            Flash_Lock();
            return -6;
        }

        offset += len;
    }

    /* 7. 锁定 Flash */
    Flash_Lock();

    /* 8. 清除 OTA_flag */
    Bootloader_ClearOTAFlag();
    printf("[OTA] Update done, resetting...\r\n");

    /* 9. 软复位 */
    HAL_Delay(100);
    NVIC_SystemReset();

    return 0;  /* 不会执行到这里 */
}
```

- [ ] **Step 2: 在 Keil 中添加 ota_update.c 到工程 Service 分组，编译确认无报错**

---

### Task 9: 接入 main.c — OTA 分支调用 OTA_Update()

**Files:**
- Modify: `Core/Src/main.c`

- [ ] **Step 1: 添加头文件引用**

在 `main.c` 第 32 行 `#include "bootloader.h"` 后添加：
```c
#include "ota_update.h"
```

- [ ] **Step 2: 替换 OTA 分支内的 TODO 为实际调用**

将 `main.c` 中 OTA 分支的 `/* TODO: 调用 OTA_Update() */` 替换为：
```c
OTA_Update();
/* 如果 OTA_Update 返回（失败），继续留在 Bootloader */
printf("[OTA] Update failed, staying in Bootloader\r\n");
```

- [ ] **Step 3: 在 Keil 中编译整个工程，确认无报错**

---

### Task 10: 编译验证 + 烧录测试

**Files:** 无修改

- [ ] **Step 1: 编译 Bootloader 工程，确认 0 Error, 0 Warning**

- [ ] **Step 2: 确认代码体积 < 20KB（B区限制）**

- [ ] **Step 3: 烧录 Bootloader，通过串口观察启动日志**

预期行为（无 OTA 事件时）：
```
[DEBUG] OTA flag cleared
===== Bootloader =====
[APP] Valid app found at 0x08005000, jumping...
```

与之前行为一致，OTA 分支尚未被触发。

---

## Self-Review Checklist

- [x] **Spec coverage:** 每个设计需求都有对应 Task (结构体 Task 1-4, Flash 驱动 Task 5-6, OTA 搬运 Task 7-8, 接入 Task 9, 验证 Task 10)
- [x] **Placeholder scan:** 无 TBD/TODO（main.c 中的 TODO 在 Task 9 被替换）
- [x] **Type consistency:** OTA_InfoCB、OTA_SET_FLAG、OTA_INFOCB_SIZE 在所有 Task 中名称一致
