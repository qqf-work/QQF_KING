# CRC32 固件校验设计

## 目标

用 STM32F103C8 硬件 CRC 外设替换现有的 8 字节头部校验和字节数比对，实现发送端（P00）和接收端（App/Bootloader）的端到端固件完整性校验。

## CRC 算法

- **硬件 CRC 外设**：STM32F1 内置 CRC 单元，多项式 0x04C11DB7，无输入/输出位反转
- **两端都是 STM32**：直接使用硬件 CRC 结果，不做反转兼容
- **CubeMX 配置**：用户在 CubeMX 中启用 CRC 外设，生成 `MX_CRC_Init()`

## CAN 协议变更

UPDATE_END 载荷扩展，携带 CRC32：

```
旧: UPDATE_END [0x03]                           (1 字节，无载荷)
新: UPDATE_END [0x03] [crc32 4B LE]             (5 字节)
```

UPDATE_ACK 不变（仍为 `[0x81] [size 4B LE]`）。

其他命令均不变。

## EEPROM 布局变更

```
0x10  status      1B   (0x01=NEED_UPDATE)
0x11  key_high    1B   (0xA5)
0x12  key_low     1B   (0xA5)
0x13  fw_size     4B   (小端)
0x17  crc32       4B   (小端)  ← 新增
```

共 11 字节。AT24C02 页大小 8 字节，分两次写入：
- 第一次：`AT24C02_Write(0x10, data_7bytes, 7)` — status + key + fw_size
- 第二次：`AT24C02_Write(0x17, crc_4bytes, 4)` — crc32

掉电安全：Bootloader 读取时先检查 key (0xA5A5)，再检查 CRC 字节是否全 0xFF（未写入），双重保护。

## CRC 驱动层

新增 `Driver/MCU/crc32.c` 和 `Driver/MCU/crc32.h`：

```c
// crc32.h
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t len);
```

实现逻辑：
1. `__HAL_CRC_DR_RESET(&hcrc)` 复位 CRC 计算单元
2. 按 4 字节字写入 `CRC->DR`（直接寄存器操作，跳过 HAL 开销）
3. 尾部不足 4 字节时补 0x00 对齐后写最后一字
4. 读取 `CRC->DR` 返回结果

需要在三个工程中添加：`P00_getway_led1_hal`、`Project02_Application`、`Project02_enterprise_bootloader`。
新建 .c 文件需手动添加到 Keil 工程中。

## 各模块改动

### P00 网关端 (`P00_getway_led1_hal`)

**`APP/app_update.c`：**
- `AppUpdate_t` 结构体增加 `uint32_t fw_crc` 字段
- `AppUpdate_WaitCmd` 收到 REQ 后，调用 `CRC32_Calculate(fw_data, fw_size)` 预计算 CRC，存入 `fw_crc`
- `UPDATE_END` 发送时携带 CRC：`buf[0]=0x03, buf[1..4]=crc32 LE`

### App 接收端 (`Project02_Application`)

**`Protocol/CAN/can_proto.h`：**
- UPDATE_END 载荷注释更新：1 字节命令 + 4 字节 CRC32

**`Service/ota_storage.h`：**
- `OTA_Storage_t` 增加 `uint32_t expected_crc` 字段
- 新增 `void OTA_Storage_SetExpectedCRC(OTA_Storage_t *ctx, uint32_t crc)`

**`Service/ota_storage.c`：**
- `OTA_Storage_Finish()` 改动：
  1. 刷缓冲到 W25Q16
  2. 回读 W25Q16 逐块（1KB/次）计算 CRC
  3. 与 `expected_crc` 比对
  4. 不匹配返回 `OTA_ERR_CRC_MISMATCH`
  5. 匹配：写 EEPROM 两次（7 字节 + 4 字节 CRC）

**`APP/app_ota_update.c`：**
- `RECV_DATA` 状态收到 `UPDATE_END` 时：
  1. 从 CAN 载荷解析 CRC32（字节 1-4）
  2. 调用 `OTA_Storage_SetExpectedCRC(ctx, crc)`
  3. 调用 `OTA_Storage_Finish(ctx)`
  4. 成功 -> DONE，失败 -> ERROR

### Bootloader 搬运端 (`Project02_enterprise_bootloader`)

