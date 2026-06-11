# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 1. 项目概况

三区 Bootloader 系统的 A 区应用程序。由 Bootloader（`Project02_enterprise_bootloader`）从 0x08008000 跳转执行。功能：通过 CAN 从网关（`P00_getway_led1_hal`）接收固件更新 -> 保存到 W25Q16 SPI Flash -> 回读 CRC32 校验 -> 写 EEPROM 标志位 -> NVIC_SystemReset() -> Bootloader 搬运固件到内部 Flash A 区。

- **角色：** A 区主应用程序，带 CAN OTA 更新能力
- **MCU：** STM32F103C8（64KB Flash, 20KB RAM），72MHz
- **Flash 起始地址：** 0x08008000（A 区）
- **ROM 大小：** 32KB
- **VTOR 偏移：** 0x8000（`system_stm32f1xx.c` 中配置）

## 2. 目录结构与模块说明

```
APP/
  app_ota_update.c/h     -> CAN OTA 状态机（5 状态）                    ★ 活跃
  app_bootloader.c/h     -> UART 下载状态机（保留备用，未编译）
  bootloader.c/h         -> 跳转/校验（保留备用，未编译）
  bootloader_conf.h      -> 分区参数（⚠️ B_PAGE_NUM=20 与 Bootloader 不一致）
Protocol/
  CAN/
    can_buf.c/h          -> CAN 轮询收发封装                           ★ 活跃
    can_proto.h          -> CAN 更新协议定义（与 P00 共享）
  UART/uart_buf.c/h      -> UART DMA 帧队列（未编译）
      uart_ringbuf.c/h   -> UART DMA 环形缓冲（未编译）
  SPI/soft_spi.c/h       -> 软件 SPI Mode 0                           ★ 活跃
  I2C/soft_i2c.c/h       -> 软件 I2C                                  ★ 活跃
Service/
  ota_storage.c/h        -> W25Q16 页缓冲 + EEPROM + CRC 校验        ★ 活跃
  ota_update.c/h         -> 固件搬运状态机（W25Q16→内部 Flash，未编译）
  flash_download.c/h     -> Flash 下载（未编译）
Driver/
  MCU/flash.c/h          -> 内部 Flash 抽象层（未编译）
    MCU/crc32.c/h         -> 硬件 CRC32 驱动（流式 API）              ★ 活跃
  Storage/
    w25q16.c/h           -> SPI Flash（页 256B / 扇区 4KB）           ★ 活跃
    at24c02.c/h          -> EEPROM（256B）                             ★ 活跃
  OLED/
    ssd1306.c/h          -> SSD1306 OLED 驱动（I2C，未编译）
BSP/
  bsp_soft_spi.c/h       -> SPI GPIO 引脚初始化                       ★ 活跃
  bsp_soft_i2c.c/h       -> I2C GPIO 引脚初始化                       ★ 活跃
Debug/module_test.c/h    -> 硬件模块测试（未编译）
Core/                    -> CubeMX 生成
```

★ = 当前在 Keil 工程中编译并参与运行。多个模块文件存在但未加入编译（bootloader、flash_download、ota_update 等），保留备用。

### 模块依赖

```
main.c
  -> APP_OTA_Process()              [app_ota_update.c -- CAN OTA 状态机]
       -> CAN_Buf_Send/Recv()       [can_buf.c -- CAN 轮询收发]
       -> OTA_Storage_xxx()         [ota_storage.c -- 存储管理]
            -> CRC32_Calculate()     [crc32.c -- 硬件 CRC 校验]
            -> W25Q16_Write/Erase   [w25q16.c -- SPI Flash]
            -> AT24C02_Write        [at24c02.c -- EEPROM]
```

分层方向：APP -> Protocol/CAN + Service -> Driver/Storage -> Protocol/SPI + Protocol/I2C -> BSP

## 3. 核心设计要点

### CAN OTA 协议（含 READY 流控）

