#ifndef __SSD1306_H__
#define __SSD1306_H__

#include "soft_i2c.h"
#include <stdint.h>

#define SSD1306_ADDR    0x78

typedef struct {
    SoftI2C_Bus_t *bus;
    uint8_t        addr;
} SSD1306_t;

void     SSD1306_Init(SSD1306_t *dev, SoftI2C_Bus_t *bus, uint8_t addr);
void     SSD1306_Clear(SSD1306_t *dev);
void     SSD1306_SetCursor(SSD1306_t *dev, uint8_t y, uint8_t x);
void     SSD1306_ShowChar(SSD1306_t *dev, uint8_t line, uint8_t column, char ch);
void     SSD1306_ShowString(SSD1306_t *dev, uint8_t line, uint8_t column, char *str);
void     SSD1306_ShowNum(SSD1306_t *dev, uint8_t line, uint8_t column, uint32_t num, uint8_t len);
void     SSD1306_ShowSignedNum(SSD1306_t *dev, uint8_t line, uint8_t column, int32_t num, uint8_t len);
void     SSD1306_ShowHexNum(SSD1306_t *dev, uint8_t line, uint8_t column, uint32_t num, uint8_t len);

#endif
