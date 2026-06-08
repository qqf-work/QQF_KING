# App Bootloader 用户交互实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 APP 层新建 `app_bootloader.c/h`，封装串口交互式 Bootloader 流程，替换 main.c 中的裸逻辑。

**Architecture:** 状态机模式（与 ota_update.c 风格一致）。main.c 主循环每次调用 `AppBootloader_Process()`，该函数根据当前状态分发到对应处理函数。状态包括 IDLE → WAIT_START → RECV_SIZE → TRANSFERRING → VERIFY → JUMP/ERROR。

**Tech Stack:** STM32 HAL, C (ARM Compiler V5), Keil MDK-ARM 5

---

### Task 1: 创建 `APP/app_bootloader.h`

**Files:**
- Create: `APP/app_bootloader.h`

- [ ] **Step 1: 创建头文件**

```c
#ifndef __APP_BOOTLOADER_H__
#define __APP_BOOTLOADER_H__

#include "flash_download.h"

/* 状态枚举 */
typedef enum {
    APPBL_IDLE,
    APPBL_WAIT_START,
    APPBL_RECV_SIZE,
    APPBL_TRANSFERRING,
    APPBL_VERIFY,
    APPBL_JUMP,
    APPBL_ERROR
} AppBL_State_t;

/* 上下文结构体 */
typedef struct {
    AppBL_State_t   state;
    uint32_t        expected_size;     /* 用户声明的固件字节数 */
    uint32_t        last_frame_tick;   /* 最后收到帧的 tick */
    FlashDownload_t dl_ctx;            /* 下载上下文（内嵌） */
} AppBootloader_t;

/* 初始化：打印欢迎菜单，进入 WAIT_START */
void AppBootloader_Init(AppBootloader_t *ctx);

/* 主循环调用，处理一帧或超时事件 */
void AppBootloader_Process(AppBootloader_t *ctx);

#endif
```

---

### Task 2: 创建 `APP/app_bootloader.c` — 辅助函数与 Init

**Files:**
- Create: `APP/app_bootloader.c`

- [ ] **Step 1: 创建源文件骨架（includes + 辅助函数 + Init）**

```c
#include "app_bootloader.h"
#include "bootloader.h"
#include "uart_buf.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ---------- 命令解析辅助 ---------- */

/* 检查帧是否匹配命令前缀，返回前缀长度（0=不匹配） */
static uint16_t match_cmd(uint8_t *data, uint16_t len, const char *cmd)
{
    uint16_t cmd_len = (uint16_t)strlen(cmd);
    if (len >= cmd_len && memcmp(data, cmd, cmd_len) == 0)
        return cmd_len;
    return 0;
}

/* 从 "SIZE:" 之后的数字串解析出 uint32，返回 0 成功, -1 失败 */
static int parse_size(uint8_t *data, uint16_t len, uint32_t *out)
{
    uint32_t val = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        if (data[i] >= '0' && data[i] <= '9')
            val = val * 10 + (data[i] - '0');
        else if (data[i] == '\r' || data[i] == '\n')
            break;
        else
            return -1;
    }
    *out = val;
    return (val == 0) ? -1 : 0;
}

/* ---------- Init ---------- */

void AppBootloader_Init(AppBootloader_t *ctx)
{
    ctx->state          = APPBL_IDLE;
    ctx->expected_size  = 0;
    ctx->last_frame_tick = HAL_GetTick();

    printf("\r\n===== STM32 Bootloader =====\r\n");
    printf("Commands:\r\n");
    printf("  START       - Begin firmware transfer\r\n");
    printf("  SIZE:<n>    - Declare firmware size in bytes\r\n");
    printf("Waiting for START...\r\n");
    printf("============================\r\n\r\n");

    ctx->state = APPBL_WAIT_START;
}
```

---

### Task 3: 添加状态处理函数与 Process 分发

**Files:**
- Modify: `APP/app_bootloader.c`（在 Init 函数之后追加）

