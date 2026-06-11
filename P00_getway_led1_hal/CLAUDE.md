# CLAUDE.md

本项目为 Claude Code 提供指导。

## 1. 项目概况

CAN 固件更新上位机（网关/Host），接收 PC 端串口下发的 `.bin` 固件存储到内部 Flash，再通过 CAN 转发给目标设备（`Project02_Application`）。

- **角色：** CAN 网关（Host），固件分发节点
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08000000
- **Flash 布局：** P00 应用区 16KB（0x08000000，页 0~15）+ 固件缓存区 48KB（0x08004000，页 16~63）
- **对端工程：** `Project02_Application`（共享 `can_proto.h` 协议定义）

数据流：
1. PC 串口发送 `.bin` -> 存储到 Flash 缓存区（0x08004000）
2. 目标设备通过 CAN 请求更新 -> 网关从 Flash 读取并通过 CAN 逐帧发送

## 2. 目录结构与模块说明

```
APP/
  app_bootloader.c/h    -> UART 串口下载状态机（START/SIZE/bin/verify）
  app_update.c/h        -> CAN 转发状态机（等待 REQ -> 发送固件）
  fw_cache_conf.h       -> Flash 分区配置（缓存区地址与大小）
Protocol/CAN/
  can_buf.c/h           -> CAN 轮询收发封装
  can_proto.h           -> CAN 更新协议定义（与 Application 共享）
Protocol/UART/
  uart_buf.c/h          -> UART DMA + IDLE 帧队列
Service/
  flash_download.c/h    -> 智能擦除 + 跨帧奇数字节缓冲 + 半字写入
Driver/MCU/
  flash.c/h             -> 内部 Flash 抽象层（解锁/擦/写/锁）
  crc32.c/h             -> 硬件 CRC32 驱动（三项工程共用）
Core/Src/
  main.c                -> 入口，编排 UART 下载与 CAN 转发
  usart.c               -> USART1 初始化（CubeMX，DMA RX）
  can.c                 -> CAN1 初始化（200kbps，Normal 模式）
  dma.c                 -> DMA 初始化（CubeMX）
  stm32f1xx_it.c        -> 中断处理（USART1 IDLE, DMA1 Ch5）
```

## 3. 核心设计要点

- **双状态机：** UART 下载状态机（`app_bootloader.c`）和 CAN 转发状态机（`app_update.c`）。当前 UART 下载已注释（固件已烧录到 Flash），仅 CAN 转发状态机在运行
- **CRC32 预计算：** 收到 REQ 后立即调用 `CRC32_Calculate()` 对 Flash 缓存区固件计算 CRC32，结果随 `UPDATE_END` 帧发送（载荷 = 4 字节小端 CRC32）
- **CAN 转发 3 状态：** `APP_WAIT_UPDATE_CMD` -> 收到 REQ 预计算 CRC + 发 ACK -> `APP_WAIT_READY` -> 收到 READY -> `APP_UPDATE_SEND` -> 发完 DATA + END -> `APP_WAIT_UPDATE_CMD`
- **CRC 随 END 发送：** `send_end()` 发送 5 字节帧：`[0x03] [crc32 4B LE]`，CRC 在收到 REQ 时预计算
- **READY 流控：** 目标设备擦除 W25Q16 完成后发 READY，网关收到才开始发 DATA，避免 FIFO 溢出。WAIT_READY 有 60 秒超时保护
- **帧间延时：** 2ms/帧，防止目标 CAN FIFO（3 帧）溢出
- **CAN 过滤器：** 只接收 CAN ID 0x000（目标设备 ID）
- **Flash 缓存区保护：** `FlashDownload_WriteFrame()` 内置溢出检测，拒绝写入超出缓存区范围的地址

### 主循环

```c
while (1) {
    /* AppBootloader_Process(&bl_ctx); -- 已注释，固件已下载 */
    AppUpdate_Poll(&update_ctx);
}
```

当前 `AppBootloader_Init/Process` 已注释，`AppUpdate_Init` 直接使用 Flash 中已存储的 1344 字节固件。如需重新下载，取消注释并设置 `fw_size=0`。

### CAN 协议

