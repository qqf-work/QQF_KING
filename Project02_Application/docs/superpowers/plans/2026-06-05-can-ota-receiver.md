# CAN OTA 接收端实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 A 区 App 中实现 CAN OTA 固件接收模块，通过 CAN 从网关接收固件数据，保存到 W25Q16，完成后写 EEPROM 标志位触发 Bootloader 更新。

**Architecture:** 2 文件分层：`Service/ota_storage.c/h`（存储层，W25Q16 页缓冲 + EEPROM 标志位）+ `APP/app_ota_update.c/h`（CAN OTA 状态机）。状态机在主循环中以轮询方式调用。

**Tech Stack:** STM32F103C8 HAL, ARM Compiler V5.05, Keil MDK-ARM 5, W25Q16 SPI Flash, AT24C02 I2C EEPROM, bxCAN

---

## 文件结构

| 操作 | 文件路径 | 职责 |
|------|----------|------|
| 修改 | `Protocol/CAN/can_proto.h` | 添加错误命令和错误码定义 |
| 新建 | `Service/ota_storage.h` | 存储层接口声明 |
| 新建 | `Service/ota_storage.c` | W25Q16 页缓冲写入 + EEPROM 标志位 |
| 新建 | `APP/app_ota_update.h` | OTA 状态机接口声明 |
| 新建 | `APP/app_ota_update.c` | CAN OTA 状态机实现 |
| 修改 | `Core/Src/main.c` | 替换手动协议处理为 OTA 模块调用 |
| 修改 | `MDK-ARM/Project01_learn_Bootloader_led/Project01_learn_Bootloader_led.uvprojx` | 添加源文件和包含路径 |

---

### Task 1: 添加协议错误命令到 can_proto.h

**Files:**
- 修改: `Protocol/CAN/can_proto.h`

- [ ] **Step 1: 在文件末尾 `#endif` 前添加错误命令和错误码定义**

在 `#define CAN_PROTO_MAX_DATA_PER_FRAME 5` 之后、`#endif` 之前添加：

```c
/* 错误命令 */
#define CAN_PROTO_CMD_UPDATE_ERR   0x84   /* A -> Host, payload: error_code(1B) */

/* 错误码 */
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_FLASH_WRITE   0x02  /* W25Q16 写入失败 */
#define OTA_ERR_EEPROM_WRITE  0x03  /* EEPROM 写入失败 */
```

- [ ] **Step 2: 验证修改正确**

确认 `can_proto.h` 完整内容为：

```c
#ifndef __CAN_PROTO_H__
#define __CAN_PROTO_H__

#include <stdint.h>

/* CAN ID */
#define CAN_PROTO_ID_A      0x000   /* A ID */
#define CAN_PROTO_ID_HOST   0x001   /* Host ID */

/* CMD */
#define CAN_PROTO_CMD_UPDATE_REQ   0x01   /* A -> Host */
#define CAN_PROTO_CMD_UPDATE_ACK   0x81   /* Host -> A, payload: size(4B LE) */
#define CAN_PROTO_CMD_UPDATE_DATA  0x02   /* Host -> A, payload: seq(2B LE) + data(<=5B) */
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A */
#define CAN_PROTO_CMD_UPDATE_DONE  0x83   /* A -> Host */

#define CAN_PROTO_MAX_DATA_PER_FRAME  5   /* DATA max payload bytes */

/* 错误命令 */
#define CAN_PROTO_CMD_UPDATE_ERR   0x84   /* A -> Host, payload: error_code(1B) */

/* 错误码 */
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_FLASH_WRITE   0x02  /* W25Q16 写入失败 */
#define OTA_ERR_EEPROM_WRITE  0x03  /* EEPROM 写入失败 */

#endif
```

---

### Task 2: 创建 ota_storage.h 头文件

**Files:**
- 新建: `Service/ota_storage.h`

- [ ] **Step 1: 创建头文件**

