/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "crc.h"
#include "dma.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stm32f1xx_hal.h"          /* HAL 库头文件，用于 GPIO、UART 等外设操作 */
#include "lora_buf.h"               /* LoRa 串口帧缓冲层，提供 DMA+IDLE 帧接收队列 */
#include "lora_proto.h"             /* LoRa OTA 协议定义（UPDATE_REQ/DATA/END 等帧类型） */
#include "bsp_soft_spi.h"           /* 软件 SPI 驱动，用于与 W25Q16 SPI Flash 通信 */
#include "bsp_soft_i2c.h"           /* 软件 I2C 驱动，用于与 AT24C02 EEPROM 通信 */
#include "w25q16.h"                 /* W25Q16 外部 SPI Flash 驱动，OTA 固件临时存储 */
#include "at24c02.h"                /* AT24C02 EEPROM 驱动，存储 OTA 更新标志和固件元数据 */
#include "ota_storage.h"            /* OTA 存储管理层，协调 W25Q16 写入和 EEPROM 标志写入 */
#include "app_ota_update.h"         /* 应用层 OTA 状态机，驱动整个 LoRa 固件接收流程 */
#include <stdio.h>                  /* printf 支持，通过 fputc 重定向到 USART1 输出调试日志 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static LORA_Buf_t lora_ctx;        /* LoRa 串口帧缓冲上下文，管理 USART3 DMA 接收和帧队列 */
static W25Q16_t   w25q_dev;        /* W25Q16 SPI Flash 设备句柄，OTA 期间暂存接收到的固件数据 */
static AT24C02_t  eeprom_dev;      /* AT24C02 EEPROM 设备句柄，存储 Bootloader 更新标志（掉电安全） */
static APP_OTA_t  ota_ctx;         /* OTA 更新状态机上下文，驱动固件接收、CRC 校验、EEPROM 写入 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */

  /*
   * 清除 Bootloader 残留的 NVIC 中断使能和挂起标志
   *
   * 原因：Bootloader 跳转到 App 之前调用了 __disable_irq() 关闭全局中断，
   * 但只是关闭了总闸，并没有清除各个 NVIC 通道的使能位（ICER）和挂起标志（ICPR）。
   * 如果不清理，App 开中断后这些残留中断会立即触发，导致不可预期的行为。
   * 这里通过写全 1 到 ICER（禁止使能）和 ICPR（清除挂起）寄存器来全量清理。
   * STM32F103 有 60 个中断通道，对应 2 个 32 位寄存器（通道 0~59）。
   */
  for (uint32_t i = 0; i < 2; i++) {
      NVIC->ICER[i] = 0xFFFFFFFF;   /* 禁止所有 NVIC 通道使能 */
      NVIC->ICPR[i] = 0xFFFFFFFF;   /* 清除所有 NVIC 通道挂起标志 */
  }
  __enable_irq();                    /* 全局中断已安全清理，重新打开中断 */

  /*
   * LED 初始化为熄灭状态（高电平 = 熄灭，低电平 = 点亮）
   * GPIO 已在 MX_GPIO_Init() 中配置为推挽输出，这里设置初始电平
   */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);

  printf("[APP] main() reached, all MX_init done\r\n");

  /*
   * LoRa 串口帧缓冲初始化
   * 绑定 USART3 句柄到帧缓冲上下文，启动 DMA Normal 模式 + IDLE 接收
   * 后续 LoRa 协议帧通过 lora_ctx 帧队列消费
   */
  LORA_Buf_Init(&lora_ctx, &huart3);
  printf("[APP] LORA_Buf_Init done\r\n");

  /*
   * 外部存储外设初始化
   * - 软件 SPI：W25Q16 的通信总线（PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI）
   * - 软件 I2C：AT24C02 的通信总线（PB8=SCL, PB9=SDA）
   * - W25Q16：OTA 固件临时存储，接收完成后 Bootloader 从此搬运到内部 Flash
   * - AT24C02：存储 OTA 更新标志（状态字节 + 有效性密钥 + 固件大小 + CRC32）
   */
  BSP_SoftSPI_Init();
  BSP_SoftI2C_Init();
  W25Q16_Init(&w25q_dev, &spi1_bus, W25Q_CS_PORT, W25Q_CS_PIN);
  AT24C02_Init(&eeprom_dev, &i2c1_bus, AT24C02_ADDR);
  printf("[APP] W25Q16 + AT24C02 init done\r\n");

  /* 读取 W25Q16 JEDEC ID 验证 SPI 通信和芯片连接是否正常（期望值 0xEF4015） */
  uint32_t jedec_id = W25Q16_ReadJEDECID(&w25q_dev);
  printf("[APP] W25Q16 JEDEC ID: 0x%06lX\r\n", jedec_id);

  /*
   * OTA 状态机初始化
   * 绑定 LoRa 帧缓冲、W25Q16 和 EEPROM 句柄到 OTA 上下文
   * 状态机初始处于空闲状态，等待 LoRa UPDATE_REQ 帧触发更新流程
   */
  APP_OTA_Init(&ota_ctx, &lora_ctx, &w25q_dev, &eeprom_dev);
  printf("[APP] OTA module init done\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /*
     * LED1 翻转：作为心跳指示，每轮循环切换一次状态
     * 如果 LED 停止闪烁，说明主循环被阻塞或程序跑飞
     */
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);

    /*
     * OTA 状态机轮询处理
     * 每轮循环从 LoRa 帧队列消费数据，驱动 OTA 状态转移：
     * IDLE -> RECV_DATA -> VERIFY -> DONE
     * 主循环不能有阻塞延时（HAL_Delay），否则会丢帧
     */
    APP_OTA_Process(&ota_ctx);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
