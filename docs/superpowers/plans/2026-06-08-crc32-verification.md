# CRC32 固件校验实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 STM32 硬件 CRC 外设替换现有的 8 字节头部校验，实现 P00/App/Bootloader 三端端到端固件完整性校验。

**Architecture:** 新增 CRC32 驱动层（`Driver/MCU/crc32.c/h`），三个工程各一份相同代码。P00 发送前计算 CRC 随 END 帧传递；App 接收后回读 W25Q16 校验，通过后 CRC 存 EEPROM；Bootloader 搬运前源校验（保护旧固件）、搬运后目标校验。CRC 不匹配时全量重试最多 3 次，超限后继续运行旧固件。

**Tech Stack:** STM32F103C8 硬件 CRC 外设（多项式 0x04C11DB7），Keil MDK-ARM 5，ARM Compiler V5

**Design Spec:** `docs/superpowers/specs/2026-06-08-crc32-verification-design.md`

---

## 文件变更清单

| 操作 | 文件路径 | 职责 |
|------|---------|------|
| 新增 | `{3个工程}/Driver/MCU/crc32.h` | CRC32 驱动头文件 |
| 新增 | `{3个工程}/Driver/MCU/crc32.c` | CRC32 驱动实现 |
| 修改 | `P00_getway_led1_hal/Protocol/CAN/can_proto.h` | UPDATE_END 载荷定义 |
| 修改 | `P00_getway_led1_hal/APP/app_update.h` | 结构体新增 fw_crc |
| 修改 | `P00_getway_led1_hal/APP/app_update.c` | 计算并发送 CRC |
| 修改 | `Project02_Application/Protocol/CAN/can_proto.h` | UPDATE_END 载荷定义 + 错误码 |
| 修改 | `Project02_Application/APP/app_ota_update.h` | 重试计数器 |
| 修改 | `Project02_Application/APP/app_ota_update.c` | 解析 END 中 CRC + 重试逻辑 |
| 修改 | `Project02_Application/Service/ota_storage.h` | expected_crc + CRC 地址常量 |
| 修改 | `Project02_Application/Service/ota_storage.c` | CRC 校验 + EEPROM 双次写入 |
| 修改 | `Project02_enterprise_bootloader/APP/app_bootloader.h` | CRC 地址常量 + 重试常量 |
| 修改 | `Project02_enterprise_bootloader/APP/app_bootloader.c` | 源校验 + 目标校验 + 安全清 EEPROM |

---

## Task 1: CRC32 驱动层（三个工程共用）

三个工程使用完全相同的 `crc32.c` 和 `crc32.h`。以下步骤以 `Project02_Application` 为例，完成后复制到另外两个工程。

**Files:**
- Create: `Project02_Application/Driver/MCU/crc32.h`
- Create: `Project02_Application/Driver/MCU/crc32.c`

- [ ] **Step 1: 创建 crc32.h**

```c
#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>

/*
 * STM32 硬件 CRC32 驱动
 *
 * 使用 STM32F1 内置 CRC 单元（多项式 0x04C11DB7，无输入/输出位反转）
 * CubeMX 中需启用 CRC 外设，main.c 中会生成 MX_CRC_Init()
 */

/*
 * 计算一段数据的 CRC32
 * @param data 数据指针
 * @param len  数据长度（字节）
 * @return CRC32 值
 */
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t len);

#endif
```

- [ ] **Step 2: 创建 crc32.c**

```c
#include "crc32.h"
#include "stm32f1xx_hal.h"

extern CRC_HandleTypeDef hcrc;

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t len)
{
    /* 复位 CRC 计算单元 */
    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t i = 0;

    /* 按 4 字节字写入 CRC->DR */
    while (i + 4 <= len)
    {
        uint32_t word = (uint32_t)data[i]
                      | ((uint32_t)data[i + 1] << 8)
                      | ((uint32_t)data[i + 2] << 16)
                      | ((uint32_t)data[i + 3] << 24);
        CRC->DR = word;
        i += 4;
    }

    /* 尾部不足 4 字节：补 0x00 对齐后写最后一字 */
    if (i < len)
    {
        uint32_t word = 0;
        uint32_t remaining = len - i;
        for (uint32_t j = 0; j < remaining; j++)
        {
            word |= (uint32_t)data[i + j] << (j * 8);
        }
        /* 高位补 0x00（已经是 0） */
        CRC->DR = word;
    }

    return CRC->DR;
}
```

- [ ] **Step 3: 复制到其他两个工程**