**`APP/app_bootloader.c`：**
- `App_bootloader_update()` 改动：
  1. 搬运前仍保留 W25Q16 JEDEC ID 校验
  2. **源校验**（新增，在擦除 A 区之前）：回读 W25Q16 fw_size 字节计算 CRC，与 EEPROM crc32 比对。不匹配则清 EEPROM 标志，跳转 A 区旧 App
  3. 擦除 A 区 + 搬运 W25Q16 -> Flash
  4. **目标校验**（替换旧字节数比对）：回读 A 区 Flash fw_size 字节计算 CRC，与 EEPROM crc32 比对
  5. 匹配：清 EEPROM 标志，跳转 App
  6. 不匹配：重试搬运（最多 `BL_MAX_COPY_RETRY` 次），全失败则清 EEPROM 标志并跳转出厂区
- **删除** `verify_w25q_firmware()` 8 字节头部校验函数
- **删除**搬运后的字节数比对逻辑
- **新增** `app_bootloader.h` 中 `BL_MAX_COPY_RETRY = 3`

**EEPROM 读取扩展：**
- 现有读取 0x10-0x16（7 字节）基础上，追加读取 0x17-0x1A（4 字节 CRC）

## 异常处理与重试策略

核心原则：**在确认新固件完整之前，不破坏旧固件。设备永远优先运行已有程序。**

CAN 通信存在帧丢失风险（FIFO 溢出、W25Q16 写入阻塞、线缆干扰等），
CRC 校验作为最终防线，发现不匹配后需有明确的恢复路径。

### App 端：更新失败不影响当前运行

App 正在运行旧固件（A 区 Flash），CAN OTA 只写 W25Q16 和 EEPROM，
不触碰 A 区 Flash。失败时旧固件完好无损：

```
CRC mismatch -> 不写 EEPROM 标志 -> 发 ERR(CRC_MISMATCH) -> ERROR 状态
  -> 3s 退避 -> IDLE -> 发 REQ -> 全量重来
  -> 最大重试 3 次 (OTA_MAX_CRC_RETRY = 3)
  -> 超限: 放弃本次更新，继续运行当前固件，LED 指示更新失败
```

- `OTA_Storage_Reset()` 每次重试前清空 W25Q16 缓冲、重置状态
- P00 收到 ERR 后回到 WAIT_CMD，等待 App 下一次 REQ
- 重试计数器放在 `APP_OTA_Update_t` 结构体中，累计不重置
- 成功 DONE 或重新上电后重置计数器
- **关键**：只有 CRC 匹配才写 EEPROM 标志，不匹配时 EEPROM 保持无更新状态，
  Bootloader 不会触发搬运，旧 App 不受影响

### Bootloader 端：先校验源数据，再擦除目标

搬运流程拆分为"源校验"和"目标校验"两步，确保源数据有问题时不破坏 A 区：

```
Step 1: 源校验（W25Q16 -> CRC，不触碰 A 区 Flash）
  读 EEPROM: expected_crc
  回读 W25Q16 fw_size 字节，计算 CRC
  与 expected_crc 比对:
    不匹配 -> 清 EEPROM 标志，跳转当前 A 区 App（旧固件完好）
    匹配   -> 进入 Step 2

Step 2: 搬运 + 目标校验
  擦除 A 区 Flash
  搬运 W25Q16 -> A 区 Flash
  回读 A 区 Flash fw_size 字节，计算 CRC
  与 expected_crc 比对:
    不匹配 -> 搬运失败（Flash 写入异常），重试搬运（最多 3 次）
              全部失败 -> 清 EEPROM 标志（A 区已损坏）
                        -> 跳转出厂区（PB0 也可手动触发）
    匹配   -> 清 EEPROM 标志，跳转 A 区新 App
```

- **源校验在擦除之前**：W25Q16 数据有问题时 A 区旧固件零风险
- **搬运失败有重试**：Flash 写入偶发错误可重试，W25Q16 源数据仍完好
- **最终兜底**：出厂程序可通过 PB0 按键触发

### 断电恢复策略

W25Q16（NOR Flash）断电不丢数据，这是所有恢复的基础。
**断电后一律从头重传/重做**，不实现断点续传（32KB 全量重传约 13 秒，复杂度不值得）。
按断电发生时刻逐场景分析：

