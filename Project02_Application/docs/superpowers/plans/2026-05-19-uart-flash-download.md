# UART Flash Download Demo 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 移除 OTA 代码，实现串口接收 bin 文件直接写入 STM32 内部 Flash A 区

**Architecture:** 在现有 `uart_buf` DMA 帧队列基础上，新建 `flash_download` 模块封装智能擦除和跨帧奇数字节写入逻辑，参照 Project02 的 `Init_bootloader.c`

**Tech Stack:** STM32F103C8, HAL 库, Keil MDK-ARM 5 (ARM Compiler V5.05)

---

## 文件变更

| 文件 | 操作 | 职责 |
|------|------|------|
| `Driver/MCU/flash.h` | 修改 | 新增 `Flash_NeedsErase()` 声明 |
| `Driver/MCU/flash.c` | 修改 | 新增 `Flash_NeedsErase()` 实现 |
| `Service/flash_download.h` | 新建 | 下载模块接口 |
| `Service/flash_download.c` | 新建 | 智能擦除 + 跨帧奇数字节写入 |
| `Core/Src/main.c` | 修改 | 精简为 init + 帧队列轮询 |

---

### Task 1: flash.c 新增 Flash_NeedsErase()

**Files:**
- Modify: `Driver/MCU/flash.h` — 末尾 `#endif` 前新增声明
- Modify: `Driver/MCU/flash.c` — `Flash_Write()` 后新增实现

- [ ] **Step 1: 在 flash.h 新增函数声明**

在 `Driver/MCU/flash.h` 的 `Flash_Write` 声明后、`#endif` 前添加：

```c
/* 检查 [addr, addr+len) 是否全为 0xFF（已擦除），返回 1=需要擦除, 0=已擦除 */
int  Flash_NeedsErase(uint32_t addr, uint16_t len);
```

- [ ] **Step 2: 在 flash.c 新增函数实现**

在 `Driver/MCU/flash.c` 的 `Flash_Write()` 函数后添加：

```c
/**
 * @brief 检查目标地址区域是否需要擦除
 * @param addr 起始地址
 * @param len  检查长度（字节）
 * @return 1=需要擦除（存在非 0xFF）, 0=已擦除（全 0xFF）
 *
 * 遍历目标地址逐字节读，发现非 0xFF 即判定需要擦除
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
```

- [ ] **Step 3: 在 Keil 中编译验证**

打开 Keil → F7 编译，预期：0 Error, 0 Warning。

---

### Task 2: 新建 flash_download.h

**Files:**
- Create: `Service/flash_download.h`

- [ ] **Step 1: 创建头文件**

```c
#ifndef __FLASH_DOWNLOAD_H__
#define __FLASH_DOWNLOAD_H__

#include <stdint.h>

/*
 * UART 串口 Flash 下载模块
 *
 * 配合 uart_buf 帧队列使用：
 *   主循环中取到一帧数据 → 调用 FlashDownload_WriteFrame()
 *   模块内部处理：智能擦页 + 跨帧奇数字节缓冲 + 半字写入
 */

typedef struct {
    uint32_t write_addr;      /* 当前写入地址（A 区内递增） */
    uint32_t total_written;   /* 已写入总字节 */
    uint8_t  last_byte_flag;  /* 跨帧奇数字节标记 */
    uint8_t  last_byte;       /* 缓存的奇数字节 */
} FlashDownload_t;

/* 初始化下载上下文，写入地址设为 A 区起始 */
void     FlashDownload_Init(FlashDownload_t *ctx);

/*
 * 处理一帧 UART 数据，写入 Flash
 * 流程：智能擦页 → 跨帧奇数字节拼接 → 半字写入
 * 返回 0 成功, -1 失败
 */
int      FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len);

/* 返回已写入总字节数 */
uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx);

#endif
```

- [ ] **Step 2: 在 Keil 工程中添加 flash_download.h 到 include 路径**

