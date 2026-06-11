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
#include "lora_buf.h"               /* LoRa 串口帧缓冲层，提供 DMA+IDLE 帧接收队列 */
#include "app_update.h"             /* 网关端 LoRa 更新状态机，管理固件分发流程 */
#include "app_bootloader.h"         /* 网关 Bootloader 辅助定义（分区地址、跳转函数等） */
#include "uart_buf.h"               /* UART DMA+IDLE 帧缓冲，用于串口接收 PC 发送的 .bin 固件 */
#include "fw_cache_conf.h"          /* 固件缓存区配置：FW_CACHE_ADDR(0x08004000) 和大小定义 */
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
static AppUpdate_t update_ctx;     /* 网关更新状态机上下文，驱动 LoRa 固件分发流程 */
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
   * LoRa 串口帧缓冲初始化
   * 绑定 USART3（LoRa 模块通信口）到帧缓冲上下文
   * 启动 DMA Normal 模式 + IDLE 线检测接收，用于接收 LoRa 协议帧
   */
  LORA_Buf_Init(&lora_ctx, &huart3);

  /*
   * 网关端 LoRa 更新状态机初始化
   *
   * 网关的职责是：
   * 1. 通过 USART1 串口从 PC 接收 .bin 固件文件，存储到内部 Flash 缓存区
   * 2. 通过 USART3 LoRa 模块将固件分帧发送给 App 节点
   *
   * FW_CACHE_ADDR = 0x08004000：固件在网关 Flash 中的缓存起始地址
   * 1344：固件大小（当前硬编码，待优化为动态读取）
   */
  AppUpdate_Init(&update_ctx, &lora_ctx,
                 (const uint8_t *)FW_CACHE_ADDR, 1344);

  /*
   * USART1 DMA 接收初始化
   * 用于从 PC 串口接收 .bin 固件数据流（UART 下载协议）
   * USART1 同时用于 printf 调试输出，DMA 仅用于接收方向
   */
  UART_DMA_Rx_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /*
     * 网关更新状态机轮询处理
     * 每轮循环检查两路输入：
     * 1. USART1 串口：检查是否有 PC 发来的固件数据（UART 下载协议）
     * 2. USART3 LoRa：检查是否有 App 节点发来的 UPDATE_REQ 等协议帧
     * 根据状态机当前状态驱动固件分发流程
     * 主循环不能有阻塞延时，以保证串口帧和 LoRa 帧都能及时处理
     */
    AppUpdate_Poll(&update_ctx);
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