| 命令 | 值 | 方向 | CAN ID | 载荷 |
|------|-----|------|--------|------|
| UPDATE_REQ | 0x01 | A->Host | 0 | 无 |
| UPDATE_ACK | 0x81 | Host->A | 1 | size (4B LE) |
| UPDATE_READY | 0x04 | A->Host | 0 | 无 |
| UPDATE_DATA | 0x02 | Host->A | 1 | seq (2B LE) + data (<=5B) |
| UPDATE_END | 0x03 | Host->A | 1 | crc32 (4B LE) |
| UPDATE_DONE | 0x83 | A->Host | 0 | 无 |

## 4. 已修复问题（2026-06-08 CRC32 校验）

- 新增硬件 CRC32 驱动（`Driver/MCU/crc32.c/h`），CubeMX 已配置 CRC 外设
- `send_end()` 载荷从 1 字节扩展为 5 字节（命令 + CRC32 4 字节小端）
- `AppUpdate_WaitCmd` 收到 REQ 后预计算 CRC32，存入 `fw_crc` 字段
- 新增 `OTA_ERR_CRC_MISMATCH` 等错误码到 `can_proto.h`

**完整错误码定义（P00 端 `can_proto.h` 定义 3 个，Application 端 `ota_storage.h` 额外定义 3 个）：**
- `0x01` SEQ_MISMATCH / `0x04` TIMEOUT / `0x06` CRC_MISMATCH（`can_proto.h`）
- `0x02` FLASH_WRITE / `0x03` EEPROM_WRITE / `0x05` SIZE_MISMATCH（`ota_storage.h`，P00 不感知）

## 已修复问题（2026-06-06 审核）

- CAN 转发状态机增加 `APP_WAIT_READY` 状态，实现 READY 流控
- `AppUpdate_Init` 增加空指针保护和 `fw_size=0` 保护
- `FlashDownload_WriteFrame()` 增加缓存区溢出保护
- `AppUpdate_WaitReady` 增加 60 秒超时保护

## 5. 已知限制

- 固件大小（1344 字节）硬编码在 `main.c` 中，重新下载固件需手动修改
- CAN 通信为轮询模式，无 CAN 中断
- printf 使用轮询发送，会阻塞 CPU
- `can_proto.h` 需手动与 `Project02_Application` 保持同步
- UART bootloader 已注释但 DMA 仍在运行，浪费资源
- 收到 UPDATE_ERR 后无重试逻辑，状态机回到 WAIT_CMD 被动等待

## 6. 开发约束

- **Keil 烧录：** 必须使用 Erase Sectors，不能用 Erase Full Chip（否则会擦掉 Flash 缓存区的固件）
- **新建 .c 文件：** 需手动添加到 Keil 工程并配置 include paths（不会自动发现）
- **CubeMX 代码保护：** `Core/Src/` 下的自定义代码只能写在 `/* USER CODE BEGIN */` / `/* USER CODE END */` 标记内，标记外代码会被覆盖
- **CubeMX CRC 外设：** 已在 .ioc 中启用 CRC，`main.c` 中调用 `MX_CRC_Init()`
- **编译器：** ARM Compiler V5，printf 字符串不能用中文（UTF-8 不支持）
- **对端工程：** `../Project02_Application/`，共享 `can_proto.h`，运行地址 0x08008000

## 7. 串口日志约定

串口打印仅在错误和关键状态变化时使用。日志前缀：`[Host]` 上位机操作（含 CRC 计算结果）、`[DL]` Flash 下载。printf 字符串使用英文（ARM Compiler V5 限制）。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，DMA1_CH5 接收 |
| CAN1 | PA11(RX), PA12(TX) | 200kbps，需外部 CAN 收发器 |

### 初始化顺序

`HAL_Init -> SystemClock_Config -> MX_GPIO_Init -> MX_DMA_Init -> MX_CAN_Init -> MX_USART1_UART_Init -> MX_CRC_Init -> CAN_Buf_Init -> AppUpdate_Init(Flash数据, 1344) -> UART_DMA_Rx_Init`

注意：`MX_DMA_Init()` 必须在 `MX_USART1_UART_Init()` 之前调用。

## 8. Keil 工程配置

添加源文件时需配置：
- 源文件分组：`APP`、`Protocol/CAN`、`Protocol/UART`、`Service`、`Driver/MCU`
- Include paths：`../APP`、`../Protocol/CAN`、`../Protocol/UART`、`../Service`、`../Driver/MCU`
