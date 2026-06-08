# STM32 Bootloader 学习工程 — 开发笔记

> 项目：STM32F103C8 Bootloader 学习
> 芯片：STM32F103C8（72MHz, 64KB Flash, 20KB RAM）
> 开发环境：Keil MDK-ARM + STM32CubeMX
> 记录周期：2026-05-07 ~ 2026-05-11

---

## 目录

- [第一章 串口 DMA + 缓冲区](#第一章-串口-dma--缓冲区)
  - [1.1 为什么需要 DMA + 缓冲区](#11-为什么需要-dma--缓冲区)
  - [1.2 DMA 基础](#12-dma-基础)
  - [1.3 UART 空闲中断（IDLE）](#13-uart-空闲中断idle)
  - [1.4 方案 A：DMA Normal + IDLE + 描述符队列](#14-方案-adma-normal--idle--描述符队列)
  - [1.5 方案 B：DMA Circular + IDLE + 环形缓冲区](#15-方案-bdma-circular--idle--环形缓冲区)
  - [1.6 两种方案对比](#16-两种方案对比)
  - [1.7 CubeMX 配置要点](#17-cubemx-配置要点)
  - [1.8 调试问题与解决](#18-调试问题与解决)
- [第二章 软件 I2C 协议](#第二章-软件-i2c-协议)
  - [2.1 为什么用软件 I2C](#21-为什么用软件-i2c)
  - [2.2 I2C 协议基础](#22-i2c-协议基础)
  - [2.3 分层设计与总线句柄](#23-分层设计与总线句柄)
  - [2.4 多设备共享总线](#24-多设备共享总线)
  - [2.5 总线恢复机制](#25-总线恢复机制)
  - [2.6 调试问题与解决](#26-调试问题与解决)
- [第三章 软件 SPI 协议](#第三章-软件-spi-协议)
  - [3.1 SPI 与 I2C 的区别](#31-spi-与-i2c-的区别)
  - [3.2 SPI Mode 0 时序](#32-spi-mode-0-时序)
  - [3.3 分层设计与 CS 引脚管理](#33-分层设计与-cs-引脚管理)
  - [3.4 全双工与半双工](#34-全双工与半双工)
- [第四章 Flash 存储器驱动](#第四章-flash-存储器驱动)
  - [4.1 EEPROM（AT24C02）](#41-eepromat24c02)
  - [4.2 SPI Flash（W25Q16）](#42-spi-flashw25q16)
  - [4.3 EEPROM vs SPI Flash](#43-eeprom-vs-spi-flash)
- [第五章 分层架构总结](#第五章-分层架构总结)
  - [5.1 目录结构](#51-目录结构)
  - [5.2 各层职责与依赖关系](#52-各层职责与依赖关系)
  - [5.3 HAL 库在项目中的角色](#53-hal-库在项目中的角色)
  - [5.4 CubeMX 的使用边界](#54-cubemx-的使用边界)
- [第六章 Debug 测试框架](#第六章-debug-测试框架)
- [附录 常用参考资料](#附录-常用参考资料)

---

# 第一章 串口 DMA + 缓冲区

## 1.1 为什么需要 DMA + 缓冲区

Bootloader 通过串口接收固件数据写入 Flash，面临三个核心问题：

| 问题 | 说明 |
|------|------|
| CPU 被占用 | 阻塞式接收时 CPU 无法同时擦 Flash / 喂狗 |
| 数据不定长 | 不知上位机何时发完，固定长度接收会死锁 |
| 不能丢字节 | 固件任何一个字节丢失都会导致损坏 |

DMA + IDLE 中断 + 缓冲区组合解决：

| 问题 | 解决方式 |
|------|----------|
| CPU 被占用 | DMA 后台搬运，CPU 同时做 Flash 操作 |
| 数据不定长 | IDLE 中断检测一帧结束 |
| 不能丢字节 | DMA 传输 + 缓冲区弹性调节 |

## 1.2 DMA 基础

DMA（Direct Memory Access）是不占用 CPU 的数据搬运硬件：

```
没有 DMA：UART 收 1 字节 → 中断 → CPU 从 DR 读出 → 存到数组（每字节都需 CPU）
有 DMA：  配置 DMA → UART 收到数据 → DMA 自动搬到内存（CPU 不介入）
```

STM32F103 DMA1 通道分配（串口相关）：

| DMA 通道 | 外设 | 方向 |
|----------|------|------|
| DMA1_Channel4 | USART1_TX | 内存→外设 |
| DMA1_Channel5 | USART1_RX | 外设→内存 |

**关键寄存器 CNDTR**：记录 DMA 剩余待传输数量。通过它可计算已接收字节数：
```
已接收 = 总配置长度 - CNDTR
```

## 1.3 UART 空闲中断（IDLE）

当串口总线上一个字节时间没有新数据时触发 IDLE 中断，用于检测"一帧结束"：

```
上位机发送：0xAA 0x55 0x01 0x02 ... [停顿]
                                      ↑ IDLE 中断触发
```

**IDLE 是 UART 外设的功能，不是 DMA 的。** 两者配合使用。

开启方式：
```c
__HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
```

清除方式（读 SR 再读 DR）：
```c
__HAL_UART_CLEAR_IDLEFLAG(&huart1);
```

## 1.4 方案 A：DMA Normal + IDLE + 描述符队列

### 设计思路

DMA 配置为 Normal 模式（单次传输），每帧结束后重启 DMA。用描述符队列记录每帧的起始/结束位置，避免数据拷贝。

### 数据结构

```c
#define UART_READ_BUF_SIZE   1024   /* 底层 DMA 缓冲区大小 */
#define UART_READ_MAX_SIZE   256    /* 每帧最大长度 */
#define UART_BUF_QUEUE_SIZE  8      /* 描述符队列深度 */

typedef struct {
    uint8_t *start;   /* 帧起始地址 */
    uint8_t *end;     /* 帧结束地址 */
} UART_Buffptr;

typedef struct {
    uint16_t      URxCounter;                    /* 缓冲区累计写入偏移 */
    UART_Buffptr  URxDataPtr[UART_BUF_QUEUE_SIZE]; /* 描述符数组 */
    UART_Buffptr *URxDataIN;     /* 写入指针（中断填充） */
    UART_Buffptr *URxDataOUT;    /* 读取指针（主循环消费） */
    UART_Buffptr *URxDataEND;    /* 末尾指针（回绕判断） */
} UART_BufQueue_t;
```

### 工作流程

```
IDLE 中断触发：
  1. 清除 IDLE 标志
  2. CNDTR 未变 → 伪中断，直接返回
  3. 计算本帧长度 = (MAX_SIZE+1) - CNDTR
  4. 记录当前描述符的 end 指针
  5. IN 指针前移，超过 END 则回绕
  6. 判断剩余空间是否够放下一帧
     够 → 新 start = 上一帧 end + 1
     不够 → 回绕到缓冲区起始，计数器归零
  7. 停止 DMA → 从新 start 位置重启 DMA

主循环消费：
  比较 IN 和 OUT 指针判断是否有新帧
  读取帧数据 → 处理 → OUT 指针前移
```

### 内存布局示例

```
DMA 缓冲区（1024 字节）：
┌─────────────────────────────────────────────────────┐
│  帧A (50B)  │  帧B (30B)  │      剩余空间 (~944B)    │
└─────────────────────────────────────────────────────┘
  ^startA  ^endA ^startB ^endB  ^新start

描述符队列：
  [0] {startA, endA}    ← OUT 正在读取
  [1] {startB, endB}    ← IN 等待消费
  [2] {新start, ?}      ← IN 正在填充
  ...
  [7] 空
```

### 关键代码（IDLE 中断处理）

```c
void UART_DMA_RxIdleHandler(void)
{
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    /* 防止上电后首次伪 IDLE */
    if (__HAL_DMA_GET_COUNTER(&hdma_usart1_rx) == (UART_READ_MAX_SIZE + 1))
        return;

    /* 累加写入位置 */
    uart_rx_queue.URxCounter +=
        (UART_READ_MAX_SIZE + 1) - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);

    /* 记录本帧结束位置 */
    uart_rx_queue.URxDataIN->end =
        &uart_dma_rx_buf[uart_rx_queue.URxCounter - 1];

    /* IN 指针前移 */
    uart_rx_queue.URxDataIN++;
    if (uart_rx_queue.URxDataIN > uart_rx_queue.URxDataEND)
        uart_rx_queue.URxDataIN = &queue->URxDataPtr[0];

    /* 剩余空间判断 */
    if (UART_READ_BUF_SIZE - uart_rx_queue.URxCounter > UART_READ_MAX_SIZE)
    {
        uart_rx_queue.URxDataIN->start =
            &uart_dma_rx_buf[uart_rx_queue.URxCounter];
    }
    else
    {
        uart_rx_queue.URxDataIN->start = uart_dma_rx_buf;
        uart_rx_queue.URxCounter = 0;
    }

    /* 重启 DMA */
    HAL_UART_DMAStop(&huart1);
    HAL_UART_Receive_DMA(&huart1,
        uart_rx_queue.URxDataIN->start, UART_READ_MAX_SIZE + 1);
}
```

## 1.5 方案 B：DMA Circular + IDLE + 环形缓冲区

### 设计思路

DMA 配置为 Circular 模式，永不停歇地循环写入缓冲区。IDLE 中断只需更新 head 指针，不需要停止或重启 DMA。

### 数据结构

```c
#define UART_RINGBUF_SIZE  512

typedef struct {
    uint8_t           buffer[UART_RINGBUF_SIZE];
    volatile uint16_t head;    /* 写入位置，IDLE 中断更新 */
    volatile uint16_t tail;    /* 读取位置，主循环更新 */
} UART_RingBuf_t;
```

### 工作流程

```
DMA Circular 模式持续运行：
  收到字节 → DMA 自动写入 buffer[当前写位置]
  CNDTR 持续递减，到 0 后自动重载为 RINGBUF_SIZE

IDLE 中断触发：
  1. 清除 IDLE 标志
  2. head = RINGBUF_SIZE - CNDTR
  3. 不需要停止或重启 DMA

主循环消费：
  available = (RINGBUF_SIZE + head - tail) % RINGBUF_SIZE
  while (available > 0) { 读取数据; tail 前移; }
```

### 关键代码

```c
void UART_RingBuf_DMA_IdleHandler(void)
{
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    uart_rx_ringbuf.head =
        UART_RINGBUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
}

uint16_t UART_RingBuf_Read(UART_RingBuf_t *rb, uint8_t *buf, uint16_t len)
{
    uint16_t read_count = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        if (rb->head == rb->tail) break;
        buf[i] = rb->buffer[rb->tail];
        rb->tail = (rb->tail + 1) % UART_RINGBUF_SIZE;
        read_count++;
    }
    return read_count;
}
```

## 1.6 两种方案对比

| 特性 | 方案 A Normal+队列 | 方案 B Circular+环形 |
|------|-------------------|---------------------|
| DMA 是否需要重启 | 每帧重启 | 不需要 |
| 丢字节风险 | 重启间隙有风险 | 几乎无 |
| 数据零拷贝 | 是（描述符指向原位） | 否（直接在 buffer 上读） |
| 缓冲区利用率 | 高（紧凑排列） | 低（环形留空隙） |
| 实现复杂度 | 较高 | 较低 |
| 本项目采用 | **实际使用** | 备用方案 |

## 1.7 CubeMX 配置要点

```
USART1：
  Mode:        Asynchronous
  Baud Rate:   115200
  Word Length: 8 Bits
  Stop Bits:   1
  NVIC:        使能 USART1 全局中断

DMA：
  USART1_RX:
    Mode:       Normal（方案A）或 Circular（方案B）
    Data Width: Byte（两边都是）
    Priority:   High

注意：DMA 必须在 UART 之前初始化（CubeMX 中调整 Init Order）
      即 MX_DMA_Init() 必须在 MX_USART1_UART_Init() 之前调用
```

## 1.8 调试问题与解决

### 问题 1：队列回绕判断 `==` 漏掉最后一个槽位

```c
/* 错误：== 只在恰好等于时触发，跳过了 END 位置的回绕 */
if (URxDataIN == URxDataEND) { ... }

/* 正确：> 在超过 END 时触发回绕 */
if (URxDataIN > URxDataEND) { ... }
```

**根因**：队列数组最后一个元素是 `URxDataPtr[QUEUE_SIZE-1]`，即 `END` 指向的位置。当 IN 指针递增到 `END+1` 时才需要回绕，用 `==` 会导致最后一个槽位被跳过。

### 问题 2：DMA 缓冲区溢出 `>=` 多写一个字节

```c
/* 错误：剩余空间恰好等于 MAX_SIZE 时也会写入，导致越界 1 字节 */
if (UART_READ_BUF_SIZE - counter >= UART_READ_MAX_SIZE)

/* 正确：必须大于，留出余量 */
if (UART_READ_BUF_SIZE - counter > UART_READ_MAX_SIZE)
```

**根因**：DMA 配置接收 `MAX_SIZE+1` 字节，剩余空间恰好等于 `MAX_SIZE` 时不够放。

### 问题 3：上电后首次伪 IDLE 中断

**现象**：上电后立即触发一次 IDLE 中断，但此时没有收到任何数据。

**原因**：IDLE 标志在初始化时就是置位状态。

**解决**：在 IDLE 处理函数中检查 CNDTR，若等于初始值说明没收到数据，直接返回：
```c
if (__HAL_DMA_GET_COUNTER(&hdma_usart1_rx) == (UART_READ_MAX_SIZE + 1))
    return;
```

### 问题 4：`hdma_usart1_rx` 找不到

**现象**：编译报错 `hdma_usart1_rx` 未定义。

**原因**：CubeMX 将 DMA 句柄定义在 `dma.c` 中，但 `uart_buf.c` 没有包含对应头文件。

**解决**：在 `usart.h` 中添加声明：
```c
#include "dma.h"
extern DMA_HandleTypeDef hdma_usart1_rx;
```

### 问题 5：串口助手发送数据多出 2 字节

**现象**：发送 `hello`（5字节），实际收到 7 字节。

**原因**：串口助手设置了"发送新行"（Append \r\n），实际发出 `hello\r\n`。

**解决**：关闭串口助手的"发送新行"选项，或在协议层过滤 `\r\n`。

---

# 第二章 软件 I2C 协议

## 2.1 为什么用软件 I2C

STM32F103 的硬件 I2C 存在众所周知的总线死锁 bug（BB 标志位无法清除）。软件 I2C（GPIO 模拟）虽然速度慢，但稳定可靠，适合外设通信（传感器、EEPROM、OLED 等）。

## 2.2 I2C 协议基础

### 信号线

| 信号 | 方向 | 说明 |
|------|------|------|
| SCL | 主机驱动 | 时钟线 |
| SDA | 双向 | 数据线 |

两条线都必须通过上拉电阻接 VCC（开漏/开集输出）。

### 时序

```
起始信号（START）：SCL 高电平期间，SDA 从高拉低
  SCL: ‾‾‾\___
  SDA: ‾\____

停止信号（STOP）：SCL 高电平期间，SDA 从低拉高
  SCL: ‾‾‾\___
  SDA: __/‾‾

数据传输：SCL 低电平期间改变 SDA，SCL 上升沿采样
  每个 byte 后跟 1 bit ACK（SDA 低=应答，高=无应答）

地址字节：7位设备地址 + 1位 R/W（0=写，1=读）
  例如 OLED 地址 0x78 = 0b0111100_0（写）
  例如 EEPROM 地址 0xA0 = 0b1010000_0（写）
```

### GPIO 模式

软件 I2C 使用**开漏输出 + 上拉**模式：
```c
GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;  /* 开漏输出 */
GPIO_InitStruct.Pull = GPIO_PULLUP;          /* 上拉 */
```

开漏模式下，引脚只能输出低电平（拉低），高电平靠外部/内部上拉电阻。这与 I2C 规范一致——任何设备都可以拉低总线，实现线与逻辑。

## 2.3 分层设计与总线句柄

### 设计思路

将 I2C 协议逻辑与具体引脚解耦，通过总线句柄传递引脚信息：

```
Protocol 层（soft_i2c.h/c）
  → 不知道引脚在哪，只操作句柄里的 port/pin

BSP 层（bsp_soft_i2c.h/c）
  → 定义引脚（PB8=SCL, PB9=SDA）
  → 使能时钟 + 配置 GPIO
  → 调用 Protocol 层 SoftI2C_Init 填充句柄

Driver 层（ssd1306.c / at24c02.c）
  → 持有 SoftI2C_Bus_t* 指针 + 设备地址
  → 不知道引脚在哪
```

### 总线句柄

```c
typedef struct {
    GPIO_TypeDef *scl_port;  uint16_t scl_pin;
    GPIO_TypeDef *sda_port;  uint16_t sda_pin;
} SoftI2C_Bus_t;
```

协议层通过宏操作引脚，BSP 层填充具体值：
```c
#define SCL_HIGH(bus)  HAL_GPIO_WritePin((bus)->scl_port, (bus)->scl_pin, GPIO_PIN_SET)
#define SDA_LOW(bus)   HAL_GPIO_WritePin((bus)->sda_port, (bus)->sda_pin, GPIO_PIN_RESET)
#define SDA_READ(bus)  HAL_GPIO_ReadPin((bus)->sda_port, (bus)->sda_pin)
```

### 初始化流程

```c
/* BSP 层 */
void BSP_SoftI2C_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull  = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    GPIO_InitStruct.Pin = I2C1_SCL_PIN;
    HAL_GPIO_Init(I2C1_SCL_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = I2C1_SDA_PIN;
    HAL_GPIO_Init(I2C1_SDA_PORT, &GPIO_InitStruct);

    SoftI2C_Init(&i2c1_bus, I2C1_SCL_PORT, I2C1_SCL_PIN,
                           I2C1_SDA_PORT, I2C1_SDA_PIN);
    SoftI2C_BusRecovery(&i2c1_bus);
}
```

换引脚只改 BSP 头文件宏定义，不影响 Protocol 和 Driver 层。

## 2.4 多设备共享总线

I2C 总线支持多设备并联，通过设备地址区分。所有设备的 SDA 和 SCL 分别接到同一组引脚：

```
STM32 PB8 (SCL) ──┬── OLED SCL
                   └── EEPROM SCL

STM32 PB9 (SDA) ──┬── OLED SDA
                   └── EEPROM SDA
```

### 上拉电阻

I2C 总线需要上拉电阻（典型 4.7kΩ）：
- STM32 内部上拉约 30~50kΩ，**太弱**，不适合做 I2C 主上拉
- 大多数模块（OLED、EEPROM）板载 4.7k 上拉
- 多个模块并联后上拉并联，等效电阻更小，不影响工作

**本项目**：内部上拉 + 模块板载上拉，不需要额外接电阻。

### 设备地址

| 设备 | 7位地址 | 写地址（8位） | 读地址（8位） |
|------|---------|--------------|--------------|
| SSD1306 OLED | 0x3C | 0x78 | 0x79 |
| AT24C02 EEPROM | 0x50 | 0xA0 | 0xA1 |

R/W 位是地址最低位，代码中设备地址是**已左移的 8 位写地址**。读操作时用 `dev_addr | 1` 自动转为读地址。

### 寄存器读写时序

```
写寄存器：
  START → 设备地址(W) → 寄存器地址 → 数据[0..N] → STOP

读寄存器：
  START → 设备地址(W) → 寄存器地址
  → RESTART → 设备地址(R) → 读数据[0..N] → STOP
```

读操作需要先写寄存器地址再重新起始切换读方向，这在 `SoftI2C_ReadReg` 中实现。

## 2.5 总线恢复机制

当从机在通信中途被复位或断电，可能持续拉低 SDA 导致总线锁死。恢复方法：

```c
void SoftI2C_BusRecovery(SoftI2C_Bus_t *bus)
{
    SDA_HIGH(bus);
    for (uint8_t i = 0; i < 9; i++)
    {
        SCL_HIGH(bus);
        I2C_Delay();
        if (SDA_READ(bus)) break;   /* SDA 恢复高，总线释放 */
        SCL_LOW(bus);
        I2C_Delay();
    }
    SoftI2C_Stop(bus);
}
```

原理：发送 9 个 SCL 脉冲让从机完成未完成的传输，然后发 STOP 释放总线。在 BSP 初始化时调用一次，确保上电后总线干净。

## 2.6 调试问题与解决

### 问题 1：I2C 总线扫描所有地址都响应

**现象**：扫描发现 0x02~0xFC 所有地址都存在。

**诊断**：
```
SDA 被持续拉低 → 每次发送地址后第 9 个时钟读 SDA 都是 0（ACK）
→ 所有地址看起来都有设备
```

**根因**：SDA 被某个设备或短路持续拉低。

**排查步骤**：
1. 在主循环中周期性读取 SDA 引脚电平并打印
2. 逐一拔掉设备，看 SDA 何时恢复高电平
3. 确认模块的 SDA/SCL 引脚标注与实际一致
4. 确认 GND 共地

### 问题 2：两个设备同时接入时总线锁死

**现象**：OLED 单独接正常（扫描到 0x78），EEPROM 单独接也正常（扫描到 0xA0），但两个同时接入后 SDA 被拉死。

**排查过程**：
1. 添加 SDA/SCL 电平诊断代码，确认 SDA 被拉低
2. 拔掉 OLED → 仍然死 → 问题在 EEPROM 模块
3. 更换 EEPROM 模块后正常

**结论**：EEPROM 模块硬件故障导致 SDA 对地短路。软件代码没有问题，更换模块后即可正常工作。

### 问题 3：开机自检虚假报 OK

**现象**：OLED 未接入时 SelfCheck 打印 `[OLED] OK`。

**原因**：原始代码直接调用 OLED 函数后打印 OK，没有检测设备是否应答：

```c
/* 错误：不检测设备是否存在就直接报 OK */
SSD1306_Clear(&oled);
SSD1306_ShowString(&oled, 1, 1, "OLED    OK");
printf("[OLED] OK\r\n");
```

**解决**：先发地址探测 ACK，有应答才报 OK：

```c
SoftI2C_Start(&i2c1_bus);
uint8_t oled_ack = SoftI2C_SendByte(&i2c1_bus, SSD1306_ADDR);
SoftI2C_Stop(&i2c1_bus);

if (oled_ack == 0) {
    /* 设备在线，显示 OK */
} else {
    printf("[OLED] FAIL (no ACK at 0x%02X)\r\n", SSD1306_ADDR);
}
```

---

# 第三章 软件 SPI 协议

## 3.1 SPI 与 I2C 的区别

| 特性 | I2C | SPI |
|------|-----|-----|
| 信号线 | 2（SCL + SDA） | 4（SCK + MOSI + MISO + CS） |
| 设备寻址 | 地址（7/10位） | CS 片选（每个设备独占一根 CS） |
| 通信方式 | 半双工 | 全双工 |
| 速度 | 通常 ≤400kHz | 可达几十 MHz |
| 拓扑 | 多主多从 | 一主多从 |

SPI 没有地址机制，靠 CS（片选）引脚选择设备。CS 低电平有效，同一时间只能选中一个设备。

## 3.2 SPI Mode 0 时序

SPI 有 4 种模式（CPOL + CPHA 组合），本项目使用 Mode 0（CPOL=0, CPHA=0）：

```
CPOL=0：空闲时 SCK 为低电平
CPHA=0：上升沿采样数据

时序（发送 0xA5 = 10100101）：
  MOSI: ‾\_/‾‾\_\_/‾\_/‾‾
  SCK:  _/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_
        ↑   ↑   ↑   ↑   ↑   ↑   ↑   ↑
      采样点（上升沿）
```

SSD1306 OLED 和 W25Q16 Flash 都兼容 Mode 0。

## 3.3 分层设计与 CS 引脚管理

### 架构（与 I2C 完全对称）

```
Protocol 层（soft_spi.h/c）
  SoftSPI_Bus_t 句柄：SCK + MOSI + MISO（不含 CS）

BSP 层（bsp_soft_spi.h/c）
  定义引脚（PA5=SCK, PA6=MISO, PA7=MOSI）
  GPIO 推挽输出（SCK/MOSI）+ 上拉输入（MISO）

Driver 层（w25q16.h/c）
  持有 SoftSPI_Bus_t* + CS 引脚（port + pin）
  每次操作前拉低 CS，操作后拉高 CS
```

### 为什么 CS 不在协议层

I2C 靠设备地址区分从机，所以地址在 Driver 层传入（如 `SoftI2C_WriteReg(bus, addr, ...)`）。

SPI 靠 CS 引脚区分从机，CS 操作在 Driver 层完成。协议层只管时序，不管选谁：

```c
/* W25Q16 驱动中的 CS 操作 */
#define CS_LOW(dev)   HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_RESET)
#define CS_HIGH(dev)  HAL_GPIO_WritePin((dev)->cs_port, (dev)->cs_pin, GPIO_PIN_SET)

uint32_t W25Q16_ReadJEDECID(W25Q16_t *dev)
{
    CS_LOW(dev);                               /* 选中 */
    SoftSPI_WriteByte(dev->bus, 0x9F);         /* 协议层只管时序 */
    id = SoftSPI_TransferByte(dev->bus, 0xFF);
    CS_HIGH(dev);                              /* 释放 */
    return id;
}
```

### GPIO 模式

```c
/* SCK / MOSI：推挽输出（SPI 是点对点，不需要开漏） */
GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;

/* MISO：上拉输入 */
GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
GPIO_InitStruct.Pull = GPIO_PULLUP;
```

与 I2C 不同，SPI 使用推挽输出而非开漏。I2C 需要开漏是因为多设备共享总线需要线与逻辑；SPI 有独立的 CS 选通，同一时间只有一个从机驱动 MISO。

## 3.4 全双工与半双工

SPI 支持同时收发（全双工），本项目提供两种接口：

```c
/* 只发不收：用于 OLED 等只需要写入的设备 */
void SoftSPI_WriteByte(SoftSPI_Bus_t *bus, uint8_t byte);

/* 全双工收发：用于 W25Q64 等需要读取数据的设备 */
uint8_t SoftSPI_TransferByte(SoftSPI_Bus_t *bus, uint8_t byte);
```

读取 W25Q16 时，主机发送虚拟字节（0xFF）产生时钟，同时读取从机返回的数据：

```c
/* 读 JEDEC ID：发送命令后，发 3 个 0xFF 产生时钟，同时接收 3 字节 ID */
CS_LOW(dev);
SoftSPI_WriteByte(dev->bus, 0x9F);                    /* 发命令 */
id  = (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF) << 16;  /* 读 byte 0 */
id |= (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF) << 8;   /* 读 byte 1 */
id |= (uint32_t)SoftSPI_TransferByte(dev->bus, 0xFF);         /* 读 byte 2 */
CS_HIGH(dev);
```

---

# 第四章 Flash 存储器驱动

## 4.1 EEPROM（AT24C02）

### 芯片特性

| 参数 | 值 |
|------|-----|
| 容量 | 256 字节（2Kbit） |
| 接口 | I2C |
| 页大小 | 8 字节 |
| 写入寿命 | 100 万次 |
| 写入时间 | ~5ms |

### 页写边界问题

AT24C02 页大小 8 字节，写入跨页时地址在页内回绕覆盖。例如从地址 5 写入 10 字节：

```
不拆分写入（错误）：
  地址 5~7 写入 A B C
  地址 8~12 应该写 D E F G H J
  但实际地址 8 会回绕到地址 0，覆盖之前的旧数据

自动拆分写入（正确）：
  第 1 段：地址 5~7（3 字节，到页边界）→ 等 5ms
  第 2 段：地址 8~12（7 字节）→ 等 5ms
```

```c
int AT24C02_Write(AT24C02_t *dev, uint8_t addr, uint8_t *data, uint16_t len)
{
    uint16_t offset = 0;
    while (offset < len)
    {
        uint8_t page_remain = AT24C02_PAGE_SIZE - (addr % AT24C02_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        SoftI2C_WriteReg(dev->bus, dev->addr, addr + offset,
                         data + offset, write_len);
        HAL_Delay(5);   /* 等待内部写入周期 */
        offset += write_len;
    }
    return 0;
}
```

### 读取无边界限制

EEPROM 读取支持顺序读，不会回绕，可一次性连续读取。

## 4.2 SPI Flash（W25Q16）

### 芯片特性

| 参数 | 值 |
|------|-----|
| 容量 | 16Mbit / 2MB |
| 接口 | SPI（Mode 0 / Mode 3） |
| 页大小 | 256 字节 |
| 扇区大小 | 4KB |
| 块大小 | 64KB |
| JEDEC ID | 0xEF4015 |
| 擦除寿命 | 10 万次 |

### Flash 写入特性（与 EEPROM 根本不同）

**EEPROM**：可以直接覆盖写（先擦除后写入由芯片内部自动完成）

**Flash**：只能将 1 写为 0，不能将 0 写为 1。写入前必须先擦除（擦除将所有位置 1）。

```
Flash 写入规则：
  擦除 → 所有位变为 1（0xFF）
  写入 → 将需要为 0 的位拉低
  已写入 0 的位，不擦除就不能再写为 1

因此：写入前必须先擦除对应扇区
```

### 常用命令

| 命令 | 代码 | 功能 |
|------|------|------|
| Write Enable | 0x06 | 写入/擦除前必须先发此命令 |
| Read Data | 0x03 | 从指定地址读取数据 |
| Page Program | 0x02 | 写入一页（最多 256 字节） |
| Sector Erase | 0x20 | 擦除 4KB 扇区 |
| Chip Erase | 0xC7 | 整片擦除 |
| Read Status | 0x05 | 读取状态寄存器（检查 BUSY 位） |
| JEDEC ID | 0x9F | 读取厂商标识 + 容量信息 |

### 写入流程

```
1. Write Enable（0x06）        ← 每次写入前必须发
2. Page Program（0x02 + 24位地址 + 数据）
3. 等待 BUSY 位清零            ← 轮询状态寄存器 bit0
```

### 页写边界（与 EEPROM 同理）

W25Q16 页大小 256 字节，Page Program 不能跨页。多字节写入需要自动拆分：

```c
int W25Q16_Write(W25Q16_t *dev, uint32_t addr, uint8_t *data, uint16_t len)
{
    uint16_t offset = 0;
    while (offset < len)
    {
        uint16_t page_remain = W25Q16_PAGE_SIZE - (addr % W25Q16_PAGE_SIZE);
        uint16_t write_len = len - offset;
        if (write_len > page_remain)
            write_len = page_remain;

        W25Q_WriteEnable(dev);
        /* ... Page Program ... */
        W25Q_WaitBusy(dev);
        offset += write_len;
    }
    return 0;
}
```

### 典型使用流程

```c
/* 1. 初始化 */
BSP_SoftSPI_Init();
W25Q16_Init(&flash, &spi1_bus, GPIOA, GPIO_PIN_4);

/* 2. 读 ID 验证通信 */
uint32_t id = W25Q16_ReadJEDECID(&flash);
// id 应为 0xEF4015

/* 3. 擦除目标扇区 */
W25Q16_EraseSector(&flash, 0x0000);

/* 4. 写入数据 */
uint8_t data[512] = {...};
W25Q16_Write(&flash, 0x0000, data, 512);

/* 5. 读回验证 */
uint8_t buf[512];
W25Q16_Read(&flash, 0x0000, buf, 512);
```

## 4.3 EEPROM vs SPI Flash

| 特性 | AT24C02 | W25Q16 |
|------|---------|--------|
| 接口 | I2C | SPI |
| 容量 | 256B | 2MB |
| 页大小 | 8B | 256B |
| 擦除粒度 | 不需要（字节级覆盖） | 4KB 扇区 |
| 写入寿命 | 100万次 | 10万次 |
| 适用场景 | 存配置参数/校准数据 | 存固件/大量数据/OTA |

---

# 第五章 分层架构总结

## 5.1 目录结构

```
Project01_learn_Bootloader_led/
│
├── Core/                ← CubeMX 生成：时钟/GPIO/UART/DMA 初始化
│   ├── Inc/             (main.h, usart.h, dma.h, gpio.h)
│   └── Src/             (main.c, usart.c, dma.c, gpio.c, stm32f1xx_it.c)
│
├── BSP/                 ← 板级支持包：引脚映射 + GPIO 初始化
│   ├── bsp_soft_i2c.*   PB8=SCL, PB9=SDA，开漏+上拉
│   └── bsp_soft_spi.*   PA5=SCK, PA6=MISO, PA7=MOSI，推挽+输入
│
├── Protocol/            ← 通信协议层：与硬件无关的通用时序
│   ├── I2C/
│   │   └── soft_i2c.*   Start/Stop/字节收发/寄存器读写/总线恢复
│   ├── SPI/
│   │   └── soft_spi.*   WriteByte/TransferByte/多字节传输
│   └── UART/
│       ├── uart_buf.*       方案A：Normal + IDLE + 描述符队列
│       └── uart_ringbuf.*   方案B：Circular + IDLE + 环形缓冲区
│
├── Driver/              ← 设备驱动层：具体芯片的操作封装
│   ├── OLED/
│   │   ├── ssd1306.*        SSD1306 OLED 驱动（I2C 接口）
│   │   └── ssd1306_font.h   8x16 字库
│   └── Storage/
│       ├── at24c02.*        AT24C02 EEPROM 驱动（页写边界处理）
│       └── w25q16.*         W25Q16 SPI Flash 驱动（扇区擦除+页编程）
│
├── Debug/               ← 调试测试层
│   └── module_test.*    串口命令分发 + 开机自检 + I2C 总线扫描
│
├── APP/                 ← (待开发) 应用层
├── Service/             ← (待开发) 数据处理、日志服务
├── Middleware/OTA/       ← (待开发) OTA 升级中间件
├── doc/                 ← 学习文档
└── Drivers/             ← STM32 HAL 库 + CMSIS（不修改）
```

## 5.2 各层职责与依赖关系

```
┌──────────────────────────────────────────────┐
│  APP 层          应用业务逻辑                  │
├──────────────────────────────────────────────┤
│  Debug 层        串口命令测试 + 开机自检        │
├──────────────────────────────────────────────┤
│  Driver 层       SSD1306 / AT24C02 / W25Q16   │
│                  持有 Protocol 层句柄           │
│                  持有设备私有信息（地址/CS引脚）  │
├──────────────────────────────────────────────┤
│  Protocol 层     SoftI2C / SoftSPI / UART_DMA │
│                  纯协议时序，不知道引脚在哪       │
│                  不知道驱动的是哪个具体设备       │
├──────────────────────────────────────────────┤
│  BSP 层          引脚定义 + GPIO 初始化         │
│                  填充 Protocol 层句柄           │
├──────────────────────────────────────────────┤
│  HAL 层          HAL_GPIO_WritePin / HAL_Delay │
│                  HAL_UART_Receive_DMA 等        │
│                  CubeMX 生成，不修改             │
└──────────────────────────────────────────────┘

依赖方向：上层依赖下层，下层不知道上层存在
换引脚：改 BSP 头文件宏定义
换芯片：改 BSP 的 GPIO 初始化代码
换设备：写新的 Driver，Protocol 层不动
```

### I2C 设备的依赖链示例

```c
/* main.c（APP 层）*/
SSD1306_t oled;                          /* 设备实例 */
BSP_SoftI2C_Init();                      /* BSP：配 GPIO + 填句柄 */
SSD1306_Init(&oled, &i2c1_bus, 0x78);   /* Driver：绑定总线+地址 */

/* ssd1306.c（Driver 层）调用 Protocol 层 */
SoftI2C_Start(dev->bus);
SoftI2C_SendByte(dev->bus, dev->addr);
SoftI2C_SendByte(dev->bus, 0x00);       /* Control byte */
SoftI2C_SendByte(dev->bus, cmd);
SoftI2C_Stop(dev->bus);

/* soft_i2c.c（Protocol 层）调用 HAL */
HAL_GPIO_WritePin(bus->scl_port, bus->scl_pin, GPIO_PIN_SET);
```

### SPI 设备的依赖链示例

```c
/* main.c（APP 层）*/
W25Q16_t flash;
BSP_SoftSPI_Init();                                  /* BSP */
W25Q16_Init(&flash, &spi1_bus, GPIOA, GPIO_PIN_4);  /* Driver */

/* w25q16.c（Driver 层）管理 CS + 调用 Protocol 层 */
CS_LOW(dev);
SoftSPI_WriteByte(dev->bus, W25Q_CMD_READ_DATA);
SoftSPI_WriteByte(dev->bus, (addr >> 16) & 0xFF);
SoftSPI_WriteByte(dev->bus, (addr >> 8) & 0xFF);
SoftSPI_WriteByte(dev->bus, addr & 0xFF);
for (uint16_t i = 0; i < len; i++)
    buf[i] = SoftSPI_TransferByte(dev->bus, 0xFF);
CS_HIGH(dev);
```

## 5.3 HAL 库在项目中的角色

| 功能 | HAL 提供的接口 | 是否必需 |
|------|--------------|---------|
| 系统时钟 72MHz | `HAL_Init()` + `SystemClock_Config()` | 是，手写极易出错 |
| SysTick 定时 | `HAL_Delay()` / `HAL_GetTick()` | 是 |
| UART + DMA | `HAL_UART_Receive_DMA` 全套 | 是，DMA 配置复杂 |
| GPIO 读写 | `HAL_GPIO_WritePin` / `ReadPin` | 可用寄存器替代 |
| 中断管理 | `HAL_UART_IRQHandler` 等 | 可用寄存器替代 |

软件 I2C/SPI 只用了 HAL 最简单的 GPIO 读写，理论上可以换成寄存器操作（更快更省空间）。UART DMA 是真正依赖 HAL 的复杂外设。

## 5.4 CubeMX 的使用边界

**CubeMX 管理的内容**（不要手动改）：
- `SystemClock_Config()` — 时钟树配置
- UART + DMA 初始化 — 通道映射、中断优先级
- `MX_GPIO_Init()` 中硬件外设的引脚配置

**BSP 层管理的内容**（不经过 CubeMX）：
- 软件 I2C 引脚（PB8/PB9）— 在 `bsp_soft_i2c.c` 中初始化
- 软件 SPI 引脚（PA5/PA6/PA7）— 在 `bsp_soft_spi.c` 中初始化
- W25Q16 的 CS 引脚 — 在 Driver 层或 main.c 中初始化

**原则**：CubeMX 管硬件外设，BSP 层管软件协议引脚，互不干扰。

---

# 第六章 Debug 测试框架

## 串口命令系统

通过串口助手发送单字符命令触发测试：

| 命令 | 功能 |
|------|------|
| `0` | UART DMA 回环测试（原样回发收到的数据） |
| `1` | OLED 显示测试（计数器递增） |
| `2` | EEPROM 写入测试（向 0x00 写 8 字节） |
| `3` | EEPROM 读取测试（从 0x00 读 8 字节） |
| `9` | I2C 总线扫描（探测所有设备地址） |

## 开机自检

上电后自动执行 `Module_Test_SelfCheck()`：

1. **OLED 检测**：发送 I2C 地址探测 ACK，有应答才报 OK
2. **EEPROM 检测**：向 0xF0 写入 0xA5 → 读回 → 比对是否一致
3. 结果同时输出到串口和 OLED 屏幕

## I2C 总线扫描

```c
for (uint8_t addr = 0x02; addr < 0xFE; addr += 2)
{
    SoftI2C_Start(&i2c1_bus);
    if (SoftI2C_SendByte(&i2c1_bus, addr) == 0)
        printf("[SCAN] Found device at 0x%02X\r\n", addr);
    SoftI2C_Stop(&i2c1_bus);
}
```

扫描地址范围 0x02~0xFC，步进 2（只扫描写地址，读地址 = 写地址 | 1）。

正常应只发现：
- `0x78` — SSD1306 OLED
- `0xA0` — AT24C02 EEPROM

如果所有地址都响应，说明 SDA 被拉低（总线锁死）。

---

# 附录 常用参考资料

| 资料 | 说明 |
|------|------|
| STM32F1xx 参考手册（RM0008） | DMA/USART/GPIO/I2C/SPI 章节 |
| AT24C02 数据手册 | I2C EEPROM 时序和页写规则 |
| W25Q16 数据手册 | SPI Flash 命令集和擦写时序 |
| SSD1306 数据手册 | OLED 控制器初始化序列 |
| AN2606 | STM32 系统存储器 Bootloader 说明 |
| AN3155 | USART 协议用于 STM32 Bootloader |
| 《企业级嵌入式项目架构分层解耦》 | 本项目架构设计参考 |

---

> 项目当前状态：UART DMA + 软件 I2C/SPI + OLED/EEPROM/Flash 驱动已完成，待开发 OTA 升级中间件。
