# LoRa Firmware Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace CAN communication with E32-433T20D LoRa (USART3) in two new STM32 projects for wireless firmware updates.

**Architecture:** Create `lora_buf.c/h` transport layer (USART3 DMA+IDLE) and `lora_proto.h` protocol definitions, then port the existing CAN OTA state machines to use LoRa with 50-byte DATA payloads instead of 5-byte.

**Tech Stack:** STM32F103C8 HAL, Keil MDK-ARM 5 (ARM Compiler V5), CubeMX, E32-433T20D LoRa module (transparent mode)

**Design Spec:** `docs/superpowers/specs/2026-06-10-lora-firmware-update-design.md`

---

## File Map

### Shared (identical in both projects)

| File | Responsibility |
|------|---------------|
| `Protocol/LoRa/lora_proto.h` | Frame format constants, command codes, error codes |
| `Protocol/LoRa/lora_buf.h` | `LORA_Buf_t` struct, `Init/Send/Recv/IdleHandler` API |
| `Protocol/LoRa/lora_buf.c` | USART3 DMA Normal + IDLE receive, polling TX, frame build/parse |

### Application LoRa project (`Project03_Application_LoRa/`)

| File | Action | Responsibility |
|------|--------|---------------|
| `APP/app_ota_update.h` | Adapt | Replace `CAN_Buf_t` → `LORA_Buf_t`, update timeouts |
| `APP/app_ota_update.c` | Adapt | Replace CAN calls with LoRa, adjust DATA payload to 50B |
| `Core/Src/main.c` | Adapt | Replace CAN init with LoRa init, update init sequence |
| `Core/Src/stm32f1xx_it.c` | Adapt | Add USART3 IDLE handler in USER CODE block |
| `APP/bootloader_conf.h` | Copy | Unchanged |
| `Service/ota_storage.c/h` | Copy | Unchanged |
| `Driver/MCU/crc32.c/h` | Copy | Unchanged |
| `Driver/Storage/w25q16.c/h` | Copy | Unchanged |
| `Driver/Storage/at24c02.c/h` | Copy | Unchanged |
| `Protocol/SPI/soft_spi.c/h` | Copy | Unchanged |
| `Protocol/I2C/soft_i2c.c/h` | Copy | Unchanged |
| `BSP/bsp_soft_spi.c/h` | Copy | Unchanged |
| `BSP/bsp_soft_i2c.c/h` | Copy | Unchanged |
| `Core/` | CubeMX | New .ioc: USART1 + USART3(DMA) + CRC + GPIO, no CAN |
| `Drivers/` | Copy | Unchanged |

### Gateway LoRa project (`Project03_Gateway_LoRa/`)

| File | Action | Responsibility |
|------|--------|---------------|
| `APP/app_update.h` | Adapt | Replace `CAN_Buf_t` → `LORA_Buf_t` |
| `APP/app_update.c` | Adapt | Replace CAN calls with LoRa, 50B DATA chunks, 50ms interval |
| `APP/fw_cache_conf.h` | Copy | Unchanged |
| `APP/app_bootloader.c/h` | Copy | Unchanged (kept for optional UART download) |
| `Core/Src/main.c` | Adapt | Replace CAN init with LoRa init |
| `Core/Src/stm32f1xx_it.c` | Adapt | Add USART3 IDLE handler in USER CODE block |
| `Protocol/UART/uart_buf.c/h` | Copy | Unchanged (PC serial download) |
| `Service/flash_download.c/h` | Copy | Unchanged |
| `Driver/MCU/flash.c/h` | Copy | Unchanged |
| `Driver/MCU/crc32.c/h` | Copy | Unchanged |
| `Core/` | CubeMX | New .ioc: USART1 + USART3(DMA) + CRC + GPIO, no CAN |
| `Drivers/` | Copy | Unchanged |

---

## Task 1: Create `lora_proto.h`

**Files:**
- Create: `Project03_Application_LoRa/Protocol/LoRa/lora_proto.h`
- Create: `Project03_Gateway_LoRa/Protocol/LoRa/lora_proto.h`

Identical file copied to both projects.

- [ ] **Step 1: Write `lora_proto.h`**

```c
#ifndef __LORA_PROTO_H__
#define __LORA_PROTO_H__

#include <stdint.h>

/*
 * LoRa OTA 更新协议定义
 *
 * 替代 can_proto.h，用于 E32-433T20D 透明传输模式。
 * 帧格式：[0xAA HEADER] [CMD 1B] [LEN 1B] [PAYLOAD 0~55B]
 * 总帧最大 58 字节 = LoRa 单包限制
 */

/* 帧头 */
#define LORA_FRAME_HEADER       0xAA

/* 最大载荷（58 - 3 字节帧头） */
#define LORA_MAX_PAYLOAD        55

/* DATA 帧中固件数据的最大字节数（帧头3B + SEQ 2B + DATA 50B = 55B <= 58B） */
#define LORA_MAX_DATA_PER_FRAME 50

/* DATA 帧间隔（ms）-- 匹配 9.6kbps 空中速率，防止 E32 模块缓冲溢出 */
#define LORA_DATA_FRAME_DELAY   50

/* ---- 命令码 ---- */

/* App -> Gateway */
#define LORA_CMD_UPDATE_REQ     0x01   /* 请求更新，无载荷 */
#define LORA_CMD_UPDATE_READY   0x04   /* 擦除完成，无载荷 */
#define LORA_CMD_UPDATE_DONE    0x83   /* 更新完成，无载荷 */
#define LORA_CMD_UPDATE_ERR     0x84   /* 更新错误，载荷：error_code(1B) */

/* Gateway -> App */
#define LORA_CMD_UPDATE_ACK     0x81   /* 载荷：fw_size(4B LE) */
#define LORA_CMD_UPDATE_DATA    0x02   /* 载荷：seq(2B LE) + data(<=50B) */
#define LORA_CMD_UPDATE_END     0x03   /* 载荷：crc32(4B LE) */

/* ---- 错误码 ---- */
#define OTA_ERR_SEQ_MISMATCH    0x01   /* 序号不连续 */
#define OTA_ERR_FLASH_WRITE     0x02   /* W25Q16 写入失败 */
#define OTA_ERR_EEPROM_WRITE    0x03   /* EEPROM 写入失败 */
#define OTA_ERR_TIMEOUT         0x04   /* 接收超时 */
#define OTA_ERR_SIZE_MISMATCH   0x05   /* 接收量与声明大小不匹配 */
#define OTA_ERR_CRC_MISMATCH    0x06   /* CRC32 不匹配 */

#endif
```

