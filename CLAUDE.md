# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

STM32F103C8（64KB Flash, 20KB RAM, 72MHz）Bootloader 学习项目，包含 6 个独立的 Keil MDK-ARM 5 工程，从基础 UART 接收逐步进阶到企业级三区 Flash 管理、OTA 更新和 CAN 通信。每个子工程有自己的 `CLAUDE.md` 记录具体细节；本文件覆盖跨工程的共享约定与整体架构。

## 构建与烧录

- **IDE：** Keil MDK-ARM 5，ARM Compiler V5.05（非 V6）
- **编译：** 用 Keil 打开 `MDK-ARM/*.uvprojx`，按 F7 编译
- **烧录：** ST-Link，Keil 中 Debug -> Download
- **输出：** `.hex` + `.bin` 生成在 `MDK-ARM/<工程名>/`（bin 通过 fromelf 后处理生成）
- **新建 .c 文件：** 必须手动添加到 Keil 工程中（不会自动发现）
- **重要：** 烧录时必须使用 "Erase Sectors" 模式 -- "Erase Full Chip" 会擦除其他区域的程序

## 工程列表

| 工程 | 角色 | Flash 起始地址 |
|------|------|---------------|
| `P00_getway_led1_hal` | CAN 上位机（网关），负责固件分发 | 0x08000000 |
| `Project01_learn_Bootloader_led` | 基础 UART Bootloader（学习用） | 0x08000000 |
| `Project02_learn_Bootloader_UART` | UART Bootloader 变体（使用 HAL UARTEx 接收） | 0x08000000 |
| `Project02_enterprise_bootloader` | 企业级三区 Bootloader，带出厂恢复 | 0x08000000 |
| `Project02_factory_app` | 出厂恢复程序（串口下载固件到 A 区） | 0x08004000 |
| `Project02_Application` | 主应用（A 区），通过 CAN 接收固件更新 | 0x08008000 |

**Keil 工程名不一致：** 部分 `.uvprojx` 文件名与目录名不匹配（历史原因），例如 `Project02_Application/MDK-ARM/Project01_learn_Bootloader_led.uvprojx`。打开工程时以 `.uvprojx` 文件名为准。

## Flash 布局

### 企业级三区布局（Bootloader / 出厂 / App）

```
0x08000000  Bootloader  16KB  (页 0~15)
0x08004000  出厂程序    16KB  (页 16~31)
0x08008000  A区 App     32KB  (页 32~63)
```

分区参数集中在 `APP/bootloader_conf.h`，换芯片只需修改该文件前 5 个宏定义。

### P00 网关布局（App + 固件缓存）

```
0x08000000  P00 App     ~8KB  (页 0~15)
0x08004000  固件缓存    48KB  (页 16~63)
```

P00 通过 `APP/fw_cache_conf.h` 配置缓存区地址与大小。

## 端到端 OTA 更新流程

```
PC 串口发送 .bin -> P00 存储到 Flash 缓存区 (0x08004000)
                            |
                    App 发 UPDATE_REQ (CAN)
                            |
                    P00 回 UPDATE_ACK (含 fw_size)
                            |
                    App 擦除 W25Q16 -> 发 UPDATE_READY
                            |
                    P00 逐帧发 UPDATE_DATA (2ms/帧)
                            |
                    App 写 W25Q16 页缓冲 -> 收到 END
                            |
                    App 刷缓冲 + 写 EEPROM 标志 -> 发 DONE
                            |
                    App 复位 -> Bootloader 读 EEPROM
                            |
                    Bootloader 从 W25Q16 搬运到 A 区 Flash
                            |
                    Bootloader 跳转 A 区新 App
```

掉电安全保证：EEPROM 标志在 W25Q16 写完之后才写入，中途掉电不会触发 Bootloader 更新。

## 分层架构

所有工程遵循统一的目录结构：

