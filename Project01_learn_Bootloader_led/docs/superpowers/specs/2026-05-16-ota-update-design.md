# OTA 固件更新设计文档

## 概述

实现 Bootloader 检测到 OTA 事件后，从外部 W25Q16 Flash 读取固件写入 A区 内部 Flash 的完整流程。

## 内存布局

```
内部 Flash:
0x08000000 +------------------+
           |  B区 Bootloader  |  20KB (0x5000)
0x08005000 +------------------+
           |  A区 Application |  44KB (0xB000)
0x08010000 +------------------+

外部 W25Q16:
0x00000    +------------------+
           |  OTA 固件区域    |  从地址 0 开始
           |  (最多 44KB)     |
           +------------------+
           |  预留            |
           |  (后续多程序扩展) |
0x1FFFFF   +------------------+  (W25Q16 共 2MB)

AT24C02 EEPROM (256B, 页大小 8B):
0x00       +------------------+
           |  OTA_InfoCB      |  48B = 6 页
           |  (OTA_flag +     |
           |   Firelen[11])   |
0x2F       +------------------+
           |  预留            |
0xFF       +------------------+
```

## OTA 整体流程

### A区 App 侧（下载阶段）

```
1. 收到服务器升级通知，获知固件大小 fw_size
2. 将 fw_size 写入 24C02 的 Firelen[0]
3. 分片下载固件到 W25Q16（地址 0，256B/片）
4. 下载完成 → 写 OTA_flag = OTA_SET_FLAG 到 24C02
5. 软复位
```

### B区 Bootloader 侧（搬运阶段）

```
1. 读 24C02 → OTA_InfoCB
2. 校验 OTA_flag == OTA_SET_FLAG
3. fw_size = Firelen[0]
4. 计算 A区 擦除页数 = (fw_size + 1024 - 1) / 1024
5. Flash_Unlock()
6. 逐页擦除 A区 Flash
7. 循环搬运：W25Q16(0x00000) 读 256B → Flash 写 256B，直到写完 fw_size
8. Flash_Lock()
9. 清除 OTA_flag
10. NVIC_SystemReset() → 重启后检测到有效 App → 跳转
```

## 数据结构

### OTA_InfoCB（存储在 AT24C02 地址 0x00）

```c
#define OTA_SET_FLAG     0xA5A5A5A5
#define MAX_FW_SLOTS     11

typedef struct __attribute__((packed)) {
    uint32_t OTA_flag;                   // OTA 标志，== OTA_SET_FLAG 触发升级
    uint32_t Firelen[MAX_FW_SLOTS];      // Firelen[0]=OTA固件大小
                                          // Firelen[1-10]=预留，其他程序大小
} OTA_InfoCB;

#define OTA_INFOCB_SIZE  sizeof(OTA_InfoCB)   // 48B = 6 页 AT24C02
```

- `OTA_flag`：32 位魔数值，A区下载完成后写入
- `Firelen[0]`：OTA 固件的字节数
- `Firelen[1-10]`：预留给多程序场景，记录其他程序大小

## 分层架构

```
APP/           → Bootloader 主流程（决策：跳转 or 升级）
Service/       → OTA 搬运服务（执行：W25Q16 → Flash）
Driver/MCU/    → 内部 Flash 抽象（工具：解锁/擦/写/锁）
Driver/Storage → W25Q16、AT24C02（已有）
Protocol/      → SoftI2C、SoftSPI、UART（已有）
BSP/           → 引脚配置（已有）
```

## 模块接口

### Driver/MCU/flash.h — 内部 Flash 抽象

```c
int   Flash_Unlock(void);
int   Flash_Lock(void);
int   Flash_ErasePage(uint32_t addr);
int   Flash_Write(uint32_t addr, uint8_t *data, uint16_t len);
```

换芯片只改实现文件，接口不变。

### Service/ota_update.h — OTA 搬运入口

```c
int  OTA_Update(void);    // 返回 0=成功, 负数=失败
```

### APP/bootloader.h — 修改

- `OTA_Info_t` → `OTA_InfoCB`
- `OTA_FLAG_VALUE` → `OTA_SET_FLAG` (0xA5A5A5A5)
- 读写函数签名适配新结构体名

## 文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Service/ota_update.h` | 新增 | OTA_Update 接口 |
| `Service/ota_update.c` | 新增 | 搬运逻辑实现 |
| `Driver/MCU/flash.h` | 新增 | 内部 Flash 抽象接口 |
| `Driver/MCU/flash.c` | 新增 | STM32F103 Flash 操作实现 |
| `APP/bootloader.h` | 修改 | 结构体替换为 OTA_InfoCB |
| `APP/bootloader.c` | 修改 | 适配新结构体名 |
| `Core/Src/main.c` | 修改 | OTA 分支调用 OTA_Update() |

## 调用关系

```
main.c
  └→ Bootloader_ReadOTAInfo(&info)
     ├→ OTA_flag == OTA_SET_FLAG ?
     │   └→ OTA_Update()
     │       ├→ W25Q16_Read()       // 从外部 Flash 读
     │       ├→ Flash_ErasePage()   // 擦除 A区
     │       ├→ Flash_Write()       // 写入 A区
     │       └→ Bootloader_ClearOTAFlag()
     └→ App valid ?
         └→ Bootloader_JumpToApp()
```

## 搬运策略

- 分段搬运，每次 256 字节（与 W25Q16 页大小对齐）
- 内部 Flash 擦除以 1KB 页为单位
- 最后一次搬运可能不足 256B，按实际剩余字节数写入
- 全程只使用一个 256B 栈上缓冲区，不占用额外 RAM

## 后续扩展

- CRC 校验：在 OTA_Update 搬运完成后增加校验步骤
- 多程序管理：利用 Firelen[1-10] + W25Q16 地址偏移
- 通信协议：A区 App 的固件下载协议（待定）