- [ ] **Step 2: Copy to both project directories**

```bash
# 创建目录
mkdir -p Project03_Application_LoRa/Protocol/LoRa
mkdir -p Project03_Gateway_LoRa/Protocol/LoRa
# 复制文件到两个项目（文件内容完全相同）
```

- [ ] **Step 3: Commit**

```bash
git add Project03_Application_LoRa/Protocol/LoRa/lora_proto.h \
        Project03_Gateway_LoRa/Protocol/LoRa/lora_proto.h
git commit -m "feat: add lora_proto.h shared protocol definitions"
```

---

## Task 2: Create `lora_buf.h` + `lora_buf.c`

**Files:**
- Create: `Project03_Application_LoRa/Protocol/LoRa/lora_buf.h`
- Create: `Project03_Application_LoRa/Protocol/LoRa/lora_buf.c`
- Create: `Project03_Gateway_LoRa/Protocol/LoRa/lora_buf.h`
- Create: `Project03_Gateway_LoRa/Protocol/LoRa/lora_buf.c`

Identical files copied to both projects.

- [ ] **Step 1: Write `lora_buf.h`**

```c
#ifndef __LORA_BUF_H__
#define __LORA_BUF_H__

#include "main.h"
#include <stdint.h>

/*
 * LoRa USART3 DMA+IDLE 收发封装
 *
 * 对 E32-433T20D 透明传输模式进行二次封装：
 *   LORA_Buf_Init      — 启动 USART3 DMA 接收 + IDLE 中断
 *   LORA_Buf_Send      — 构建帧并通过 USART3 发送
 *   LORA_Buf_Recv      — 非阻塞读取一帧
 *   LORA_Buf_IdleHandler — IDLE 中断回调（在 stm32f1xx_it.c 中调用）
 *
 * 调用流程：
 *   1. CubeMX 的 MX_USART3_UART_Init() 完成硬件初始化
 *   2. LORA_Buf_Init() 启动 DMA + IDLE
 *   3. 主循环中 LORA_Buf_Recv() 查询收到的帧
 *   4. LORA_Buf_Send() 发送命令帧
 */

/* DMA 接收缓冲区大小（> 58 字节 LoRa 单包限制） */
#define LORA_RX_BUF_SIZE     64

/* 发送帧缓冲区大小（3 字节帧头 + 最大 55 字节载荷） */
#define LORA_TX_BUF_SIZE     (3 + 55)

/* LoRa 收发上下文 */
typedef struct {
    UART_HandleTypeDef *huart;              /* USART3 句柄 */
    uint8_t  rx_buf[LORA_RX_BUF_SIZE];     /* DMA 接收缓冲区 */
    uint16_t rx_len;                        /* IDLE 中断记录的帧长度 */
    uint8_t  rx_ready;                      /* 完整帧就绪标志 */
    uint8_t  tx_buf[LORA_TX_BUF_SIZE];     /* 发送帧构建缓冲 */
} LORA_Buf_t;

/*
 * 初始化 LoRa 传输层
 * - 保存 huart 句柄
 * - 启用 IDLE 中断
 * - 启动 DMA Normal 模式接收
 * 必须在 MX_USART3_UART_Init() 之后调用
 */
void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart);

/*
 * 发送一帧 LoRa 数据
 * - 自动构建帧：[0xAA] [cmd] [len] [payload...]
 * - 阻塞式 UART 发送，超时 100ms
 * 返回 0 成功，-1 参数错误，HAL 错误码其他
 */
int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len);

/*
 * 非阻塞接收一帧
 * - 如果收到完整帧，解析 cmd/payload/len 并返回 1
 * - 如果没有新帧，返回 0
 * - 帧格式错误时返回 0 并丢弃
 */
int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len);

/*
 * USART3 IDLE 中断处理
 * - 在 stm32f1xx_it.c 的 USART3_IRQHandler 中调用
 * - 记录帧长度，设置 rx_ready，重启 DMA
 */
void LORA_Buf_IdleHandler(void);

#endif
```

- [ ] **Step 2: Write `lora_buf.c`**