```
APP/          -> Bootloader 业务逻辑（状态机、更新决策、跳转）
Service/      -> 业务服务（Flash 下载、OTA 存储）
Driver/
  MCU/        ->   内部 Flash 抽象层（flash.c/h）
  Storage/    ->   外部存储（at24c02 EEPROM, w25q16 SPI Flash）
Protocol/
  UART/       ->   DMA + IDLE 帧队列（uart_buf.c/h）
  CAN/        ->   CAN 轮询封装 + 协议定义
  I2C/        ->   软件 I2C
  SPI/        ->   软件 SPI
BSP/          -> 板级支持（按键、GPIO 初始化）
Debug/        -> 硬件测试（module_test.c/h）
Core/         -> CubeMX 生成文件（main.c, usart.c, gpio.c 等）
Drivers/      -> STM32 HAL 库（禁止修改）
```

上层代码不直接调用 HAL Flash API -- 所有操作通过 `Driver/MCU/flash.c` 抽象层。

## CubeMX 代码保护

`Core/Src/` 和 `Core/Inc/` 由 CubeMX 生成。所有自定义代码**必须**写在 `/* USER CODE BEGIN xxx */` / `/* USER CODE END xxx */` 标记块内，标记外的代码在 CubeMX 重新生成时会被覆盖。

## 编码约定

- **注释：** 中文（详尽文档）
- **Printf 字符串：** 英文（ARM Compiler V5 不支持字符串中的中文 UTF-8）
- **串口日志策略：** 串口打印仅在错误和关键状态变化时使用，避免冗余日志
- **串口日志前缀：** `[DL]` Flash 下载、`[BL]` Bootloader/跳转、`[OTA]` OTA 迁移、`[APP]` 应用层、`[Host]` 上位机、`[DEBUG]` 调试信息
- **调试串口：** USART1，115200 波特率，`printf` 重定向到 UART

## 跨工程约束

- **`can_proto.h` 同步：** P00 和 Application 各自持有 `can_proto.h` 副本，修改协议定义时**两个文件必须同步更新**
- **`bootloader_conf.h` 一致性：** Bootloader 和 Application 使用相同的分区参数，地址定义必须一致
- **DMA 初始化顺序：** 所有使用 DMA 的工程，`MX_DMA_Init()` 必须在 `MX_USART1_UART_Init()` 之前调用

## 共享硬件引脚

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，调试串口 |
| CAN1 | PA11(RX), PA12(TX) | 200kbps，需外部 CAN 收发器（P00/Application） |
| LED1 | PA0 | 低电平点亮 |
| LED2 | PA1 | 低电平点亮（Application） |
| W25Q16 SPI Flash | PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI) | 软件 SPI Mode 0 |
| AT24C02 EEPROM | PB8(SCL), PB9(SDA) | 软件 I2C，地址 0xA0 |
| PB0 按键 | PB0 | EXTI0 下降沿，内部上拉，按住=低电平（Bootloader 出厂恢复） |

## 关键设计模式

- **智能擦除：** `flash_download.c` 维护 `next_erase_addr`，每个 1KB 页只擦除一次（单调递增向前），绝不回头擦除
- **跨帧奇数字节缓冲：** STM32 Flash 最小写入单位是半字（2 字节），`FlashDownload_t.last_byte` 缓存帧尾多余的奇数字节，与下一帧首字节配对写入
- **DMA 帧队列（Normal 模式 + IDLE）：** `uart_buf.c` 在 IDLE 中断中记录帧描述符，主循环消费，无数据拷贝
- **NVIC 残留中断清理：** Bootloader 跳转前在 `bootloader.c` 中清除所有 NVIC 通道使能和挂起标志；App 端也在 `main()` 开头做一次全量清理作为双重保险
- **统一跳转函数：** `Bootloader_JumpToApp(addr)` 接受区域基地址参数，自动根据地址判断区域边界校验 ResetHandler
- **READY 流控：** CAN OTA 中 App 收到 ACK 后先擦除 W25Q16（阻塞 ~100ms/扇区），擦除完成发 READY，防止 CAN FIFO 溢出

## 通信协议