```c
#ifndef __OTA_STORAGE_H__
#define __OTA_STORAGE_H__

#include "w25q16.h"
#include "at24c02.h"
#include <stdint.h>

/*
 * OTA 存储管理模块
 *
 * 负责 W25Q16 页缓冲写入 + AT24C02 EEPROM 标志位更新。
 * 不依赖通信协议（CAN/UART），可被不同传输层复用。
 *
 * 写入流程：
 *   OTA_Storage_Init()   — 绑定驱动句柄
 *   OTA_Storage_Start()  — 重置状态 + 擦除扇区
 *   OTA_Storage_Write()  — 多次调用，内部 256B 页缓冲
 *   OTA_Storage_Finish() — 刷剩余缓冲 + 写 EEPROM 标志位
 */

#define OTA_STORAGE_PAGE_BUF_SIZE  256  /* W25Q16 页大小 */

/* EEPROM 写入地址（与 Bootloader app_bootloader.h 定义一致） */
#define OTA_EEPROM_STATUS_ADDR   0x10
#define OTA_EEPROM_KEY_ADDR      0x11
#define OTA_EEPROM_SIZE_ADDR     0x13

/* EEPROM 状态值 */
#define OTA_EEPROM_NEED_UPDATE   0x01
#define OTA_EEPROM_CHECK_KEY     0xA5

typedef struct {
    W25Q16_t   *w25q;                          /* W25Q16 设备句柄 */
    AT24C02_t  *eeprom;                        /* AT24C02 设备句柄 */
    uint32_t    flash_addr;                    /* 当前 W25Q16 写入偏移 */
    uint32_t    fw_size;                       /* 固件总大小 */
    uint8_t     page_buf[OTA_STORAGE_PAGE_BUF_SIZE]; /* 页缓冲 */
    uint16_t    buf_pos;                       /* 缓冲区填充位置 */
    uint32_t    total_recv;                    /* 已接收字节总数 */
} OTA_Storage_t;

/* 初始化：绑定驱动句柄 */
void OTA_Storage_Init(OTA_Storage_t *ctx, W25Q16_t *w25q, AT24C02_t *eeprom);

/* 开始接收：重置状态，根据 fw_size 擦除 W25Q16 扇区（4KB 对齐） */
int OTA_Storage_Start(OTA_Storage_t *ctx, uint32_t fw_size);

/* 写入数据：填页缓冲，满 256B 自动刷到 W25Q16 */
int OTA_Storage_Write(OTA_Storage_t *ctx, const uint8_t *data, uint16_t len);

/* 完成接收：刷剩余缓冲 + 写 EEPROM 标志位（原子操作） */
int OTA_Storage_Finish(OTA_Storage_t *ctx);

/* 错误复位：清空缓冲，不写 EEPROM */
void OTA_Storage_Reset(OTA_Storage_t *ctx);

#endif
```

---

### Task 3: 创建 ota_storage.c 实现

**Files:**
- 新建: `Service/ota_storage.c`

- [ ] **Step 1: 创建实现文件**

