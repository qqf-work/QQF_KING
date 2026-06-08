#include "ssd1306.h"
#include "ssd1306_font.h"
#include <stdint.h>

static void SSD1306_WriteCommand(SSD1306_t *dev, uint8_t cmd)
{
    SoftI2C_Start(dev->bus);
    SoftI2C_SendByte(dev->bus, dev->addr);
    SoftI2C_SendByte(dev->bus, 0x00);
    SoftI2C_SendByte(dev->bus, cmd);
    SoftI2C_Stop(dev->bus);
}

static void SSD1306_WriteData(SSD1306_t *dev, uint8_t data)
{
    SoftI2C_Start(dev->bus);
    SoftI2C_SendByte(dev->bus, dev->addr);
    SoftI2C_SendByte(dev->bus, 0x40);
    SoftI2C_SendByte(dev->bus, data);
    SoftI2C_Stop(dev->bus);
}

void SSD1306_SetCursor(SSD1306_t *dev, uint8_t y, uint8_t x)
{
    SSD1306_WriteCommand(dev, 0xB0 | y);
    SSD1306_WriteCommand(dev, 0x10 | ((x & 0xF0) >> 4));
    SSD1306_WriteCommand(dev, 0x00 | (x & 0x0F));
}

void SSD1306_Clear(SSD1306_t *dev)
{
    for (uint8_t j = 0; j < 8; j++)
    {
        SSD1306_SetCursor(dev, j, 0);
        for (uint8_t i = 0; i < 128; i++)
        {
            SSD1306_WriteData(dev, 0x00);
        }
    }
}

void SSD1306_ShowChar(SSD1306_t *dev, uint8_t line, uint8_t column, char ch)
{
    SSD1306_SetCursor(dev, (line - 1) * 2, (column - 1) * 8);
    for (uint8_t i = 0; i < 8; i++)
    {
        SSD1306_WriteData(dev, OLED_F8x16[ch - ' '][i]);
    }
    SSD1306_SetCursor(dev, (line - 1) * 2 + 1, (column - 1) * 8);
    for (uint8_t i = 0; i < 8; i++)
    {
        SSD1306_WriteData(dev, OLED_F8x16[ch - ' '][i + 8]);
    }
}

void SSD1306_ShowString(SSD1306_t *dev, uint8_t line, uint8_t column, char *str)
{
    for (uint8_t i = 0; str[i] != '\0'; i++)
    {
        SSD1306_ShowChar(dev, line, column + i, str[i]);
    }
}

static uint32_t Pow(uint32_t x, uint32_t y)
{
    uint32_t result = 1;
    while (y--) result *= x;
    return result;
}

void SSD1306_ShowNum(SSD1306_t *dev, uint8_t line, uint8_t column, uint32_t num, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++)
    {
        SSD1306_ShowChar(dev, line, column + i, num / Pow(10, len - i - 1) % 10 + '0');
    }
}

void SSD1306_ShowSignedNum(SSD1306_t *dev, uint8_t line, uint8_t column, int32_t num, uint8_t len)
{
    uint32_t abs_num;
    if (num >= 0)
    {
        SSD1306_ShowChar(dev, line, column, '+');
        abs_num = num;
    }
    else
    {
        SSD1306_ShowChar(dev, line, column, '-');
        abs_num = -num;
    }
    for (uint8_t i = 0; i < len; i++)
    {
        SSD1306_ShowChar(dev, line, column + i + 1, abs_num / Pow(10, len - i - 1) % 10 + '0');
    }
}

void SSD1306_ShowHexNum(SSD1306_t *dev, uint8_t line, uint8_t column, uint32_t num, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t digit = num / Pow(16, len - i - 1) % 16;
        if (digit < 10)
            SSD1306_ShowChar(dev, line, column + i, digit + '0');
        else
            SSD1306_ShowChar(dev, line, column + i, digit - 10 + 'A');
    }
}

void SSD1306_Init(SSD1306_t *dev, SoftI2C_Bus_t *bus, uint8_t addr)
{
    dev->bus  = bus;
    dev->addr = addr;

    HAL_Delay(50);

    SSD1306_WriteCommand(dev, 0xAE);
    SSD1306_WriteCommand(dev, 0xD5);
    SSD1306_WriteCommand(dev, 0x80);
    SSD1306_WriteCommand(dev, 0xA8);
    SSD1306_WriteCommand(dev, 0x3F);
    SSD1306_WriteCommand(dev, 0xD3);
    SSD1306_WriteCommand(dev, 0x00);
    SSD1306_WriteCommand(dev, 0x40);
    SSD1306_WriteCommand(dev, 0xA1);
    SSD1306_WriteCommand(dev, 0xC8);
    SSD1306_WriteCommand(dev, 0xDA);
    SSD1306_WriteCommand(dev, 0x12);
    SSD1306_WriteCommand(dev, 0x81);
    SSD1306_WriteCommand(dev, 0xCF);
    SSD1306_WriteCommand(dev, 0xD9);
    SSD1306_WriteCommand(dev, 0xF1);
    SSD1306_WriteCommand(dev, 0xDB);
    SSD1306_WriteCommand(dev, 0x30);
    SSD1306_WriteCommand(dev, 0xA4);
    SSD1306_WriteCommand(dev, 0xA6);
    SSD1306_WriteCommand(dev, 0x8D);
    SSD1306_WriteCommand(dev, 0x14);
    SSD1306_WriteCommand(dev, 0xAF);

    SSD1306_Clear(dev);
}