**UART 下载协议：** `START\r\n` -> `SIZE:<字节数>\r\n` -> 原始 .bin 流 -> 2 秒超时 -> 自动校验 -> 跳转

**CAN 更新协议（P00 <-> Application）：**
- `UPDATE_REQ (0x01)`：App -> 上位机，请求更新
- `UPDATE_ACK (0x81)`：上位机 -> App，载荷 = 4 字节小端固件大小
- `UPDATE_READY (0x04)`：App -> 上位机，擦除 W25Q16 完成后通知
- `UPDATE_DATA (0x02)`：上位机 -> App，载荷 = 2 字节小端序号 + <=5 字节数据
- `UPDATE_END (0x03)`：上位机 -> App，传输完成
- `UPDATE_DONE (0x83)`：App -> 上位机，确认完成
- `UPDATE_ERR (0x84)`：App -> 上位机，错误帧

**EEPROM 布局（AT24C02）：** 地址 0x10 = 状态字节，0x11-0x12 = 有效性密钥（0xA5A5），0x13-0x16 = 固件大小（4 字节小端）

## 已知陷阱

- **Keil "Erase Full Chip"：** 会擦除 Bootloader/出厂程序 -- 必须使用 "Erase Sectors"
- **DMA IDLE 假中断：** 初始化时 IDLE 标志已置位；中断处理函数中必须检查 CNDTR 值是否变化，未变化则直接返回
- **环形队列翻转检测：** 用 `>` 判断翻转，不能用 `==`（`==` 会漏掉最后一个槽位）
- **VTOR 设置：** App 必须将 `SCB->VTOR` 设为自身所在区域的基地址（在 `system_stm32f1xx.c` 中配置）
- **NVIC 残留：** Bootloader 跳转前 `__disable_irq()` 只关总闸，NVIC 使能位和挂起标志仍残留。已在 `bootloader.c` 跳转序列中增加 NVIC 全量清理；App 端也需在 `__enable_irq()` 之前做清理
- **主循环延时：** Application 的主循环不能有 `HAL_Delay`，CAN 轮询必须全速运行，>4ms 延迟会导致 CAN FIFO 溢出

## CAN 时序参数

- **波特率：** 200kbps（Prescaler=36, TimeSeg1=2TQ, TimeSeg2=2TQ, 总 5TQ/bit）
- **单帧传输：** ~0.55~0.67ms（标准帧 8 字节载荷，含位填充）
- **P00 帧间隔：** `HAL_Delay(2)` + 帧传输 ≈ 2.5ms/帧
- **App FIFO 深度：** 3 帧，溢出边界 ≈ 7.5ms 不读取
- **W25Q16 页写入：** 0.7~3ms（每 51 帧触发一次，256B 页缓冲满时）
- **W25Q16 扇区擦除：** ~100ms/4KB 扇区（OTA 开始时一次性擦除，由 READY 流控保护）

**接收路径禁止阻塞：** RECV_DATA 状态的正常 DATA 帧处理路径中不能有 `printf` 或 `HAL_Delay`，否则 FIFO 溢出丢帧。

## 待优化方向

- **固件 CRC32 校验：** 当前仅校验 W25Q16 固件头 8 字节（MSP + ResetHandler），无全量完整性校验。计划在 `OTA_Storage_Finish()` 计算 CRC32 写入 EEPROM 尾部，Bootloader 搬运完成后校验
- **W25Q16 双缓冲：** 当前页写入阻塞 0.7~3ms 期间无法接收 CAN 帧，FIFO 余量紧张。计划用双缓冲交替读写，消除接收路径阻塞
- **CAN 帧重传机制：** 当前 seq 不匹配直接 ERROR 并全量重来，计划加 NACK + 指定 seq 重发
- **`can_proto.h` 统一管理：** 当前 P00 和 Application 各持副本需手动同步，计划提取到仓库共享层
- **P00 fw_size 动态获取：** 当前硬编码 1344 字节，计划从 Flash 缓存区头部自动读取
- **调试日志宏开关：** 计划定义 `DEBUG_PRINT` 宏，发布时全局关闭 printf
