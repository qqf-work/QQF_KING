# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

STM32F103C8 企业级 Bootloader。上电检测 PB0 按键：按住则直接跳转到出厂区（0x08004000），未按则读取 EEPROM 标志判断是否需要从 W25Q16 更新固件，更新完成后跳转 A 区 App。项目使用中文注释和日志。

## 构建与烧录

- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz（HSE 8MHz × PLL ×9）
- **IDE：** Keil MDK-ARM 5（ARM Compiler V5.05）
- **构建：** 用 Keil 打开 `MDK-ARM/Project01_learn_Bootloader_led.uvprojx`，按 F7 编译
- **输出：** `.hex` + `.bin` 生成在 `MDK-ARM/Project01_learn_Bootloader_led/` 目录
- **烧录：** 通过 ST-Link 在 Keil 中下载（Debug → Download）
- **注意：** Bootloader、出厂程序、App 是独立 Keil 工程，烧录时都须用 "Erase Sectors" 模式，避免擦除其他区域
- **调试串口：** `printf` 重定向到 USART1（115200 波特率），所有 `[DL]`/`[BL]`/`[OTA]`/`[DEBUG]` 日志走串口输出
- **空间约束：** Bootloader 区域仅 16KB，代码体积紧张。Keil 编译优化建议使用 `-Os`（Level 2，优先体积优化）

## CubeMX 代码保护

`Core/Src/` 和 `Core/Inc/` 中的文件由 CubeMX 生成，包含 `/* USER CODE BEGIN xxx */` / `/* USER CODE END xxx */` 标记。所有自定义代码必须写在这些标记块内，否则重新生成时会被覆盖。不要修改标记外的代码。

## Flash 三区布局

分区参数定义在 `APP/bootloader_conf.h`，换芯片只改该文件（前 5 个宏）。

```
0x08000000  ┌──────────────────┐
            │  Bootloader 16KB │ 16页 (page 0~15)
0x08004000  ├──────────────────┤
            │  出厂程序 16KB   │ 16页 (page 16~31)，出厂时预烧录
0x08008000  ├──────────────────┤
            │  A区 App 32KB    │ 32页 (page 32~63)，正常运行/更新目标
0x08010000  └──────────────────┘
```

**出厂程序编译地址：** 出厂程序的 .bin 按出厂区地址 (0x08004000) 编译链接，直接存储并运行于出厂区。恢复出厂时 Bootloader 直接跳转到出厂区（`Bootloader_JumpToApp(FACTORY_REGION_ADDR)`），设置 VTOR = 0x08004000。

**RAM：** `0x20000000–0x20004FFF`（20KB）

**Scatter 文件：** `MDK-ARM/Project01_learn_Bootloader_led/Project01_learn_Bootloader_led.sct`

## 上电启动流程

```
上电 → HAL/GPIO/DMA/UART 初始化（CubeMX）→ UART_DMA_Rx_Init
  → BSP_Button_Init()（PB0 EXTI 就绪，尽早检测上电已按住）
  → BSP_SoftI2C_Init() + AT24C02_Init(&eeprom_dev, &i2c1_bus, AT24C02_ADDR)
  → BSP_SoftSPI_Init() + W25Q16_Init(&w25q_dev, &spi1_bus, W25Q_CS_PORT, W25Q_CS_PIN)

  → BSP_Button_Pressed()?
     YES → App_bootloader_factory_reset()
              → Bootloader_JumpToApp(FACTORY_REGION_ADDR)  跳转到出厂区(0x08004000)
     NO  → App_bootloader_check_update()   读 EEPROM 校验密钥
            → BOOT_NO_UPDATE → App_bootloader_jump_app() 直接跳转
            → 其他           → App_bootloader_update()   W25Q16→Flash 搬运
                                → 成功: 清EEPROM标志, 跳新App
                                → 失败: 清EEPROM标志, 跳出厂区
```

更新失败时跳转出厂区恢复，用户也可按住 PB0 + 复位手动进入出厂恢复。

### EEPROM 布局（AT24C02, 地址 0x10 起）

| 地址 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0x10 | 1 | status | 更新状态：0x00=无需更新, 0x01=需要更新, 0x02=强制更新 |
| 0x11 | 1 | key_high | 密钥高字节 |
| 0x12 | 1 | key_low | 密钥低字节 |
| 0x13 | 4 | fw_size | 固件大小（小端序，单位字节） |
| 0x17 | 4 | crc32 | 固件 CRC32（小端序）— Bootloader 不使用，App 端写入 |

密钥 0xA5A5 校验 EEPROM 数据有效性。密钥校验通过后保留不清除，留给后续 update 流程使用。搬运完成后由 `clear_eeprom_flag_safe()` 安全清除（先废密钥再清状态，断电安全）。

## 分层架构（自底向上）