```c
#include "ota_storage.h"
#include "w25q16.h"
#include "at24c02.h"
#include <string.h>

void OTA_Storage_Init(OTA_Storage_t *ctx, W25Q16_t *w25q, AT24C02_t *eeprom)
{
    memset(ctx, 0, sizeof(OTA_Storage_t));
    ctx->w25q    = w25q;
    ctx->eeprom  = eeprom;
}

int OTA_Storage_Start(OTA_Storage_t *ctx, uint32_t fw_size)
{
    /* 重置状态 */
    ctx->flash_addr  = 0;
    ctx->fw_size     = fw_size;
    ctx->buf_pos     = 0;
    ctx->total_recv  = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);

    /* 计算需要擦除的扇区数（4KB 对齐向上取整） */
    uint32_t sectors = (fw_size + W25Q16_SECTOR_SIZE - 1) / W25Q16_SECTOR_SIZE;
    for (uint32_t i = 0; i < sectors; i++)
    {
        int ret = W25Q16_EraseSector(ctx->w25q, i * W25Q16_SECTOR_SIZE);
        if (ret != 0) return -1;
    }

    return 0;
}

/* 内部函数：将页缓冲中数据写入 W25Q16 */
static int ota_storage_flush(OTA_Storage_t *ctx)
{
    if (ctx->buf_pos == 0) return 0;

    int ret = W25Q16_Write(ctx->w25q, ctx->flash_addr,
                           ctx->page_buf, ctx->buf_pos);
    if (ret != 0) return -1;

    ctx->flash_addr += ctx->buf_pos;
    ctx->buf_pos = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
    return 0;
}

int OTA_Storage_Write(OTA_Storage_t *ctx, const uint8_t *data, uint16_t len)
{
    uint16_t offset = 0;

    while (offset < len)
    {
        uint16_t space = OTA_STORAGE_PAGE_BUF_SIZE - ctx->buf_pos;
        uint16_t chunk = (len - offset < space) ? (len - offset) : space;

        memcpy(&ctx->page_buf[ctx->buf_pos], &data[offset], chunk);
        ctx->buf_pos += chunk;
        offset += chunk;

        /* 页缓冲满，刷到 W25Q16 */
        if (ctx->buf_pos >= OTA_STORAGE_PAGE_BUF_SIZE)
        {
            int ret = ota_storage_flush(ctx);
            if (ret != 0) return -1;
        }
    }

    ctx->total_recv += len;
    return 0;
}

int OTA_Storage_Finish(OTA_Storage_t *ctx)
{
    /* 1. 刷剩余缓冲到 W25Q16 */
    int ret = ota_storage_flush(ctx);
    if (ret != 0) return OTA_ERR_FLASH_WRITE;

    /* 2. 写 EEPROM 标志位（与 Bootloader 定义一致）
     *    地址 0x10: status = 0x01 (BOOT_NEED_UPDATE)
     *    地址 0x11-0x12: key = 0xA5, 0xA5
     *    地址 0x13-0x16: fw_size (4B 小端) */
    uint8_t eeprom_data[7];
    eeprom_data[0] = OTA_EEPROM_NEED_UPDATE;
    eeprom_data[1] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[2] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[3] = (uint8_t)(ctx->fw_size);
    eeprom_data[4] = (uint8_t)(ctx->fw_size >> 8);
    eeprom_data[5] = (uint8_t)(ctx->fw_size >> 16);
    eeprom_data[6] = (uint8_t)(ctx->fw_size >> 24);

    ret = AT24C02_Write(ctx->eeprom, OTA_EEPROM_STATUS_ADDR,
                        eeprom_data, sizeof(eeprom_data));
    if (ret != 0) return OTA_ERR_EEPROM_WRITE;

    return 0;
}

void OTA_Storage_Reset(OTA_Storage_t *ctx)
{
    ctx->flash_addr = 0;
    ctx->fw_size    = 0;
    ctx->buf_pos    = 0;
    ctx->total_recv = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
}
```

---

### Task 4: 创建 app_ota_update.h 头文件

**Files:**
- 新建: `APP/app_ota_update.h`

- [ ] **Step 1: 创建头文件**

```c
#ifndef __APP_OTA_UPDATE_H__
#define __APP_OTA_UPDATE_H__

#include "can_buf.h"
#include "ota_storage.h"
#include <stdint.h>

/*
 * CAN OTA 更新状态机
 *
 * 通过 CAN 从网关接收固件数据，调用 ota_storage 写入 W25Q16，
 * 完成后写 EEPROM 标志位触发 Bootloader 更新。
 *
 * 使用方式：
 *   APP_OTA_Init()   — 初始化
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
#define OTA_WAIT_ACK_TIMEOUT  5000

/* DONE 状态等待时间（ms），之后重新请求 */
#define OTA_DONE_DELAY        2000

typedef struct {
    OTA_State_t    state;
    OTA_Storage_t  storage;
    CAN_Buf_t     *can_ctx;
    uint16_t       expect_seq;    /* 期望的下一个数据帧序号 */
    uint32_t       state_tick;    /* 进入当前状态时的 tick */
    uint8_t        error_code;    /* 错误码 */
} APP_OTA_t;

/* 初始化：绑定 CAN 上下文和存储驱动 */
void APP_OTA_Init(APP_OTA_t *ctx, CAN_Buf_t *can_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom);

/* 主循环轮询调用，处理 CAN 消息并推进状态机 */
void APP_OTA_Process(APP_OTA_t *ctx);

#endif
```

