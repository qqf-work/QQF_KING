/*
 * 按键模块实现 —— PB0 EXTI0 外部中断
 *
 * GPIO + EXTI + NVIC 由 CubeMX 在 gpio.c 中配置
 * 本模块仅提供回调处理和查询接口
 */

#include "bsp_button.h"
#include "stm32f1xx_hal.h"

static volatile uint8_t button_flag = 0;

void BSP_Button_Init(void)
{
    /* GPIO + EXTI 已由 CubeMX 的 MX_GPIO_Init() 配置 */
    button_flag = 0;
}

int BSP_Button_Pressed(void)
{
    if (button_flag || HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET)
        return 1;
    return 0;
}

void BSP_Button_ClearFlag(void)
{
    button_flag = 0;
}

/* HAL EXTI 回调：由 stm32f1xx_it.c 中的 EXTI0_IRQHandler 调用 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == BTN_PIN)
    {
        button_flag = 1;
    }
}
