# OTA 状态机设计文档

## 概述

将 OTA_Update 从阻塞函数改造为主循环驱动的状态机，每次主循环执行一步操作，便于串口输出进度和错误处理。

## 状态定义

```c
typedef enum {
    OTA_STATE_IDLE,       // 空闲
    OTA_STATE_READ_INFO,  // 读 EEPROM 校验
    OTA_STATE_ERASE,      // 逐页擦除 A区 Flash
    OTA_STATE_TRANSFER,   // 逐段搬运 W25Q16 → Flash
    OTA_STATE_FINISH,     // 清除 OTA_flag + 软复位
    OTA_STATE_ERROR       // 出错停留
} OTA_State_t;
```

## 上下文结构体

```c
typedef struct {
    OTA_State_t state;
    uint32_t    fw_size;       // 固件大小
    uint32_t    page_count;    // 需擦除页数
    uint32_t    erase_index;   // 当前擦除到第几页
    uint32_t    offset;        // 当前搬运偏移量
    uint8_t     buf[256];      // 搬运缓冲区
} OTA_Context_t;
```

所有运行时状态保存在 `OTA_Context_t` 中，状态机无全局依赖。

## 状态转换

```
IDLE ──(OTA_flag检测)──→ READ_INFO
READ_INFO ──(校验通过)──→ ERASE
READ_INFO ──(校验失败)──→ ERROR
ERASE ──(每循环擦1页)──→ ERASE (未完)
ERASE ──(擦完)──→ TRANSFER
TRANSFER ──(每循环搬256B)──→ TRANSFER (未完)
TRANSFER ──(搬完)──→ FINISH
FINISH ──(清flag+复位)──→ 系统重启
ERROR ──(停留)──→ IDLE
```

## 调用方式

main.c 中：

```c
OTA_Context_t ota_ctx;

Bootloader_ReadOTAInfo(&ota_info);
if (ota_info.OTA_flag == OTA_SET_FLAG)
    ota_ctx.state = OTA_STATE_READ_INFO;
else if (Bootloader_IsAppValid())
    Bootloader_JumpToApp();

while (1)
{
    OTA_Process(&ota_ctx);
}
```

## 各状态行为

### OTA_STATE_IDLE
- 什么都不做，等待外部设置状态

### OTA_STATE_READ_INFO
- 读 EEPROM，校验 OTA_flag 和 fw_size
- 通过 → 解锁 Flash，切换 ERASE
- 失败 → 切换 ERROR

### OTA_STATE_ERASE
- 每次循环擦除 1 页
- 打印进度 `[OTA] Erasing page X/N`
- 擦完 → 切换 TRANSFER
- 失败 → 锁定 Flash，切换 ERROR

### OTA_STATE_TRANSFER
- 每次循环从 W25Q16 读 256B，写入 A区 Flash
- 最后一次可能不足 256B
- 打印进度 `[OTA] Transfer X/N bytes`
- 搬完 → 锁定 Flash，切换 FINISH
- 失败 → 锁定 Flash，切换 ERROR

### OTA_STATE_FINISH
- 清除 OTA_flag
- 打印 `[OTA] Update done, resetting...`
- 延时 100ms
- NVIC_SystemReset()

### OTA_STATE_ERROR
- 打印错误信息
- 切换 IDLE，停留在 Bootloader

## 涉及文件

| 文件 | 操作 |
|------|------|
| `Service/ota_update.h` | 修改：添加状态枚举、上下文结构体、OTA_Process 声明 |
| `Service/ota_update.c` | 修改：替换阻塞函数为状态机实现 |
| `Core/Src/main.c` | 修改：OTA 分支改为状态机驱动 |