```bash
cp Project02_Application/Driver/MCU/crc32.h P00_getway_led1_hal/Driver/MCU/crc32.h
cp Project02_Application/Driver/MCU/crc32.c P00_getway_led1_hal/Driver/MCU/crc32.c
cp Project02_Application/Driver/MCU/crc32.h Project02_enterprise_bootloader/Driver/MCU/crc32.h
cp Project02_Application/Driver/MCU/crc32.c Project02_enterprise_bootloader/Driver/MCU/crc32.c
```

- [ ] **Step 4: Keil 工程中手动添加 crc32.c**

在三个工程的 Keil 中分别添加 `Driver/MCU/crc32.c` 到源文件分组，并在 Include Paths 中确认 `../Driver/MCU` 路径已存在。

- [ ] **Step 5: 确认 CubeMX CRC 配置**

P00 工程 `main.c` 已有 `#include "crc.h"` 和 `MX_CRC_Init()` 调用（已确认）。
Application 和 Bootloader 工程需在 CubeMX 中启用 CRC 外设并重新生成代码。

- [ ] **Step 6: 验证编译**

三个工程分别在 Keil 中按 F7 编译，确认无错误。

---

## Task 2: P00 网关端 — 计算并发送 CRC

**Files:**
- Modify: `P00_getway_led1_hal/Protocol/CAN/can_proto.h:14`
- Modify: `P00_getway_led1_hal/APP/app_update.h:36`
- Modify: `P00_getway_led1_hal/APP/app_update.c:86-91,29-37`

- [ ] **Step 1: 更新 P00 can_proto.h — UPDATE_END 注释**

将 `P00_getway_led1_hal/Protocol/CAN/can_proto.h` 第 14 行：

```c
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A */
```

改为：

```c
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A, payload: crc32(4B LE) */
```

- [ ] **Step 2: 在 app_update.h 结构体中添加 fw_crc 字段**

在 `P00_getway_led1_hal/APP/app_update.h` 的 `AppUpdate_t` 结构体中，在 `wait_ready_tick` 后面添加：

```c
    uint32_t wait_ready_tick; /* WAIT_READY 状态超时计时 */
    uint32_t fw_crc;          /* 固件 CRC32（发送前预计算） */
```

并在头部 includes 中添加：

```c
#include "crc32.h"
```

- [ ] **Step 3: 修改 app_update.c — 收到 REQ 时预计算 CRC**

在 `P00_getway_led1_hal/APP/app_update.c` 中添加 include：

```c
#include "crc32.h"
```

修改 `AppUpdate_WaitCmd` 函数，在 `send_ack(ctx)` 之前添加 CRC 计算。在原代码第 109 行 `send_ack(ctx);` 之前插入：

```c
            /* 预计算固件 CRC32 */
            ctx->fw_crc = CRC32_Calculate(ctx->fw_data, ctx->fw_size);
            printf("[Host] CRC calc: 0x%08lX\r\n", ctx->fw_crc);
```

- [ ] **Step 4: 修改 app_update.c — send_end 携带 CRC**

将 `send_end` 函数整体替换为：

```c
/**
 * @brief  发送 UPDATE_END 帧，携带 CRC32（4 字节小端序）
 */
static void send_end(AppUpdate_t *ctx)
{
    uint8_t end[5];
    end[0] = CAN_PROTO_CMD_UPDATE_END;
    end[1] = (uint8_t)(ctx->fw_crc);
    end[2] = (uint8_t)(ctx->fw_crc >> 8);
    end[3] = (uint8_t)(ctx->fw_crc >> 16);
    end[4] = (uint8_t)(ctx->fw_crc >> 24);
    CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_HOST, end, 5);
    printf("[Host] Send END, crc=0x%08lX\r\n", ctx->fw_crc);
}
```

- [ ] **Step 5: 验证编译**

Keil 中编译 P00 工程，确认无错误。

---

## Task 3: App 接收端 — OTA 存储模块 CRC 校验

**Files:**
- Modify: `Project02_Application/Service/ota_storage.h:23-45`
- Modify: `Project02_Application/Service/ota_storage.c:76-103`

- [ ] **Step 1: 更新 ota_storage.h — 新增常量和字段**

在 `Project02_Application/Service/ota_storage.h` 中：

a) 在 `OTA_EEPROM_SIZE_ADDR` 定义之后添加 CRC 地址常量和新错误码：

```c
#define OTA_EEPROM_SIZE_ADDR     0x13
#define OTA_EEPROM_CRC_ADDR      0x17    /* CRC32（4 字节小端） */
```

在错误码区域添加：