```c
#include "lora_buf.h"
#include "lora_proto.h"
#include <string.h>

/* 全局指针，供 IDLE ISR 通过 LORA_Buf_IdleHandler() 访问 */
static LORA_Buf_t *g_lora_ctx = NULL;

void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->huart = huart;
    g_lora_ctx = ctx;

    /* 启用 IDLE 中断 */
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);

    /* 启动 DMA Normal 模式接收 */
    HAL_UART_Receive_DMA(huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}

int LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd,
                  const uint8_t *payload, uint8_t len)
{
    if (len > LORA_MAX_PAYLOAD) return -1;

    /* 构建帧：[0xAA] [CMD] [LEN] [PAYLOAD...] */
    ctx->tx_buf[0] = LORA_FRAME_HEADER;
    ctx->tx_buf[1] = cmd;
    ctx->tx_buf[2] = len;
    if (len > 0 && payload != NULL) {
        memcpy(&ctx->tx_buf[3], payload, len);
    }

    /* 阻塞式 UART 发送，超时 100ms */
    return HAL_UART_Transmit(ctx->huart, ctx->tx_buf, 3 + len, 100);
}

int LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd,
                  uint8_t *payload, uint8_t *len)
{
    if (!ctx->rx_ready) return 0;
    ctx->rx_ready = 0;

    /* 最小帧长度：HEADER + CMD + LEN = 3 */
    if (ctx->rx_len < 3) return 0;
    if (ctx->rx_buf[0] != LORA_FRAME_HEADER) return 0;

    uint8_t frame_cmd = ctx->rx_buf[1];
    uint8_t frame_len = ctx->rx_buf[2];

    /* 合法性检查 */
    if (frame_len > LORA_MAX_PAYLOAD) return 0;
    if (ctx->rx_len < (uint16_t)(3 + frame_len)) return 0;

    /* 输出解析结果 */
    *cmd = frame_cmd;
    *len = frame_len;
    if (frame_len > 0 && payload != NULL) {
        memcpy(payload, &ctx->rx_buf[3], frame_len);
    }

    return 1;
}

void LORA_Buf_IdleHandler(void)
{
    LORA_Buf_t *ctx = g_lora_ctx;
    if (ctx == NULL || ctx->huart == NULL) return;

    /* 检查 IDLE 标志 */
    if (__HAL_UART_GET_FLAG(ctx->huart, UART_FLAG_IDLE) == RESET) return;
    __HAL_UART_CLEAR_IDLEFLAG(ctx->huart);

    /* 计算本次接收到的字节数 */
    uint16_t recv_len = LORA_RX_BUF_SIZE -
                        (uint16_t)__HAL_DMA_GET_COUNTER(ctx->huart->hdmarx);

    /* 假中断（初始化时 IDLE 已置位）：recv_len == 0 时忽略 */
    if (recv_len == 0) return;

    /* 仅在前一帧已被消费时接受新帧 */
    if (!ctx->rx_ready) {
        ctx->rx_len = recv_len;
        ctx->rx_ready = 1;
    }

    /* 重启 DMA 接收 */
    HAL_UART_DMAStop(ctx->huart);
    HAL_UART_Receive_DMA(ctx->huart, ctx->rx_buf, LORA_RX_BUF_SIZE);
}
```

- [ ] **Step 3: Copy to both project directories**

```bash
# 两个项目的 Protocol/LoRa/ 目录放相同的 lora_buf.h 和 lora_buf.c
```

- [ ] **Step 4: Commit**

```bash
git add Project03_Application_LoRa/Protocol/LoRa/lora_buf.h \
        Project03_Application_LoRa/Protocol/LoRa/lora_buf.c \
        Project03_Gateway_LoRa/Protocol/LoRa/lora_buf.h \
        Project03_Gateway_LoRa/Protocol/LoRa/lora_buf.c
git commit -m "feat: add lora_buf.c/h USART3 DMA+IDLE transport layer"
```

---

## Task 3: Setup Application LoRa project (CubeMX + file copy)

This task is manual. No code to write — project scaffolding only.

**Files:**
- Create: `Project03_Application_LoRa/` (entire directory)

- [ ] **Step 1: Create CubeMX project for Application**

1. Open STM32CubeMX, create new project: MCU = STM32F103C8
2. Configure peripherals:
   - **USART1**: Asynchronous, 115200, 8N1 (debug printf)
   - **USART3**: Asynchronous, 115200, 8N1 → Add DMA RX (DMA1_CH3, Normal) → Enable USART3 global interrupt (NVIC)
   - **CRC**: Enabled (for CRC32)
   - **GPIO**: PA0 output (LED1), PA1 output (LED2), PA4 output (SPI CS), PA5/PA6/PA7 output (SPI), PB8/PB9 output (I2C)
   - **NO CAN** — do not enable CAN
3. Project Settings: Toolchain = MDK-ARM V5, Project Name = `Project03_Application_LoRa`
4. Set ROM: IROM1 = 0x08008000, size = 0x8000 (32KB) — in Project → Settings → Linker
5. In `Core/Src/system_stm32f1xx.c`: set `VECT_TAB_OFFSET = 0x00008000U` (in `USER CODE BEGIN VECT_TAB` if available, or directly modify the define)
6. Generate code

- [ ] **Step 2: Copy unchanged modules from `Project02_Application/`**

Copy these directories/files into `Project03_Application_LoRa/`:

```
APP/bootloader_conf.h          → APP/bootloader_conf.h
Service/ota_storage.c          → Service/ota_storage.c
Service/ota_storage.h          → Service/ota_storage.h
Driver/MCU/crc32.c             → Driver/MCU/crc32.c
Driver/MCU/crc32.h             → Driver/MCU/crc32.h
Driver/Storage/w25q16.c        → Driver/Storage/w25q16.c
Driver/Storage/w25q16.h        → Driver/Storage/w25q16.h
Driver/Storage/at24c02.c       → Driver/Storage/at24c02.c
Driver/Storage/at24c02.h       → Driver/Storage/at24c02.h
Protocol/SPI/soft_spi.c        → Protocol/SPI/soft_spi.c
Protocol/SPI/soft_spi.h        → Protocol/SPI/soft_spi.h
Protocol/I2C/soft_i2c.c        → Protocol/I2C/soft_i2c.c
Protocol/I2C/soft_i2c.h        → Protocol/I2C/soft_i2c.h
BSP/bsp_soft_spi.c             → BSP/bsp_soft_spi.c
BSP/bsp_soft_spi.h             → BSP/bsp_soft_spi.h
BSP/bsp_soft_i2c.c             → BSP/bsp_soft_i2c.c
BSP/bsp_soft_i2c.h             → BSP/bsp_soft_i2c.h
Drivers/                        → Drivers/ (entire HAL library)
```

- [ ] **Step 3: Configure Keil project**

1. Open `MDK-ARM/Project03_Application_LoRa.uvprojx` in Keil
2. Add source files to project groups (matching directory names):
   - `APP` group: `app_ota_update.c` (will be created in Task 4-5)
   - `Service` group: `ota_storage.c`
   - `Driver/MCU` group: `crc32.c`
   - `Driver/Storage` group: `w25q16.c`, `at24c02.c`
   - `Protocol/LoRa` group: `lora_buf.c`
   - `Protocol/SPI` group: `soft_spi.c`
   - `Protocol/I2C` group: `soft_i2c.c`
   - `BSP` group: `bsp_soft_spi.c`, `bsp_soft_i2c.c`