```
APP/           → Bootloader 业务逻辑
                  app_bootloader.c/h（更新判断、W25Q16 搬运、出厂跳转）
                  bootloader.c/h（App/出厂区有效性校验、Cortex-M 跳转）
                  bootloader_conf.h（三区分区参数，换芯片只改此文件）
Service/       → 业务服务
                  flash_download.c/h（智能擦页 + 跨帧奇数字节缓冲 + 半字写入）
Middleware/    → 预留目录
Driver/        → 设备驱动
  MCU/         →   flash.c/h（内部 Flash 抽象：解锁/擦/写/锁/擦除检测）
  Storage/     →   at24c02.c/h（EEPROM 256B, 软件 I2C）、w25q16.c/h（SPI Flash 2MB, 软件 SPI）
  OLED/        →   ssd1306.c/h + ssd1306_font.h（SSD1306 OLED，暂未使用，软件 I2C）
Protocol/      → 通信协议
  UART/        →   uart_buf.c/h（DMA + IDLE 帧队列）、uart_ringbuf.c/h（环形缓冲区，暂未使用）
  I2C/         →   soft_i2c.c/h（软件 I2C，AT24C02 使用此实现）
  SPI/         →   soft_spi.c/h（软件 SPI，W25Q16 使用此实现）
BSP/           → 板级支持
                  bsp_button.c/h（PB0 EXTI0 按键，HAL 回调模式）
                  bsp_soft_i2c.c/h（软件 I2C 引脚初始化，PB8/PB9 → AT24C02）
                  bsp_soft_spi.c/h（软件 SPI 引脚初始化，PA5/PA6/PA7 → W25Q16）
Debug/         → 硬件模块测试：module_test.c/h（UART/OLED/EEPROM/W25Q16 测试，未集成到主流程）
Core/          → CubeMX 生成：main.c、usart.c、gpio.c、dma.c、HAL 配置
Drivers/       → STM32 HAL 库（不要修改）
MDK-ARM/       → Keil 工程、scatter 文件、启动汇编
```

**注意：** Bootloader 区域不包含 CRC32 硬件驱动（`crc32.c/h`），虽然 EEPROM 布局预留了 CRC32 字段（0x17-0x1A），但 Bootloader 搬运时不做 CRC 校验，仅校验 W25Q16 JEDEC ID。CRC32 校验由 App 端（接收端）和 P00（发送端）负责。

## 核心设计约束

换芯片只改 `APP/bootloader_conf.h`（前 5 个宏），所有分区参数、Flash 操作、跳转校验均依赖此文件。Flash 操作必须通过 `Driver/MCU/flash.c` 抽象层，不要直接调用 HAL Flash API。

**软件总线分层：** Protocol 层（`soft_i2c.c`/`soft_spi.c`）→ BSP 层（`bsp_soft_*.c`）→ Driver 层（`at24c02.c`/`w25q16.c`）。添加新设备只需实现 Driver 层，复用现有总线。

**Flash 写入（`flash_download.c`）：** 智能擦除（单调递增，每页只擦一次）+ 跨帧奇数字节缓冲（`last_byte` 拼接半字）。不要绕过此模块直接写 Flash。

**跳转（`bootloader.c`）：** 统一入口 `Bootloader_JumpToApp(addr)`，跳转前校验 MSP 在 RAM 范围 + ResetHandler 在目标区域内。跳转序列：禁中断 → 停 SysTick → HAL_DeInit → NVIC 全量清理 → 设 VTOR → 设 MSP → 跳转。不要修改此序列。

**EEPROM 安全清除：** `clear_eeprom_flag_safe()` 必须先废密钥（0x11-0x12 写 0x00）再清状态（0x10 写 0x00），确保断电安全。

## 初始化顺序

`main.c` 中外设初始化顺序不可随意调整：

1. `HAL_Init` / `SystemClock_Config` / `MX_GPIO_Init` — HAL 和时钟
2. `MX_DMA_Init` — 必须在 UART 初始化之前
3. `MX_USART1_UART_Init` — 串口就绪后才能 printf
4. `MX_I2C1_Init` / `MX_SPI1_Init` — CubeMX 生成的 I2C/SPI 初始化（当前未使用，实际用软件 I2C/SPI）
5. `UART_DMA_Rx_Init` — 启动 DMA 接收 + IDLE 中断
6. `BSP_Button_Init()` — PB0 EXTI 就绪，尽早检测上电已按住
7. `BSP_SoftI2C_Init()` + `AT24C02_Init(&eeprom_dev, &i2c1_bus, AT24C02_ADDR)` — 软件 I2C + EEPROM 就绪
8. `BSP_SoftSPI_Init()` + `W25Q16_Init(&w25q_dev, &spi1_bus, W25Q_CS_PORT, W25Q_CS_PIN)` — 软件 SPI + W25Q16 就绪
9. 按键检测 → 出厂跳转 或 EEPROM 状态判断 → 更新/跳转

## 关键 API 速查

