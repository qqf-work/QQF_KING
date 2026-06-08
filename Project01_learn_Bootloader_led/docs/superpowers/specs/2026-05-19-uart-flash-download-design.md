# UART Flash Download Demo 设计

移除 OTA 相关代码，实现通过串口直接将 bin 文件写入 STM32 内部 Flash A 区。

## 变更范围

| 文件 | 操作 | 说明 |
|------|------|------|
| `Driver/MCU/flash.c/h` | 修改 | 新增 `Flash_NeedsErase()` |
| `Service/flash_download.c/h` | 新建 | 下载逻辑封装 |
| `Core/Src/main.c` | 修改 | 移除 OTA，精简为 init + 帧队列轮询 |

## flash.c 新增接口

```c
// 检查 [addr, addr+len) 是否全为 0xFF
// 返回 1=需要擦除, 0=已擦除
int Flash_NeedsErase(uint32_t addr, uint16_t len);
```

实现：遍历目标地址逐字节读，发现非 0xFF 即返回 1。参照 Project02 的 `Init_flash_erase()` 逻辑。

## flash_download 模块

### 上下文结构体

```c
typedef struct {
    uint32_t write_addr;      // 当前写入地址（A 区内递增）
    uint32_t total_written;   // 已写入总字节
    uint8_t  last_byte_flag;  // 跨帧奇数字节标记
    uint8_t  last_byte;       // 缓存的奇数字节
} FlashDownload_t;
```

### API

```c
void FlashDownload_Init(FlashDownload_t *ctx);
int  FlashDownload_WriteFrame(FlashDownload_t *ctx, uint8_t *data, uint16_t len);
uint32_t FlashDownload_GetTotal(FlashDownload_t *ctx);
```

### 每帧处理流程（FlashDownload_WriteFrame）

参照 Project02 的 `HAL_UARTEx_RxEventCallback` + `Init_flash_write_halfworf` 逻辑：

```
1. Flash_Unlock()
2. 智能擦除: Flash_NeedsErase(write_addr, len) → 需要则 Flash_ErasePage()
3. 半字写入（含跨帧奇数字节拼接）:
   - 无 last_byte 且 len 为偶数 → 直接写
   - 无 last_byte 且 len 为奇数 → 写前 len-1 字节，缓存末字节
   - 有 last_byte 且拼接后为偶数 → last_byte 拼首字节 + 写剩余
   - 有 last_byte 且拼接后为奇数 → last_byte 拼首字节 + 写前 len-1 + 缓存末字节
4. Flash_Lock()
5. printf 进度
```

### 跨帧奇数字节处理

与 Project02 一致：
- 每帧数据量不确定（串口助手分包大小不一），长度可能是奇数
- STM32 Flash 最小编程单位是半字（2 字节），无法写单字节
- 用 `last_byte` 缓存奇数帧的末字节，下次写入时拼接为半字

## main.c 精简

```
初始化: HAL → GPIO → DMA → UART → uart_buf → FlashDownload_Init()
主循环: 轮询 uart_rx_queue → 有帧则调 FlashDownload_WriteFrame()
```

约 30 行用户代码。

## 依赖

- `uart_buf.c/h` — DMA + IDLE 帧队列接收（已有）
- `Driver/MCU/flash.c/h` — 内部 Flash 擦写（已有，扩展一个函数）
- `APP/bootloader_conf.h` — A_REGION_ADDR, A_PAGE_NUM, FLASH__PAGE_SIZE（已有）