---

### Task 5: 创建 app_ota_update.c 实现

**Files:**
- 新建: `APP/app_ota_update.c`

- [ ] **Step 1: 创建实现文件**

```c
#include "app_ota_update.h"
#include "can_buf.h"
#include "can_proto.h"
#include "ota_storage.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

void APP_OTA_Init(APP_OTA_t *ctx, CAN_Buf_t *can_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom)
{
    ctx->state       = OTA_STATE_IDLE;
    ctx->can_ctx     = can_ctx;
    ctx->expect_seq  = 0;
    ctx->state_tick  = 0;
    ctx->error_code  = 0;

    OTA_Storage_Init(&ctx->storage, w25q, eeprom);
}

void APP_OTA_Process(APP_OTA_t *ctx)
{
    CAN_RxMsg_t rx_msg[3];
    uint8_t msg_count = 0;

    switch (ctx->state)
    {
    case OTA_STATE_IDLE:
        {
            /* 发送更新请求 */
            uint8_t req[1] = { CAN_PROTO_CMD_UPDATE_REQ };
            CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, req, 1);
            printf("[OTA] UPDATE_REQ sent\r\n");

            ctx->state      = OTA_STATE_WAIT_ACK;
            ctx->state_tick = HAL_GetTick();
        }
        break;

    case OTA_STATE_WAIT_ACK:
        CAN_Buf_Recv(ctx->can_ctx, rx_msg, &msg_count);

        for (uint8_t i = 0; i < msg_count; i++)
        {
            if (rx_msg[i].data[0] == CAN_PROTO_CMD_UPDATE_ACK)
            {
                /* 解析固件大小（4B 小端） */
                uint32_t fw_size = (uint32_t)rx_msg[i].data[1]
                                 | ((uint32_t)rx_msg[i].data[2] << 8)
                                 | ((uint32_t)rx_msg[i].data[3] << 16)
                                 | ((uint32_t)rx_msg[i].data[4] << 24);
                printf("[OTA] ACK received, fw_size=%lu\r\n", fw_size);

                /* 擦除 W25Q16 扇区 */
                int ret = OTA_Storage_Start(&ctx->storage, fw_size);
                if (ret != 0)
                {
                    printf("[OTA] Storage start failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                ctx->expect_seq = 0;
                ctx->state      = OTA_STATE_RECV_DATA;
                ctx->state_tick = HAL_GetTick();
                printf("[OTA] Sectors erased, receiving data...\r\n");
                break;
            }
        }

        /* 超时检查 */
        if (ctx->state == OTA_STATE_WAIT_ACK &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_WAIT_ACK_TIMEOUT)
        {
            printf("[OTA] WAIT_ACK timeout\r\n");
            ctx->error_code = OTA_ERR_SEQ_MISMATCH;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_RECV_DATA:
        CAN_Buf_Recv(ctx->can_ctx, rx_msg, &msg_count);

        for (uint8_t i = 0; i < msg_count; i++)
        {
            uint8_t cmd = rx_msg[i].data[0];

            if (cmd == CAN_PROTO_CMD_UPDATE_DATA)
            {
                /* 校验序号 */
                uint16_t seq = (uint16_t)rx_msg[i].data[1]
                             | ((uint16_t)rx_msg[i].data[2] << 8);

                if (seq != ctx->expect_seq)
                {
                    printf("[OTA] seq mismatch: got %u, expected %u\r\n",
                           seq, ctx->expect_seq);
                    ctx->error_code = OTA_ERR_SEQ_MISMATCH;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 写入数据（跳过 cmd 1B + seq 2B） */
                uint8_t dlen = rx_msg[i].rxHeader.DLC - 3;
                int ret = OTA_Storage_Write(&ctx->storage,
                                            &rx_msg[i].data[3], dlen);
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
            else if (cmd == CAN_PROTO_CMD_UPDATE_END)
            {
                printf("[OTA] END received, total_recv=%lu\r\n",
                       ctx->storage.total_recv);

                /* 刷缓冲 + 写 EEPROM */
                int ret = OTA_Storage_Finish(&ctx->storage);
                if (ret != 0)
                {
                    printf("[OTA] Storage finish failed: %d\r\n", ret);
                    ctx->error_code = (uint8_t)ret;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 发送完成确认 */
                uint8_t done[1] = { CAN_PROTO_CMD_UPDATE_DONE };
                CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, done, 1);
                printf("[OTA] UPDATE_DONE sent, EEPROM flags set\r\n");

                ctx->state      = OTA_STATE_DONE;
                ctx->state_tick = HAL_GetTick();
            }
        }

        /* 超时检查（10s 无数据） */
        if (ctx->state == OTA_STATE_RECV_DATA &&
            (HAL_GetTick() - ctx->state_tick) >= 10000)
        {
            printf("[OTA] RECV_DATA timeout\r\n");
            ctx->error_code = OTA_ERR_SEQ_MISMATCH;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_DONE:
        /* 等待 2s 后重新请求下一轮 */
        if ((HAL_GetTick() - ctx->state_tick) >= OTA_DONE_DELAY)
        {
            ctx->state = OTA_STATE_IDLE;
        }
        break;

    case OTA_STATE_ERROR:
        {
            /* 发送错误帧 */
            uint8_t err[2] = { CAN_PROTO_CMD_UPDATE_ERR, ctx->error_code };
            CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, err, 2);
            printf("[OTA] ERROR: code=0x%02X\r\n", ctx->error_code);

            OTA_Storage_Reset(&ctx->storage);
            ctx->state = OTA_STATE_IDLE;
        }
        break;
    }
}
```