| 断电时刻 | A 区 Flash | W25Q16 | EEPROM | 重启后行为 |
|----------|-----------|--------|--------|-----------|
| CAN 传输中 | 旧 App 完好 | 部分数据 | 无标志 | Bootloader 跳转旧 App -> App 重新 REQ -> 全量重传 |
| App 写 EEPROM 两次之间 | 旧 App 完好 | 完整固件 | 有 status+key+fw_size，无 CRC | Bootloader 源校验 CRC=0xFFFFFFFF 不匹配，清标志跳旧 App -> 重新 OTA |
| Bootloader 源校验中 | 旧 App 完好 | 完整固件 | 有完整标志 | Bootloader 重新执行源校验 -> 通过 -> 搬运 |
| Bootloader 擦除 A 区中 | 部分损坏 | 完整固件 | 有完整标志 | Bootloader 重新擦除+搬运（擦除幂等） |
| Bootloader 搬运中 | 部分写入 | 完整固件 | 有完整标志 | Bootloader 重新搬运（覆盖写入，幂等） |
| Bootloader 清 EEPROM 中 | 新 App 完好 | 完整固件 | 可能损坏 | 见下方 EEPROM 安全清除 |

**关键结论**：
- 搬运阶段断电后重启，Bootloader 从头重做（源校验 -> 擦除 -> 搬运 -> 目标校验），
  操作幂等，无需额外状态记录
- 唯一风险点：EEPROM 标志清除时断电

#### EEPROM 安全清除顺序

搬运成功后需清除 EEPROM 标志。清除顺序决定断电安全性：

```
Step 1: 写 key = 0x0000 (地址 0x11-0x12)     ← 先废掉密钥
Step 2: 写 status = 0x00 (地址 0x10)           ← 再清状态
```

断电恢复：
- Step 1 后断电：key=0x0000（无效），Bootloader 检查 key 不通过，跳转 A 区（新 App 已通过 CRC 校验，可直接运行）
- Step 1 前断电：key=0xA5A5（有效），Bootloader 重新源校验+搬运（幂等），新 App 覆盖写入
- Step 2 中断电：key 已失效，Bootloader 不触发更新，跳转新 App

#### App 端 EEPROM 写入顺序

App 端写入也需考虑断电安全：

```
Step 1: 写 status + key + fw_size (0x10-0x16, 7 字节)    ← 含有效密钥
Step 2: 写 crc32 (0x17-0x1A, 4 字节)                     ← CRC 数据
```

断电恢复：
- Step 1 后 Step 2 前断电：EEPROM 有 key=0xA5A5 + fw_size，但 CRC 字段=0xFFFFFFFF
  Bootloader 源校验时 W25Q16 的 CRC ≠ 0xFFFFFFFF -> 校验失败 -> 清标志跳旧 App
  需要重新 OTA，但旧 App 完好（安全）
- Step 1 前断电：无标志，Bootloader 跳旧 App
- 两步都完成：正常触发更新流程

### 丢包防护层次

| 防护层 | 机制 | 检测对象 | 失败影响 |
|--------|------|---------|---------|
| 帧级 | seq 序号连续性检查 | 单帧丢失/乱序 | ERR -> 重试 |
| 传输级 | CRC32 全量校验 | 多帧丢失、数据损坏 | ERR -> 重试 |
| 重试级 | 最大 3 次全量重试 | 临时性干扰 | 放弃更新，运行旧固件 |
| 搬运级 | 源校验 + 目标校验 | Flash 写入异常 | 重试搬运 / 出厂恢复 |
| 最终保障 | 出厂恢复（PB0） | 固件彻底损坏 | 串口重新下载 |

## 错误码新增

`Service/ota_storage.h`：
- `OTA_ERR_CRC_MISMATCH = 0x06` — CRC 校验不匹配

`APP/app_ota_update.h`：
- `OTA_MAX_CRC_RETRY = 3` — CRC 校验失败最大重试次数

`APP/app_bootloader.h`：
- `BL_MAX_COPY_RETRY = 3` — Flash 搬运最大重试次数

## 端到端 CRC 校验流程

```
P00 收到 REQ
  -> CRC32_Calculate(fw_data, fw_size) -> fw_crc
  -> 发 ACK(fw_size)
  -> 等待 READY
  -> 逐帧发 DATA
  -> 发 END(crc32)
  -> 等待 DONE/ERR
     ERR -> WAIT_CMD (等待下一次 REQ)

App 收到 END(crc32)
  -> OTA_Storage_SetExpectedCRC(crc32)
  -> OTA_Storage_Finish():
       刷缓冲 -> 回读 W25Q16 计算 CRC -> 比对
       匹配 -> 写 EEPROM (status+key+fw_size 一次, crc32 第二次) -> 返回成功
       不匹配 -> 不写 EEPROM, 返回 OTA_ERR_CRC_MISMATCH
  -> 成功: 发 DONE, 重置重试计数器, 2s 后复位
  -> 失败: 发 ERR(CRC_MISMATCH), retry_count++
     retry_count < 3 -> ERROR -> 3s -> IDLE -> REQ (全量重来)
     retry_count >= 3 -> 继续运行当前固件, LED 指示更新失败

App 复位 -> Bootloader
  -> 读 EEPROM: status + key + fw_size + crc32

  Step 1: 源校验 (不触碰 A 区)
  -> 回读 W25Q16 fw_size 字节，计算 CRC
  -> 与 EEPROM crc32 比对
  -> 不匹配: 清 EEPROM 标志, 跳转 A 区旧 App

  Step 2: 搬运 + 目标校验
  -> 擦除 A 区 Flash
  -> 搬运 W25Q16 -> A 区
  -> 回读 A 区 fw_size 字节，计算 CRC
  -> 与 EEPROM crc32 比对
  -> 匹配: 清 EEPROM 标志, 跳转 A 区新 App
  -> 不匹配: 重试搬运 (最多 3 次)
     全部失败: 清 EEPROM 标志, 跳转出厂区
```

