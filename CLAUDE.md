# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

STM32F103C8（64KB Flash, 20KB RAM, 72MHz）Bootloader 学习项目，包含 6 个独立的 Keil MDK-ARM 5 工程（P00 CAN 网关 + 三区 Bootloader 体系 + P03 LoRa 网关/应用），覆盖企业级三区 Flash 管理、OTA 更新、CAN 通信和 LoRa 无线通信。每个子工程有自己的 `CLAUDE.md` 记录具体细节；本文件覆盖跨工程的共享约定与整体架构。

## 构建与烧录

- **IDE：** Keil MDK-ARM 5，ARM Compiler V5.05（非 V6）
- **编译：** 用 Keil 打开 `MDK-ARM/*.uvprojx`，按 F7 编译
- **烧录：** ST-Link，Keil 中 Debug -> Download
- **输出：** `.hex` + `.bin` 生成在 `MDK-ARM/<工程名>/`（bin 通过 fromelf 后处理生成）
- **新建 .c 文件：** 必须手动添加到 Keil 工程中（不会自动发现）
- **Keil 分组命名：** 工程分组名应与目录名一致（APP、BSP、Service、Driver、Protocol），不要混合放置。`.h` 文件不需要加入 Keil 工程
- **重要：** 烧录时必须使用 "Erase Sectors" 模式 -- "Erase Full Chip" 会擦除其他区域的程序

## 工程列表

| 工程 | 角色 | 传输方式 | Flash 起始地址 |
|------|------|----------|---------------|
| `P00_getway_led1_hal` | CAN 上位机（网关），负责固件分发 | CAN | 0x08000000 |
| `Project02_enterprise_bootloader` | 企业级三区 Bootloader，带出厂恢复 | - | 0x08000000 |
| `Project02_factory_app` | 出厂恢复程序（串口下载固件到 A 区） | UART | 0x08004000 |
| `Project02_Application` | 主应用（A 区），通过 CAN 接收固件更新 | CAN | 0x08008000 |
| `Project03_Gateway_LoRa` | LoRa 上位机（网关），负责固件分发 | LoRa | 0x08000000 |
| `Project03_Application_LoRa` | 主应用（A 区），通过 LoRa 接收固件更新 | LoRa | 0x08008000 |

**LoRa 工程说明：** P03 是 P02 的 LoRa 无线变体，共享同一个 Bootloader（Project02_enterprise_bootloader）和出厂程序（Project02_factory_app），仅替换通信传输层。P03 网关使用 USART3 + E32-433T20D 模块（433MHz, 9.6kbps 空中速率）替代 CAN 总线。

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
0x08000000  P00 App     16KB  (页 0~15)
0x08004000  固件缓存    48KB  (页 16~63)
```

P00 通过 `APP/fw_cache_conf.h` 配置缓存区地址与大小。

### P03 LoRa 网关布局（App + 固件缓存）

```
0x08000000  P03 App     16KB  (页 0~15)
0x08004000  固件缓存    48KB  (页 16~63)
```

P03 网关布局与 P00 相同，通过 `APP/fw_cache_conf.h` 配置。

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
                    App 刷缓冲 + 回读 W25Q16 CRC 校验 + 写 EEPROM -> 发 DONE
                            |
                    App 延时 100ms -> NVIC_SystemReset() -> Bootloader 读 EEPROM
                            |
                    Bootloader 从 W25Q16 搬运到 A 区 Flash
                            |
                    Bootloader 跳转 A 区新 App
```

掉电安全保证：EEPROM 标志在 W25Q16 写完并回读 CRC 校验通过后才写入，中途掉电不会触发 Bootloader 更新。App 发送 DONE 后执行 `NVIC_SystemReset()`（非延时等待），100ms 延时仅用于确保 CAN 帧和 printf 完成传输。

### LoRa OTA 更新流程（P03 网关 <-> P03 App）