```c
#define OTA_ERR_SIZE_MISMATCH 0x05  /* 接收量与声明大小不匹配 */
#define OTA_ERR_CRC_MISMATCH  0x06  /* CRC32 校验不匹配 */
```

b) 在 `OTA_Storage_t` 结构体中，在 `total_recv` 之后添加：

```c
    uint32_t    total_recv;                    /* 已接收字节总数 */
    uint32_t    expected_crc;                  /* 期望的 CRC32（从 END 帧获取） */
```

c) 在函数声明区域，`OTA_Storage_Finish` 之后添加：

```c
/* 设置期望的 CRC32 值（从 UPDATE_END 帧解析） */
void OTA_Storage_SetExpectedCRC(OTA_Storage_t *ctx, uint32_t crc);
```

d) 添加 include：

```c
#include "crc32.h"
```

- [ ] **Step 2: 更新 ota_storage.c — 实现 CRC 校验和 EEPROM 双次写入**

将 `OTA_Storage_Finish` 函数整体替换为：

```c
int OTA_Storage_Finish(OTA_Storage_t *ctx)
{
    /* 校验实际接收量是否等于声明大小 */
    if (ctx->total_recv != ctx->fw_size) return OTA_ERR_SIZE_MISMATCH;

    /* 1. 刷剩余缓冲到 W25Q16 */
    int ret = ota_storage_flush(ctx);
    if (ret != 0) return OTA_ERR_FLASH_WRITE;

    /* 2. 回读 W25Q16 计算 CRC32 */
    uint32_t actual_crc = 0;
    uint8_t read_buf[256];
    uint32_t offset = 0;

    __HAL_CRC_DR_RESET(&hcrc);

    while (offset < ctx->fw_size)
    {
        uint16_t chunk = sizeof(read_buf);
        if (offset + chunk > ctx->fw_size)
            chunk = (uint16_t)(ctx->fw_size - offset);

        if (W25Q16_Read(ctx->w25q, offset, read_buf, chunk) != 0)
            return OTA_ERR_FLASH_WRITE;

        /* 逐块喂 CRC，复用 CRC32_Calculate 的逐字逻辑 */
        uint32_t i = 0;
        while (i + 4 <= chunk)
        {
            uint32_t word = (uint32_t)read_buf[i]
                          | ((uint32_t)read_buf[i + 1] << 8)
                          | ((uint32_t)read_buf[i + 2] << 16)
                          | ((uint32_t)read_buf[i + 3] << 24);
            CRC->DR = word;
            i += 4;
        }
        if (i < chunk)
        {
            uint32_t word = 0;
            for (uint32_t j = 0; j < chunk - i; j++)
                word |= (uint32_t)read_buf[i + j] << (j * 8);
            CRC->DR = word;
        }

        offset += chunk;
    }
    actual_crc = CRC->DR;

    /* 3. CRC 比对 */
    printf("[OTA] CRC expected: 0x%08lX, got: 0x%08lX\r\n",
           ctx->expected_crc, actual_crc);

    if (actual_crc != ctx->expected_crc)
        return OTA_ERR_CRC_MISMATCH;

    printf("[OTA] CRC pass, saving to EEPROM\r\n");

    /* 4. 第一次写 EEPROM：status + key + fw_size（7 字节，地址 0x10-0x16） */
    uint8_t eeprom_data[7];
    eeprom_data[0] = OTA_EEPROM_NEED_UPDATE;
    eeprom_data[1] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[2] = OTA_EEPROM_CHECK_KEY;
    eeprom_data[3] = (uint8_t)(ctx->fw_size);
    eeprom_data[4] = (uint8_t)(ctx->fw_size >> 8);
    eeprom_data[5] = (uint8_t)(ctx->fw_size >> 16);
    eeprom_data[6] = (uint8_t)(ctx->fw_size >> 24);

    ret = AT24C02_Write(ctx->eeprom, OTA_EEPROM_STATUS_ADDR,
                        eeprom_data, sizeof(eeprom_data));
    if (ret != 0) return OTA_ERR_EEPROM_WRITE;

    /* 5. 第二次写 EEPROM：crc32（4 字节，地址 0x17-0x1A） */
    uint8_t crc_data[4];
    crc_data[0] = (uint8_t)(ctx->expected_crc);
    crc_data[1] = (uint8_t)(ctx->expected_crc >> 8);
    crc_data[2] = (uint8_t)(ctx->expected_crc >> 16);
    crc_data[3] = (uint8_t)(ctx->expected_crc >> 24);

    ret = AT24C02_Write(ctx->eeprom, OTA_EEPROM_CRC_ADDR,
                        crc_data, sizeof(crc_data));
    if (ret != 0) return OTA_ERR_EEPROM_WRITE;

    return 0;
}
```

