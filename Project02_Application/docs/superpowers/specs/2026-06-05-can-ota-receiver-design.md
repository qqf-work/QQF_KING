# CAN OTA 接收端设计

日期：2026-06-05

## 目标

在 A 区 App 中实现 CAN OTA 固件接收功能：通过 CAN 从网关（P00_getway_led1_hal）接收固件数据，保存到 W25Q16 SPI Flash，完成后更新 AT24C02 EEPROM 标志位，触发 Bootloader 在下次重启时搬运固件到内部 Flash。

## 架构

采用 2 文件分层：

- **`Service/ota_storage.c/h`** — 存储层：W25Q16 页缓冲写入 + 扇区按需擦除 + EEPROM 标志位。不依赖 CAN，可被其他传输方式复用。
- **`APP/app_ota_update.c/h`** — CAN OTA 状态机：协议解析、错误处理、调度 ota_storage。

## 状态机

```
OTA_IDLE ──[发送 UPDATE_REQ]──> OTA_WAIT_ACK
                                       │
                           [收到 UPDATE_ACK]
                                       │
                                擦除 W25Q16 扇区
                                       │
                                       ▼
                                 OTA_RECV_DATA ◄───────┐
                                       │                │
                               [收到 UPDATE_DATA]       │
                                       │                │
                                写入页缓冲              │
                                (满 256B 刷到 W25Q16)    │
                                       │                │
                               [收到 UPDATE_END]         │
                                       │                │
                                刷剩余缓冲               │
                                写 EEPROM 标志位         │
                                发 UPDATE_DONE           │
                                       │                │
                                       ▼                │
                                 OTA_DONE ──[2s 后]─────┘

任何状态出错 → OTA_ERROR → 发错误帧 → OTA_IDLE
```

5 个状态：

| 状态 | 含义 |
|------|------|
| `OTA_STATE_IDLE` | 初始态，发送 UPDATE_REQ |
| `OTA_STATE_WAIT_ACK` | 等待网关回复固件大小 |
| `OTA_STATE_RECV_DATA` | 接收数据帧并写入 W25Q16 |
| `OTA_STATE_DONE` | 更新完成，2s 后重新请求 |
| `OTA_STATE_ERROR` | 出错，发错误帧后复位 |

## OTA_Storage 模块

### 数据结构

```c
typedef struct {
    W25Q16_t   *w25q;           // W25Q16 设备句柄
    AT24C02_t  *eeprom;         // AT24C02 设备句柄
    uint32_t    flash_addr;     // 当前 W25Q16 写入偏移（从 0x000000 开始）
    uint32_t    fw_size;        // 固件总大小
    uint8_t     page_buf[256];  // 页缓冲（凑满 256B 再写入 W25Q16）
    uint16_t    buf_pos;        // 缓冲区当前填充位置
    uint32_t    total_recv;     // 已接收字节总数
} OTA_Storage_t;
```

### API

```c
// 初始化：绑定驱动句柄
void OTA_Storage_Init(OTA_Storage_t *ctx, W25Q16_t *w25q, AT24C02_t *eeprom);

// 开始接收：重置状态，根据 fw_size 擦除所需扇区
// sectors = ceil(fw_size / 4096)，一次性全部擦除
int OTA_Storage_Start(OTA_Storage_t *ctx, uint32_t fw_size);

// 写入数据：先填 page_buf，满 256B 自动调用 W25Q16_Write
int OTA_Storage_Write(OTA_Storage_t *ctx, const uint8_t *data, uint16_t len);

// 完成接收：刷剩余缓冲到 W25Q16 + 写 EEPROM 标志位
// EEPROM 布局：
//   0x10 = 0x01 (BOOT_NEED_UPDATE)
//   0x11-0x12 = 0xA5, 0xA5 (密钥)
//   0x13-0x16 = fw_size (4B 小端)
int OTA_Storage_Finish(OTA_Storage_t *ctx);

// 错误复位：清空缓冲，不写 EEPROM
void OTA_Storage_Reset(OTA_Storage_t *ctx);
```

### W25Q16 写入流程

1. `OTA_Storage_Start()` 一次性擦除所有扇区
2. 每次收到 CAN 数据帧（<=5B），`OTA_Storage_Write()` 将数据追加到 `page_buf`
3. 当 `buf_pos == 256` 时，调用 `W25Q16_Write()` 写入一整页，`flash_addr += 256`，`buf_pos = 0`
4. `OTA_Storage_Finish()` 将 `page_buf` 中剩余数据（不足 256B）也写入 W25Q16

### EEPROM 写入时机

`OTA_Storage_Finish()` 是原子操作：先确保所有 W25Q16 数据写入完成，最后才写 EEPROM。掉电保护：
- 如果 W25Q16 写入中途中断，EEPROM 没被写入 → Bootloader 不会触发更新 → 安全
- 如果 EEPROM 已写入但没来得及重启 → 重启后 Bootloader 检测到标志位，正常搬运

### W25Q16 地址

固件从 W25Q16 地址 `0x000000` 开始存储（与 Bootloader 读取地址一致）。

## APP_OTA_Update 模块

### 数据结构