- [ ] **Step 1: 添加所有状态处理函数和 Process**

在 Task 2 创建的文件末尾（`AppBootloader_Init` 函数之后）追加以下代码：

```c
/* ---------- 状态处理函数 ---------- */

static void handle_wait_start(AppBootloader_t *ctx, uint8_t *data, uint16_t len)
{
    if (match_cmd(data, len, "START"))
    {
        printf("[BL] Ready. Send SIZE:<bytes>\r\n");
        ctx->state = APPBL_RECV_SIZE;
    }
    else
    {
        printf("[BL] Unknown command. Send START to begin.\r\n");
    }
}

static void handle_recv_size(AppBootloader_t *ctx, uint8_t *data, uint16_t len)
{
    uint16_t prefix_len = match_cmd(data, len, "SIZE:");
    if (prefix_len == 0)
    {
        printf("[BL] Expected SIZE:<bytes>\r\n");
        return;
    }

    uint32_t size;
    if (parse_size(data + prefix_len, len - prefix_len, &size) != 0)
    {
        printf("[BL] Invalid SIZE value\r\n");
        return;
    }

    if (size > A_PAGE_NUM * FLASH__PAGE_SIZE)
    {
        printf("[BL] SIZE too large: %lu > %lu\r\n",
               size, (uint32_t)(A_PAGE_NUM * FLASH__PAGE_SIZE));
        ctx->state = APPBL_ERROR;
        return;
    }

    ctx->expected_size = size;
    FlashDownload_Init(&ctx->dl_ctx);
    printf("[BL] Expecting %lu bytes. Send bin file now.\r\n", size);
    ctx->state = APPBL_TRANSFERRING;
}

static void handle_verify(AppBootloader_t *ctx)
{
    uint32_t total = FlashDownload_GetTotal(&ctx->dl_ctx);

    if (total == ctx->expected_size)
    {
        printf("[BL] Verify OK: %lu bytes\r\n", total);
        ctx->state = APPBL_JUMP;
    }
    else
    {
        printf("[BL] Verify FAIL: expected %lu, got %lu\r\n",
               ctx->expected_size, total);
        ctx->state = APPBL_ERROR;
    }
}

static void handle_jump(AppBootloader_t *ctx)
{
    printf("[BL] Jumping to App...\r\n");
    if (Bootloader_JumpToApp() != 0)
    {
        printf("[BL] Jump failed\r\n");
        ctx->state = APPBL_ERROR;
    }
}

/* ---------- 主处理函数 ---------- */

void AppBootloader_Process(AppBootloader_t *ctx)
{
    /* 有新帧：根据状态分发 */
    if (uart_rx_queue.URxDataOUT != uart_rx_queue.URxDataIN)
    {
        uint16_t len = uart_rx_queue.URxDataOUT->end
                      - uart_rx_queue.URxDataOUT->start + 1;
        uint8_t *data = uart_rx_queue.URxDataOUT->start;

        ctx->last_frame_tick = HAL_GetTick();

        switch (ctx->state)
        {
        case APPBL_WAIT_START:
            handle_wait_start(ctx, data, len);
            break;
        case APPBL_RECV_SIZE:
            handle_recv_size(ctx, data, len);
            break;
        case APPBL_TRANSFERRING:
            FlashDownload_WriteFrame(&ctx->dl_ctx, data, len);
            break;
        default:
            break;
        }

        /* 释放帧 */
        uart_rx_queue.URxDataOUT++;
        if (uart_rx_queue.URxDataOUT > uart_rx_queue.URxDataEND)
            uart_rx_queue.URxDataOUT = &uart_rx_queue.URxDataPtr[0];
    }
    else
    {
        /* 无帧：超时检查 */
        uint32_t elapsed = HAL_GetTick() - ctx->last_frame_tick;

        if (ctx->state == APPBL_RECV_SIZE && elapsed > 30000)
        {
            printf("[BL] SIZE timeout, back to WAIT_START\r\n");
            ctx->state = APPBL_WAIT_START;
        }
        else if (ctx->state == APPBL_TRANSFERRING && elapsed > 2000)
        {
            ctx->state = APPBL_VERIFY;
            handle_verify(ctx);
        }
        else if (ctx->state == APPBL_JUMP)
        {
            handle_jump(ctx);
        }
        else if (ctx->state == APPBL_ERROR)
        {
            /* 停机：不再处理任何帧，等待硬件复位 */
        }
    }
}
```