添加 `OTA_Storage_SetExpectedCRC` 和 `OTA_Storage_Reset` 更新：

```c
void OTA_Storage_SetExpectedCRC(OTA_Storage_t *ctx, uint32_t crc)
{
    ctx->expected_crc = crc;
}
```

同时更新 `OTA_Storage_Reset` 清空 expected_crc：

```c
void OTA_Storage_Reset(OTA_Storage_t *ctx)
{
    ctx->flash_addr   = 0;
    ctx->fw_size      = 0;
    ctx->buf_pos      = 0;
    ctx->total_recv   = 0;
    ctx->expected_crc = 0;
    memset(ctx->page_buf, 0xFF, OTA_STORAGE_PAGE_BUF_SIZE);
}
```

添加 include：

```c
#include "crc32.h"
```

在文件顶部添加 hcrc extern：

```c
extern CRC_HandleTypeDef hcrc;
```

- [ ] **Step 3: 验证编译**

Keil 中编译 Application 工程，确认无错误。

---

## Task 4: App 接收端 — OTA 状态机 CRC 集成 + 重试计数

**Files:**
- Modify: `Project02_Application/Protocol/CAN/can_proto.h:14,26-27`
- Modify: `Project02_Application/APP/app_ota_update.h:44`
- Modify: `Project02_Application/APP/app_ota_update.c:135-163,185-196`

- [ ] **Step 1: 更新 Application can_proto.h**

在 `Project02_Application/Protocol/CAN/can_proto.h` 中：

a) 将第 14 行改为：

```c
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A, payload: crc32(4B LE) */
```

b) 在错误码区域添加：

```c
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_TIMEOUT       0x04  /* 接收超时 */
#define OTA_ERR_CRC_MISMATCH  0x06  /* CRC32 校验不匹配 */
```

- [ ] **Step 2: 更新 app_ota_update.h — 添加重试计数器**

在 `Project02_Application/APP/app_ota_update.h` 中：

a) 在 `OTA_ERROR_BACKOFF` 后添加常量：

```c
#define OTA_ERROR_BACKOFF     3000

/* CRC 校验失败最大重试次数 */
#define OTA_MAX_CRC_RETRY     3
```

b) 在 `APP_OTA_t` 结构体 `error_code` 之后添加：

```c
    uint8_t        error_code;    /* 错误码 */
    uint8_t        crc_retry_cnt; /* CRC 校验失败重试计数 */
```

- [ ] **Step 3: 修改 app_ota_update.c — 解析 END 中的 CRC + 重试逻辑**

在 `Project02_Application/APP/app_ota_update.c` 中：

a) `APP_OTA_Init` 中初始化新字段，在 `ctx->error_code = 0;` 之后添加：

```c
    ctx->crc_retry_cnt = 0;
```

b) 替换 `OTA_STATE_RECV_DATA` 中处理 `UPDATE_END` 的代码块（原第 135-163 行）。

将原 `else if (cmd == CAN_PROTO_CMD_UPDATE_END)` 块替换为：

```c
            else if (cmd == CAN_PROTO_CMD_UPDATE_END)
            {
                /* 从 END 帧解析 CRC32（字节 1-4，小端序） */
                uint32_t expected_crc = 0;
                if (rx_msg[i].rxHeader.DLC >= 5)
                {
                    expected_crc = (uint32_t)rx_msg[i].data[1]
                                 | ((uint32_t)rx_msg[i].data[2] << 8)
                                 | ((uint32_t)rx_msg[i].data[3] << 16)
                                 | ((uint32_t)rx_msg[i].data[4] << 24);
                }
                OTA_Storage_SetExpectedCRC(&ctx->storage, expected_crc);

                printf("[OTA] END received, total_recv=%lu\r\n",
                       ctx->storage.total_recv);

                /* 刷缓冲 + 回读 W25Q16 校验 CRC + 写 EEPROM */
                int ret = OTA_Storage_Finish(&ctx->storage);
                if (ret != 0)
                {
                    printf("[OTA] Storage finish failed: %d\r\n", ret);
                    ctx->error_code = (uint8_t)ret;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 发送完成确认 */
                uint8_t done[1] = { CAN_PROTO_CMD_UPDATE_DONE };
                CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, done, 1);
                printf("[APP] Update complete, resetting\r\n");

                /* LED2 翻转指示成功 */
                HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);

                /* 延时确保 CAN 帧发出 + 串口打印完成，然后复位让 Bootloader 搬运固件 */
                HAL_Delay(100);
                NVIC_SystemReset();
            }
```

