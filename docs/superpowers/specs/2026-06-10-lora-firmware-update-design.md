# LoRa 固件更新设计文档

**日期：** 2026-06-10
**模块：** E32-433T20D（SX1278, 433MHz LoRa, UART TTL 透明传输）
**空中速率：** 9.6kbps
**传输模式：** 透明传输（Mode 0, M0=M1=GND）

## 1. 目标

将现有 CAN 通道固件更新替换为 LoRa 无线通道。新建两个工程，移植现有 Application 和 Gateway 代码，替换 CAN 通信层为 LoRa UART 通信层。Bootloader 侧完全不变。

## 2. 新建工程

| 新工程 | 模板来源 | 角色 |
|--------|----------|------|
| `Project03_Application_LoRa` | `Project02_Application` | LoRa 接收端 App（A 区 0x08008000） |
| `Project03_Gateway_LoRa` | `P00_getway_led1_hal` | LoRa 网关/Host（0x08000000） |

### 目录变化（相对于模板工程）

```
删除：Protocol/CAN/（can_buf.c/h, can_proto.h）
新增：Protocol/LoRa/
        lora_buf.c/h    USART3 DMA+IDLE 收发封装
        lora_proto.h    帧格式定义 + 命令码 + 错误码
保留：Protocol/UART/（Gateway PC 串口下载仍用 USART1）
保留：Protocol/SPI/、Protocol/I2C/
保留：APP/、Service/、Driver/、BSP/（全部复制）
```

### 不变的模块

- `ota_storage.c/h` — 协议无关的存储层（W25Q16 页缓冲 + EEPROM + CRC32 校验）
- `w25q16.c/h`、`at24c02.c/h` — 外部存储驱动
- `crc32.c/h` — 硬件 CRC32 驱动
- `flash.c/h` — 内部 Flash 抽象层
- `bootloader_conf.h` — 三区分区参数
- `bsp_soft_spi.c/h`、`bsp_soft_i2c.c/h` — 板级支持
- EEPROM 布局 — 地址 0x10 起，与 Bootloader 一致
- W25Q16 存储布局 — 从地址 0x000000 起
- Bootloader 工程 — 完全不变

## 3. LoRa 帧格式

```
偏移  字段        长度    说明
0     HEADER      1B      固定 0xAA
1     CMD         1B      命令码
2     LEN         1B      载荷长度（0~55）
3..N  PAYLOAD     LEN B   命令载荷
```

总帧长 = 3 + LEN，最大 = 3 + 55 = 58 字节 = LoRa 单包限制。

无帧内校验（依赖 LoRa 硬件 CRC + 端到端 CRC32 校验 + seq 序号检测）。

### 命令定义

| CMD | 值 | 方向 | 载荷 |
|-----|-----|------|------|
| REQ | 0x01 | App→GW | 无 |
| ACK | 0x81 | GW→App | fw_size (4B LE) |
| READY | 0x04 | App→GW | 无 |
| DATA | 0x02 | GW→App | seq (2B LE) + data (<=50B) |
| END | 0x03 | GW→App | crc32 (4B LE) |
| DONE | 0x83 | App→GW | 无 |
| ERR | 0x84 | App→GW | error_code (1B) |

### 错误码（统一在 `lora_proto.h`）

| 错误码 | 值 | 含义 |
|--------|-----|------|
| OTA_ERR_SEQ_MISMATCH | 0x01 | 序号不连续 |
| OTA_ERR_FLASH_WRITE | 0x02 | W25Q16 写入失败 |
| OTA_ERR_EEPROM_WRITE | 0x03 | EEPROM 写入失败 |
| OTA_ERR_TIMEOUT | 0x04 | 接收超时 |
| OTA_ERR_SIZE_MISMATCH | 0x05 | 接收量与声明大小不匹配 |
| OTA_ERR_CRC_MISMATCH | 0x06 | CRC32 校验不匹配 |

## 4. `lora_buf.c` 传输层接口

```c
typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t  rx_buf[64];     // DMA 接收缓冲区
    uint16_t rx_len;         // IDLE 中断记录的帧长度
    uint8_t  rx_ready;       // 完整帧就绪标志
    uint8_t  tx_buf[58];     // 发送帧构建缓冲
} LORA_Buf_t;

void LORA_Buf_Init(LORA_Buf_t *ctx, UART_HandleTypeDef *huart);
int  LORA_Buf_Send(LORA_Buf_t *ctx, uint8_t cmd, const uint8_t *payload, uint8_t len);
int  LORA_Buf_Recv(LORA_Buf_t *ctx, uint8_t *cmd, uint8_t *payload, uint8_t *len);
```

### 接收流程（DMA Normal + IDLE）

1. `LORA_Buf_Init()` 启动 DMA Normal 模式接收到 `rx_buf`（最大 64 字节）
2. LoRa 模块通过 USART3 TXD 输出数据，DMA 自动写入 `rx_buf`
3. USART3 IDLE 中断 → 记录 `rx_len = 64 - DMA_CNDTR` → 设置 `rx_ready = 1` → 重启 DMA
4. 主循环调用 `LORA_Buf_Recv()` → 校验 `0xAA` 头 → 解析 CMD/LEN/PAYLOAD → 返回

### 发送流程（UART 轮询 TX）

1. `LORA_Buf_Send()` 在 `tx_buf` 构建 `[0xAA] [CMD] [LEN] [PAYLOAD...]`
2. `HAL_UART_Transmit()` 发送（阻塞，56B @ 115200bps ≈ 4.9ms）
3. LoRa 模块收到 UART 数据后自动无线转发

### IDLE 中断处理要点