```
目标(A=0x000)                            网关(Host=0x001)
    |-- REQ(0x01) ------------------>    |  WAIT_CMD
    |  <------ ACK(0x81, 4B LE size) --  |  -> WAIT_READY
    |                                    |
    |  擦除 W25Q16 扇区（阻塞）           |  等待 READY...
    |-- READY(0x04) ----------------->   |  收到 -> SEND
    |  <------ DATA(0x02,seq,data) ----  |  每 2ms 一帧
    |  ...                               |
    |  <------ END(0x03, crc32 4B) ---   |  -> WAIT_CMD
    |  (回读 W25Q16 CRC 校验)            |
    |  校验通过: 写 EEPROM -> 发 DONE     |
    |  校验失败: 发 ERR(CRC_MISMATCH)     |
    |-- DONE(0x83) ----------------->    |
    |  LED2 翻转 + 100ms 延时            |
    |  NVIC_SystemReset() -> Bootloader  |
```

**READY 流控（关键）：** 目标收到 ACK 后先擦除 W25Q16（阻塞 ~100ms/扇区），擦除完成后发 READY。没有 READY 这一步，扇区擦除期间 CAN FIFO 溢出会导致帧丢失。

**CRC 校验（关键）：** 收到 END 后回读 W25Q16 计算 CRC32 与 END 帧携带的 CRC 比对。不匹配不发 DONE、不写 EEPROM，发 ERR(CRC_MISMATCH) 触发重试。校验失败最多重试 3 次，超限后放弃更新继续运行旧固件。

**更新完成（关键）：** CRC 通过 -> 写 EEPROM（两次） -> 发 DONE -> LED2 翻转 -> 延时 100ms（确保 CAN 帧和 printf 传输） -> `NVIC_SystemReset()` 直接复位到 Bootloader。不复位后重新请求。

### OTA 状态机（5 状态）

| 状态 | 行为 |
|------|------|
| `IDLE` | 发 UPDATE_REQ -> WAIT_ACK。ERROR 后有 3s 退避（`OTA_ERROR_BACKOFF`） |
| `WAIT_ACK` | 等待 ACK（5s 超时 `OTA_WAIT_ACK_TIMEOUT`）。收到后擦除扇区 -> 发 READY -> RECV_DATA |
| `RECV_DATA` | 接收 DATA 帧，校验 seq 连续性，写页缓冲。收到 END -> 解析 CRC32 -> 刷缓冲 + 回读 W25Q16 CRC 校验 + 写 EEPROM -> 发 DONE -> LED2 翻转 -> 延时 100ms -> `NVIC_SystemReset()`。CRC 失败 -> ERROR |
| `DONE` | （保留状态，当前流程在 RECV_DATA 中直接 SystemReset，不进入此状态） |
| `ERROR` | 发错误帧（ERR 0x84 + 错误码），CRC 失败累计重试计数器，复位存储，回到 IDLE（带 3s 退避） |

### 错误码

错误码分散定义在两个文件中：

**`can_proto.h` 定义（P00 和 Application 共享）：**
`OTA_ERR_SEQ_MISMATCH(0x01)` = 序号不连续，`OTA_ERR_TIMEOUT(0x04)` = 超时，`OTA_ERR_CRC_MISMATCH(0x06)` = CRC32 校验不匹配。

**`ota_storage.h` 定义（Application 专用）：**
`OTA_ERR_FLASH_WRITE(0x02)` = W25Q16 写入失败，`OTA_ERR_EEPROM_WRITE(0x03)` = EEPROM 写入失败，`OTA_ERR_SIZE_MISMATCH(0x05)` = 接收量与声明大小不匹配。

CRC 校验失败最多重试 3 次（`OTA_MAX_CRC_RETRY`），超限后放弃更新，继续运行当前固件（无退避立即回到 IDLE）。重试计数器 `crc_retry_cnt` 在 `APP_OTA_t` 结构体中累计。非 CRC 错误（SEQ_MISMATCH、FLASH_WRITE、TIMEOUT 等）不计入重试次数。

### OTA 存储模块（`Service/ota_storage.c`）