---

### Task 6: 修改 main.c 集成 OTA 模块

**Files:**
- 修改: `Core/Src/main.c`

- [ ] **Step 1: 添加 include（USER CODE BEGIN Includes 区域）**

在 `USER CODE BEGIN Includes` 块内，将现有内容替换为：

```c
/* USER CODE BEGIN Includes */
#include "stm32f1xx_hal.h"
#include "can_buf.h"
#include "can.h"
#include "can_proto.h"
#include "bsp_soft_spi.h"
#include "bsp_soft_i2c.h"
#include "w25q16.h"
#include "at24c02.h"
#include "ota_storage.h"
#include "app_ota_update.h"
#include <stdio.h>
/* USER CODE END Includes */
```

- [ ] **Step 2: 替换私有变量（USER CODE BEGIN PV 区域）**

将 `static CAN_Buf_t can_ctx;` 替换为：

```c
/* USER CODE BEGIN PV */
static CAN_Buf_t can_ctx;
static W25Q16_t  w25q_dev;
static AT24C02_t eeprom_dev;
static APP_OTA_t ota_ctx;
/* USER CODE END PV */
```

- [ ] **Step 3: 修改 USER CODE BEGIN 2 区域**

将整个 `USER CODE BEGIN 2` 到 `USER CODE END 2` 的内容替换为：

```c
  /* USER CODE BEGIN 2 */
  /* 清除 Bootloader 残留的所有挂起中断，防止 __enable_irq() 触发未初始化的 ISR */
  for (uint32_t i = 0; i < 2; i++) {
      NVIC->ICER[i] = 0xFFFFFFFF;  /* 禁用所有中断通道 */
      NVIC->ICPR[i] = 0xFFFFFFFF;  /* 清除所有挂起标志 */
  }
  __enable_irq();

  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);

  printf("[APP] main() reached, all MX_Init done\r\n");

  /* CAN 初始化 */
  CAN_Buf_Init(&can_ctx, &hcan);
  printf("[APP] CAN_Buf_Init done\r\n");

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
  APP_OTA_Init(&ota_ctx, &can_ctx, &w25q_dev, &eeprom_dev);
  printf("[APP] OTA module init done\r\n");
  /* USER CODE END 2 */
```