- 初始化时 IDLE 标志已置位（假中断），首次中断需检查 `rx_len > 0`
- DMA 重启：`HAL_UART_DMAStop()` → `HAL_UART_Receive_DMA()`
- IDLE ISR 需在 `stm32f1xx_it.c` 的 `USART3_IRQHandler` 中调用

## 5. App 端 OTA 状态机适配

5 状态结构不变：IDLE → WAIT_ACK → RECV_DATA → DONE → ERROR。

### 替换传输接口

```c
// CAN 版本
CAN_Buf_t *can;
// LoRa 版本
LORA_Buf_t *lora;
```

### DATA 帧载荷变化

- CAN：每帧 2B seq + 5B data = 7 字节有效载荷
- LoRa：每帧 2B seq + 50B data = 52 字节有效载荷

```c
// RECV_DATA 状态处理
seq      = payload[0] | (payload[1] << 8);
data_ptr = &payload[2];
data_len = len - 2;   // 最后一帧可能 < 50 字节
OTA_Storage_Write(&ctx->storage, data_ptr, data_len);
```

### 时序参数调整

| 参数 | CAN 版本 | LoRa 版本 |
|------|----------|-----------|
| WAIT_ACK 超时 | 5s | 5s |
| RECV_DATA 超时 | 10s | 30s |
| ERROR 退避 | 3s | 5s |
| CRC 重试次数 | 3 | 3 |
| DONE 后延时 | 100ms | 100ms |

### W25Q16 页缓冲无阻塞风险

页缓冲 256B，每 5 帧 DATA（50B×5）刷一次。W25Q16 页写入 0.7~3ms，远快于 50ms 帧间隔。

## 6. Gateway 端状态机适配

3 状态结构不变：WAIT_CMD → WAIT_READY → UPDATE_SEND。

### DATA 帧发送

```c
// UPDATE_SEND 状态
payload[0] = seq & 0xFF;
payload[1] = (seq >> 8) & 0xFF;
// 从 Flash 缓存区读取 50 字节到 payload[2..51]
LORA_Buf_Send(lora, CMD_DATA, payload, 2 + chunk_size);
HAL_Delay(50);  // 匹配空中传输速率，防止 E32 模块 512B 缓冲溢出
```

### 帧间隔调整

| 参数 | CAN 版本 | LoRa 版本 |
|------|----------|-----------|
| DATA 帧间隔 | 2ms | 50ms |
| WAIT_READY 超时 | 60s | 60s |

### 传输时间估算（32KB 固件）

```
DATA 帧：32768 / 50 = 656 帧
每帧：50ms（含空中传输）
DATA 总时间：656 × 50ms ≈ 33 秒
+ 协议开销（REQ/ACK/READY/END/DONE）：~0.5 秒
+ W25Q16 扇区擦除：~0.2 秒
+ CRC32 校验（App 端）：~0.5 秒
总计 ≈ 34 秒
```

## 7. 硬件配置

### App 端引脚分配

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200，调试 printf |
| USART3 | PB10(TX), PB11(RX) | 115200，LoRa 通信（DMA1_CH3 RX） |
| LED1 | PA0 | 低电平点亮 |
| LED2 | PA1 | 低电平点亮 |
| W25Q16 | PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI) | 软件 SPI |
| AT24C02 | PB8(SCL), PB9(SDA) | 软件 I2C |

### Gateway 端引脚分配

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200，调试 printf + PC 串口下载 |
| USART3 | PB10(TX), PB11(RX) | 115200，LoRa 通信（DMA1_CH3 RX） |

### E32-433T20D 连接

| E32 引脚 | 连接到 MCU | 说明 |
|----------|-----------|------|
| VCC | 3.3V 或 5V | 电源（5V 保证满功率） |
| GND | GND | 地 |
| RXD | PB10 (USART3_TX) | 模块接收 = MCU 发送 |
| TXD | PB11 (USART3_RX) | 模块发送 = MCU 接收 |
| M0 | GND | Normal 模式 |
| M1 | GND | Normal 模式 |
| AUX | 悬空 | 可选，当前不使用 |

### CubeMX 配置步骤（两个工程相同）

1. 启用 USART3：Asynchronous, 115200, 8N1
2. 启用 USART3 DMA RX：DMA1_CH3, Normal 模式
3. 启用 USART3 全局中断（NVIC，用于 IDLE 检测）
4. 删除 CAN 外设
5. 其余（USART1, CRC, GPIO）保持不变

### 初始化顺序（App 端）

```
HAL_Init → SystemClock → MX_GPIO → MX_DMA → MX_USART1_UART → MX_USART3_UART → MX_CRC
→ NVIC 全量清理 + __enable_irq()
→ LED 初始化
→ LORA_Buf_Init(&lora, &huart3)
→ BSP_SoftSPI_Init / BSP_SoftI2C_Init
→ W25Q16_Init / AT24C02_Init
→ APP_OTA_Init(&ota, &lora, &storage)
```

## 8. CAN 与 LoRa 对比

| 方面 | CAN | LoRa |
|------|-----|------|
| 传输层 | can_buf.c（CAN 轮询） | lora_buf.c（USART3 DMA+IDLE） |
| DATA 载荷 | 5 字节 | 50 字节 |
| 帧间隔 | 2ms | 50ms |
| 传输时间（32KB） | ~17 秒 | ~34 秒 |
| 帧格式 | CAN 硬件（ID+DLC+Data） | 软件帧（0xAA+CMD+LEN+PAYLOAD） |
| 寻址 | CAN ID（0x000/0x001） | 不需要（P2P 透明） |
| 错误检测 | CAN 硬件 CRC | LoRa 硬件 CRC + 端到端 CRC32 |
| 通信距离 | ~数十米（取决于总线） | 数百米~数公里 |
