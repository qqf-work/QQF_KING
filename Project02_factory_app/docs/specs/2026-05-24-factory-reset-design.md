# 出厂恢复与 Flash 分区重设计

## 背景

当前 Bootloader 仅支持两种启动路径：直接跳转 A 区 App 或从 W25Q16 更新后跳转。更新失败后无恢复手段，设备变砖。需要增加出厂恢复功能，并重新划分 Flash 以容纳出厂默认程序。

## Flash 分区方案

```
0x08000000  ┌──────────────────┐
            │  Bootloader 16KB │ 16页 (page 0~15)
0x08004000  ├──────────────────┤
            │  出厂程序 16KB   │ 16页 (page 16~31)，出厂时预烧录
0x08008000  ├──────────────────┤
            │  A区 App 32KB    │ 32页 (page 32~63)，正常运行/更新目标
0x08010000  └──────────────────┘
```

### 关键设计：出厂程序编译地址

出厂程序的 `.bin` 文件按 **A 区地址 0x08008000** 编译链接（scatter 文件 IROM1 起始 = 0x08008000），但**物理存储在出厂区 0x08004000**。

恢复出厂时 Bootloader 将出厂区的原始字节原样复制到 A 区（0x08004000 → 0x08008000），复制后向量表中的绝对地址（MSP、ResetHandler）自然与 A 区地址对齐，跳转正常工作。

### 出厂程序工程

出厂程序是独立的 Keil 工程：
- Scatter 文件：IROM1 起始 0x08008000，大小 0x8000（32KB）
- VTOR = 0x08008000
- 编译输出 .bin，通过 ST-Link 烧录到 0x08004000
- 烧录时使用 "Erase Sectors" 模式，仅擦除出厂区，不影响 Bootloader 和 A 区

## 按键模块

### 文件

- `BSP/bsp_button.h` — 引脚定义 + API 声明
- `BSP/bsp_button.c` — GPIO + EXTI 实现

### 硬件配置

| 项目 | 值 |
|------|-----|
| 引脚 | PB0 |
| 触发方式 | 下降沿（按下 = 低电平） |
| EXTI 线 | EXTI0 |
| 上拉 | 内部上拉（松开 = 高电平） |

### API

```c
void     BSP_Button_Init(void);           /* GPIO + EXTI 初始化 */
int      BSP_Button_Pressed(void);         /* 返回1=已按下, 0=未按下 */
void     BSP_Button_ClearFlag(void);       /* 清除标志位 */
```

### 实现

- `BSP_Button_Init()`:
  1. 使能 GPIOB 时钟
  2. PB0 配置为输入模式 + 上拉
  3. 配置 EXTI0 下降沿触发，连接到 PB0
  4. 设置 NVIC 优先级并使能 EXTI0 中断
- ISR 中设置 `volatile uint8_t button_flag = 1`
- `BSP_Button_Pressed()` 返回标志位，**同时检查引脚当前电平**（解决上电已按住无下降沿的问题）
- `BSP_Button_ClearFlag()` 清除标志位

### 初始化时机

`BSP_Button_Init()` 在 `MX_GPIO_Init()` 之后立即调用，早于 I2C/SPI/EEPROM 初始化。这样即使按键在上电时已按住，EXTI 配置后立即可以检测到。

## 启动流程

```
上电
  → HAL_Init / SystemClock / MX_GPIO_Init
  → MX_DMA_Init / MX_USART1_UART_Init / UART_DMA_Rx_Init
  → BSP_Button_Init()                          ← EXTI 就绪
  → LED1 点亮
  → BSP_SoftI2C_Init + AT24C02_Init            ← EEPROM 就绪
  → BSP_SoftSPI_Init + W25Q16_Init             ← SPI Flash 就绪

  → BSP_Button_Pressed()?
       YES → App_bootloader_factory_reset()
              → 擦除 A 区（32 页）
              → 逐页复制出厂区 → A 区（16KB，每页 1KB）
              → App_bootloader_jump_app()
              → 失败则打印错误停机

       NO  → App_bootloader_check_update()      ← 读 EEPROM
              → BOOT_NO_UPDATE → App_bootloader_jump_app()
              → BOOT_NEED_UPDATE → App_bootloader_update()
                                    → 成功 → App_bootloader_jump_app()
                                    → 失败 → 打印错误停机
                                              （用户按住 PB0 + 复位即可恢复出厂）

  → while(1) 停机（不应到达）
```

## 新增函数

### App_bootloader_factory_reset()

```c
int App_bootloader_factory_reset(void);
```

- 打印 `[BL] Factory reset triggered`
- 调用 `Flash_Unlock()`
- 逐页擦除 A 区（从 0x08008000 开始，32 页）
- 逐页复制：从出厂区读 1KB → `Flash_Write()` 写入 A 区，共 16 页
- `Flash_Lock()`
- 校验：逐字节比对出厂区与 A 区前 16KB
- 返回 0 成功, -1 失败

## 需要修改的文件

| 文件 | 变更 |
|------|------|
| `APP/bootloader_conf.h` | `B_PAGE_NUM` 20→16，新增 `FACTORY_PAGE_NUM=16`、`FACTORY_REGION_ADDR=0x08004000`，`A_START_PAGE` 改为 32 |
| `BSP/bsp_button.h` | 新建，PB0 引脚定义 + API 声明 |
| `BSP/bsp_button.c` | 新建，GPIO + EXTI0 初始化 + ISR + 查询 |
| `APP/app_bootloader.h` | 新增 `App_bootloader_factory_reset()` 声明 |
| `APP/app_bootloader.c` | 实现 `App_bootloader_factory_reset()` |
| `APP/bootloader.c` | `validate_entry` 范围更新为 A 区新边界 (0x08008000~0x08010000) |
| `Core/Src/main.c` | 加入 `BSP_Button_Init()` + factory_reset 分支 |
| Scatter 文件 | IROM1 大小改为 0x4000（16KB） |
| Keil 工程 | 添加 `bsp_button.c`，更新 scatter 文件引用 |

## 校验策略

出厂恢复的校验采用**逐字节比对**：复制完成后，循环比较出厂区和 A 区的前 16KB 数据，任一字节不匹配则返回失败。简单可靠，无需 CRC 计算。

## RAM 和 Flash 预算

| 项目 | 占用 |
|------|------|
| 出厂复制缓冲 | 1KB（栈上分配，函数结束后释放） |
| 按键模块 | 1 字节（volatile flag） |
| Bootloader Flash | 16KB（分区上限，当前代码约 10KB，扩容空间充足） |