## 涉及文件清单

| 工程 | 文件 | 改动类型 |
|------|------|---------|
| P00_getway_led1_hal | Driver/MCU/crc32.c/h | 新增 |
| P00_getway_led1_hal | APP/app_update.c/h | 修改 |
| P00_getway_led1_hal | Core/Src/main.c | CubeMX 生成 CRC init |
| Project02_Application | Driver/MCU/crc32.c/h | 新增 |
| Project02_Application | Service/ota_storage.c/h | 修改 |
| Project02_Application | APP/app_ota_update.c | 修改 |
| Project02_Application | Protocol/CAN/can_proto.h | 修改 |
| Project02_Application | Core/Src/main.c | CubeMX 生成 CRC init |
| Project02_enterprise_bootloader | Driver/MCU/crc32.c/h | 新增 |
| Project02_enterprise_bootloader | APP/app_bootloader.c/h | 修改 |
| Project02_enterprise_bootloader | Core/Src/main.c | CubeMX 生成 CRC init |

## 日志打印

遵守项目约定：串口日志仅在错误和关键状态变化时打印，RECV_DATA 路径的 DATA 帧处理中禁止 printf。
日志前缀遵循现有规范：`[Host]`、`[APP]`、`[OTA]`、`[BL]`。

### P00 网关端 (`[Host]`)

```
[Host] CRC calc: 0x%08lX                    -- 收到 REQ 后计算 CRC 完成
[Host] Send END, crc=0x%08lX                 -- 发送 END 帧时
[Host] Update done                            -- 收到 DONE
[Host] Update error, wait retry               -- 收到 ERR
```

### App 接收端 (`[OTA]` / `[APP]`)

```
[OTA] CRC expected: 0x%08lX, got: 0x%08lX    -- CRC 比对结果（成功或失败）
[OTA] CRC pass, saving to EEPROM              -- CRC 匹配，写 EEPROM 前
[OTA] CRC fail, retry %d/%d                   -- CRC 不匹配，重试计数
[OTA] CRC retry limit reached, abort          -- 超过最大重试次数，放弃更新
[APP] Update complete, resetting              -- 发送 DONE 后，即将复位
```

**禁止打印的位置**：RECV_DATA 状态的 DATA 帧处理循环内（会导致 CAN FIFO 溢出）。

### Bootloader 搬运端 (`[BL]` / `[OTA]`)

```
[OTA] Source verify: expected=0x%08lX, got=0x%08lX  -- W25Q16 源校验结果
[OTA] Source verify pass                             -- 源校验通过
[BL] Source verify fail, skip update, jump app       -- 源校验失败，跳转旧 App
[OTA] Copying firmware %lu bytes...                  -- 开始搬运
[OTA] Target verify: expected=0x%08lX, got=0x%08lX   -- Flash 目标校验结果
[OTA] Target verify pass                             -- 目标校验通过
[BL] Target verify fail, retry %d/%d                 -- 目标校验失败，重试
[BL] Copy failed, jump factory                       -- 全部重试失败，跳转出厂区
[BL] Update success, jump app                        -- 更新成功，跳转新 App
```

## 约束

- 新建 .c 文件需手动添加到 Keil 工程并配置 include paths
- CubeMX 启用 CRC 外设后重新生成代码，`MX_CRC_Init()` 会在 `main.c` 中生成
- 所有工程的 CRC 驱动层代码相同，可直接复制
- printf 字符串使用英文（ARM Compiler V5 限制）
- **RECV_DATA 状态 DATA 帧处理路径中禁止 printf**（CAN FIFO 溢出风险）