c) 修改 `OTA_STATE_ERROR` 块，增加 CRC 重试计数和超限处理：

将整个 `OTA_STATE_ERROR` case 替换为：

```c
    case OTA_STATE_ERROR:
        {
            /* 发送错误帧 */
            uint8_t err[2] = { CAN_PROTO_CMD_UPDATE_ERR, ctx->error_code };
            CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, err, 2);
            printf("[OTA] ERROR: code=0x%02X\r\n", ctx->error_code);

            /* CRC 不匹配时累计重试 */
            if (ctx->error_code == OTA_ERR_CRC_MISMATCH)
            {
                ctx->crc_retry_cnt++;
                printf("[OTA] CRC fail, retry %d/%d\r\n",
                       ctx->crc_retry_cnt, OTA_MAX_CRC_RETRY);

                if (ctx->crc_retry_cnt >= OTA_MAX_CRC_RETRY)
                {
                    printf("[OTA] CRC retry limit reached, abort\r\n");
                    /* 放弃更新，继续运行当前固件 */
                    ctx->crc_retry_cnt = 0;
                    ctx->state         = OTA_STATE_IDLE;
                    ctx->state_tick    = 0;  /* 清零，不进入退避 */
                    break;
                }
            }

            OTA_Storage_Reset(&ctx->storage);
            ctx->state      = OTA_STATE_IDLE;
            ctx->state_tick = HAL_GetTick();
        }
        break;
```

d) 在 `OTA_STATE_WAIT_ACK` 中收到 ACK 成功时重置 CRC 重试计数（在 `ctx->expect_seq = 0;` 之后添加）：

```c
                ctx->expect_seq = 0;
                ctx->crc_retry_cnt = 0;
```

- [ ] **Step 4: 验证编译**

Keil 中编译 Application 工程，确认无错误。

---

## Task 5: Bootloader 端 — 源校验 + 目标校验 + 安全清 EEPROM

**Files:**
- Modify: `Project02_enterprise_bootloader/APP/app_bootloader.h:9,23-24,37-38`
- Modify: `Project02_enterprise_bootloader/APP/app_bootloader.c:35-69,136-214`

- [ ] **Step 1: 更新 app_bootloader.h — 新增常量**

在 `Project02_enterprise_bootloader/APP/app_bootloader.h` 中：

a) 更新文件头 EEPROM 布局注释，将原来的：

```
 *   [0x13~0x16] fw_size 固件大小（小端序，4 字节）
```

替换为：

```
 *   [0x13~0x16] fw_size  固件大小（小端序，4 字节）
 *   [0x17~0x1A] crc32    固件 CRC32（小端序，4 字节）
```

b) 在 `FW_SIZE_ADDR` 定义之后添加：

```c
#define FW_SIZE_ADDR        (CHECK_UPDATE_ADDR + 3)   /* 0x13 */
#define CRC32_ADDR          0x17                       /* CRC32（4 字节小端） */
```

c) 在 `CHECK_KEY` 定义之后添加搬运重试常量：

```c
#define CHECK_KEY           0xA5A5

/* Flash 搬运最大重试次数（搬运后 CRC 校验失败时） */
#define BL_MAX_COPY_RETRY   3
```

d) 添加 include：

```c
#include "crc32.h"
```

- [ ] **Step 2: 修改 app_bootloader.c — 替换校验和搬运逻辑**

在 `Project02_enterprise_bootloader/APP/app_bootloader.c` 中：

a) 添加 include：

```c
#include "crc32.h"
```

b) 在文件顶部添加 hcrc extern（在 extern 变量区域）：

```c
extern CRC_HandleTypeDef hcrc;
```

c) 删除整个 `verify_w25q_firmware` 函数（原第 23-69 行）

d) 新增两个辅助函数：`verify_w25q_source` 和 `verify_flash_target`，放在 `App_bootloader_check_update` 之前：

e) **关键修改：`App_bootloader_check_update` 不再立即清除 EEPROM key**

现有 `App_bootloader_check_update` 在读取 EEPROM 后立即清零密钥（一次性触发）。但断电恢复需要 key 保持到搬运成功后才清除。将函数中第 113-117 行的"立即清零密钥"代码删除：

原代码（第 111-119 行）：
```c
    app_boot_update_status = data[0];

    /* 立即清零密钥和状态，防止复位后再次触发（一次性触发） */
    data[0] = BOOT_NO_UPDATE;
    data[1] = 0x00;
    data[2] = 0x00;
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR, data, 3);

    printf("[BL] Update status: 0x%02X\r\n", app_boot_update_status);
```

