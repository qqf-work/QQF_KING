# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

STM32F103C8 App 端工程，通过 E32-433T20D LoRa 模块（USART3 透明传输，9.6kbps 空中速率）接收网关推送的固件数据，写入 W25Q16 SPI Flash，CRC32 校验通过后写 AT24C02 EEPROM 标志触发系统复位，由 Bootloader 从 W25Q16 搬运固件到内部 Flash A 区完成升级。

本工程是 Project02_Application 的 LoRa 无线变体，替代 CAN 有线通信，存储层（ota_storage）和 Bootloader 侧完全复用。

## 构建与烧录

- **IDE：** Keil MDK-ARM 5，ARM Compiler V5（非 V6）
- **工程文件：** `MDK-ARM/Project03_Application_LoRa.uvprojx`
- **编译：** Keil 打开 `.uvprojx`，F7
- **烧录：** ST-Link，Keil Debug -> Download，**必须用 "Erase Sectors"**（Erase Full Chip 会擦除其他区域）
- **输出：** `.hex` + `.bin` 在 `MDK-ARM/Project03_Application_LoRa/`
- **新建 .c 文件：** 必须手动添加到 Keil 工程分组中（不会自动发现）；`.h` 不需要加入工程
- **Keil 分组命名：** 与目录名一致（APP、BSP、Service、Driver、Protocol）

## Flash 布局

本工程实际链接地址 0x08008000（由 scatter file `Project03_Application_LoRa.sct` 和 `system_stm32f1xx.c` 的 `VECT_TAB_OFFSET=0x8000` 确定），32KB 空间。

**`bootloader_conf.h` 不一致：** 该文件定义 `B_PAGE_NUM=20`，推导 `A_REGION_ADDR=0x08005000`，与实际运行地址 0x08008000 不符。当前该文件未被任何活跃代码引用（本工程不直接操作内部 Flash），不影响运行，但未来集成时必须修正为三区布局（`B_PAGE_NUM=16 + FACTORY_PAGE_NUM=16`）。

## 分层架构

```
APP/          → OTA 状态机（app_ota_update.c/h）+ 分区配置（bootloader_conf.h）
Service/      → OTA 存储管理（ota_storage.c/h），W25Q16 页缓冲写入 + EEPROM 标志
Driver/
  MCU/        →   硬件 CRC32（crc32.c/h）
  Storage/    →   W25Q16 SPI Flash + AT24C02 EEPROM
Protocol/
  LoRa/       →   USART3 DMA+IDLE 帧收发（lora_buf.c/h）+ 协议定义（lora_proto.h）
  I2C/        →   软件 I2C
  SPI/        →   软件 SPI
BSP/          → 板级支持（软件 SPI/I2C 总线初始化）
Core/         → CubeMX 生成（main.c, usart.c, gpio.c, dma.c, crc.c 等）
Drivers/      → STM32 HAL 库（禁止修改）
```

## LoRa OTA 协议

**帧格式：** `[0xAA][CMD][LEN][PAYLOAD 0~55B]`，总帧最大 58 字节（E32-433T20D 单包限制）

| 命令码 | 方向 | 名称 | 载荷 |
|--------|------|------|------|
| 0x01 | App→GW | UPDATE_REQ | 无 |
| 0x81 | GW→App | UPDATE_ACK | 4B LE 固件大小 |
| 0x04 | App→GW | UPDATE_READY | 无（W25Q16 擦除完成通知） |
| 0x02 | GW→App | UPDATE_DATA | 2B LE seq + ≤50B 数据 |
| 0x03 | GW→App | UPDATE_END | 4B LE CRC32 |
| 0x83 | App→GW | UPDATE_DONE | 无 |
| 0x84 | App→GW | UPDATE_ERR | 1B 错误码 |

**错误码：** 0x01 序号不连续 / 0x02 W25Q16 写入失败 / 0x03 EEPROM 写入失败 / 0x04 超时 / 0x05 大小不匹配 / 0x06 CRC 不匹配

**OTA 流程：** App 发 REQ → GW 回 ACK(fw_size) → App 擦除 W25Q16 → 发 READY → GW 逐帧发 DATA(50ms间隔) → GW 发 END(CRC32) → App 回读 W25Q16 校验 CRC → 写 EEPROM → 发 DONE → 延时 100ms → NVIC_SystemReset()

## OTA 状态机

5 个状态，主循环轮询 `APP_OTA_Process()` 驱动：

```
IDLE ──REQ──> WAIT_ACK ──ACK──> RECV_DATA ──END+CRC OK──> DONE+Reset
 ^               |                   |
 |               |超时/错误          |超时/错误/seq错误
 +─── ERROR <────┴───────────────────┘
         退避5s后回 IDLE
```