3. Configure include paths:
   ```
   ../APP; ../Service; ../Driver/MCU; ../Driver/Storage;
   ../Protocol/LoRa; ../Protocol/SPI; ../Protocol/I2C; ../BSP
   ```
4. Compiler: ARM Compiler V5, optimization `-O1` or `-O2`
5. Scatter file: verify ROM starts at 0x08008000, size 0x8000

- [ ] **Step 4: Commit project skeleton**

```bash
git add Project03_Application_LoRa/
git commit -m "feat: scaffold Project03_Application_LoRa project structure"
```

---

## Task 4: Create adapted `app_ota_update.h`

**Files:**
- Create: `Project03_Application_LoRa/APP/app_ota_update.h`

- [ ] **Step 1: Write adapted header**

Changes from CAN version:
- `#include "can_buf.h"` → `#include "lora_buf.h"`
- `CAN_Buf_t *can_ctx` → `LORA_Buf_t *lora_ctx`
- `OTA_ERROR_BACKOFF` 3000 → 5000
- `OTA_RECV_DATA_TIMEOUT` added as 30000
- Remove CAN-specific constants

```c
#ifndef __APP_OTA_UPDATE_H__
#define __APP_OTA_UPDATE_H__

#include "lora_buf.h"
#include "ota_storage.h"
#include <stdint.h>

/*
 * LoRa OTA 更新状态机（App 侧）
 *
 * 通过 LoRa 从网关接收固件数据，调用 ota_storage 写入 W25Q16，
 * 完成后写 EEPROM 标志位触发 Bootloader 更新。
 *
 * 使用方式：
 *   APP_OTA_Init()    — 初始化，绑定 LoRa 上下文和存储驱动
 *   APP_OTA_Process() — 主循环中轮询调用
 */

/* 状态机状态 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_WAIT_ACK,
    OTA_STATE_RECV_DATA,
    OTA_STATE_DONE,
    OTA_STATE_ERROR
} OTA_State_t;

/* 等待 ACK 超时（ms） */
#define OTA_WAIT_ACK_TIMEOUT    5000

/* RECV_DATA 状态无数据超时（ms）-- LoRa 帧间隔长，需更长超时 */
#define OTA_RECV_DATA_TIMEOUT   30000

/* ERROR 后退避等待（ms） */
#define OTA_ERROR_BACKOFF       5000

/* CRC 校验失败最大重试次数 */
#define OTA_MAX_CRC_RETRY       3

typedef struct {
    OTA_State_t    state;
    OTA_Storage_t  storage;
    LORA_Buf_t    *lora_ctx;         /* LoRa 收发上下文 */
    uint16_t       expect_seq;       /* 期望的下一个数据帧序号 */
    uint32_t       state_tick;       /* 进入当前状态时的 tick */
    uint8_t        error_code;       /* 错误码 */
    uint8_t        crc_retry_cnt;    /* CRC 校验失败重试计数 */
} APP_OTA_t;

/* 初始化：绑定 LoRa 上下文和存储驱动 */
void APP_OTA_Init(APP_OTA_t *ctx, LORA_Buf_t *lora_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom);

/* 主循环轮询调用，处理 LoRa 消息并推进状态机 */
void APP_OTA_Process(APP_OTA_t *ctx);

#endif
```

- [ ] **Step 2: Commit**

```bash
git add Project03_Application_LoRa/APP/app_ota_update.h
git commit -m "feat: add LoRa-adapted app_ota_update.h"
```

---

## Task 5: Create adapted `app_ota_update.c`

**Files:**
- Create: `Project03_Application_LoRa/APP/app_ota_update.c`

This is the largest task. All CAN calls are replaced with LoRa calls, frame parsing is updated.

- [ ] **Step 1: Write adapted state machine**

Key changes from CAN version:
- `CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, data, len)` → `LORA_Buf_Send(ctx->lora_ctx, cmd, payload, len)`
- `CAN_Buf_Recv` loop over multiple messages → single `LORA_Buf_Recv` call (LoRa is slower, one frame at a time)
- Frame parsing: `rx_msg[i].data[0]` (CMD) → `cmd` parameter, `rx_msg[i].data[1..]` → `payload[0..]`
- DATA payload: `rx_msg[i].data[3..]` (5B max) → `payload[2..]` (50B max)
- RECV_DATA timeout: 10000 → `OTA_RECV_DATA_TIMEOUT` (30000)
- ERROR backoff: `OTA_ERROR_BACKOFF` (5000)