- [ ] **Step 4: 替换主循环（USER CODE BEGIN WHILE 到 USER CODE END 3）**

将 `USER CODE BEGIN WHILE` 到 `USER CODE END 3` 的全部内容替换为：

```c
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    APP_OTA_Process(&ota_ctx);
    HAL_Delay(10);
  }
  /* USER CODE END 3 */
```

---

### Task 7: 更新 Keil 工程——添加包含路径

**Files:**
- 修改: `MDK-ARM/Project01_learn_Bootloader_led/Project01_learn_Bootloader_led.uvprojx`

当前 IncludePath（位于 `IncludePath` 标签，约第 343 行）：
```
../Core/Inc;../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy;../Drivers/STM32F1xx_HAL_Driver/Inc;../Drivers/CMSIS/Device/ST/STM32F1xx/Include;../Drivers/CMSIS/Include;../Protocol/CAN
```

- [ ] **Step 1: 在 IncludePath 末尾追加缺少的目录**

将 IncludePath 替换为（在原有路径后追加 7 个目录）：

```
../Core/Inc;../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy;../Drivers/STM32F1xx_HAL_Driver/Inc;../Drivers/CMSIS/Device/ST/STM32F1xx/Include;../Drivers/CMSIS/Include;../Protocol/CAN;../Protocol/SPI;../Protocol/I2C;../Driver/Storage;../Driver/MCU;../BSP;../Service;../APP
```

具体操作：在 `.uvprojx` 文件中搜索字符串 `../Protocol/CAN`（约第 343 行的 `<IncludePath>` 标签），将其后的 `</IncludePath>` 前插入：

```
;../Protocol/SPI;../Protocol/I2C;../Driver/Storage;../Driver/MCU;../BSP;../Service;../APP
```

即把：
```xml
<IncludePath>../Core/Inc;../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy;../Drivers/STM32F1xx_HAL_Driver/Inc;../Drivers/CMSIS/Device/ST/STM32F1xx/Include;../Drivers/CMSIS/Include;../Protocol/CAN</IncludePath>
```

改为：
```xml
<IncludePath>../Core/Inc;../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy;../Drivers/STM32F1xx_HAL_Driver/Inc;../Drivers/CMSIS/Device/ST/STM32F1xx/Include;../Drivers/CMSIS/Include;../Protocol/CAN;../Protocol/SPI;../Protocol/I2C;../Driver/Storage;../Driver/MCU;../BSP;../Service;../APP</IncludePath>
```

---

### Task 8: 更新 Keil 工程——添加源文件

**Files:**
- 修改: `MDK-ARM/Project01_learn_Bootloader_led/Project01_learn_Bootloader_led.uvprojx`

需要在 `.uvprojx` 的 `<Group>` 列表中添加源文件。有两组需要添加：

**组 1：现有 Protocol 组（已包含 can_buf.c）—— 添加 SPI/I2C 协议文件**

在 Protocol 组的 `<Files>` 节点中，在 `can_proto.h` 的 `</File>` 后追加：

```xml
        <File>
          <FileName>soft_spi.c</FileName>
          <FileType>1</FileType>
          <FilePath>../Protocol/SPI/soft_spi.c</FilePath>
        </File>
        <File>
          <FileName>soft_i2c.c</FileName>
          <FileType>1</FileType>
          <FilePath>../Protocol/I2C/soft_i2c.c</FilePath>
        </File>
```

**组 2：新建 Driver 组 —— 存储驱动**

在 Protocol 组的 `</Group>` 之后、`<Group>` 标签之间插入新组：

```xml
      <Group>
        <GroupName>Driver</GroupName>
        <Files>
          <File>
            <FileName>w25q16.c</FileName>
            <FileType>1</FileType>
            <FilePath>../Driver/Storage/w25q16.c</FilePath>
          </File>
          <File>
            <FileName>at24c02.c</FileName>
            <FileType>1</FileType>
            <FilePath>../Driver/Storage/at24c02.c</FilePath>
          </File>
          <File>
            <FileName>bsp_soft_spi.c</FileName>
            <FileType>1</FileType>
            <FilePath>../BSP/bsp_soft_spi.c</FilePath>
          </File>
          <File>
            <FileName>bsp_soft_i2c.c</FileName>
            <FileType>1</FileType>
            <FilePath>../BSP/bsp_soft_i2c.c</FilePath>
          </File>
        </Files>
      </Group>
```