**关键参数：**
- ACK 超时：5s（`OTA_WAIT_ACK_TIMEOUT`）
- DATA 超时：30s（`OTA_RECV_DATA_TIMEOUT`，LoRa 无线信道不确定性）
- 错误退避：5s（`OTA_ERROR_BACKOFF`）
- CRC 重试上限：3 次（`OTA_MAX_CRC_RETRY`），超限后清零继续尝试
- DATA 帧间隔：50ms（`LORA_DATA_FRAME_DELAY`，≥48ms 空中传输时间）

## 关键设计模式

- **DMA Normal + IDLE 帧接收：** USART3 DMA Normal 模式配合 IDLE 线空闲检测，每帧收完后 ISR 记录长度并重启 DMA，无需环形缓冲区
- **W25Q16 256B 页缓冲：** `ota_storage.c` 内部维护页缓冲，累积到 256B 后自动写入 W25Q16，减少 SPI Flash 编程次数
- **掉电安全 EEPROM 标志：** 只有 W25Q16 回读 CRC32 校验通过后才写 EEPROM 标志，中途掉电不会触发 Bootloader 更新
- **READY 流控：** App 擦除 W25Q16 后（~100ms/扇区）发 READY，网关收到后才开始发 DATA
- **NVIC 残留清理：** App 启动时清除所有 NVIC 使能和挂起标志（Bootloader 跳转前只关总闸不清通道）
- **VTOR 设置：** `system_stm32f1xx.c` 中 `VECT_TAB_OFFSET=0x8000`，将中断向量表重定位到 App 起始地址

## 硬件引脚

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 调试串口，printf 重定向 |
| USART3 | PB10(TX), PB11(RX) | 115200 连接 E32-433T20D LoRa 模块 |
| DMA1_Channel3 | - | USART3_RX DMA Normal 模式 |
| W25Q16 SPI | PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI) | 软件 SPI Mode 0 |
| AT24C02 I2C | PB8(SCL), PB9(SDA) | 软件 I2C，地址 0xA0 |
| LED1 | PA0 | 心跳指示，主循环翻转 |
| LED2 | PA1 | 状态指示（更新成功翻转） |
| CRC 外设 | - | 硬件 CRC32，多项式 0x04C11DB7 |

## EEPROM 布局

| 地址 | 内容 |
|------|------|
| 0x10 | 状态字节（0x01 = 需要更新） |
| 0x11-0x12 | 有效性密钥（0xA5A5） |
| 0x13-0x16 | 固件大小（4B LE） |
| 0x17-0x1A | CRC32（4B LE） |

## LoRa 与 CAN 版差异

| 方面 | CAN 版 (Project02) | LoRa 版 (本项目) |
|------|-------------------|-----------------|
| 传输层 | CAN 200kbps 有线 | LoRa 9.6kbps 无线 |
| 每帧数据量 | 5 字节 | 50 字节 |
| 帧间隔 | 2ms | 50ms |
| DATA 超时 | 无（CAN 可靠） | 30s |
| 协议文件 | can_proto.h | lora_proto.h |
| 缓冲层 | CAN FIFO 3帧 | USART3 DMA+IDLE |
| 物理模块 | CAN 收发器 | E32-433T20D |

## CubeMX 代码保护

`Core/Src/` 和 `Core/Inc/` 由 CubeMX 生成，自定义代码必须写在 `/* USER CODE BEGIN xxx */` / `/* USER CODE END xxx */` 标记块内。

## 编码约定

- **注释：** 中文
- **Printf 字符串：** 英文（ARM Compiler V5 不支持字符串中的中文 UTF-8）
- **日志前缀：** `[APP]` 应用层、`[OTA]` OTA 状态机
- **调试串口：** USART1，115200 波特率
- **DMA 初始化顺序：** `MX_DMA_Init()` 必须在 `MX_USART3_UART_Init()` 之前

## 已知陷阱

- **bootloader_conf.h 地址不一致：** `B_PAGE_NUM=20` 推导 `A_REGION_ADDR=0x08005000`，实际链接地址 0x08008000。当前未影响运行，未来集成需修正
- **DMA IDLE 假中断：** 初始化时 IDLE 标志已置位，`LORA_Buf_IdleHandler()` 通过检查 CNDTR 变化过滤
- **主循环禁止阻塞：** 虽然 LoRa 帧间隔 50ms 比 CAN 宽松得多，但 RECV_DATA 正常路径中仍不能有 `printf` 或 `HAL_Delay`，保持非阻塞设计
- **Keil "Erase Full Chip"：** 会擦除 Bootloader 和出厂程序区域
- **LoRa 单包限制：** 帧总长不能超过 58 字节，超过会被 E32 模块自动分包导致帧边界不可控
- **CRC 重试上限行为：** 达到 3 次上限后清零计数器并设置 `state_tick=0`，实际上会立即重发 REQ 而非放弃更新
