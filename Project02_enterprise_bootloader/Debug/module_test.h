#ifndef __MODULE_TEST_H__
#define __MODULE_TEST_H__

#include <stdint.h>

/*
 * 硬件模块测试框架
 *
 * 通过串口命令字符触发各模块的独立测试，用于调试外设连通性
 * 使用方式：串口助手发送字符命令
 *   '0' = UART DMA 回环测试       '1' = OLED 显示测试
 *   '2' = EEPROM 写入测试          '3' = EEPROM 读取测试
 *   '4' = W25Q16 JEDEC ID 读取    '5' = W25Q16 写读擦验证
 *   '9' = I2C 总线扫描
 */

/* 测试命令 ID（与串口命令字符 '0'~'5'、'9' 对应） */
typedef enum {
    TEST_UART_DMA = 0,      /* UART DMA 收发回环测试 */
    TEST_OLED,              /* OLED 显示计数器测试 */
    TEST_AT24C02_WRITE,     /* AT24C02 写入 8 字节测试数据 */
    TEST_AT24C02_READ,      /* AT24C02 从地址 0x00 读回 8 字节 */
    TEST_W25Q16_ID,         /* W25Q16 读取 JEDEC ID（期望 0xEF4015） */
    TEST_W25Q16_RW,         /* W25Q16 扇区 0 写入/读回/比对/恢复测试 */
    TEST_MAX                /* 命令数量上限，用于边界检查 */
} TestCmd_t;

/* 根据串口命令字符执行对应模块测试（主循环中调用） */
void Module_Test_Process(uint8_t cmd);

/* 开机自检：依次测试 OLED、EEPROM、W25Q16，结果通过串口和 OLED 输出 */
void Module_Test_SelfCheck(void);

#endif
