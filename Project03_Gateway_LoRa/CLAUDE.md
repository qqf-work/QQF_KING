# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

P00 网关的 LoRa 变体，通过 USART3 连接 E32-433T20D LoRa 模块（433MHz），替代 CAN 为远端 App 设备提供无线 OTA 固件分发。网关双通道工作：USART1 从 PC 串口接收 .bin 固件存入内部 Flash 缓存区，USART3 通过 LoRa 将固件分帧无线发送给 App。

MCU：STM32F103C8T6（64KB Flash, 20KB RAM, 72MHz）

## 构建与烧录

- **IDE：** Keil MDK-ARM 5，ARM Compiler V5.05（非 V6）
- **工程文件：** `MDK-ARM/Project03_Gateway_LoRa.uvprojx`
- **编译：** Keil 打开 `.uvprojx`，按 F7
- **烧录：** ST-Link，Debug -> Download，**必须使用 "Erase Sectors"** 模式（Erase Full Chip 会擦除缓存区）
- **新建 .c 文件：** 必须手动添加到 Keil 工程分组，`.h` 不需要加入
- **Keil 分组命名：** 与目录名一致（APP、Service、Driver、Protocol）

## Flash 布局

```
0x08000000  应用程序   16KB  (页 0~15)
0x08004000  固件缓存   48KB  (页 16~63)
```

分区参数集中在 `APP/fw_cache_conf.h`，换芯片只改该文件前 3 个宏。

## 硬件引脚

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200，PC 串口 + 调试 printf 输出 |
| USART3 | PB10(TX), PB11(RX) | 115200，LoRa 模块通信口 |
| LED1 | PA0 | 低电平点亮 |

注意：USART3 在 PB10/PB11，与 P00 CAN 版本（PA11/PA12）不同。

## 端到端 OTA 流程

```
PC 串口发 .bin -> USART1 DMA+IDLE -> 存 Flash 缓存区 (0x08004000)
                                              |
                                    App 发 UPDATE_REQ (LoRa)
                                              |
                                    网关预计算 CRC32 -> 回 UPDATE_ACK(含 fw_size)
                                              |
                                    App 擦除 W25Q16 -> 发 UPDATE_READY
                                              |
                                    网关逐帧发 UPDATE_DATA (50ms/帧, 50B/帧)
                                              |
                                    网关发 UPDATE_END(含 CRC32)
                                              |
                                    App 回读 W25Q16 校验 -> 发 DONE 或 ERR
```

## LoRa 协议

帧格式：`[0xAA] [CMD 1B] [LEN 1B] [PAYLOAD 0~55B]`，最大 58 字节/帧。

命令码（与 CAN 协议语义一致，编号兼容）：

| 命令 | 码 | 方向 | 载荷 |
|------|----|------|------|
| UPDATE_REQ | 0x01 | App→网关 | 无 |
| UPDATE_ACK | 0x81 | 网关→App | 4B 小端 fw_size |
| UPDATE_READY | 0x04 | App→网关 | 无 |
| UPDATE_DATA | 0x02 | 网关→App | 2B 小端 seq + ≤50B 数据 |
| UPDATE_END | 0x03 | 网关→App | 4B 小端 CRC32 |
| UPDATE_DONE | 0x83 | App→网关 | 无 |
| UPDATE_ERR | 0x84 | App→网关 | 1B 错误码 |

错误码：0x01 序号不连续、0x02 Flash 写入失败、0x03 EEPROM 写入失败、0x04 超时、0x05 大小不匹配、0x06 CRC 不匹配。

关键时序参数（`Protocol/LoRa/lora_proto.h`）：
- DATA 帧间隔：50ms（`LORA_DATA_FRAME_DELAY`）
- 单帧固件数据：50 字节（`LORA_MAX_DATA_PER_FRAME`）
- 32KB 固件约 655 帧 × 50ms ≈ 33 秒

## 目录结构

```
APP/              -> app_update.c/h (LoRa OTA 状态机)、app_bootloader.c/h (串口下载状态机)、fw_cache_conf.h
Service/          -> flash_download.c/h (Flash 写入：智能擦页 + 跨帧奇数字节缓冲)
Driver/MCU/       -> flash.c/h (内部 Flash 抽象)、crc32.c/h (硬件 CRC32)
Protocol/
  LoRa/           -> lora_buf.c/h (USART3 DMA+IDLE 帧收发)、lora_proto.h (协议常量)
  UART/           -> uart_buf.c/h (USART1 DMA+IDLE 帧队列，用于 PC 串口接收)
Core/             -> CubeMX 生成文件（main.c, usart.c, gpio.c 等）
```

## 关键架构决策

- **LoRa 单帧缓冲 vs UART 帧队列：** `lora_buf` 使用单帧缓冲（rx_ready 标志 + DMA Normal 手动重启），因为 LoRa 低速率、点对点通信，不会出现帧积压。`uart_buf` 使用环形帧队列（8 帧深度），因为 PC 串口突发数据快
- **IDLE 中断处理顺序：** `stm32f1xx_it.c` 中 USART3 的 `LORA_Buf_IdleHandler()` 在 `HAL_UART_IRQHandler()` **之前**调用，因为 HAL 处理会清除 IDLE 标志
- **全局指针 g_lora_ctx：** IDLE 中断无参数，通过全局指针访问上下文，限制为单实例（单 LoRa 模块够用）
- **CRC32 预计算：** 收到 REQ 时一次性计算整份固件 CRC（硬件 CRC <1ms），而非发送时逐帧累加
- **阻塞式发送：** DATA 帧间使用 `HAL_Delay(50ms)`，网关作为专用分发设备可接受此延迟
- **假 IDLE 中断过滤：** 初始化时 IDLE 标志已置位，`LORA_Buf_IdleHandler()` 中 CNDTR == LORA_RX_BUF_SIZE 时 recv_len=0，直接返回
- **DMA 初始化顺序：** `MX_DMA_Init()` 必须在 `MX_USARTx_UART_Init()` 之前调用

## 编码约定

- **注释：** 中文
- **Printf 字符串：** 英文（ARM Compiler V5 不支持字符串中的中文 UTF-8）
- **日志前缀：** `[Host]` 网关操作、`[BL]` 串口下载、`[DEBUG]` 调试
- **调试串口：** USART1，115200 波特率

## 跨工程同步

- **`lora_proto.h` vs `can_proto.h`：** 命令码和错误码语义一致，LoRa 版本在 `Protocol/LoRa/lora_proto.h`，修改时需确保对端 App 工程的协议定义同步
- **`crc32.c/h`：** 与 P00 CAN 版本共享相同实现，修改时需同步
- **`flash_download.c/h`、`uart_buf.c/h`：** 从 P00 CAN 版本移植，逻辑一致

## 已知限制

- **fw_size 硬编码：** `main.c` 中 `AppUpdate_Init` 传入 1344 字节，需改为从 Flash 缓存区动态读取
- **USART3 波特率：** 当前 115200，需确认是否匹配 E32 模块配置（E32 空中速率 9.6kbps，但串口波特率可独立配置）
- **无 DONE/ERR 处理：** 网关发送 END 后直接回到空闲状态，未等待 App 的 DONE/ERR 响应
- **AppBootloader 未集成到主循环：** `app_bootloader.c` 的 `AppBootloader_Process()` 存在但未在 `main.c` while(1) 中调用，串口下载功能未激活