替换为：
```c
    app_boot_update_status = data[0];
    printf("[BL] Update status: 0x%02X\r\n", app_boot_update_status);
```

密钥改由 `App_bootloader_update` 中的 `clear_eeprom_flag_safe()` 在搬运成功后清除。
如果搬运中途断电，重启后 Bootloader 重新检测到有效 key，会重新执行源校验 + 搬运（幂等）。

```c
/*
 * verify_w25q_source —— 搬运前源校验：回读 W25Q16 计算 CRC
 *
 * 不触碰 A 区 Flash，校验不通过时旧固件完好
 *
 * 返回: 0=CRC 匹配, -1=不匹配或读取失败
 */
static int verify_w25q_source(uint32_t fw_size, uint32_t expected_crc)
{
    uint8_t buf[TRANSFER_BUF_SIZE];
    uint32_t offset = 0;

    __HAL_CRC_DR_RESET(&hcrc);

    while (offset < fw_size)
    {
        uint16_t chunk = TRANSFER_BUF_SIZE;
        if (offset + chunk > fw_size)
            chunk = (uint16_t)(fw_size - offset);

        if (W25Q16_Read(&w25q_dev, W25Q16_FW_ADDR + offset, buf, chunk) != 0)
        {
            printf("[OTA] W25Q16 read failed at offset %lu\r\n", offset);
            return -1;
        }

        uint32_t i = 0;
        while (i + 4 <= chunk)
        {
            uint32_t word = (uint32_t)buf[i]
                          | ((uint32_t)buf[i + 1] << 8)
                          | ((uint32_t)buf[i + 2] << 16)
                          | ((uint32_t)buf[i + 3] << 24);
            CRC->DR = word;
            i += 4;
        }
        if (i < chunk)
        {
            uint32_t word = 0;
            for (uint32_t j = 0; j < chunk - i; j++)
                word |= (uint32_t)buf[i + j] << (j * 8);
            CRC->DR = word;
        }

        offset += chunk;
    }

    uint32_t actual_crc = CRC->DR;
    printf("[OTA] Source verify: expected=0x%08lX, got=0x%08lX\r\n",
           expected_crc, actual_crc);

    if (actual_crc != expected_crc)
        return -1;

    printf("[OTA] Source verify pass\r\n");
    return 0;
}

/*
 * verify_flash_target —— 搬运后目标校验：回读 A 区 Flash 计算 CRC
 *
 * 返回: 0=CRC 匹配, -1=不匹配
 */
static int verify_flash_target(uint32_t fw_size, uint32_t expected_crc)
{
    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t addr = A_REGION_ADDR;
    uint32_t end  = A_REGION_ADDR + fw_size;

    while (addr < end)
    {
        uint32_t word = *(volatile uint32_t *)addr;
        CRC->DR = word;
        addr += 4;
    }

    uint32_t actual_crc = CRC->DR;
    printf("[OTA] Target verify: expected=0x%08lX, got=0x%08lX\r\n",
           expected_crc, actual_crc);

    if (actual_crc != expected_crc)
        return -1;

    printf("[OTA] Target verify pass\r\n");
    return 0;
}

/*
 * clear_eeprom_flag_safe —— 安全清除 EEPROM 更新标志
 *
 * 清除顺序：先废密钥（0x0000），再清状态
 * 断电安全：即使第一步后断电，密钥已无效，Bootloader 不会触发更新
 */
static void clear_eeprom_flag_safe(void)
{
    /* Step 1: 先废密钥（地址 0x11-0x12 写 0x00） */
    uint8_t zero_key[2] = { 0x00, 0x00 };
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR + 1, zero_key, 2);

    /* Step 2: 再清状态（地址 0x10 写 0x00） */
    uint8_t zero_status = BOOT_NO_UPDATE;
    AT24C02_Write(&eeprom_dev, CHECK_UPDATE_ADDR, &zero_status, 1);
}
```

f) 替换 `App_bootloader_update` 函数整体为：