- `OTA_Storage_Init(ctx, w25q, eeprom)` -- 绑定驱动句柄
- `OTA_Storage_Start(ctx, fw_size)` -- 擦除扇区（`ceil(fw_size/4096)` 个）
- `OTA_Storage_Write(ctx, data, len)` -- 填 256B 页缓冲，满自动刷到 W25Q16
- `OTA_Storage_Finish(ctx)` -- 刷缓冲 + 校验接收字节数 + 回读 W25Q16 计算 CRC32 校验 + 匹配则写 EEPROM（两次：7 字节 status+key+fw_size + 4 字节 crc32）。失败返回具体错误码（`OTA_ERR_SIZE_MISMATCH`/`OTA_ERR_FLASH_WRITE`/`OTA_ERR_CRC_MISMATCH`/`OTA_ERR_EEPROM_WRITE`）
- `OTA_Storage_SetExpectedCRC(ctx, crc)` -- 设置期望 CRC32（从 UPDATE_END 帧解析）
- `OTA_Storage_Reset(ctx)` -- 错误复位，清空缓冲

**掉电安全：** `Finish()` 先写完 W25Q16 再写 EEPROM。中途掉电时 EEPROM 未写入 -> Bootloader 不触发更新 -> 安全。

### 固件搬运状态机（`Service/ota_update.c`，暂未集成）

与 CAN 接收阶段的 `ota_storage.c` 不同，`ota_update.c` 负责**将已存储在 W25Q16 中的固件搬运到 A 区内部 Flash**。状态流转：IDLE → READ_INFO（校验 EEPROM）→ ERASE（逐页擦除）→ TRANSFER（256B/次搬运）→ FINISH（清标志 + 软复位）。依赖 `extern W25Q16_t w25q` 全局句柄。此模块保留待集成。

### EEPROM 布局

与 Bootloader `app_bootloader.h` 一致：地址 0x10 = 状态(0x01)，0x11-0x12 = 密钥(0xA5A5)，0x13-0x16 = 固件大小(4B LE)，0x17-0x1A = CRC32(4B LE)。W25Q16 从地址 0x000000 开始存储固件。

EEPROM 分两次写入（AT24C02 页大小 8 字节）：第一次写 0x10-0x16（7 字节 status+key+fw_size），第二次写 0x17-0x1A（4 字节 CRC32）。只有 CRC 匹配才写 EEPROM，不匹配时 EEPROM 保持无更新状态。

### NVIC 残留中断清理

Bootloader 跳转前调用 `__disable_irq()`，NVIC 使能位和挂起标志仍残留。本程序在 `main()` 开头 `USER CODE BEGIN 2` 区域做全量清理：

```c
for (uint32_t i = 0; i < 2; i++) {
    NVIC->ICER[i] = 0xFFFFFFFF;  // 禁用所有中断通道
    NVIC->ICPR[i] = 0xFFFFFFFF;  // 清除所有挂起标志
}
__enable_irq();
```

清理在 LED 初始化之前、外设初始化之前完成。`__enable_irq()` 必须在 NVIC 清理之后调用，否则残留中断触发未初始化 ISR 导致 HardFault。

## 4. 已修复问题

### 2026-06-06 审核

- NVIC 全量清理代码移入 `USER CODE BEGIN 2` 保护区域，防止 CubeMX 重新生成时丢失
- `__enable_irq()` 放在 NVIC 清理之后、外设初始化之前，避免残留中断触发未初始化 ISR
- OTA 状态机增加 `OTA_STATE_WAIT_ACK` 超时保护（5s）
- `OTA_Storage_Start` 返回值修复：擦除失败时返回 -1
- 增加 READY 流控步骤，解决 W25Q16 扇区擦除期间 CAN FIFO 溢出问题

### 2026-06-08 CRC32 校验