**组 3：新建 Application 组 —— OTA 模块**

在 Driver 组之后插入：

```xml
      <Group>
        <GroupName>Application</GroupName>
        <Files>
          <File>
            <FileName>ota_storage.c</FileName>
            <FileType>1</FileType>
            <FilePath>../Service/ota_storage.c</FilePath>
          </File>
          <File>
            <FileName>app_ota_update.c</FileName>
            <FileType>1</FileType>
            <FilePath>../APP/app_ota_update.c</FilePath>
          </File>
        </Files>
      </Group>
```

- [ ] **Step 1: 在 Protocol 组中添加 SPI/I2C 源文件**

找到 `can_proto.h` 对应的 `</File>` 标签，在其后添加 `soft_spi.c` 和 `soft_i2c.c` 的条目。

- [ ] **Step 2: 在 Protocol 组之后添加 Driver 组**

包含 `w25q16.c`、`at24c02.c`、`bsp_soft_spi.c`、`bsp_soft_i2c.c`。

- [ ] **Step 3: 在 Driver 组之后添加 Application 组**

包含 `ota_storage.c`、`app_ota_update.c`。

---

### Task 9: 编译验证

- [ ] **Step 1: 在 Keil 中编译（F7）**

用 Keil 打开 `MDK-ARM/Project01_learn_Bootloader_led.uvprojx`，按 F7 编译。

- [ ] **Step 2: 修复编译错误（如有）**

常见问题及修复方法：

| 错误 | 原因 | 修复 |
|------|------|------|
| `cannot open source input file "xxx.h"` | IncludePath 缺少目录 | 检查 Task 7 的路径是否正确 |
| `undefined symbol xxx` | 缺少 .c 文件或链接问题 | 检查 Task 8 是否正确添加了源文件 |
| `redefinition of 'xxx'` | 重复定义 | 检查 include guard |
| `incompatible pointer type` | 类型不匹配 | 检查函数参数类型 |

- [ ] **Step 3: 确认 0 Error 编译通过**

确认 Build Output 显示 `0 Error(s), x Warning(s)`。

---

### Task 10: 硬件验证

- [ ] **Step 1: 使用 "Erase Sectors" 模式烧录**

在 Keil 中 Debug → Download 烧录。确认使用 "Erase Sectors" 模式（不会擦除 Bootloader 区域）。

- [ ] **Step 2: 串口观察启动日志**

连接 USART1（115200），观察输出：

期望看到：
```
[APP] main() reached, all MX_Init done
[APP] CAN_Buf_Init done
[APP] W25Q16 + AT24C02 init done
[APP] W25Q16 JEDEC ID: 0xEF4015
[APP] OTA module init done
[OTA] UPDATE_REQ sent
[OTA] ACK received, fw_size=...
[OTA] Sectors erased, receiving data...
[OTA] END received, total_recv=...
[OTA] UPDATE_DONE sent, EEPROM flags set
```

- [ ] **Step 3: 验证 OTA 流程完整**

确认：网关发数据 → W25Q16 写入 → EEPROM 标志位设置 → UPDATE_DONE 发送。

---

## 自检清单

- [x] **Spec 覆盖：** 状态机 5 个状态（Task 5）、W25Q16 页缓冲（Task 3）、EEPROM 标志位（Task 3）、错误帧（Task 1）、main.c 集成（Task 6）—— 均有对应 Task
- [x] **占位符扫描：** 无 TODO/TBD/"implement later"/"add error handling" 等占位符
- [x] **类型一致性：** `APP_OTA_t.storage` 类型为 `OTA_Storage_t`，所有函数签名在 .h 和 .c 中一致；`can_ctx` 类型为 `CAN_Buf_t*`，`W25Q16_t*` 和 `AT24C02_t*` 传递链路一致
