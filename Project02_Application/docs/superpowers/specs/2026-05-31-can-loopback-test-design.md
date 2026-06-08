# CAN 自发自收测试模块设计

## 背景

Project02_Application 已通过 CubeMX 配置 CAN1（Silent Loopback, 200kbps, PA11/PA12），但缺少收发逻辑和过滤器配置。需要封装可复用的 CAN 收发模块，用于单机 Loopback 自测。

## 方案选择

**Protocol 层封装**（`Protocol/CAN/`），与现有 `Protocol/UART/uart_buf` 风格一致。CAN 是通信协议，放在 Protocol 层语义正确。不直接操作 HAL CAN API，调用方通过 `CAN_Buf_t` 上下文 + 三个函数完成收发。

## 模块结构

```
Protocol/CAN/can_buf.h   — 接口定义
Protocol/CAN/can_buf.c   — 实现
```

Keil 工程新增 `Protocol` 源文件组（`can_buf.c`），include path 添加 `../Protocol/CAN`。

## API 设计

```c
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
```

### CAN_Buf_Init

1. 配置 `CAN_FilterTypeDef`：Mask 模式，ID=0, Mask=0（接收所有标准帧），绑定 FIFO0
2. `HAL_CAN_ConfigFilter(hcan, &filter)` 应用过滤器
3. `HAL_CAN_Start(hcan)` 启动 CAN 外设

### CAN_Buf_Send

1. 填充 `CAN_TxHeaderTypeDef`：StdId=id, RTR=CAN_RTR_DATA, IDE=CAN_ID_STD, DLC=len
2. `HAL_CAN_AddTxMessage(ctx->hcan, &tx_hdr, data, &ctx->tx_mailbox)`
3. 返回 HAL 状态

### CAN_Buf_Recv

1. `HAL_CAN_GetRxFifoFillLevel(ctx->hcan, CAN_RX_FIFO0)` 检查 FIFO 中帧数
2. 无数据返回 `HAL_TIMEOUT`
3. 有数据：`HAL_CAN_GetRxMessage` 提取 RxHeader 和数据，输出 id 和 len
4. 返回 `HAL_OK`

## main.c 集成

所有代码写在 USER CODE 块内，不修改 CubeMX 生成区域。

```c
/* USER CODE BEGIN Includes */
#include "can_buf.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
static CAN_Buf_t can_ctx;
/* USER CODE END PV */

/* USER CODE BEGIN 2 */
CAN_Buf_Init(&can_ctx, &hcan);
/* USER CODE END 2 */
```

主循环中：
- 每 500ms 发送一帧固定数据
- 轮询接收，收到后 printf 打印 ID 和数据
- LED 翻转保留

## 依赖

- `can.h`（CubeMX 生成，提供 `hcan` 外部声明和 `MX_CAN_Init`）
- `stm32f1xx_hal_can.h`（HAL CAN 驱动）

## 不做的

- 不封装中断接收
- 不管理 CAN 时钟/GPIO/MSP（CubeMX 负责）
- 不支持扩展 ID（当前需求只用标准 ID）