Keil → Project → Manage → Add Files → 选择 `Service/flash_download.h` 所属组（与 ota_update 同组）。

---

### Task 3: 新建 flash_download.c

**Files:**
- Create: `Service/flash_download.c`

- [ ] **Step 1: 创建实现文件**

```c
#include "flash_download.h"
#include "flash.h"
#include "bootloader_conf.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

/* ---------- 擦除辅助 ---------- */

/**
 * @brief 智能擦除：仅在目标页未擦除时才擦除
 * @param addr 要写入的起始地址
 * @param len  数据长度
 *
 * 计算本帧数据覆盖的页范围，逐页检查是否已擦除
 * 只要页内任一字节非 0xFF，就擦除该页
 */
static void smart_erase(uint32_t addr, uint16_t len)
{
    uint32_t end = addr + len;
    uint32_t page_start = addr - (addr % FLASH__PAGE_SIZE);
    uint32_t page_end   = end - (end % FLASH__PAGE_SIZE) +
                          ((end % FLASH__PAGE_SIZE) ? FLASH__PAGE_SIZE : 0);

    for (uint32_t page = page_start; page < page_end; page += FLASH__PAGE_SIZE)
    {
        if (Flash_NeedsErase(page, FLASH__PAGE_SIZE))
        {
            Flash_ErasePage(page);
            printf("[DL] Erased page at 0x%08lX\r\n", page);
        }
    }
}

/* ---------- 跨帧奇数字节写入 ---------- */

/**
 * @brief 带跨帧奇数字节处理的半字写入
 *
 * 四种情况（与 Project02 Init_flash_write_halfworf 一致）：
 *   1. 无 last_byte 且 len 为偶数 → 直接写入
 *   2. 无 last_byte 且 len 为奇数 → 写前 len-1 字节，缓存末字节
 *   3. 有 last_byte 且 (1+len) 为偶数 → last_byte 拼首字节 + 写剩余
 *   4. 有 last_byte 且 (1+len) 为奇数 → last_byte 拼首字节 + 写前 n-1 + 缓存末字节
 */
static int write_with_last_byte(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    uint32_t addr = ctx->write_addr;
    uint16_t pos = 0;

    if (ctx->last_byte_flag)
    {
        /* 拼接上一帧缓存的奇数字节和本帧第一个字节 */
        uint16_t half = ctx->last_byte | (data[0] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
            return -1;
        addr += 2;
        pos = 1;
        ctx->total_written += 1;  /* 加上上一帧缓存的 1 字节 */
        ctx->last_byte_flag = 0;
    }

    /* 计算剩余可写字节数（必须是偶数） */
    uint16_t remaining = len - pos;
    uint16_t write_count = remaining & ~1;  /* 向下取偶 */

    /* 逐半字写入 */
    for (uint16_t i = 0; i < write_count; i += 2)
    {
        uint16_t half = data[pos + i] | (data[pos + i + 1] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, half) != HAL_OK)
            return -1;
        addr += 2;
    }

    ctx->total_written += write_count;
    ctx->write_addr = addr;

    /* 处理剩余奇数字节 */
    if (remaining & 1)
    {
        ctx->last_byte = data[pos + write_count];
        ctx->last_byte_flag = 1;
    }

    return 0;
}

/* ---------- 公开 API ---------- */

void FlashDownload_Init(FlashDownload_t *ctx)
{
    ctx->write_addr = A_REGION_ADDR;
    ctx->total_written = 0;
    ctx->last_byte_flag = 0;
    ctx->last_byte = 0;
}

int FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len)
{
    if (len == 0)
        return 0;

    /* 溢出保护 */
    if (ctx->write_addr + len > A_REGION_ADDR + A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[DL] Error: write overflow\r\n");
        return -1;
    }

    Flash_Unlock();

    /* 智能擦除 */
    smart_erase(ctx->write_addr, len);

    /* 写入（含跨帧奇数字节处理） */
    int ret = write_with_last_byte(ctx, data, len);

    Flash_Lock();

    if (ret != 0)
    {
        printf("[DL] Error: write failed at 0x%08lX\r\n", ctx->write_addr);
        return -1;
    }

    printf("[DL] Written %lu bytes\r\n", ctx->total_written);
    return 0;
}

uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx)
{
    return ctx->total_written;
}
```

