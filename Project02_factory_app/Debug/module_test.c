#include "module_test.h"
#include "uart_buf.h"
#include "bsp_soft_i2c.h"
#include "ssd1306.h"
#include "at24c02.h"
#include "w25q16.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

extern SSD1306_t oled;
extern AT24C02_t eeprom;
extern W25Q16_t  w25q;

/**
 * @brief UART DMA 收发测试
 *
 * 把串口收到的数据原样回发，验证 DMA + IDLE 中断是否正常
 */
static void Test_UART_DMA(void)
{
    if (uart_rx_queue.URxDataOUT != uart_rx_queue.URxDataIN)
    {
        uint16_t len = uart_rx_queue.URxDataOUT->end - uart_rx_queue.URxDataOUT->start + 1;
        printf("[UART] Recv %d bytes: ", len);
        for (uint16_t i = 0; i < len; i++)
        {
            printf("%c", uart_rx_queue.URxDataOUT->start[i]);
        }
        printf("\r\n");

        uart_rx_queue.URxDataOUT++;
        if (uart_rx_queue.URxDataOUT > uart_rx_queue.URxDataEND)
            uart_rx_queue.URxDataOUT = &uart_rx_queue.URxDataPtr[0];
    }
}

/**
 * @brief OLED 显示测试
 *
 * 在 4 行分别显示不同格式的数据
 */
static void Test_OLED(void)
{
    static uint32_t counter = 0;

    SSD1306_ShowString(&oled, 1, 1, "OLED Test");
    SSD1306_ShowNum(&oled, 2, 1, counter, 6);
    SSD1306_ShowHexNum(&oled, 3, 1, counter, 8);
    SSD1306_ShowSignedNum(&oled, 4, 1, (int32_t)counter - 50000, 6);

    printf("[OLED] Display counter=%lu\r\n", counter);
    counter++;
}

/**
 * @brief W25Q16 读取 JEDEC ID 并在 OLED 上显示
 */
static void Test_W25Q16_ID(void)
{
    uint32_t id = W25Q16_ReadJEDECID(&w25q);

    SSD1306_Clear(&oled);
    SSD1306_ShowString(&oled, 1, 1, "W25Q16 Test");
    SSD1306_ShowHexNum(&oled, 2, 1, id, 6);

    if (id == 0xEF4015)
    {
        SSD1306_ShowString(&oled, 3, 1, "ID      OK");
        printf("[W25Q16] JEDEC ID: %06lX, OK\r\n", id);
    }
    else
    {
        SSD1306_ShowString(&oled, 3, 1, "ID    FAIL");
        printf("[W25Q16] JEDEC ID: %06lX, unexpected\r\n", id);
    }
}

/**
 * @brief W25Q16 写入/擦写测试
 *
 * 在扇区 0 写入数据再读回比对，最后恢复扇区内容
 */
static void Test_W25Q16_RW(void)
{
    uint8_t w_data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    uint8_t r_data[8] = {0};

    /* 先备份扇区 0 前 8 字节 */
    uint8_t backup[8];
    W25Q16_Read(&w25q, 0, backup, 8);

    /* 擦除扇区 0，写入测试数据 */
    W25Q16_EraseSector(&w25q, 0);
    W25Q16_Write(&w25q, 0, w_data, 8);
    W25Q16_Read(&w25q, 0, r_data, 8);

    /* 比对 */
    uint8_t match = 1;
    for (uint8_t i = 0; i < 8; i++)
    {
        if (r_data[i] != w_data[i]) { match = 0; break; }
    }

    SSD1306_Clear(&oled);
    SSD1306_ShowString(&oled, 1, 1, "W25Q16 R/W");
    if (match)
    {
        SSD1306_ShowString(&oled, 2, 1, "Write   OK");
        SSD1306_ShowString(&oled, 3, 1, "Read    OK");
        printf("[W25Q16] R/W test PASSED\r\n");
    }
    else
    {
        SSD1306_ShowString(&oled, 2, 1, "Verify FAIL");
        printf("[W25Q16] R/W test FAILED\r\n  Write: ");
        for (uint8_t i = 0; i < 8; i++) printf("%02X ", w_data[i]);
        printf("\r\n  Read:  ");
        for (uint8_t i = 0; i < 8; i++) printf("%02X ", r_data[i]);
        printf("\r\n");
    }

    /* 恢复原始数据 */
    W25Q16_EraseSector(&w25q, 0);
    W25Q16_Write(&w25q, 0, backup, 8);
}

/**
 * @brief AT24C02 写入测试
 *
 * 向地址 0x00 写入一组测试数据
 */
