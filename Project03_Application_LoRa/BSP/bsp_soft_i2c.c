#include "bsp_soft_i2c.h"

/* 板级 I2C1 总线实例，全局唯一，OLED、MPU6050 等设备共用 */
SoftI2C_Bus_t i2c1_bus;

/**
 * @brief 初始化板级软件 I2C 引脚
 *
 * 工作流程：
 *   1. 使能 GPIOB 时钟（PB8=SCL, PB9=SDA）
 *   2. 配置为开漏输出 + 上拉（I2C 标准要求）
 *   3. 调用 Protocol 层 SoftI2C_Init 填充总线句柄
 *
 * 换引脚：改 bsp_soft_i2c.h 中的宏定义
 * 换芯片：改这个函数里的 GPIO 初始化代码
 */
void BSP_SoftI2C_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;   /* 开漏输出，I2C 必须模式 */
    GPIO_InitStruct.Pull  = GPIO_PULLUP;            /* 外部上拉，内部也上拉 */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    GPIO_InitStruct.Pin = I2C1_SCL_PIN;
    HAL_GPIO_Init(I2C1_SCL_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = I2C1_SDA_PIN;
    HAL_GPIO_Init(I2C1_SDA_PORT, &GPIO_InitStruct);

    /* 将引脚信息填入总线句柄，之后 Protocol 层通过句柄操作引脚 */
    SoftI2C_Init(&i2c1_bus, I2C1_SCL_PORT, I2C1_SCL_PIN, I2C1_SDA_PORT, I2C1_SDA_PIN);
    SoftI2C_BusRecovery(&i2c1_bus);
}
