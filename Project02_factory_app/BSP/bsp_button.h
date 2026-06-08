/*
 * 按键模块 —— PB0 外部中断检测
 *
 * GPIO + EXTI 配置由 CubeMX 生成（gpio.c）
 * 本模块提供 HAL 回调和查询接口
 */

#ifndef __BSP_BUTTON_H__
#define __BSP_BUTTON_H__

#include <stdint.h>

/* 按键引脚定义 */
#define BTN_PORT        GPIOB
#define BTN_PIN         GPIO_PIN_0

/* 初始化按键标志（GPIO+EXTI 由 CubeMX 配置） */
void     BSP_Button_Init(void);

/* 返回1=按键已按下, 0=未按下（检查标志位 + 当前电平） */
int      BSP_Button_Pressed(void);

/* 清除中断标志位 */
void     BSP_Button_ClearFlag(void);

#endif
