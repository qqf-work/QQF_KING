# CLAUDE.md

本项目为 Claude Code 提供指导。

## 1. 项目概况

UART Bootloader 变体，使用 HAL `UARTEx_ReceiveToIdle_IT` 接收串口数据并写入 Flash A 区。与 `Project01_learn_Bootloader_led` 类似但采用不同的接收方式（HAL UARTEx 接收回调 vs DMA 帧队列）。

- **角色：** UART Bootloader（学习变体，独立项目）
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08000000
- **Flash 布局：** B 区 16KB（0x08000000）+ A 区 48KB（0x08004000）

## 2. 目录结构与模块说明

```
MDK-ARM/interface/
  Init_bootloader.c/h    -> 串口接收 + Flash 写入核心逻辑
Core/
  main.c                 -> 入口，调用 Init_bootloader() 后进入主循环（5s 延时）
  usart.c                -> USART1 初始化（CubeMX）
```

注意：此项目没有 APP/、Protocol/、Service/、Driver/ 等分层目录，所有逻辑集中在 `MDK-ARM/interface/` 中。

## 3. 核心设计要点

- **接收方式：** `HAL_UARTEx_ReceiveToIdle_IT`（中断 + 接收回调），非 DMA 模式
- **Flash 写入：** 直接调用 HAL Flash API（`HAL_FLASH_Unlock/Lock/Program/Erase`），无抽象层
- **跨帧奇数字节缓冲：** `last_byte_flag` + `last_byte` 处理半字对齐
- **擦除策略：** 每次写入前检查页内是否有非 0xFF 数据，有则擦除

### 上电启动流程

```
上电 -> HAL 初始化 -> MX_GPIO_Init -> MX_USART1_UART_Init
  -> Init_bootloader()（启动 UART 接收中断）
  -> 主循环: HAL_Delay(5000)（等待串口数据通过中断接收并写入 Flash）
```

## 4. 已修复问题（2026-06-06 审核）

- 地址溢出保护：检查写入偏移不超出 A 区范围
- `HAL_UARTEx_RxEventCallback` 中增加 `uart_rec_len == 0` 保护

## 5. 已知限制

- 没有跳转 A 区 App 的逻辑（仅接收写入，不跳转执行）
- 没有 START/SIZE 握手协议，上电后直接接收
- 没有校验机制（无字节数校验、无 CRC）
- Flash 操作直接使用 HAL API，无抽象层
- 主循环仅做 `HAL_Delay(5000)`，无实际处理
- 源文件放在 `MDK-ARM/interface/` 目录，不符合标准分层架构

## 6. 串口日志约定

此项目无 printf 日志输出。接收和写入过程通过 LED 状态指示。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，中断接收 |
| LED1 | PA0 | 低电平点亮，初始化时点亮 |