- 新增硬件 CRC32 固件校验（`Driver/MCU/crc32.c/h`），替换原有的字节数比对
- `OTA_Storage_Finish()` 改为：刷缓冲 -> 校验接收字节数 -> 回读 W25Q16 计算 CRC -> 比对 -> 匹配才写 EEPROM（两次写入）
- `UPDATE_END` 载荷扩展为 5 字节（命令 + CRC32 4 字节小端）
- CRC 校验失败最多重试 3 次，超限后继续运行旧固件（不停机）
- EEPROM 布局扩展：0x17-0x1A 新增 CRC32 字段
- DONE 后不再延时 2s 重新请求，改为 `NVIC_SystemReset()` 直接复位（100ms 延时仅确保 CAN 帧和 printf 传输完成）

## 5. 已知限制

- `__enable_irq()` 必须在 NVIC 清理之后、外设初始化之前调用，否则 HardFault
- Keil 烧录必须使用 "Erase Sectors" 模式
- CAN Normal 模式需要外部 CAN 收发器和 120 欧终端电阻
- 主循环不能有 `HAL_Delay`（CAN 轮询必须全速，>4ms 延迟会导致 FIFO 溢出）
- OTA 状态机 IDLE 状态持续发送 REQ，可能占用 CAN 总线带宽
- `bootloader_conf.h` 使用 `B_PAGE_NUM=20`（无 FACTORY_PAGE_NUM），与 Bootloader/Factory 工程的三区定义（B_PAGE_NUM=16 + FACTORY_PAGE_NUM=16）不一致。该文件当前仅被未编译的备用模块引用，不影响运行
- RECV_DATA 超时 10s 无数据后放弃
- 错误码分散定义在 `can_proto.h` 和 `ota_storage.h` 两个文件中，非 P00 感知的错误码（FLASH_WRITE/EEPROM_WRITE/SIZE_MISMATCH）不在 `can_proto.h` 中

## 6. 构建与烧录

- **Keil 工程文件：** `MDK-ARM/Project01_learn_Bootloader_led.uvprojx`（目录名 `Project02_Application` ≠ 工程名，历史原因）
- **.ioc 文件：** `MDK-ARM/Project01_learn_Bootloader_led.ioc`
- **编译输出：** `.hex` + `.bin` 在 `MDK-ARM/Project01_learn_Bootloader_led/`
- **烧录模式：** 必须使用 "Erase Sectors"（"Erase Full Chip" 会擦除 0x08000000–0x08007FFF 的 Bootloader 和出厂程序）
- **CubeMX CRC 外设：** 需在 .ioc 中启用 CRC 外设，生成 `MX_CRC_Init()` 和 `crc.c/h`
- **Scatter 文件：** `MDK-ARM/Project01_learn_Bootloader_led/Project01_learn_Bootloader_led.sct`（ROM: 0x08008000, 32KB）
- **VTOR：** `Core/Src/system_stm32f1xx.c` 中 `VECT_TAB_OFFSET = 0x00008000U`

## 7. 串口日志约定

串口打印仅在错误和关键状态变化时使用。日志前缀：`[APP]` 应用初始化、`[OTA]` OTA 状态机。printf 字符串使用英文（ARM Compiler V5 限制）。

### 硬件配置

| 外设 | 引脚 | 说明 |
|------|------|------|
| LED1 | PA0 | 低电平点亮，主循环翻转指示运行 |
| LED2 | PA1 | 低电平点亮，OTA 完成翻转指示 |
| USART1 | PA9(TX), PA10(RX) | 115200 波特率，printf 轮询发送 |
| CAN1 | PA11(RX), PA12(TX) | Normal 模式, 200kbps |
| W25Q16 | PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI) | 软件 SPI Mode 0 |
| AT24C02 | PB8(SCL), PB9(SDA) | 软件 I2C，地址 0xA0 |

### 初始化顺序（`USER CODE BEGIN 2`）

1. NVIC 全量清理 + `__enable_irq()`
2. LED 初始关闭（SET）
3. `CAN_Buf_Init()` -- CAN 过滤器（ID=0x001）+ 启动
4. `BSP_SoftSPI_Init()` / `BSP_SoftI2C_Init()`
5. `W25Q16_Init()` / `AT24C02_Init()`
6. `W25Q16_ReadJEDECID()` -- 验证 SPI Flash 在线
7. `APP_OTA_Init()` -- OTA 状态机绑定 CAN + 存储驱动
