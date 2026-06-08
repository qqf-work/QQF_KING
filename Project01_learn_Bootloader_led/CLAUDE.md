# CLAUDE.md

本项目为 Claude Code 提供指导。

## 1. 项目概况

基础 UART Bootloader 学习项目。通过串口命令交互接收 bin 文件，写入内部 Flash A 区并跳转执行。

- **角色：** 基础 UART Bootloader（学习用，两区布局）
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08000000
- **Flash 布局：** B 区 Bootloader 20KB（0x08000000）+ A 区 App 44KB（0x08005000）

## 2. 目录结构与模块说明

```
APP/
  app_bootloader.c/h     -> 串口交互状态机（7 状态），当前入口
  bootloader.c/h         -> App 有效性校验、Cortex-M 跳转
  bootloader_conf.h      -> 分区参数（换芯片只改此文件前 4 个宏）
Service/
  flash_download.c/h     -> 智能擦除 + 跨帧奇数字节缓冲 + 半字写入
  ota_update.c/h         -> OTA 搬运（暂未使用）
Driver/
  MCU/flash.c/h          -> 内部 Flash 抽象层（解锁/擦/写/锁/擦除检测）
  Storage/               -> AT24C02、W25Q16（暂未使用）
  OLED/ssd1306.c/h       -> SSD1306（暂未使用）
Protocol/
  UART/uart_buf.c/h      -> DMA + IDLE 帧队列（在用）
  UART/uart_ringbuf.c/h  -> 环形缓冲区
  I2C/soft_i2c.c/h       -> 软件 I2C（暂未使用）
  SPI/soft_spi.c/h       -> 软件 SPI（暂未使用）
BSP/                     -> 引脚定义与 GPIO 初始化（暂未使用）
Debug/module_test.c/h    -> 硬件模块测试（暂未使用）
Core/                    -> CubeMX 生成：main.c、usart.c、gpio.c、dma.c
```

## 3. 核心设计要点

- **交互状态机：** `APP/app_bootloader.c` 实现 7 状态流程（IDLE -> WAIT_START -> RECV_SIZE -> TRANSFERRING -> VERIFY -> JUMP / ERROR）。文本命令阶段与 bin 数据阶段通过状态隔离区分
- **超时驱动：** RECV_SIZE 30s 超时回退 WAIT_START，TRANSFERRING 2s 超时触发 VERIFY
- **DMA 帧队列：** `uart_buf.c` 实现 DMA + IDLE 中断环形帧队列，中断写/主循环读
- **智能擦除：** `flash_download.c` 的 `next_erase_addr` 追踪已擦除位置，每页只擦一次
- **跨帧奇数字节缓冲：** `FlashDownload_t.last_byte` 缓存奇数帧末字节，下次拼接为半字
- **配置与逻辑分离：** `bootloader_conf.h` 存芯片相关参数，换芯片只改此文件

### 上电启动流程

```
上电 -> HAL/GPIO/DMA/UART 初始化 -> UART DMA 接收启动 -> AppBootloader_Init()
  -> 主循环 AppBootloader_Process():
     WAIT_START -> RECV_SIZE -> TRANSFERRING -> 2s 超时 -> VERIFY -> JUMP/ERROR
```

## 4. 已修复问题（2026-06-06 审核）

- LED 闪烁逻辑集成到主循环，不再依赖独立延时
- 交互状态机错误处理增强：ERROR 状态支持重试（3 次后停机 LED 慢闪）

## 5. 已知限制

- B/A 分区为 20KB/44KB 布局，与企业级三区布局（16KB/16KB/32KB）不同
- `flash_download.c` 直接调用 HAL Flash API 而非 `flash.c` 抽象层
- 没有 DMA 外设（相比其他项目），使用普通中断方式

## 6. 串口日志约定

串口打印仅在错误和关键状态变化时使用。日志前缀：`[DL]` Flash 下载、`[BL]` Bootloader 交互/跳转。printf 字符串使用英文（ARM Compiler V5 限制）。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，DMA1_CH5 接收 |
| LED1 | PA0 | 低电平点亮 |

### 串口命令协议

| 命令 | 格式 | 说明 |
|------|------|------|
| START | `START\r\n` | 触发进入传输模式 |
| SIZE | `SIZE:12345\r\n` | 声明固件字节数（十进制） |
| bin 数据 | 原始二进制 | SIZE 之后发送的 bin 文件帧 |