```c
#include "app_ota_update.h"
#include "lora_buf.h"
#include "lora_proto.h"
#include "ota_storage.h"
#include "w25q16.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

void APP_OTA_Init(APP_OTA_t *ctx, LORA_Buf_t *lora_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom)
{
    ctx->state         = OTA_STATE_IDLE;
    ctx->lora_ctx      = lora_ctx;
    ctx->expect_seq    = 0;
    ctx->state_tick    = 0;
    ctx->error_code    = 0;
    ctx->crc_retry_cnt = 0;

    OTA_Storage_Init(&ctx->storage, w25q, eeprom);
}

void APP_OTA_Process(APP_OTA_t *ctx)
{
    uint8_t cmd, payload[55], len;

    switch (ctx->state)
    {
    case OTA_STATE_IDLE:
        /* ERROR 退避等待 */
        if (ctx->state_tick != 0 &&
            (HAL_GetTick() - ctx->state_tick) < OTA_ERROR_BACKOFF)
        {
            break;
        }

        /* 发送更新请求（REQ 无载荷） */
        LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_REQ, NULL, 0);

        ctx->state      = OTA_STATE_WAIT_ACK;
        ctx->state_tick = HAL_GetTick();
        break;

    case OTA_STATE_WAIT_ACK:
        if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
        {
            if (cmd == LORA_CMD_UPDATE_ACK && len >= 4)
            {
                /* 解析固件大小（4B 小端） */
                uint32_t fw_size = (uint32_t)payload[0]
                                 | ((uint32_t)payload[1] << 8)
                                 | ((uint32_t)payload[2] << 16)
                                 | ((uint32_t)payload[3] << 24);

                /* fw_size 合法性校验 */
                if (fw_size == 0 || fw_size > W25Q16_TOTAL_SIZE)
                {
                    printf("[OTA] Invalid fw_size: %lu\r\n", fw_size);
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 擦除 W25Q16 扇区 */
                int ret = OTA_Storage_Start(&ctx->storage, fw_size);
                if (ret != 0)
                {
                    printf("[OTA] Storage start failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 擦除完成，发 READY 通知网关 */
                LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_READY, NULL, 0);

                ctx->expect_seq    = 0;
                ctx->crc_retry_cnt = 0;
                ctx->state         = OTA_STATE_RECV_DATA;
                ctx->state_tick    = HAL_GetTick();
                break;
            }
        }

        /* 超时检查 */
        if (ctx->state == OTA_STATE_WAIT_ACK &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_WAIT_ACK_TIMEOUT)
        {
            printf("[OTA] WAIT_ACK timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_RECV_DATA:
        if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
        {
            if (cmd == LORA_CMD_UPDATE_DATA && len >= 2)
            {
                /* 校验序号 */
                uint16_t seq = (uint16_t)payload[0]
                             | ((uint16_t)payload[1] << 8);

                if (seq != ctx->expect_seq)
                {
                    printf("[OTA] seq mismatch: got %u, expected %u\r\n",
                           seq, ctx->expect_seq);
                    ctx->error_code = OTA_ERR_SEQ_MISMATCH;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 写入数据（payload[2..] = 固件数据） */
                uint8_t dlen = len - 2;
                int ret = OTA_Storage_Write(&ctx->storage,
                                            &payload[2], dlen);
                if (ret != 0)
                {
                    printf("[OTA] Storage write failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                ctx->expect_seq++;
                ctx->state_tick = HAL_GetTick();
            }
            else if (cmd == LORA_CMD_UPDATE_END && len >= 4)
            {
                /* 解析 CRC32（4B 小端） */
                uint32_t expected_crc = (uint32_t)payload[0]
                                      | ((uint32_t)payload[1] << 8)
                                      | ((uint32_t)payload[2] << 16)
                                      | ((uint32_t)payload[3] << 24);
                OTA_Storage_SetExpectedCRC(&ctx->storage, expected_crc);

                printf("[OTA] END received, total_recv=%lu\r\n",
                       ctx->storage.total_recv);

                /* 刷缓冲 + 回读 W25Q16 CRC 校验 + 写 EEPROM */
                int ret = OTA_Storage_Finish(&ctx->storage);
                if (ret != 0)
                {
                    printf("[OTA] Storage finish failed: %d\r\n", ret);
                    ctx->error_code = (uint8_t)ret;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 发送 DONE */
                LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_DONE, NULL, 0);
                printf("[APP] Update complete, resetting\r\n");

                /* LED2 翻转 */
                HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);

                /* 延时确保 LoRa 帧和 printf 传输完成 */
                HAL_Delay(100);
                NVIC_SystemReset();
            }
        }

        /* 超时检查 */
        if (ctx->state == OTA_STATE_RECV_DATA &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_RECV_DATA_TIMEOUT)
        {
            printf("[OTA] RECV_DATA timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_DONE:
        /* 当前流程在 RECV_DATA 中直接 SystemReset，不进入此状态 */
        break;

    case OTA_STATE_ERROR:
        {
            /* 发送错误帧 */
            LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_ERR,
                          &ctx->error_code, 1);
            printf("[OTA] ERROR: code=0x%02X\r\n", ctx->error_code);

            /* CRC 失败：累计重试计数器 */
            if (ctx->error_code == OTA_ERR_CRC_MISMATCH)
            {
                ctx->crc_retry_cnt++;
                printf("[OTA] CRC fail, retry %d/%d\r\n",
                       ctx->crc_retry_cnt, OTA_MAX_CRC_RETRY);

                if (ctx->crc_retry_cnt >= OTA_MAX_CRC_RETRY)
                {
                    printf("[OTA] CRC retry limit reached, abort\r\n");
                    ctx->crc_retry_cnt = 0;
                    ctx->state         = OTA_STATE_IDLE;
                    ctx->state_tick    = 0;
                    break;
                }
            }

            OTA_Storage_Reset(&ctx->storage);
            ctx->state      = OTA_STATE_IDLE;
            ctx->state_tick = HAL_GetTick();
        }
        break;
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add Project03_Application_LoRa/APP/app_ota_update.c
git commit -m "feat: add LoRa-adapted app_ota_update.c OTA state machine"
```

---

## Task 6: Adapt Application `main.c` + `stm32f1xx_it.c`

**Files:**
- Modify: `Project03_Application_LoRa/Core/Src/main.c` (USER CODE blocks)
- Modify: `Project03_Application_LoRa/Core/Src/stm32f1xx_it.c` (USER CODE blocks)

- [ ] **Step 1: Edit `main.c` — includes section**

Find `/* USER CODE BEGIN Includes */` block, replace with:

```c
/* USER CODE BEGIN Includes */
#include "stm32f1xx_hal.h"
#include "lora_buf.h"
#include "lora_proto.h"
#include "bsp_soft_spi.h"
#include "bsp_soft_i2c.h"
#include "w25q16.h"
#include "at24c02.h"
#include "ota_storage.h"
#include "app_ota_update.h"
#include <stdio.h>
/* USER CODE END Includes */
```

- [ ] **Step 2: Edit `main.c` — variables section**

Find `/* USER CODE BEGIN PV */` block, replace with:

```c
/* USER CODE BEGIN PV */
static LORA_Buf_t lora_ctx;
static W25Q16_t   w25q_dev;
static AT24C02_t  eeprom_dev;
static APP_OTA_t  ota_ctx;
/* USER CODE END PV */
```

- [ ] **Step 3: Edit `main.c` — initialization section**

Find `/* USER CODE BEGIN 2 */` block, replace with:

```c
  /* USER CODE BEGIN 2 */
  /* 清除 Bootloader 残留的所有挂起中断 */
  for (uint32_t i = 0; i < 2; i++) {
      NVIC->ICER[i] = 0xFFFFFFFF;
      NVIC->ICPR[i] = 0xFFFFFFFF;
  }
  __enable_irq();

  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);

  printf("[APP] main() reached, all MX_init done\r\n");

  /* LoRa 初始化 */
  LORA_Buf_Init(&lora_ctx, &huart3);
  printf("[APP] LORA_Buf_Init done\r\n");

  /* 存储外设初始化 */
  BSP_SoftSPI_Init();
  BSP_SoftI2C_Init();
  W25Q16_Init(&w25q_dev, &spi1_bus, W25Q_CS_PORT, W25Q_CS_PIN);
  AT24C02_Init(&eeprom_dev, &i2c1_bus, AT24C02_ADDR);
  printf("[APP] W25Q16 + AT24C02 init done\r\n");

  /* 读取 W25Q16 JEDEC ID 验证硬件连接 */
  uint32_t jedec_id = W25Q16_ReadJEDECID(&w25q_dev);
  printf("[APP] W25Q16 JEDEC ID: 0x%06lX\r\n", jedec_id);

  /* OTA 模块初始化 */
  APP_OTA_Init(&ota_ctx, &lora_ctx, &w25q_dev, &eeprom_dev);
  printf("[APP] OTA module init done\r\n");
  /* USER CODE END 2 */
```

- [ ] **Step 4: Edit `main.c` — main loop**

Find `/* USER CODE BEGIN 3 */` block, replace with:

```c
    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    APP_OTA_Process(&ota_ctx);
    /* USER CODE END 3 */
```

- [ ] **Step 5: Edit `stm32f1xx_it.c` — add USART3 IDLE handler**

Find the `USART3_IRQHandler` function. Inside, find `/* USER CODE BEGIN USART3_IRQn */` (or add it at the start of the handler), add:

```c
  /* USER CODE BEGIN USART3_IRQn 0 */
  LORA_Buf_IdleHandler();
  /* If IDLE handler consumed the interrupt, skip default HAL handler */
  /* USER CODE END USART3_IRQn 0 */
```

Also add the include at the top of the file:

```c
/* USER CODE BEGIN Includes */
#include "lora_buf.h"
/* USER CODE END Includes */
```

- [ ] **Step 6: Commit**

```bash
git add Project03_Application_LoRa/Core/Src/main.c \
        Project03_Application_LoRa/Core/Src/stm32f1xx_it.c
git commit -m "feat: adapt Application main.c and stm32f1xx_it.c for LoRa"
```

---

## Task 7: Compile Application project in Keil

This task is manual. Compile and fix errors.

- [ ] **Step 1: Open Keil project**

Open `Project03_Application_LoRa/MDK-ARM/Project03_Application_LoRa.uvprojx`

- [ ] **Step 2: Verify all source files are in the project**

Check that all groups contain the correct files (see Task 3 Step 3).

- [ ] **Step 3: Build (F7)**

Expected: 0 errors. Common issues:
- Missing include paths → add in Keil Options → C/C++ → Include Paths
- `hcan` reference → should not exist, all CAN references removed
- `HAL_UART_Transmit` / `HAL_UART_Receive_DMA` undefined → check that `stm32f1xx_hal_uart.h` is included via `main.h`

- [ ] **Step 4: Fix any compilation errors**

Typical fixes:
- `W25Q16_TOTAL_SIZE` not defined → check `w25q16.h` is in include path
- `spi1_bus` / `i2c1_bus` not defined → these are declared in BSP headers, externed in driver headers
- `W25Q_CS_PORT` / `W25Q_CS_PIN` not defined → defined in `bsp_soft_spi.h` or `bootloader_conf.h`

- [ ] **Step 5: Commit compilation fixes**

```bash
git add -u Project03_Application_LoRa/
git commit -m "fix: Application LoRa compilation fixes"
```

---

## Task 8: Setup Gateway LoRa project (CubeMX + file copy)

This task is manual. Project scaffolding only.

**Files:**
- Create: `Project03_Gateway_LoRa/` (entire directory)

- [ ] **Step 1: Create CubeMX project for Gateway**

1. Open STM32CubeMX, create new project: MCU = STM32F103C8
2. Configure peripherals:
   - **USART1**: Asynchronous, 115200, 8N1 (debug printf + PC serial download) → Add DMA RX (DMA1_CH5, Normal) → Enable USART1 global interrupt (NVIC) for IDLE
   - **USART3**: Asynchronous, 115200, 8N1 → Add DMA RX (DMA1_CH3, Normal) → Enable USART3 global interrupt (NVIC)
   - **CRC**: Enabled
   - **GPIO**: PA0 output (LED1) (only if needed)
   - **NO CAN**
3. Project Settings: Toolchain = MDK-ARM V5, Project Name = `Project03_Gateway_LoRa`
4. ROM: IROM1 = 0x08000000, size = 0x10000 (64KB) — default
5. Generate code

- [ ] **Step 2: Copy unchanged modules from `P00_getway_led1_hal/`**

```
APP/app_bootloader.c/h         → APP/app_bootloader.c/h
APP/fw_cache_conf.h            → APP/fw_cache_conf.h
Protocol/UART/uart_buf.c/h     → Protocol/UART/uart_buf.c/h
Service/flash_download.c/h     → Service/flash_download.c/h
Driver/MCU/flash.c/h           → Driver/MCU/flash.c/h
Driver/MCU/crc32.c/h           → Driver/MCU/crc32.c/h
Drivers/                        → Drivers/
```

- [ ] **Step 3: Configure Keil project**

Add source groups and include paths similar to Task 3 Step 3. Groups:
- `APP`: `app_update.c`, `app_bootloader.c` (optional)
- `Protocol/LoRa`: `lora_buf.c`
- `Protocol/UART`: `uart_buf.c`
- `Service`: `flash_download.c`
- `Driver/MCU`: `flash.c`, `crc32.c`

Include paths:
```
../APP; ../Protocol/LoRa; ../Protocol/UART; ../Service; ../Driver/MCU
```

- [ ] **Step 4: Commit**