- [ ] **Step 2: 在 Keil 工程中添加 flash_download.c**

Keil → Project → Manage → Add Files → Service 组 → 选择 `Service/flash_download.c`。

- [ ] **Step 3: 编译验证**

F7 编译，预期：0 Error, 0 Warning。

---

### Task 4: 精简 main.c

**Files:**
- Modify: `Core/Src/main.c`

- [ ] **Step 1: 替换 Includes 区域**

将 `USER CODE BEGIN Includes` 到 `USER CODE END Includes` 替换为：

```c
/* USER CODE BEGIN Includes */
#include "uart_buf.h"
#include "flash_download.h"
#include "bootloader_conf.h"
#include <stdio.h>
/* USER CODE END Includes */
```

- [ ] **Step 2: 替换 Variables 区域**

将 `USER CODE BEGIN PV` 到 `USER CODE END PV` 替换为：

```c
/* USER CODE BEGIN PV */
static FlashDownload_t dl_ctx;
/* USER CODE END PV */
```

- [ ] **Step 3: 替换 Init 区域（USER CODE BEGIN 2）**

将 `USER CODE BEGIN 2` 到 `USER CODE END 2` 替换为：

```c
  /* USER CODE BEGIN 2 */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
  UART_DMA_Rx_Init();
  FlashDownload_Init(&dl_ctx);

  printf("\r\n===== Flash Download Demo =====\r\n");
  printf("A region: 0x%08X (%lu KB)\r\n", A_REGION_ADDR,
         (uint32_t)(A_PAGE_NUM * FLASH__PAGE_SIZE / 1024));
  printf("Send bin file via UART...\r\n");
  printf("===============================\r\n\r\n");
  /* USER CODE END 2 */
```

- [ ] **Step 4: 替换主循环（USER CODE BEGIN 3）**

将 `USER CODE BEGIN 3` 的空循环体替换为：

```c
    /* USER CODE BEGIN 3 */
    if (uart_rx_queue.URxDataOUT != uart_rx_queue.URxDataIN)
    {
        uint16_t len = uart_rx_queue.URxDataOUT->end
                      - uart_rx_queue.URxDataOUT->start + 1;
        FlashDownload_WriteFrame(&dl_ctx,
                                 uart_rx_queue.URxDataOUT->start, len);

        uart_rx_queue.URxDataOUT++;
        if (uart_rx_queue.URxDataOUT > uart_rx_queue.URxDataEND)
            uart_rx_queue.URxDataOUT = &uart_rx_queue.URxDataPtr[0];
    }
```

- [ ] **Step 5: 编译验证**

F7 编译，预期：0 Error, 0 Warning。

---

### Task 5: 烧录测试

**硬件需求：** STM32F103C8 开发板 + ST-Link + 串口助手

- [ ] **Step 1: 烧录 Bootloader**

Keil → Debug → Download（Erase Sectors 模式）。

- [ ] **Step 2: 验证串口输出**

打开串口助手（115200, 8N1），预期收到：
```
===== Flash Download Demo =====
A region: 0x08005000 (44 KB)
Send bin file via UART...
===============================
```

- [ ] **Step 3: 发送 bin 文件测试**

串口助手选择一个 .bin 文件发送（如简单 LED 闪烁 App 的 bin），预期：
```
[DL] Erased page at 0x08005000
[DL] Written 256 bytes
[DL] Written 512 bytes
...
[DL] Written NNNN bytes
```

- [ ] **Step 4: 验证写入数据**

读回 A 区前几个字节，确认与 bin 文件内容一致。
