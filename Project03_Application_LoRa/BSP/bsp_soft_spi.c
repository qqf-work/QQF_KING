#include "bsp_soft_spi.h"

/* 板级 SPI1 总线实例，全局唯一 */
SoftSPI_Bus_t spi1_bus;

/**
 * @brief 初始化板级软件 SPI 引脚
 *
 * 工作流程：
 *   1. 使能 GPIOA 时钟（PA5=SCK, PA6=MISO, PA7=MOSI）
 *   2. SCK/MOSI 配置为推挽输出
 *   3. MISO 配置为上拉输入
 *   4. 调用 Protocol 层 SoftSPI_Init 填充总线句柄
 *
 * 换引脚：改 bsp_soft_spi.h 中的宏定义
 * 换芯片：改这个函数里的 GPIO 初始化代码
 */
void BSP_SoftSPI_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* SCK / MOSI：推挽输出，高速 */
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    GPIO_InitStruct.Pin = SPI1_SCK_PIN;
    HAL_GPIO_Init(SPI1_SCK_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = SPI1_MOSI_PIN;
    HAL_GPIO_Init(SPI1_MOSI_PORT, &GPIO_InitStruct);

    /* MISO：上拉输入 */
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;

    GPIO_InitStruct.Pin = SPI1_MISO_PIN;
    HAL_GPIO_Init(SPI1_MISO_PORT, &GPIO_InitStruct);

    /* W25Q16 CS：推挽输出，默认高电平（不选中） */
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Pin   = W25Q_CS_PIN;
    HAL_GPIO_Init(W25Q_CS_PORT, &GPIO_InitStruct);
    HAL_GPIO_WritePin(W25Q_CS_PORT, W25Q_CS_PIN, GPIO_PIN_SET);

    /* 将引脚信息填入总线句柄，之后 Protocol 层通过句柄操作引脚 */
    SoftSPI_Init(&spi1_bus,
                 SPI1_SCK_PORT,  SPI1_SCK_PIN,
                 SPI1_MOSI_PORT, SPI1_MOSI_PIN,
                 SPI1_MISO_PORT, SPI1_MISO_PIN);
}