```bash
git add Project03_Gateway_LoRa/
git commit -m "feat: scaffold Project03_Gateway_LoRa project structure"
```

---

## Task 9: Create adapted `app_update.h`

**Files:**
- Create: `Project03_Gateway_LoRa/APP/app_update.h`

- [ ] **Step 1: Write adapted header**

Changes from CAN version:
- `#include "can_buf.h"` → `#include "lora_buf.h"`
- `CAN_Buf_t *can_ctx` → `LORA_Buf_t *lora_ctx`

```c
#ifndef __APP_UPDATE_H__
#define __APP_UPDATE_H__

#include "lora_buf.h"
#include "crc32.h"
#include "lora_proto.h"
#include <stdint.h>

/**
 * @brief  状态机状态枚举
 */
typedef enum {
    APP_WAIT_UPDATE_CMD = 0,  /* 等待 App 发送 REQ */
    APP_WAIT_READY,           /* 等待 App 擦除完成后发 READY */
    APP_UPDATE_SEND           /* 逐帧发送固件数据 */
} AppUpdate_State_t;

/**
 * @brief  更新状态机上下文结构体
 */
typedef struct {
    AppUpdate_State_t state;
    LORA_Buf_t *lora_ctx;         /* LoRa 收发上下文 */
    const uint8_t *fw_data;       /* 固件数据指针（Flash 缓存区） */
    uint32_t fw_size;             /* 固件总大小（字节） */
    uint32_t fw_offset;           /* 当前发送偏移量 */
    uint16_t fw_seq;              /* 当前帧序号 */
    uint32_t wait_ready_tick;     /* WAIT_READY 超时计时 */
    uint32_t fw_crc;              /* 固件 CRC32（收到 REQ 时预计算） */
} AppUpdate_t;

void AppUpdate_Init(AppUpdate_t *ctx, LORA_Buf_t *lora_ctx,
                    const uint8_t *fw_data, uint32_t fw_size);
void AppUpdate_Poll(AppUpdate_t *ctx);

#endif
```

- [ ] **Step 2: Commit**

```bash
git add Project03_Gateway_LoRa/APP/app_update.h
git commit -m "feat: add LoRa-adapted app_update.h for Gateway"
```

---

## Task 10: Create adapted `app_update.c`

**Files:**
- Create: `Project03_Gateway_LoRa/APP/app_update.c`

- [ ] **Step 1: Write adapted state machine**

Key changes from CAN version:
- `CAN_Buf_Send` → `LORA_Buf_Send` with cmd/payload separation
- `CAN_Buf_Recv` loop → single `LORA_Buf_Recv` call
- DATA chunk: 5 bytes → `LORA_MAX_DATA_PER_FRAME` (50 bytes)
- Frame interval: `HAL_Delay(2)` → `HAL_Delay(LORA_DATA_FRAME_DELAY)` (50ms)
- ACK payload: `data[1..4]` → `payload[0..3]` (CMD no longer in payload)

```c
#include "app_update.h"
#include "lora_proto.h"
#include <stdio.h>
#include <string.h>
#include "crc32.h"

void AppUpdate_Init(AppUpdate_t *ctx, LORA_Buf_t *lora_ctx,
                    const uint8_t *fw_data, uint32_t fw_size)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = APP_WAIT_UPDATE_CMD;
    ctx->lora_ctx = lora_ctx;
    ctx->fw_data = fw_data;
    ctx->fw_size = (fw_size == 0) ? 0 : fw_size;
}

/* 发送 ACK：载荷 = fw_size (4B LE) */
static void send_ack(AppUpdate_t *ctx)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(ctx->fw_size);
    payload[1] = (uint8_t)(ctx->fw_size >> 8);
    payload[2] = (uint8_t)(ctx->fw_size >> 16);
    payload[3] = (uint8_t)(ctx->fw_size >> 24);
    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_ACK, payload, 4);
    printf("[Host] ACK sent, size=%lu\r\n", ctx->fw_size);
}

/* 发送一帧 DATA：载荷 = seq(2B LE) + data(<=50B) */
static void send_data_frame(AppUpdate_t *ctx)
{
    uint8_t buf[2 + LORA_MAX_DATA_PER_FRAME];
    buf[0] = (uint8_t)(ctx->fw_seq);
    buf[1] = (uint8_t)(ctx->fw_seq >> 8);

    uint32_t remain = ctx->fw_size - ctx->fw_offset;
    uint8_t chunk = (remain > LORA_MAX_DATA_PER_FRAME)
                  ? LORA_MAX_DATA_PER_FRAME : (uint8_t)remain;

    memcpy(&buf[2], &ctx->fw_data[ctx->fw_offset], chunk);

    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_DATA, buf, 2 + chunk);
    ctx->fw_offset += chunk;
    ctx->fw_seq++;

    if (ctx->fw_seq % 10 == 0 || ctx->fw_offset >= ctx->fw_size)
        printf("[Host] %lu/%lu\r\n", ctx->fw_offset, ctx->fw_size);
}

/* 发送 END：载荷 = crc32 (4B LE) */
static void send_end(AppUpdate_t *ctx)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(ctx->fw_crc);
    payload[1] = (uint8_t)(ctx->fw_crc >> 8);
    payload[2] = (uint8_t)(ctx->fw_crc >> 16);
    payload[3] = (uint8_t)(ctx->fw_crc >> 24);
    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_END, payload, 4);
    printf("[Host] Send END, crc=0x%08lX\r\n", ctx->fw_crc);
}

void AppUpdate_WaitCmd(AppUpdate_t *ctx)
{
    uint8_t cmd, payload[55], len;
    if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
    {
        if (cmd == LORA_CMD_UPDATE_REQ)
        {
            /* 预计算固件 CRC32 */
            ctx->fw_crc = CRC32_Calculate(ctx->fw_data, ctx->fw_size);
            printf("[Host] CRC calc: 0x%08lX\r\n", ctx->fw_crc);
            send_ack(ctx);
            ctx->fw_offset = 0;
            ctx->fw_seq = 0;
            ctx->state = APP_WAIT_READY;
            ctx->wait_ready_tick = HAL_GetTick();
        }
    }
}

void AppUpdate_WaitReady(AppUpdate_t *ctx)
{
    if (HAL_GetTick() - ctx->wait_ready_tick > 60000)
    {
        printf("[Host] READY timeout\r\n");
        ctx->state = APP_WAIT_UPDATE_CMD;
        return;
    }

    uint8_t cmd, payload[55], len;
    if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
    {
        if (cmd == LORA_CMD_UPDATE_READY)
        {
            ctx->state = APP_UPDATE_SEND;
        }
    }
}

void AppUpdate_Send(AppUpdate_t *ctx)
{
    if (ctx->fw_offset < ctx->fw_size)
    {
        send_data_frame(ctx);
        HAL_Delay(LORA_DATA_FRAME_DELAY);
    }
    else
    {
        send_end(ctx);
        ctx->state = APP_WAIT_UPDATE_CMD;
    }
}

void AppUpdate_Poll(AppUpdate_t *ctx)
{
    if (ctx == NULL || ctx->lora_ctx == NULL)
        return;

    switch (ctx->state)
    {
    case APP_WAIT_UPDATE_CMD:
        AppUpdate_WaitCmd(ctx);
        break;
    case APP_WAIT_READY:
        AppUpdate_WaitReady(ctx);
        break;
    case APP_UPDATE_SEND:
        AppUpdate_Send(ctx);
        break;
    default:
        ctx->state = APP_WAIT_UPDATE_CMD;
        break;
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add Project03_Gateway_LoRa/APP/app_update.c
git commit -m "feat: add LoRa-adapted app_update.c Gateway state machine"
```