注意：VERIFY 状态在无帧分支中触发（2s 超时后），JUMP 状态也在无帧分支中执行（确保所有帧处理完毕后再跳转）。

---

### Task 4: 修改 `Core/Src/main.c`

**Files:**
- Modify: `Core/Src/main.c`（仅在 USER CODE 块内修改）

- [ ] **Step 1: 替换 Includes 块**

将 `/* USER CODE BEGIN Includes */` 到 `/* USER CODE END Includes */` 的内容替换为：

```c
/* USER CODE BEGIN Includes */
#include "app_bootloader.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
/* USER CODE END Includes */
```

- [ ] **Step 2: 删除 JUMP_TIMEOUT_MS 定义**

将 `/* USER CODE BEGIN PD */` 到 `/* USER CODE END PD */` 替换为空：

```c
/* USER CODE BEGIN PD */

/* USER CODE END PD */
```

- [ ] **Step 3: 替换私有变量**

将 `/* USER CODE BEGIN PV */` 到 `/* USER CODE END PV */` 替换为：

```c
/* USER CODE BEGIN PV */
static AppBootloader_t app_bl_ctx;
/* USER CODE END PV */
```

- [ ] **Step 4: 替换 USER CODE BEGIN 2 初始化**

将 `/* USER CODE BEGIN 2 */` 到 `/* USER CODE END 2 */` 替换为：

```c
/* USER CODE BEGIN 2 */

  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
  UART_DMA_Rx_Init();
  AppBootloader_Init(&app_bl_ctx);
  /* USER CODE END 2 */
```

- [ ] **Step 5: 替换主循环体**

将 `/* USER CODE BEGIN 3 */` 到 `/* USER CODE END 3 */` 替换为：

```c
    /* USER CODE BEGIN 3 */
    AppBootloader_Process(&app_bl_ctx);
    /* USER CODE END 3 */
```

---

### Task 5: 更新 Keil 工程文件

**Files:**
- Modify: `MDK-ARM/Project01_learn_Bootloader_led.uvprojx`

- [ ] **Step 1: 在 APP 组中添加新文件**

在 `<FilePath>..\APP\bootloader.h</FilePath>` 的 `</File>` 后面（第 699 行附近），插入：

```xml
            <File>
              <FileName>app_bootloader.c</FileName>
              <FileType>1</FileType>
              <FilePath>..\APP\app_bootloader.c</FilePath>
            </File>
            <File>
              <FileName>app_bootloader.h</FileName>
              <FileType>5</FileType>
              <FilePath>..\APP\app_bootloader.h</FilePath>
            </File>
```

---

### Task 6: 编译验证

- [ ] **Step 1: 在 Keil 中编译**

用 Keil 打开工程，按 F7 编译。预期：0 Error，0 Warning。

- [ ] **Step 2: 串口功能验证**

烧录后通过串口助手（115200）验证交互流程：
1. 上电后看到欢迎菜单和 "Waiting for START..."
2. 发送 `START` → 回复 "Ready. Send SIZE:<bytes>"
3. 发送 `SIZE:1234` → 回复 "Expecting 1234 bytes. Send bin file now."
4. 发送 bin 文件 → 看到 [DL] 写入进度
5. 2s 无新帧 → 显示校验结果
6. 校验通过 → 跳转 App；校验失败 → 显示错误，停机