```c
/*
 * App_bootloader_update —— 搬运固件（带源校验 + 目标校验）
 *
 * 流程：
 *   1. 读 EEPROM 获取 fw_size 和 expected_crc
 *   2. 源校验：回读 W25Q16 计算 CRC（不触碰 A 区 Flash）
 *      不通过 -> 清 EEPROM 标志，跳转旧 App
 *   3. 搬运 W25Q16 -> A 区 Flash
 *   4. 目标校验：回读 A 区 Flash 计算 CRC
 *      不通过 -> 重试搬运（最多 BL_MAX_COPY_RETRY 次）
 *      全失败 -> 清 EEPROM 标志，跳转出厂区
 *   5. 成功 -> 安全清除 EEPROM 标志，跳转新 App
 */
int App_bootloader_update(void)
{
    uint8_t buf[TRANSFER_BUF_SIZE];

    /* 第 1 步：确认 W25Q16 芯片在线且型号正确 */
    uint32_t id = W25Q16_ReadJEDECID(&w25q_dev);
    if (id != 0xEF4015)
    {
        printf("[BL] W25Q16 ID error: 0x%06lX\r\n", id);
        return -1;
    }

    /* 第 2 步：从 EEPROM 读出固件大小（4 字节小端） */
    uint8_t size_buf[4];
    if (AT24C02_Read(&eeprom_dev, FW_SIZE_ADDR, size_buf, 4) != 0)
    {
        printf("[BL] EEPROM read fw_size failed\r\n");
        return -1;
    }
    app_boot_fw_size = size_buf[0] | ((uint32_t)size_buf[1] << 8)
                     | ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);

    if (app_boot_fw_size == 0 || app_boot_fw_size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[BL] Invalid fw_size: %lu\r\n", app_boot_fw_size);
        return -1;
    }

    /* 第 3 步：从 EEPROM 读出 CRC32（4 字节小端） */
    uint8_t crc_buf[4];
    if (AT24C02_Read(&eeprom_dev, CRC32_ADDR, crc_buf, 4) != 0)
    {
        printf("[BL] EEPROM read crc32 failed\r\n");
        return -1;
    }
    uint32_t expected_crc = (uint32_t)crc_buf[0]
                          | ((uint32_t)crc_buf[1] << 8)
                          | ((uint32_t)crc_buf[2] << 16)
                          | ((uint32_t)crc_buf[3] << 24);

    printf("[BL] Start update: %lu bytes, crc=0x%08lX\r\n",
           app_boot_fw_size, expected_crc);

    /* 第 4 步：源校验 — 回读 W25Q16 验证 CRC（不触碰 A 区） */
    if (verify_w25q_source(app_boot_fw_size, expected_crc) != 0)
    {
        printf("[BL] Source verify fail, skip update, jump app\r\n");
        clear_eeprom_flag_safe();
        return -2;  /* -2 表示应跳转旧 App（调用者判断） */
    }

    /* 第 5 步：搬运 W25Q16 -> A 区 Flash（最多重试 BL_MAX_COPY_RETRY 次） */
    int copy_result = -1;
    for (int retry = 0; retry < BL_MAX_COPY_RETRY; retry++)
    {
        FlashDownload_t dl_ctx;
        FlashDownload_Init(&dl_ctx);

        uint32_t offset = 0;
        int write_ok = 1;

        while (offset < app_boot_fw_size)
        {
            uint16_t chunk = TRANSFER_BUF_SIZE;
            if (offset + chunk > app_boot_fw_size)
                chunk = (uint16_t)(app_boot_fw_size - offset);

            if (W25Q16_Read(&w25q_dev, W25Q16_FW_ADDR + offset, buf, chunk) != 0)
            {
                printf("[BL] W25Q16 read failed at offset %lu\r\n", offset);
                write_ok = 0;
                break;
            }

            if (FlashDownload_WriteFrame(&dl_ctx, buf, chunk) != 0)
            {
                printf("[BL] Flash write failed at offset %lu\r\n", offset);
                write_ok = 0;
                break;
            }

            offset += chunk;
        }

        if (!write_ok) continue;

        /* 目标校验：回读 A 区 Flash 验证 CRC */
        if (verify_flash_target(app_boot_fw_size, expected_crc) != 0)
        {
            printf("[BL] Target verify fail, retry %d/%d\r\n",
                   retry + 1, BL_MAX_COPY_RETRY);
            continue;
        }

        copy_result = 0;
        break;
    }

    if (copy_result != 0)
    {
        printf("[BL] Copy failed, jump factory\r\n");
        clear_eeprom_flag_safe();
        return -3;  /* -3 表示应跳转出厂区 */
    }

    /* 第 6 步：安全清除 EEPROM 标志 */
    clear_eeprom_flag_safe();
    printf("[BL] Update success, jump app\r\n");
    return 0;
}
```

f) 更新 `main.c` 调用方（如果有判断返回值的逻辑）。需要确认 Bootloader 的 `main.c` 中 `App_bootloader_update()` 返回值的处理方式。根据 `CLAUDE.md` 中的启动流程：

```c
App_bootloader_check_update();
// 根据 app_boot_update_status 决定分支
```

