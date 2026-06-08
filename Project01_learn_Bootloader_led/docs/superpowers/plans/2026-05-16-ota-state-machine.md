# OTA 状态机改造实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 OTA_Update 阻塞函数改造为主循环驱动的状态机，每次循环执行一步操作。

**Architecture:** 用 OTA_State_t 枚举定义状态，OTA_Context_t 结构体保存运行时上下文，OTA_Process() 函数在主循环中被调用，根据当前状态执行对应操作后转换状态。

**Tech Stack:** STM32 HAL (stm32f1xx_hal.h), ARM Compiler V5.05, Keil MDK-ARM 5

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `Service/ota_update.h` | Modify | 添加状态枚举、上下文结构体、OTA_Process 声明，移除 OTA_Update |
| `Service/ota_update.c` | Modify | 替换阻塞函数为状态机 switch-case 实现 |
| `Core/Src/main.c` | Modify | OTA 分支改为状态机驱动 |

---

### Task 1: 重写 ota_update.h — 添加状态机和上下文定义

**Files:**
- Modify: `Service/ota_update.h`

- [ ] **Step 1: 替换全部内容**

```c
#ifndef __OTA_UPDATE_H__
#define __OTA_UPDATE_H__

#include "bootloader.h"
#include <stdint.h>

/*
 * OTA 固件搬运状态机 —— 从 W25Q16 读取固件写入 A区 内部 Flash
 * 由主循环每次调用 OTA_Process() 驱动一步
 */

/* 状态定义 */
typedef enum {
    OTA_STATE_IDLE,       /* 空闲 */
    OTA_STATE_READ_INFO,  /* 读 EEPROM 校验 */
    OTA_STATE_ERASE,      /* 逐页擦除 A区 Flash */
    OTA_STATE_TRANSFER,   /* 逐段搬运 W25Q16 → Flash */
    OTA_STATE_FINISH,     /* 清除 OTA_flag + 软复位 */
    OTA_STATE_ERROR       /* 出错停留 */
} OTA_State_t;

/* 运行时上下文 */
typedef struct {
    OTA_State_t state;
    uint32_t    fw_size;       /* 固件大小 */
    uint32_t    page_count;    /* 需擦除页数 */
    uint32_t    erase_index;   /* 当前擦除到第几页 */
    uint32_t    offset;        /* 当前搬运偏移量 */
    uint8_t     buf[256];      /* 搬运缓冲区 */
} OTA_Context_t;

/* 接口 */
void OTA_Process(OTA_Context_t *ctx);

#endif
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错（ota_update.c 会报错，预期中）**

---

### Task 2: 重写 ota_update.c — 状态机实现

**Files:**
- Modify: `Service/ota_update.c`

- [ ] **Step 1: 替换全部内容**

```c
#include "ota_update.h"
#include "bootloader_conf.h"
#include "flash.h"
#include "w25q16.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

extern W25Q16_t w25q;