---

## Task 11: Adapt Gateway `main.c` + `stm32f1xx_it.c`

**Files:**
- Modify: `Project03_Gateway_LoRa/Core/Src/main.c` (USER CODE blocks)
- Modify: `Project03_Gateway_LoRa/Core/Src/stm32f1xx_it.c` (USER CODE blocks)

- [ ] **Step 1: Edit `main.c` — includes**

Find `/* USER CODE BEGIN Includes */`, replace with:

```c
/* USER CODE BEGIN Includes */
#include "lora_buf.h"
#include "app_update.h"
#include "app_bootloader.h"
#include "uart_buf.h"
#include "fw_cache_conf.h"
#include <stdio.h>
/* USER CODE END Includes */
```

- [ ] **Step 2: Edit `main.c` — variables**

Find `/* USER CODE BEGIN PV */`, replace with:

```c
/* USER CODE BEGIN PV */
static LORA_Buf_t lora_ctx;
static AppUpdate_t update_ctx;
/* USER CODE END PV */
```

- [ ] **Step 3: Edit `main.c` — initialization**

Find `/* USER CODE BEGIN 2 */`, replace with:

```c
  /* USER CODE BEGIN 2 */
  LORA_Buf_Init(&lora_ctx, &huart3);

  /* CAN 更新状态机：固件数据指向 Flash 缓存区 */
  AppUpdate_Init(&update_ctx, &lora_ctx,
                 (const uint8_t *)FW_CACHE_ADDR, 1344);

  UART_DMA_Rx_Init();
  /* USER CODE END 2 */
```

- [ ] **Step 4: Edit `main.c` — main loop**

Find `/* USER CODE BEGIN 3 */`, replace with:

```c
    /* USER CODE BEGIN 3 */
    /* AppBootloader_Process(&bl_ctx); -- UART download commented out */
    AppUpdate_Poll(&update_ctx);
    /* USER CODE END 3 */
```

- [ ] **Step 5: Edit `stm32f1xx_it.c` — add USART3 IDLE handler**

Same as Task 6 Step 5 — add `LORA_Buf_IdleHandler()` call and include:

```c
/* USER CODE BEGIN Includes */
#include "lora_buf.h"
/* USER CODE END Includes */
```

In `USART3_IRQHandler`:
```c
  /* USER CODE BEGIN USART3_IRQn 0 */
  LORA_Buf_IdleHandler();
  /* USER CODE END USART3_IRQn 0 */
```

- [ ] **Step 6: Commit**

```bash
git add Project03_Gateway_LoRa/Core/Src/main.c \
        Project03_Gateway_LoRa/Core/Src/stm32f1xx_it.c
git commit -m "feat: adapt Gateway main.c and stm32f1xx_it.c for LoRa"
```

---

## Task 12: Compile Gateway project in Keil

This task is manual. Same process as Task 7.

- [ ] **Step 1: Open Keil project**

Open `Project03_Gateway_LoRa/MDK-ARM/Project03_Gateway_LoRa.uvprojx`

- [ ] **Step 2: Build (F7)**

Expected: 0 errors.

- [ ] **Step 3: Fix any compilation errors**

Common issues same as Task 7.

- [ ] **Step 4: Commit fixes**

```bash
git add -u Project03_Gateway_LoRa/
git commit -m "fix: Gateway LoRa compilation fixes"
```

---

## Task 13: Update CLAUDE.md with new project info

**Files:**
- Modify: `CLAUDE.md` (root)

- [ ] **Step 1: Add new projects to engineering list**

In the "工程列表" table, add two new rows:

```
| `Project03_Application_LoRa` | LoRa App（A 区），通过 LoRa 接收固件更新 | 0x08008000 |
| `Project03_Gateway_LoRa` | LoRa 网关（Host），通过 LoRa 分发固件 | 0x08000000 |
```

- [ ] **Step 2: Add LoRa protocol section**

Add a "LoRa OTA 协议" section documenting the frame format and key differences from CAN.

- [ ] **Step 3: Update CLAUDE.md for each sub-project**

Create `Project03_Application_LoRa/CLAUDE.md` and `Project03_Gateway_LoRa/CLAUDE.md` documenting LoRa-specific details (UART pin assignments, module configuration, frame format).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md Project03_Application_LoRa/CLAUDE.md Project03_Gateway_LoRa/CLAUDE.md
git commit -m "docs: update CLAUDE.md with LoRa project info"
```