static void Test_AT24C02_Write(void)
{
    uint8_t test_data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    if (AT24C02_Write(&eeprom, 0x00, test_data, 8) == 0)
    {
        printf("[EEPROM] Write OK: ");
        for (uint8_t i = 0; i < 8; i++)
            printf("%02X ", test_data[i]);
        printf("\r\n");
    }
    else
    {
        printf("[EEPROM] Write FAILED\r\n");
    }
}

/**
 * @brief AT24C02 读取测试
 *
 * 从地址 0x00 读回 8 字节，通过串口打印
 */
static void Test_AT24C02_Read(void)
{
    uint8_t buf[8] = {0};

    if (AT24C02_Read(&eeprom, 0x00, buf, 8) == 0)
    {
        printf("[EEPROM] Read OK: ");
        for (uint8_t i = 0; i < 8; i++)
            printf("%02X ", buf[i]);
        printf("\r\n");
    }
    else
    {
        printf("[EEPROM] Read FAILED\r\n");
    }
}

/**
 * @brief 根据命令字节执行对应的模块测试
 *
 * 使用方式：在串口助手中发送命令字符
 *   '0' = UART DMA 收发测试
 *   '1' = OLED 显示测试
 *   '2' = EEPROM 写入测试
 *   '3' = EEPROM 读取测试
 */
void Module_Test_Process(uint8_t cmd)
{
    switch (cmd)
    {
        case '0': Test_UART_DMA();                          break;
        case '1': Test_OLED();                              break;
        case '2': Test_AT24C02_Write();                     break;
        case '3': Test_AT24C02_Read();                      break;
        case '4': Test_W25Q16_ID();                         break;
        case '5': Test_W25Q16_RW();                         break;
            /* 在串口助手中发送 '9' 触发总线扫描 */
        case '9': /* 扫描 I2C 总线上所有设备 */
            printf("[SCAN] Scanning I2C bus...\r\n");
            for (uint8_t addr = 0x02; addr < 0xFE; addr += 2)
            {
                SoftI2C_Start(&i2c1_bus);
                if (SoftI2C_SendByte(&i2c1_bus, addr) == 0)
                    printf("[SCAN] Found device at 0x%02X\r\n", addr);
                SoftI2C_Stop(&i2c1_bus);
            }
            printf("[SCAN] Done\r\n");
            break;

        default:
            printf("[TEST] Unknown cmd: %c (0x%02X)\r\n", cmd, cmd);
            break;
    }
}

/**
 * @brief 开机自检
 *
 * 依次测试各模块是否正常，结果通过串口输出
 */
void Module_Test_SelfCheck(void)
{
    printf("\r\n===== Module Self Check =====\r\n");

    /* OLED 检测：发送地址探测是否应答 */
    SoftI2C_Start(&i2c1_bus);
    uint8_t oled_ack = SoftI2C_SendByte(&i2c1_bus, SSD1306_ADDR);
    SoftI2C_Stop(&i2c1_bus);

    if (oled_ack == 0)
    {
        SSD1306_Clear(&oled);
        SSD1306_ShowString(&oled, 1, 1, "Self Check");
        SSD1306_ShowString(&oled, 2, 1, "OLED    OK");
        printf("[OLED] OK\r\n");
    }
    else
    {
        printf("[OLED] FAIL (no ACK at 0x%02X)\r\n", SSD1306_ADDR);
    }

    /* EEPROM 检测：写入再读回 */
    uint8_t w = 0xA5, r = 0;
    AT24C02_WriteByte(&eeprom, 0xF0, w);
    AT24C02_ReadByte(&eeprom, 0xF0, &r);
    if (r == w)
    {
        if (oled_ack == 0)
            SSD1306_ShowString(&oled, 3, 1, "EEPROM  OK");
        printf("[EEPROM] OK (write 0x%02X, read 0x%02X)\r\n", w, r);
    }
    else
    {
        if (oled_ack == 0)
            SSD1306_ShowString(&oled, 3, 1, "EEPROM  FAIL");
        printf("[EEPROM] FAIL (write 0x%02X, read 0x%02X)\r\n", w, r);
    }

    if (oled_ack == 0)
        SSD1306_ShowString(&oled, 4, 1, "All Done");

    /* W25Q16 检测：读取 JEDEC ID */
    uint32_t flash_id = W25Q16_ReadJEDECID(&w25q);
    if (flash_id == 0xEF4015)
    {
        if (oled_ack == 0)
            SSD1306_ShowString(&oled, 4, 1, "FLASH   OK");
        printf("[W25Q16] OK (JEDEC ID: 0x%06lX)\r\n", flash_id);
    }
    else
    {
        if (oled_ack == 0)
            SSD1306_ShowString(&oled, 4, 1, "FLASH   ??");
        printf("[W25Q16] Unexpected ID: 0x%06lX (expected 0xEF4015)\r\n", flash_id);
    }

    printf("===== Self Check Done =====\r\n\r\n");
}