```
PC 串口发送 .bin -> P03 网关存储到 Flash 缓存区 (0x08004000)
                              |
                      App 发 UPDATE_REQ (LoRa)
                              |
                      P03 回 UPDATE_ACK (含 fw_size)
                              |
                      App 擦除 W25Q16 -> 发 UPDATE_READY
                              |
                      P03 逐帧发 UPDATE_DATA (50ms/帧, ≤50B/帧)
                              |
                      App 写 W25Q16 页缓冲 -> 收到 END (含 CRC32)
                              |
                      App 刷缓冲 + 回读 W25Q16 CRC 校验 + 写 EEPROM -> 发 DONE
                              |
                      App 延时 100ms -> NVIC_SystemReset() -> Bootloader 读 EEPROM
                              |
                      Bootloader 从 W25Q16 搬运到 A 区 Flash
                              |
                      Bootloader 跳转 A 区新 App
```

LoRa OTA 使用相同的掉电安全机制（EEPROM 标志写后校验）。与 CAN 版本的主要差异：帧载荷更大（50B vs 5B）、帧间隔更长（50ms vs 2ms）、DATA 超时 30 秒（LoRa 无线传输不确定性）。

## 分层架构

所有工程遵循统一的目录结构：

```
APP/          -> Bootloader 业务逻辑（状态机、更新决策、跳转）
Service/      -> 业务服务（Flash 下载、OTA 存储）
Driver/
  MCU/        ->   内部 Flash 抽象层（flash.c/h）
  MCU/        ->   硬件 CRC32 驱动（crc32.c/h）
  Storage/    ->   外部存储（at24c02 EEPROM, w25q16 SPI Flash）
Protocol/
  UART/       ->   DMA + IDLE 帧队列（uart_buf.c/h）
  CAN/        ->   CAN 轮询封装 + 协议定义
  LoRa/       ->   LoRa DMA + IDLE 帧缓冲 + 协议定义（lora_buf.c/h, lora_proto.h）
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
- **`crc32.c/h` 同步：** P00 和 Application 各持一份相同的 CRC 驱动代码，修改时必须同步更新（Bootloader 当前未使用 CRC32，但 Driver/MCU/crc32.c/h 已保留备用）
- **`bootloader_conf.h` 一致性：** Bootloader 和 Application 使用相同的分区参数，地址定义必须一致。**注意：** Application 当前的 `bootloader_conf.h` 有 `B_PAGE_NUM=20`（无 FACTORY_PAGE_NUM），与 Bootloader/Factory 的 `B_PAGE_NUM=16 + FACTORY_PAGE_NUM=16` 不一致。因 Application 中该文件仅被未编译的备用模块引用，暂不影响运行，但未来集成时必须修正
- **DMA 初始化顺序：** 所有使用 DMA 的工程，`MX_DMA_Init()` 必须在 `MX_USART1_UART_Init()` 之前调用
- **`lora_proto.h` 同步：** P03 网关和 P03 App 各自持有 `lora_proto.h` 副本，修改协议定义时**两个文件必须同步更新**
- **LoRa OTA 与 CAN OTA 独立：** P03（LoRa）和 P00/P02（CAN）是独立的两套 OTA 方案，协议文件不交叉引用。P03 复用 Project02 的 Bootloader 和出厂程序，仅替换传输层

## 共享硬件引脚

| 外设 | 引脚 | 说明 |
|------|------|------|
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，调试串口 |
| USART3 | PB10(TX), PB11(RX) | 115200 波特率，LoRa 模块通信（P03 网关/App） |
| CAN1 | PA11(RX), PA12(TX) | 200kbps，需外部 CAN 收发器（P00/Application） |
| E32-433T20D LoRa | USART3 | 433MHz，9.6kbps 空中速率（P03 网关/App） |
| LED1 | PA0 | 低电平点亮 |
| LED2 | PA1 | 低电平点亮（Application） |
| W25Q16 SPI Flash | PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI) | 软件 SPI Mode 0 |
| AT24C02 EEPROM | PB8(SCL), PB9(SDA) | 软件 I2C，地址 0xA0 |
| PB0 按键 | PB0 | EXTI0 下降沿，内部上拉，按住=低电平（Bootloader 出厂恢复） |

## 关键设计模式

- **CRC32 硬件校验：** P00 和 Application 使用相同的 `Driver/MCU/crc32.c/h`（Bootloader 保留备用），通过 STM32F1 硬件 CRC 外设（多项式 0x04C11DB7）计算固件完整性。P00 发送前预计算 CRC 随 UPDATE_END 帧传递；App 收完固件后回读 W25Q16 校验，CRC 失败最多重试 3 次（`OTA_MAX_CRC_RETRY`），超限后放弃更新继续运行旧固件；Bootloader 搬运前源校验（保护旧固件）、搬运后目标校验
- **智能擦除：** `flash_download.c` 维护 `next_erase_addr`，每个 1KB 页只擦除一次（单调递增向前），绝不回头擦除
- **跨帧奇数字节缓冲：** STM32 Flash 最小写入单位是半字（2 字节），`FlashDownload_t.last_byte` 缓存帧尾多余的奇数字节，与下一帧首字节配对写入
- **DMA 帧队列（Normal 模式 + IDLE）：** `uart_buf.c` 在 IDLE 中断中记录帧描述符，主循环消费，无数据拷贝
- **NVIC 残留中断清理：** Bootloader 跳转前在 `bootloader.c` 中清除所有 NVIC 通道使能和挂起标志；App 端也在 `main()` 开头做一次全量清理作为双重保险
- **统一跳转函数：** `Bootloader_JumpToApp(addr)` 接受区域基地址参数，自动根据地址判断区域边界校验 ResetHandler
- **READY 流控：** CAN OTA 中 App 收到 ACK 后先擦除 W25Q16（阻塞 ~100ms/扇区），擦除完成发 READY，防止 CAN FIFO 溢出。LoRa OTA 同样使用 READY 流控
- **LoRa 单帧 DMA 缓冲（Normal 模式 + IDLE）：** `lora_buf.c` 使用 DMA Normal 模式接收 USART3 数据，IDLE 中断触发时记录帧长度并设置 `rx_ready` 标志，主循环消费后手动重启 DMA。与 CAN 版本的环形队列不同，LoRa 采用单帧缓冲（适合低速率点对点通信）
- **LoRa 帧协议封装：** 所有 LoRa 帧使用 `[0xAA][CMD][LEN][PAYLOAD]` 格式，`lora_buf.c` 提供统一的 `LORA_SendCmd()` 发送和 `LORA_Buf_Recv()` 接收接口，上层代码不直接操作 UART

## 通信协议

**UART 下载协议：** `START\r\n` -> `SIZE:<字节数>\r\n` -> 原始 .bin 流 -> 2 秒超时 -> 自动校验 -> 跳转

**CAN 更新协议（P00 <-> Application）：**
- `UPDATE_REQ (0x01)`：App -> 上位机，请求更新
- `UPDATE_ACK (0x81)`：上位机 -> App，载荷 = 4 字节小端固件大小
- `UPDATE_READY (0x04)`：App -> 上位机，擦除 W25Q16 完成后通知
- `UPDATE_DATA (0x02)`：上位机 -> App，载荷 = 2 字节小端序号 + <=5 字节数据
- `UPDATE_END (0x03)`：上位机 -> App，载荷 = 4 字节小端 CRC32
- `UPDATE_DONE (0x83)`：App -> 上位机，确认完成
- `UPDATE_ERR (0x84)`：App -> 上位机，载荷 = 1 字节错误码

**LoRa 更新协议（P03 网关 <-> P03 App）：**
- 帧格式：`[0xAA 帧头][CMD 1B][LEN 1B][PAYLOAD 0~55B]`，最大 58 字节
- 命令码与 CAN 版本一致（0x01/0x81/0x04/0x02/0x03/0x83/0x84）
- `UPDATE_DATA` 载荷 = 2 字节小端序号 + ≤50 字节数据（vs CAN 的 5 字节）
- 错误码定义与 CAN 版本一致（0x01~0x06）

**OTA 错误码（分散定义在 `can_proto.h` 和 `ota_storage.h`）：**
- `0x01` 序号不连续（SEQ_MISMATCH）
- `0x02` W25Q16 写入失败（FLASH_WRITE）
- `0x03` EEPROM 写入失败（EEPROM_WRITE）
- `0x04` 接收超时（TIMEOUT）
- `0x05` 接收量与声明大小不匹配（SIZE_MISMATCH）
- `0x06` CRC32 校验不匹配（CRC_MISMATCH）

**EEPROM 布局（AT24C02）：** 地址 0x10 = 状态字节，0x11-0x12 = 有效性密钥（0xA5A5），0x13-0x16 = 固件大小（4 字节小端），0x17-0x1A = CRC32（4 字节小端）

## 已知陷阱

- **Keil "Erase Full Chip"：** 会擦除 Bootloader/出厂程序 -- 必须使用 "Erase Sectors"
- **DMA IDLE 假中断：** 初始化时 IDLE 标志已置位；中断处理函数中必须检查 CNDTR 值是否变化，未变化则直接返回
- **环形队列翻转检测：** 用 `>` 判断翻转，不能用 `==`（`==` 会漏掉最后一个槽位）
- **VTOR 设置：** App 必须将 `SCB->VTOR` 设为自身所在区域的基地址（在 `system_stm32f1xx.c` 中配置）。出厂程序的 VTOR 由 Bootloader 跳转前设置（`SCB->VTOR = 0x08004000`），出厂程序内部 `USER_VECT_TAB_ADDRESS` 未启用，不自行设置 VTOR
- **EEPROM 安全清除顺序：** Bootloader 清除 EEPROM 标志时必须先废密钥（0x11-0x12 写 0x00）再清状态（0x10 写 0x00），确保断电安全。密钥清除推迟到搬运成功后由 `clear_eeprom_flag_safe()` 完成
- **Application bootloader_conf.h 不一致：** Application 的 `bootloader_conf.h` 使用 `B_PAGE_NUM=20`（无 FACTORY_PAGE_NUM），推导出 A_REGION_ADDR=0x08005000，与实际 App 运行地址 0x08008000 不一致。该文件当前仅被未编译的备用模块引用，不影响运行
- **NVIC 残留：** Bootloader 跳转前 `__disable_irq()` 只关总闸，NVIC 使能位和挂起标志仍残留。已在 `bootloader.c` 跳转序列中增加 NVIC 全量清理；App 端也需在 `__enable_irq()` 之前做清理
- **主循环延时：** Application 的主循环不能有 `HAL_Delay`，CAN 轮询必须全速运行，>4ms 延迟会导致 CAN FIFO 溢出
- **LoRa 帧大小限制：** E32-433T20D 模块单帧最大 58 字节（含帧头），`LORA_MAX_DATA_PER_FRAME` 限制为 50 字节。LoRa App 的 RECV_DATA 正常处理路径中同样不能有 `printf` 或 `HAL_Delay`
- **LoRa DMA 单帧模式：** `lora_buf.c` 使用 DMA Normal 模式（非 Circular），每次 IDLE 中断后必须手动重启 DMA 接收。中断处理函数中 `LORA_Buf_IdleHandler()` 必须在 `HAL_UART_IRQHandler()` 之前调用（HAL 处理会清除 IDLE 标志）

## CAN 时序参数

- **波特率：** 200kbps（Prescaler=36, TimeSeg1=2TQ, TimeSeg2=2TQ, 总 5TQ/bit）
- **单帧传输：** ~0.55~0.67ms（标准帧 8 字节载荷，含位填充）
- **P00 帧间隔：** `HAL_Delay(2)` + 帧传输 ≈ 2.5ms/帧
- **App FIFO 深度：** 3 帧，溢出边界 ≈ 7.5ms 不读取
- **W25Q16 页写入：** 0.7~3ms（每 51 帧触发一次，256B 页缓冲满时）
- **OTA CRC 校验时序：** App 收到 END 后回读 W25Q16 计算 CRC（32KB 固件约 500ms），期间无 CAN 通信。校验通过后写 EEPROM + 发 DONE + 延时 100ms + `NVIC_SystemReset()`。Bootloader 源校验同样需回读 W25Q16（约 500ms），在擦除 A 区之前完成
- **W25Q16 扇区擦除：** ~100ms/4KB 扇区（OTA 开始时一次性擦除，由 READY 流控保护）

**接收路径禁止阻塞：** RECV_DATA 状态的正常 DATA 帧处理路径中不能有 `printf` 或 `HAL_Delay`，否则 FIFO 溢出丢帧。

## LoRa 时序参数

- **LoRa 模块：** E32-433T20D，433MHz，9.6kbps 空中速率
- **串口波特率：** USART3 115200（MCU 与 LoRa 模块之间）
- **帧格式：** `[0xAA][CMD][LEN][PAYLOAD]`，最大 58 字节
- **每帧数据载荷：** ≤50 字节（2B seq + 50B data）
- **帧间隔：** `HAL_Delay(50)` ≈ 50ms/帧（≥48ms 空中传输时间）
- **DATA 超时：** 30 秒（LoRa 无线传输不确定性）
- **ACK 超时：** 5 秒
- **错误退避：** 5 秒后重试
- **CRC 重试上限：** 3 次（`OTA_MAX_CRC_RETRY`）
- **32KB 固件传输：** ~655 帧 × 50ms ≈ 33 秒
- **OTA CRC 校验时序：** 与 CAN 版本相同，App 回读 W25Q16 计算 CRC（32KB 约 500ms）
- **LoRa 缓冲模型：** 单帧 DMA Normal 模式（非环形队列），每帧 IDLE 中断后手动重启 DMA

**LoRa 与 CAN 时序对比：**

| 参数 | CAN (P00/P02) | LoRa (P03) |
|------|--------------|------------|
| 每帧数据 | 5B | 50B |
| 帧间隔 | 2ms | 50ms |
| 32KB 传输时间 | ~13s | ~33s |
| DATA 超时 | 无 | 30s |
| 接收缓冲 | CAN FIFO 3帧 | DMA 单帧 |
| 接收路径阻塞敏感度 | 高（>4ms 溢出） | 低（50ms 间隔充足） |

## 待优化方向

- **W25Q16 双缓冲：** 当前页写入阻塞 0.7~3ms 期间无法接收 CAN 帧，FIFO 余量紧张。计划用双缓冲交替读写，消除接收路径阻塞
- **CAN 帧重传机制：** 当前 seq 不匹配直接 ERROR 并全量重来，计划加 NACK + 指定 seq 重发
- **`can_proto.h` 统一管理：** 当前 P00 和 Application 各持副本需手动同步，计划提取到仓库共享层
- **P00 fw_size 动态获取：** 当前硬编码 1344 字节，计划从 Flash 缓存区头部自动读取
- **调试日志宏开关：** 计划定义 `DEBUG_PRINT` 宏，发布时全局关闭 printf
- **Application bootloader_conf.h 修正：** 需同步为三区布局（B_PAGE_NUM=16 + FACTORY_PAGE_NUM=16），确保备用模块地址正确
- **P03 网关串口下载集成：** `app_bootloader.c/h`（PC 串口下载固件到 Flash 缓存区）已实现但未集成到主循环，当前只能通过其他方式烧录固件到缓存区
- **`lora_proto.h` 统一管理：** 当前 P03 网关和 App 各持副本需手动同步，计划提取到仓库共享层
- **P03 网关 fw_size 动态获取：** 当前硬编码 1344 字节，计划从 Flash 缓存区头部自动读取
- **P03 网关 DONE/ERR 处理：** 发送 UPDATE_END 后未处理 App 回复的 DONE/ERR，无法确认更新结果
