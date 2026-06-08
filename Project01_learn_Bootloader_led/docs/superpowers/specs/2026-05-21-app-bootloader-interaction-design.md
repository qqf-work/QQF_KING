# App Bootloader 用户交互设计

## 目标

在 APP 层新建 `app_bootloader.c/h`，封装 Bootloader 的完整用户交互流程：串口命令触发传输、字节数校验、跳转。`main.c` 精简为 Init + Process 调用。

## 架构：状态机

采用与 `ota_update.c` 一致的状态机模式，`main.c` 主循环每次调用 `AppBootloader_Process()`，根据当前状态分发处理。

## 状态流转

```
IDLE ──(启动)──→ WAIT_START ──(收到 "START")──→ RECV_SIZE ──(收到 "SIZE:<n>")──→ TRANSFERRING
                                                                              │
                        ERROR ←──(校验失败)── VERIFY ←──(超时2s无新帧)─────────────┘
                          │                      │
                          ↓                      ↓
                       (停机等复位)           JUMP → Bootloader_JumpToApp()
```

## 各状态职责

| 状态 | 触发条件 | 动作 |
|------|----------|------|
| `IDLE` | 上电/复位 | 打印欢迎菜单，立即转入 `WAIT_START` |
| `WAIT_START` | 收到 `START` 命令 | 打印 "ready"，转入 `RECV_SIZE` |
| `RECV_SIZE` | 收到 `SIZE:<十进制数>` 命令 | 解析字节数存入 `expected_size`，初始化 `FlashDownload`，转入 `TRANSFERRING` |
| `TRANSFERRING` | 收到 bin 帧数据 | `FlashDownload_WriteFrame()` 写入 Flash，更新已接收字节数 |
| `VERIFY` | 2 秒无新帧 | 比较 `total_written` 与 `expected_size`，匹配则转 `JUMP`，否则转 `ERROR` |
| `JUMP` | 校验通过 | 调用 `Bootloader_JumpToApp()` |
| `ERROR` | 校验失败 / 解析失败 | 打印错误详情，停机（需复位恢复） |

## 命令格式

ASCII 文本，以 `\r\n` 或 `\n` 结尾：

- `START` — 用户确认开始传输
- `SIZE:12345` — 告知固件字节数（十进制）
- 非 `START` / `SIZE:` 开头的帧 → 视为 bin 数据

## 命令解析策略

UART 帧队列不区分命令和数据，通过当前状态判断帧类型：

- `WAIT_START` / `RECV_SIZE` 状态：对帧内容做字符串匹配（前缀检查）
- `TRANSFERRING` 状态：所有帧视为 bin 原始数据

辅助函数：

```c
// 检查帧是否匹配命令前缀，返回匹配长度（0=不匹配）
static uint16_t match_cmd(uint8_t *data, uint16_t len, const char *cmd);

// 从 "SIZE:12345" 中解析出数字，返回 0 成功, -1 失败
static int parse_size(uint8_t *data, uint16_t len, uint32_t *out);
```

## 上下文结构体

```c
typedef enum {
    APPBL_IDLE,
    APPBL_WAIT_START,
    APPBL_RECV_SIZE,
    APPBL_TRANSFERRING,
    APPBL_VERIFY,
    APPBL_JUMP,
    APPBL_ERROR
} AppBL_State_t;

typedef struct {
    AppBL_State_t  state;
    uint32_t       expected_size;    // 用户声明的固件字节数
    uint32_t       last_frame_tick;  // 最后收到帧的 tick
    FlashDownload_t dl_ctx;          // 下载上下文（内嵌）
} AppBootloader_t;
```

## 超时处理

- `WAIT_START`：无超时，一直等待
- `RECV_SIZE`：30 秒内没收到 SIZE 命令 → 打印超时提示，回到 `WAIT_START`
- `TRANSFERRING`：2 秒无新帧 → 进入 `VERIFY`

## 公开 API

```c
// 初始化：打印欢迎菜单，设状态为 WAIT_START
void AppBootloader_Init(AppBootloader_t *ctx);

// 主循环调用，每次处理一帧或一个超时事件
void AppBootloader_Process(AppBootloader_t *ctx);
```

`AppBootloader_Process()` 内部逻辑：

1. 检查 `uart_rx_queue` 有无新帧
2. 有帧：根据 state 分发到对应处理函数，更新 `last_frame_tick`，释放帧（OUT 指针前移）
3. 无帧：RECV_SIZE 检查 30s 超时；TRANSFERRING 检查 2s 超时

## main.c 变更

```c
static AppBootloader_t app_bl_ctx;

// USER CODE BEGIN 2
AppBootloader_Init(&app_bl_ctx);

// 主循环
while (1) {
    AppBootloader_Process(&app_bl_ctx);
}
```

原来的 `dl_ctx`、`last_write_tick`、`data_received`、`JUMP_TIMEOUT_MS` 全部移入 `app_bootloader.c` 内部管理。

## 文件变更范围

| 文件 | 操作 | 说明 |
|------|------|------|
| `APP/app_bootloader.c` | 新建 | 状态机 + 命令解析 |
| `APP/app_bootloader.h` | 新建 | 公开接口和状态枚举 |
| `Core/Src/main.c` | 修改 | 精简为 Init + Process 调用 |

不改动 `bootloader.c/h`、`flash_download.c/h`、`uart_buf.c/h`。

## 日志前缀

新增 `[BL]` 前缀用于 Bootloader 交互日志（与现有 `bootloader.c` 的 `[BL]` 前缀一致）。