static void OTA_HandleReadInfo(OTA_Context_t *ctx)
{
    OTA_InfoCB info;

    if (Bootloader_ReadOTAInfo(&info) != 0)
    {
        printf("[OTA] Read EEPROM failed\r\n");
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    if (info.OTA_flag != OTA_SET_FLAG)
    {
        printf("[OTA] Flag mismatch\r\n");
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    ctx->fw_size = info.Firelen[0];
    if (ctx->fw_size == 0 || ctx->fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[OTA] Invalid fw_size=%lu\r\n", ctx->fw_size);
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    ctx->page_count = (ctx->fw_size + FLASH__PAGE_SIZE - 1) / FLASH__PAGE_SIZE;
    ctx->erase_index = 0;
    ctx->offset = 0;

    printf("[OTA] fw_size=%lu bytes, pages=%lu\r\n", ctx->fw_size, ctx->page_count);

    Flash_Unlock();
    ctx->state = OTA_STATE_ERASE;
}

static void OTA_HandleErase(OTA_Context_t *ctx)
{
    if (Flash_ErasePage(A_REGION_ADDR + ctx->erase_index * FLASH__PAGE_SIZE) != 0)
    {
        printf("[OTA] Erase page %lu failed\r\n", ctx->erase_index);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    printf("[OTA] Erased page %lu/%lu\r\n", ctx->erase_index + 1, ctx->page_count);
    ctx->erase_index++;

    if (ctx->erase_index >= ctx->page_count)
    {
        printf("[OTA] Erase done, start transfer\r\n");
        ctx->state = OTA_STATE_TRANSFER;
    }
}

static void OTA_HandleTransfer(OTA_Context_t *ctx)
{
    uint16_t len = 256;
    if (ctx->offset + len > ctx->fw_size)
        len = (uint16_t)(ctx->fw_size - ctx->offset);

    if (W25Q16_Read(&w25q, ctx->offset, ctx->buf, len) != 0)
    {
        printf("[OTA] W25Q read failed at offset=%lu\r\n", ctx->offset);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    if (Flash_Write(A_REGION_ADDR + ctx->offset, ctx->buf, len) != 0)
    {
        printf("[OTA] Flash write failed at offset=%lu\r\n", ctx->offset);
        Flash_Lock();
        ctx->state = OTA_STATE_ERROR;
        return;
    }

    ctx->offset += len;
    printf("[OTA] Transfer %lu/%lu bytes\r\n", ctx->offset, ctx->fw_size);

    if (ctx->offset >= ctx->fw_size)
    {
        Flash_Lock();
        ctx->state = OTA_STATE_FINISH;
    }
}

static void OTA_HandleFinish(OTA_Context_t *ctx)
{
    Bootloader_ClearOTAFlag();
    printf("[OTA] Update done, resetting...\r\n");
    HAL_Delay(100);
    NVIC_SystemReset();
}

void OTA_Process(OTA_Context_t *ctx)
{
    switch (ctx->state)
    {
    case OTA_STATE_IDLE:
        break;

    case OTA_STATE_READ_INFO:
        OTA_HandleReadInfo(ctx);
        break;

    case OTA_STATE_ERASE:
        OTA_HandleErase(ctx);
        break;

    case OTA_STATE_TRANSFER:
        OTA_HandleTransfer(ctx);
        break;

    case OTA_STATE_FINISH:
        OTA_HandleFinish(ctx);
        break;

    case OTA_STATE_ERROR:
        printf("[OTA] Error occurred, staying in Bootloader\r\n");
        ctx->state = OTA_STATE_IDLE;
        break;

    default:
        ctx->state = OTA_STATE_IDLE;
        break;
    }
}
```

- [ ] **Step 2: 在 Keil 中编译，确认无报错（main.c 会报错，预期中）**

---

### Task 3: 更新 main.c — 状态机驱动方式

**Files:**
- Modify: `Core/Src/main.c`

- [ ] **Step 1: 修改全局变量区**

将 `main.c` 中的：
```c
OTA_InfoCB ota_info;
```
替换为：
```c
OTA_InfoCB ota_info;
OTA_Context_t ota_ctx;
```

- [ ] **Step 2: 修改 OTA 分支逻辑**

将 OTA 分支中的：
```c
  if (ota_info.OTA_flag == OTA_SET_FLAG)
  {
      printf("[OTA] Flag detected, enter upgrade mode\r\n");
      printf("[OTA] fw_size=%lu\r\n", ota_info.Firelen[0]);
      OTA_Update();
      printf("[OTA] Update failed, staying in Bootloader\r\n");
  }
```
替换为：
```c
  if (ota_info.OTA_flag == OTA_SET_FLAG)
  {
      printf("[OTA] Flag detected, enter upgrade mode\r\n");
      printf("[OTA] fw_size=%lu\r\n", ota_info.Firelen[0]);
      ota_ctx.state = OTA_STATE_READ_INFO;
  }
```

- [ ] **Step 3: 修改主循环**

将 `while (1)` 循环中的：
```c
    /* TODO: 在此编写模块测试代码 */
```
替换为：
```c
    OTA_Process(&ota_ctx);
```

- [ ] **Step 4: 在 Keil 中编译整个工程，确认 0 Error**

---

### Task 4: 编译验证

- [ ] **Step 1: 编译工程，确认 0 Error, 0 Warning**

- [ ] **Step 2: 烧录 Bootloader，通过串口观察启动日志**

预期行为（无 OTA 事件时，行为不变）：
```
[DEBUG] OTA flag cleared
===== Bootloader =====
[APP] Valid app found at 0x08005000, jumping...
```

---

## Self-Review Checklist

- [x] **Spec coverage:** 6 个状态全部有对应处理函数，OTA_Context_t 包含所有运行时变量
- [x] **Placeholder scan:** 无 TBD/TODO
- [x] **Type consistency:** OTA_Context_t、OTA_State_t、OTA_Process 在所有 Task 中名称一致