```c
typedef struct {
    OTA_State_t    state;
    OTA_Storage_t  storage;
    CAN_Buf_t     *can_ctx;
    uint16_t       expect_seq;   // 期望的下一个序号
    uint32_t       done_tick;    // DONE 状态的时间戳（HAL_GetTick）
    uint8_t        error_code;   // 错误码
} APP_OTA_t;
```

### API

```c
// 初始化：绑定 CAN 上下文 + 存储驱动
void APP_OTA_Init(APP_OTA_t *ctx, CAN_Buf_t *can_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom);

// 主循环轮询调用，每次处理一帧 CAN 消息
void APP_OTA_Process(APP_OTA_t *ctx);
```

### Process 内部逻辑

```
OTA_IDLE:
  发送 UPDATE_REQ(can_ctx, CAN_PROTO_ID_A, [0x01])
  state = WAIT_ACK

OTA_WAIT_ACK:
  can_recv →
    如果 ACK(0x81):
      fw_size = 解析 4B 小端
      OTA_Storage_Start(storage, fw_size)
      expect_seq = 0
      state = RECV_DATA
    其他帧: 忽略
  超时(5s): state = ERROR

OTA_RECV_DATA:
  can_recv →
    如果 DATA(0x02):
      校验 seq == expect_seq
        不等 → error_code = 0x01, state = ERROR
      OTA_Storage_Write(storage, &data[3], dlc - 3)
      如果写入失败 → error_code = 0x02, state = ERROR
      expect_seq++
    如果 END(0x03):
      OTA_Storage_Finish(storage)
      如果失败 → error_code = 0x03, state = ERROR
      发送 UPDATE_DONE
      state = DONE
    如果 ACK(0x81): 忽略（可能是重复帧）

OTA_DONE:
  如果 (HAL_GetTick() - done_tick >= 2000):
    state = IDLE  // 重新请求下一轮

OTA_ERROR:
  发送错误帧 [0x84, error_code]
  OTA_Storage_Reset(storage)
  state = IDLE
```

## 新增协议命令

添加到 `Protocol/CAN/can_proto.h`：

```c
#define CAN_PROTO_CMD_UPDATE_ERR   0x84   /* A -> Host, payload: error_code(1B) */

/* 错误码 */
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_FLASH_WRITE   0x02  /* W25Q16 写入失败 */
#define OTA_ERR_EEPROM_WRITE  0x03  /* EEPROM 写入失败 */
```

## 与 main.c 的集成

### 新增初始化（USER CODE BEGIN 2 区域）

```c
/* 存储外设初始化 */
BSP_SoftSPI_Init();
BSP_SoftI2C_Init();

static W25Q16_t w25q_dev;
static AT24C02_t eeprom_dev;
W25Q16_Init(&w25q_dev, &spi1_bus, W25Q_CS_PORT, W25Q_CS_PIN);
AT24C02_Init(&eeprom_dev, &i2c1_bus, AT24C02_ADDR);

/* OTA 模块初始化 */
static APP_OTA_t ota_ctx;
APP_OTA_Init(&ota_ctx, &can_ctx, &w25q_dev, &eeprom_dev);
```

### 主循环（USER CODE BEGIN WHILE 区域）

替换当前的手动协议处理代码为：

```c
while (1) {
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
    APP_OTA_Process(&ota_ctx);
    HAL_Delay(10);
}
```

## 需要添加到 Keil 工程的文件

- `Service/ota_storage.c`
- `APP/app_ota_update.c`

需要在 Keil 中手动添加这两个 .c 文件到工程。

## 依赖的现有模块（已有但未编译，无需修改）

- `BSP/bsp_soft_spi.c` — SPI GPIO 初始化
- `BSP/bsp_soft_i2c.c` — I2C GPIO 初始化
- `Protocol/SPI/soft_spi.c` — 软 SPI 协议
- `Protocol/I2C/soft_i2c.c` — 软 I2C 协议
- `Driver/Storage/w25q16.c` — W25Q16 驱动
- `Driver/Storage/at24c02.c` — AT24C02 驱动

这些文件也需要添加到 Keil 工程中才能编译。

## 错误恢复策略

- 序号不连续 → 发错误帧 + 复位状态机 → 等待网关超时重发
- W25Q16 写入失败 → 发错误帧 + 复位 → 不写 EEPROM → 安全
- EEPROM 写入失败 → 发错误帧 + 复位 → W25Q16 数据可能不完整但 Bootloader 不触发 → 安全
- 5s 超时未收到 ACK → 复位 → 重新发送 UPDATE_REQ

## 掉电安全分析

| 掉电时机 | W25Q16 状态 | EEPROM 状态 | 结果 |
|----------|------------|------------|------|
| 擦除扇区后 | 已擦除（0xFF） | 未写 | 安全，无影响 |
| 接收数据中 | 部分写入 | 未写 | 安全，Bootloader 不触发 |
| W25Q16 写完、EEPROM 未写 | 完整 | 未写 | 安全，Bootloader 不触发 |
| EEPROM 已写 | 完整 | 已写 | Bootloader 检测到标志 → 搬运固件 |
| 搬运中掉电 | - | 已写 | Bootloader 下次继续（EEPROM 密钥已清除后不重复） |