如果 `App_bootloader_update()` 返回 -2（源校验失败），调用者应跳转旧 App；如果返回 -3，应跳转出厂区。需要确认 main.c 中的调用逻辑。

- [ ] **Step 3: 验证编译**

Keil 中编译 Bootloader 工程，确认无错误。

---

## Task 6: Bootloader main.c 适配返回值

**Files:**
- Modify: `Project02_enterprise_bootloader/Core/Src/main.c`

- [ ] **Step 1: 读取 main.c USER CODE 区域**

读取 `Project02_enterprise_bootloader/Core/Src/main.c`，找到 `USER CODE BEGIN 2` 和 `USER CODE BEGIN 3` 中调用 `App_bootloader_update()` 的逻辑。需要确认返回值处理方式。

- [ ] **Step 2: 更新 update 返回值处理**

`App_bootloader_update()` 新增了返回值 -2（源校验失败）和 -3（搬运全失败），需要在 main.c 中正确处理：

- 返回 0：搬运成功，跳转新 App（`App_bootloader_jump_app()`）
- 返回 -2：源校验失败，EEPROM 已清除，跳转旧 App（`App_bootloader_jump_app()`）
- 返回 -3：搬运全失败，EEPROM 已清除，跳转出厂区（`App_bootloader_factory_reset()`）
- 返回 -1：其他错误（W25Q16 ID 错误、EEPROM 读取失败等），保持原逻辑（停机或跳转旧 App）

- [ ] **Step 3: 验证编译**

Keil 中编译 Bootloader 工程，确认无错误。

---

## Task 7: 同步 can_proto.h

**Files:**
- Modify: `P00_getway_led1_hal/Protocol/CAN/can_proto.h`
- Modify: `Project02_Application/Protocol/CAN/can_proto.h`

- [ ] **Step 1: 确认两个 can_proto.h 的 UPDATE_END 注释一致**

P00 和 Application 的 `CAN_PROTO_CMD_UPDATE_END` 注释都应为：

```c
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A, payload: crc32(4B LE) */
```

（Task 2 和 Task 4 中已分别修改，此步骤为确认同步。）

- [ ] **Step 2: 确认 P00 can_proto.h 包含错误码定义**

P00 的 `can_proto.h` 目前缺少 `OTA_ERR_CRC_MISMATCH`。虽然 P00 不直接使用此错误码，但为保持同步，在 P00 的 `can_proto.h` 末尾添加：

```c
/* 错误码 */
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_TIMEOUT       0x04  /* 接收超时 */
#define OTA_ERR_CRC_MISMATCH  0x06  /* CRC32 校验不匹配 */
```

- [ ] **Step 3: 验证两端编译通过**

两个工程分别编译确认无错误。

---

## Task 8: 端到端集成测试

- [ ] **Step 1: 烧录 P00**

Keil 中 Erase Sectors 模式烧录 P00 工程到 0x08000000。

- [ ] **Step 2: 烧录 Bootloader**

Keil 中 Erase Sectors 模式烧录 Bootloader 工程到 0x08000000。

- [ ] **Step 3: 烧录 App**

Keil 中 Erase Sectors 模式烧录 Application 工程到 0x08008000。

- [ ] **Step 4: 测试正常 OTA 更新**

1. 通过串口向 P00 发送固件（或使用 Flash 中已有的固件）
2. App 上电后发送 REQ
3. 观察串口日志：
   - P00: `[Host] CRC calc: 0x...`
   - P00: `[Host] Send END, crc=0x...`
   - App: `[OTA] CRC expected: 0x..., got: 0x...`
   - App: `[OTA] CRC pass, saving to EEPROM`
   - App: `[APP] Update complete, resetting`
4. Bootloader 应打印：
   - `[OTA] Source verify: expected=0x..., got=0x...`
   - `[OTA] Source verify pass`
   - `[OTA] Copying firmware N bytes...`
   - `[OTA] Target verify: expected=0x..., got=0x...`
   - `[OTA] Target verify pass`
   - `[BL] Update success, jump app`

- [ ] **Step 5: 测试 CRC 不匹配场景**

在 P00 修改固件数据（或发送错误大小的固件），验证 App 端 CRC 校验失败后：
1. 串口打印 `[OTA] CRC fail, retry 1/3`
2. 自动重试
3. 3 次后打印 `[OTA] CRC retry limit reached, abort`
4. App 继续运行旧固件

- [ ] **Step 6: 提交代码**

```bash
git add -A
git commit -m "feat: add hardware CRC32 firmware verification for OTA updates"
```