**Bootloader 业务**（`APP/app_bootloader.c`）：
- `App_bootloader_check_update()` — 读 EEPROM 判断是否需要更新，设置全局 `app_boot_update_status`
- `App_bootloader_update()` — W25Q16 → Flash A 区搬运。搬运失败清除 EEPROM 标志并返回 -1
- `App_bootloader_jump_app()` — 校验 App 有效性 + 跳转 A 区
- `App_bootloader_factory_reset()` — 校验出厂区有效性 + 直接跳转到出厂区

**跳转**（`APP/bootloader.c`）：
- `Bootloader_IsAppValid()` — 校验 MSP 在 RAM 范围 + ResetHandler 在 A 区
- `Bootloader_JumpToApp(addr)` — 统一跳转函数，传入区域基地址（`A_REGION_ADDR` 或 `FACTORY_REGION_ADDR`），内部自动判断区域边界校验 ResetHandler

**Flash Download**（`Service/flash_download.c`）：
- `FlashDownload_Init(ctx)` — 初始化上下文，写入地址 = A_REGION_ADDR
- `FlashDownload_WriteFrame(ctx, data, len)` — 智能擦除 + 奇数字节缓冲 + 半字写入

**Flash 抽象**（`Driver/MCU/flash.c`）：
- `Flash_Unlock()` / `Flash_Lock()` — 解锁/锁定 Flash
- `Flash_ErasePage(addr)` — 擦除指定地址所在页（1KB）
- `Flash_Write(addr, data, len)` — 半字编程写入
- `Flash_NeedsErase(addr, len)` — 检查目标区域是否全为 0xFF

## 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，DMA1_CH5 接收 |
| LED1 | PA0 | 低电平点亮 |
| 恢复出厂按键 | PB0 | EXTI0 下降沿，内部上拉，按住=低电平 |
| 软件 I2C | PB8(SCL), PB9(SDA) | GPIO 开漏输出 + 上拉，连接 AT24C02 |
| 软件 SPI | PA5(SCK), PA6(MISO), PA7(MOSI) | GPIO 模拟 SPI Mode 0，连接 W25Q16 |
| W25Q16 CS | PA4 | 低电平有效片选，推挽输出 |

## 日志前缀约定

串口日志使用方括号前缀区分模块：`[DL]`（Flash Download）、`[BL]`（Bootloader 交互/跳转）、`[OTA]`（OTA 搬运）、`[DEBUG]`（调试信息）。新增模块日志应遵循此约定。Printf 字符串使用英文（ARM Compiler V5 不支持字符串中的中文 UTF-8）。

## 设备句柄管理

`AT24C02_t eeprom_dev` 和 `W25Q16_t w25q_dev` 在 `main.c` 中定义为全局变量并初始化，`app_bootloader.c` 通过 `extern` 引用。设备句柄使用软件 I2C/SPI 总线（`SoftI2C_Bus_t i2c1_bus` / `SoftSPI_Bus_t spi1_bus`，由 BSP 层初始化）。所有硬件初始化集中在 `main.c` 的 `USER CODE BEGIN 2` 区域内。

## App 开发注意事项（A 区 / 出厂区）

Bootloader 跳转前调用 `__disable_irq()` 关闭全局中断（设 PRIMASK=1）。被跳转的程序（出厂程序或 A 区 App）必须在初始化阶段重新开启中断。

**NVIC 残留处理：** `Bootloader_JumpToApp()` 跳转序列中已包含 NVIC 全量清理（禁用所有中断通道 + 清除所有挂起标志），App 端也建议在 `__enable_irq()` 之前做一次清理作为双重保险。

A 区 App 的必要配置：
- **Scatter 文件：** IROM1 起始 `0x08008000`，大小 `0x8000`（32KB）
- **VTOR：** 偏移 `0x8000`，即 `SCB->VTOR = 0x08008000`。HAL 工程在 `system_stm32f1xx.c` 中设置 `VECT_TAB_OFFSET`

出厂程序的必要配置：
- **Scatter 文件：** IROM1 起始 `0x08004000`，大小 `0x4000`（16KB）
- **VTOR：** Bootloader 跳转前已设置 `SCB->VTOR = 0x08004000`，出厂程序内部无需再设置

出厂程序串口下载协议：
1. 发送 `START` → 进入接收模式
2. 发送 `SIZE:<十进制字节数>` → 声明固件大小
3. 发送 .bin 原始数据 → 逐帧写入 A 区 Flash
4. 2 秒无新帧 → 自动校验字节数 → 跳转 A 区

## 关联工程

本 Bootloader 与两个独立 Keil 工程配合使用：

| 角色 | 工程路径 | ROM 起始 | ROM 大小 | VTOR | 说明 |
|------|----------|----------|----------|------|------|
| A 区 App | `Project02_Application` | 0x08008000 | 32KB | `system_stm32f1xx.c` 中设置 | 正常运行的应用程序，通过 CAN OTA 更新 |
| 出厂程序 | `Project02_factory_app` | 0x08004000 | 16KB | 由 Bootloader 跳转前设置 | 出厂恢复 + 串口下载协议 |

烧录时三个工程各自独立下载，必须使用 "Erase Sectors" 模式避免误擦其他区域。
