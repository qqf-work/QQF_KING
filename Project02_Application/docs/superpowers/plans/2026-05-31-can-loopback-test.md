# CAN 自发自收测试模块 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Protocol/CAN/ 下封装可复用的 CAN 轮询收发模块，main 中调用验证 Silent Loopback 自发自收。

**Architecture:** 新建 `Protocol/CAN/can_buf.c/h`，封装 HAL CAN API 为 Init/Send/Recv 三个函数。Init 负责过滤器配置和启动 CAN，Send 打包 TxHeader 发送，Recv 轮询 FIFO0 接收。所有自定义代码在 USER CODE 块内。

**Tech Stack:** STM32 HAL CAN 驱动、Keil MDK-ARM 5、ARM Compiler V5

---

### Task 1: 修复 LED2 编译缺失

main.c 引用了 `LED2_Pin`/`LED2_GPIO_Port`，但 main.h 中未定义。CubeMX 只生成了 LED1 定义。

**Files:**
- Modify: `Core/Inc/main.h:63-65`（USER CODE BEGIN Private defines 块）
- Modify: `Core/Src/gpio.c:64-66`（USER CODE BEGIN 2 块）

- [ ] **Step 1: 在 main.h 中添加 LED2 定义**

```c
/* USER CODE BEGIN Private defines */
#define LED2_Pin GPIO_PIN_2
#define LED2_GPIO_Port GPIOA
/* USER CODE END Private defines */
```

- [ ] **Step 2: 在 gpio.c 中添加 LED2 初始化**

在 `USER CODE BEGIN 2` 块内：

```c
  /* USER CODE BEGIN 2 */
  GPIO_InitStruct.Pin = LED2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED2_GPIO_Port, &GPIO_InitStruct);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
  /* USER CODE END 2 */
```

---

### Task 2: 创建 Protocol/CAN 目录和 can_buf.h

**Files:**
- Create: `Protocol/CAN/can_buf.h`

- [ ] **Step 1: 创建目录**

```bash
mkdir -p ../Protocol/CAN
```

- [ ] **Step 2: 创建 can_buf.h**

```c
#ifndef __CAN_BUF_H__
#define __CAN_BUF_H__

#include "main.h"
#include <stdint.h>

/*
 * CAN 轮询收发封装
 *
 * 对 CubeMX 生成的 hcan 句柄做二次封装：
 *   CAN_Buf_Init  — 配置过滤器 + 启动 CAN
 *   CAN_Buf_Send  — 发送一帧（标准 ID，最多 8 字节）
 *   CAN_Buf_Recv  — 轮询接收一帧
 */

#define CAN_BUF_MAX_DLC  8

typedef struct {
    CAN_HandleTypeDef *hcan;
    uint32_t           tx_mailbox;
} CAN_Buf_t;

void              CAN_Buf_Init(CAN_Buf_t *ctx, CAN_HandleTypeDef *hcan);
HAL_StatusTypeDef CAN_Buf_Send(CAN_Buf_t *ctx, uint32_t id,
                               const uint8_t *data, uint8_t len);
HAL_StatusTypeDef CAN_Buf_Recv(CAN_Buf_t *ctx, uint32_t *id,
                               uint8_t *data, uint8_t *len);

#endif
```

---

### Task 3: 创建 can_buf.c 实现

**Files:**
- Create: `Protocol/CAN/can_buf.c`

- [ ] **Step 1: 创建 can_buf.c**

