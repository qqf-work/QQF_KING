# CLAUDE.md

本项目为 Claude Code 提供指导。

## 1. 项目概况

企业级三区 Bootloader。上电检测 PB0 按键：按住则直接跳转到出厂区（0x08004000），未按则读取 EEPROM 标志判断是否需要从 W25Q16 更新固件，更新完成后跳转 A 区 App。

- **角色：** 企业级 Bootloader，三区 Flash 管理 + OTA 搬运 + 出厂恢复
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08000000
- **Flash 布局：** Bootloader 16KB + 出厂程序 16KB + A 区 App 32KB

## 2. 目录结构与模块说明

```
APP/
  app_bootloader.c/h     -> Bootloader 业务：EEPROM 检查、W25Q16 搬运、出厂跳转
  bootloader.c/h         -> App 有效性校验、Cortex-M 统一跳转函数
  bootloader_conf.h      -> 三区分区参数（换芯片只改前 5 个宏）
Service/
  flash_download.c/h     -> 智能擦除 + 跨帧奇数字节缓冲 + 半字写入
Middleware/              -> 预留目录
Driver/
  MCU/flash.c/h          -> 内部 Flash 抽象层（解锁/擦/写/锁/擦除检测）
  Storage/
    at24c02.c/h          -> EEPROM 256B（软件 I2C）
    w25q16.c/h           -> SPI Flash 2MB（软件 SPI）
  OLED/ssd1306.c/h       -> SSD1306 OLED（暂未使用）
Protocol/
  UART/uart_buf.c/h      -> DMA + IDLE 帧队列
  UART/uart_ringbuf.c/h  -> 环形缓冲区
  I2C/soft_i2c.c/h       -> 软件 I2C（AT24C02 使用）
  SPI/soft_spi.c/h       -> 软件 SPI（W25Q16 使用）
BSP/
  bsp_button.c/h         -> PB0 EXTI0 按键（HAL 回调模式）
  bsp_soft_i2c.c/h       -> 软件 I2C 引脚初始化（PB8/PB9）
  bsp_soft_spi.c/h       -> 软件 SPI 引脚初始化（PA5/PA6/PA7）
Debug/module_test.c/h    -> 硬件模块测试（暂未使用）
Core/                    -> CubeMX 生成：main.c、usart.c、gpio.c、dma.c
```

## 3. 核心设计要点

### 出厂恢复（直接跳转，非复制）

按住 PB0 时直接跳转到出厂区（`Bootloader_JumpToApp(FACTORY_REGION_ADDR)`），设置 VTOR = 0x08004000。出厂程序按出厂区地址编译链接，直接运行。

### Flash 三区布局

```
0x08000000  Bootloader  16KB  (页 0~15)
0x08004000  出厂程序    16KB  (页 16~31)，出厂时预烧录
0x08008000  A区 App     32KB  (页 32~63)，正常运行/更新目标
```

分区参数在 `APP/bootloader_conf.h`，换芯片只改前 5 个宏。

### 上电启动流程

```
上电 -> HAL/GPIO/DMA/UART 初始化 -> UART_DMA_Rx_Init
  -> BSP_Button_Init()（PB0 EXTI 就绪）
  -> BSP_SoftI2C_Init() + AT24C02_Init()
  -> BSP_SoftSPI_Init() + W25Q16_Init()

  -> BSP_Button_Pressed()?
     YES -> App_bootloader_factory_reset()
              -> Bootloader_JumpToApp(FACTORY_REGION_ADDR)  直接跳转出厂区
     NO  -> App_bootloader_check_update()   读 EEPROM 校验密钥
            -> BOOT_NO_UPDATE -> App_bootloader_jump_app()
            -> 其他           -> App_bootloader_update()    W25Q16->Flash 搬运
                                -> App_bootloader_jump_app()
```

更新失败时停机，用户按住 PB0 + 复位可跳转出厂程序。

### W25Q16 -> Flash 搬运前校验

`verify_w25q_firmware()` 在擦除/写入 A 区前，先读取 W25Q16 固件头 8 字节校验 MSP 和 ResetHandler 合法性，防止损坏固件覆盖 A 区有效 App。

### 统一跳转函数

`Bootloader_JumpToApp(addr)` 接受区域基地址，自动根据地址判断区域边界校验 ResetHandler。跳转序列：禁中断 -> 停 SysTick -> HAL_DeInit -> NVIC 全量清理 -> 设 VTOR -> 设 MSP -> 跳转。

### EEPROM 布局（AT24C02, 地址 0x10 起）

| 地址 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0x10 | 1 | status | 0x00=无需更新, 0x01=需要更新, 0x02=强制更新 |
| 0x11 | 1 | key_high | 密钥高字节 |
| 0x12 | 1 | key_low | 密钥低字节 |
| 0x13 | 4 | fw_size | 固件大小（小端序，字节） |

密钥 0xA5A5 校验 EEPROM 数据有效性。读取后自动重置密钥为 0x0000（一次性触发）。

## 4. 已修复问题（2026-06-06 审核）

- 出厂恢复从"出厂区->A区复制"简化为"直接跳转出厂区"，出厂程序按出厂区地址（0x08004000）编译
- 跳转序列增加 NVIC 全量清理（禁用所有中断通道 + 清除所有挂起标志），防止残留中断影响被跳转程序
- `Bootloader_JumpToApp()` 改为统一接口，接受区域基地址参数，自动判断区域边界
- W25Q16 搬运前增加 `verify_w25q_firmware()` 固件头校验（MSP + ResetHandler）
- 搬运缓冲区大小改为 `FLASH__PAGE_SIZE`（1KB），与 Flash 页对齐

## 5. 已知限制

- 搬运前校验仅检查固件头 8 字节，非 CRC32 全量校验
- W25Q16 JEDEC ID 检查为固定值 0xEF4015，换 Flash 型号需修改
- 按键检测使用 EXTI 下降沿 + 电平双检，无去抖逻辑
- 外部存储使用软件 I2C/SPI，速度较慢

## 6. 串口日志约定

串口打印仅在错误和关键状态变化时使用。日志前缀：`[BL]` Bootloader 跳转/EEPROM 操作、`[OTA]` W25Q16 搬运、`[DL]` Flash 下载。printf 字符串使用英文（ARM Compiler V5 限制）。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，DMA1_CH5 接收 |
| LED1 | PA0 | 低电平点亮 |
| PB0 按键 | PB0 | EXTI0 下降沿，内部上拉，按住=低电平 |
| 软件 I2C | PB8(SCL), PB9(SDA) | 连接 AT24C02 |
| 软件 SPI | PA5(SCK), PA6(MISO), PA7(MOSI) | 连接 W25Q16 |
| W25Q16 CS | PA4 | 低电平有效 |

### 初始化顺序

1. `HAL_Init` / `SystemClock_Config` / `MX_GPIO_Init`
2. `MX_DMA_Init` -- 必须在 UART 之前
3. `MX_USART1_UART_Init`
4. `UART_DMA_Rx_Init`
5. `BSP_Button_Init()` -- 尽早检测按键
6. `BSP_SoftI2C_Init()` + `AT24C02_Init()`
7. `BSP_SoftSPI_Init()` + `W25Q16_Init()`
8. 按键检测 -> 出厂跳转 或 EEPROM 判断 -> 更新/跳转

### 关联工程

| 角色 | 工程路径 | ROM 起始 | VTOR |
|------|----------|----------|------|
| 出厂程序 | `Project02_factory_app` | 0x08004000 | Bootloader 跳转前设置 |
| A 区 App | `Project02_Application` | 0x08008000 | `system_stm32f1xx.c` 中设置 |
