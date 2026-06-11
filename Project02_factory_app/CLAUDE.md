# CLAUDE.md

本项目为 Claude Code 提供指导。

## 1. 项目概况

出厂恢复程序，运行在出厂区（0x08004000）。由 Bootloader（`Project02_enterprise_bootloader`）在按住 PB0 时直接跳转执行。功能：通过串口接收固件 `.bin` 文件，写入 A 区 Flash（0x08008000），校验通过后跳转到 A 区 App。

- **角色：** 出厂恢复程序（运行在出厂区）
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08004000（出厂区）
- **目标写入区：** 0x08008000（A 区，32KB）

## 2. 目录结构与模块说明

```
APP/
  app_bootloader.c/h     -> 串口下载交互状态机（START/SIZE/bin/verify/jump）
  bootloader.c/h         -> A 区 App 有效性校验、跳转
  bootloader_conf.h      -> 三区分区参数（与 Bootloader 工程一致）
Service/
  flash_download.c/h     -> 智能擦除 + 跨帧奇数字节缓冲 + 半字写入
Driver/
  MCU/flash.c/h          -> 内部 Flash 抽象层
  Storage/
    at24c02.c/h          -> EEPROM（暂未使用）
    w25q16.c/h           -> SPI Flash（暂未使用）
  OLED/ssd1306.c/h       -> SSD1306（暂未使用）
Protocol/
  UART/uart_buf.c/h      -> DMA + IDLE 帧队列
  UART/uart_ringbuf.c/h  -> 环形缓冲区
  I2C/soft_i2c.c/h       -> 软件 I2C（暂未使用）
  SPI/soft_spi.c/h       -> 软件 SPI（暂未使用）
BSP/
  bsp_soft_i2c.c/h       -> 软件 I2C 引脚初始化（暂未使用）
  bsp_soft_spi.c/h       -> 软件 SPI 引脚初始化（暂未使用）
  bsp_button.c/h         -> PB0 按键驱动（暂未使用，出厂程序不需要按键检测）
Debug/module_test.c/h    -> 硬件模块测试（暂未使用）
Core/                    -> CubeMX 生成
```

## 3. 核心设计要点

### 出厂程序串口下载协议

1. 发送 `START` -> 进入接收模式
2. 发送 `SIZE:<十进制字节数>` -> 声明固件大小
3. 发送 `.bin` 原始数据 -> 逐帧写入 A 区 Flash
4. 2 秒无新帧 -> 自动校验字节数 -> 跳转 A 区

### 交互状态机

`APP/app_bootloader.c` 实现 7 状态流程：`IDLE -> WAIT_START -> RECV_SIZE -> TRANSFERRING -> VERIFY -> JUMP / ERROR`

- **超时机制：** RECV_SIZE 30s 超时回退 WAIT_START，TRANSFERRING 2s 无数据超时触发 VERIFY
- **错误恢复：** ERROR 状态自动重试（NVIC_SystemReset），3 次后停机 LED 慢闪
- **校验：** VERIFY 状态比较 `FlashDownload_GetTotal()` + `last_byte_flag` 与 `expected_size`

### 启动流程

```
Bootloader 检测 PB0 按住 -> 跳转出厂区 0x08004000
  -> HAL_Init + 外设初始化
  -> __enable_irq()（Bootloader 跳转前 __disable_irq()）
  -> UART_DMA_Rx_Init + AppBootloader_Init
  -> 主循环: AppBootloader_Process() + LED 闪烁
```

### VTOR 配置

Bootloader 跳转前已设置 `SCB->VTOR = 0x08004000`，出厂程序内部无需再设置。

## 4. 已修复问题（2026-06-06 审核）

- `__enable_irq()` 放在 `USER CODE BEGIN Init` 区域，确保 Bootloader 跳转后中断正常恢复
- VERIFY 校验增加 `last_byte_flag` 补偿，确保奇数字节场景校验正确
- ERROR 状态增加 3 次重试机制（超限后 LED 慢闪停机）

## 5. 已知限制

- VTOR 由 Bootloader 跳转前设置，出厂程序自身不设置 VTOR
- 只支持 A 区地址写入，不支持写入其他区域
- 校验仅比较字节数，无 CRC 校验
- 暂未使用 EEPROM/W25Q16/OLED 等模块

## 6. 串口日志约定

串口打印仅在错误和关键状态变化时使用。日志前缀：`[BL]` 串口交互/跳转。printf 字符串使用英文（ARM Compiler V5 限制）。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，DMA1_CH5 接收 |
| LED1 | PA0 | 低电平点亮，500ms 翻转闪烁 |