```c
#include "can_buf.h"
#include "can.h"
#include <string.h>

void CAN_Buf_Init(CAN_Buf_t *ctx, CAN_HandleTypeDef *hcan)
{
    ctx->hcan = hcan;

    CAN_FilterTypeDef filter = {
        .FilterBank           = 0,
        .FilterMode           = CAN_FILTERMODE_IDMASK,
        .FilterScale          = CAN_FILTERSCALE_32BIT,
        .FilterIdHigh         = 0x0000,
        .FilterIdLow          = 0x0000,
        .FilterMaskIdHigh     = 0x0000,
        .FilterMaskIdLow      = 0x0000,
        .FilterFIFOAssignment = CAN_RX_FIFO0,
        .FilterActivation     = CAN_FILTER_ENABLE,
        .SlaveStartFilterBank = 14,
    };

    HAL_CAN_ConfigFilter(ctx->hcan, &filter);
    HAL_CAN_Start(ctx->hcan);
}

HAL_StatusTypeDef CAN_Buf_Send(CAN_Buf_t *ctx, uint32_t id,
                               const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef tx_hdr = {
        .StdId = id,
        .IDE   = CAN_ID_STD,
        .RTR   = CAN_RTR_DATA,
        .DLC   = len,
    };

    return HAL_CAN_AddTxMessage(ctx->hcan, &tx_hdr,
                                (uint8_t *)data, &ctx->tx_mailbox);
}

HAL_StatusTypeDef CAN_Buf_Recv(CAN_Buf_t *ctx, uint32_t *id,
                               uint8_t *data, uint8_t *len)
{
    if (HAL_CAN_GetRxFifoFillLevel(ctx->hcan, CAN_RX_FIFO0) == 0)
        return HAL_TIMEOUT;

    CAN_RxHeaderTypeDef rx_hdr;
    HAL_StatusTypeDef status = HAL_CAN_GetRxMessage(ctx->hcan, CAN_RX_FIFO0,
                                                     &rx_hdr, data);
    if (status != HAL_OK)
        return status;

    *id  = rx_hdr.StdId;
    *len = rx_hdr.DLC;
    return HAL_OK;
}
```

---

### Task 4: 更新 Keil 工程文件

**Files:**
- Modify: `MDK-ARM/Project01_learn_Bootloader_led.uvprojx`

- [ ] **Step 1: 添加 include path**

在 line 340 的 `<IncludePath>` 末尾追加 `../Protocol/CAN`：

```
../Core/Inc;../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy;../Drivers/STM32F1xx_HAL_Driver/Inc;../Drivers/CMSIS/Device/ST/STM32F1xx/Include;../Drivers/CMSIS/Include;../Protocol/CAN
```

- [ ] **Step 2: 添加 Protocol 源文件组**

在 `</Group>` (Drivers/CMSIS 组结束，约 line 725) 和 `<Group>` (::CMSIS 组开始，约 line 726) 之间插入：

```xml
        <Group>
          <GroupName>Protocol</GroupName>
          <Files>
            <File>
              <FileName>can_buf.c</FileName>
              <FileType>1</FileType>
              <FilePath>../Protocol/CAN/can_buf.c</FilePath>
            </File>
          </Files>
        </Group>
```

---

### Task 5: 更新 main.c 集成 CAN 测试

**Files:**
- Modify: `Core/Src/main.c`（仅 USER CODE 块）

- [ ] **Step 1: 添加 include 和变量**

在 `USER CODE BEGIN Includes` 块：

```c
#include "can_buf.h"
#include "can.h"
#include <stdio.h>
```

在 `USER CODE BEGIN PV` 块：

```c
static CAN_Buf_t can_ctx;
```

- [ ] **Step 2: 在初始化区域启动 CAN**

在 `USER CODE BEGIN 2` 块（`MX_CAN_Init()` 之后）：

```c
  CAN_Buf_Init(&can_ctx, &hcan);
  printf("[CAN] Loopback test start\r\n");
```

- [ ] **Step 3: 在主循环中添加 CAN 测试逻辑**

替换 `USER CODE BEGIN 3` 块：

```c
    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);

    /* CAN 发送测试帧 */
    uint8_t tx_data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    HAL_StatusTypeDef status = CAN_Buf_Send(&can_ctx, 0x123, tx_data, 8);
    if (status != HAL_OK)
    {
        printf("[CAN] Send failed: %ld\r\n", status);
    }

    /* 轮询接收 */
    uint32_t rx_id;
    uint8_t  rx_data[8];
    uint8_t  rx_len;
    if (CAN_Buf_Recv(&can_ctx, &rx_id, rx_data, &rx_len) == HAL_OK)
    {
        printf("[CAN]Recv ID:0x%03lX Len:%d Data:", rx_id, rx_len);
        for (uint8_t i = 0; i < rx_len; i++)
            printf(" %02X", rx_data[i]);
        printf("\r\n");
    }

    HAL_Delay(500);
    /* USER CODE END 3 */
```

---

### Task 6: Keil 编译验证

- [ ] **Step 1: 在 Keil 中按 F7 编译**

Expected: 0 Error, 0 Warning

- [ ] **Step 2: 烧录并通过串口观察输出**

Expected: 串口每隔 500ms 打印 `[CAN]Recv ID:0x123 Len:8 Data: 01 02 03 04 05 06 07 08`，LED1/LED2 闪烁。