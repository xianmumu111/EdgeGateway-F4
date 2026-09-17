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
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mb_master.h"
#include "mb_port_f4.h"
#include <stdarg.h>
#include <stdio.h>
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
static uint8_t g_req[8];
static uint8_t g_rsp[256];
static uint16_t g_out[8];
static uint8_t g_step = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


/* 极简串口打印：stdarg + vsnprintf + HAL_UART_Transmit
 * 不走 printf/semihosting，避免没勾 MicroLIB 时卡死。
 * 调试口 = huart1（USART1）。
 */
 
 static void uprintf(const char *fmt,...)
 {
	 char buf[128];
	 va_list ap;
	 int n;
	 va_start(ap, fmt);
     n = vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);

     if (n > 0) {
        if (n > (int)sizeof(buf)) {
            n = (int)sizeof(buf);   /* 截断时 vsnprintf 返回"想写的长度"，clamp 一下 */
        }
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 100);
    }
 }
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
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
	uprintf("\r\n=== Modbus RTU master @ STM32F4 ===\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	uint16_t req_len = 0, rsp_len = 0, out_n = 0;
	uint8_t exc = 0;
	uint32_t time_start = 0, time_end = 0;
	mb_m_err_t e;
	  /* ---- 1) 组帧：按 g_step 决定这一步干什么 ---- */
	  switch(g_step)
	  {
		  case 0:
		  {
			  uprintf("[S0] read  slave=01 start=0   qty=2\r\n");
			  req_len = mb_master_build_read(g_req, sizeof(g_req), 0x01, 0, 2);
			  break;
		  }
		  case 1:
		  {
			  uprintf("[S1] write slave=01 addr=4    val=0x037F\r\n");
			  req_len = mb_master_build_write_single(g_req, sizeof(g_req), 0x01, 4, 0x037F);
			  break;
		  }
		  case 2:
		  {
			  uprintf("[S2] read  slave=01 start=4   qty=1\r\n");
			  req_len = mb_master_build_read(g_req, sizeof(g_req), 0x01, 4, 1);
			  break;
		  }
		  case 3:
		  {
			  uprintf("[S3] read  slave=01 start=200 qty=1\r\n");
			  req_len = mb_master_build_read(g_req, sizeof(g_req), 0x01, 200, 1);
			  break;
		  }
		  case 4:
		  {
			  uprintf("[S4] read  slave=02 start=0   qty=1\r\n");
			  req_len = mb_master_build_read(g_req, sizeof(g_req), 0x02, 0, 1);
			  break;
		  }
		  default:
			  g_step = 0;
			  continue;
	  }
	  if(req_len == 0)
		  uprintf("[S%u] build failed\r\n", g_step);
	  else
	  {
		  /* ---- TX 回显 ---- */
		  uprintf("[S%u] TX:", g_step);
		  for(uint16_t i = 0; i< req_len; i++)
				uprintf("%02X",g_req[i]);
		  /* ---- 2) 收发 + 计时 ---- */
		  time_start = HAL_GetTick();
		  e = mb_port_transfer(g_req, req_len, g_rsp, sizeof(g_rsp), &rsp_len, 200);
		  time_end = HAL_GetTick() - time_start;
		  /* ---- 3) 解析 + 打印 ---- */
		  if(e != MB_M_OK)
			  /* 收发就失败：超时 / 缓冲不够 / 参数错 */
            uprintf("[S%u] %s  %lums\r\n", g_step, mb_m_err_str(e), time_end);
		  else
		  {
			  uprintf("[S%u] RX:", g_step);
			  for(uint16_t i = 0; i < rsp_len; i++)
					uprintf("%02X",g_rsp[i]);
			  uprintf("\r\n");
			  
			  e = mb_master_parse(g_req, req_len, g_rsp, rsp_len, g_out, sizeof(g_out) / sizeof(g_out[0]), &out_n, &exc);
			   
			  if (e == MB_M_OK) {
                uprintf("[S%u] OK  %lums    ", g_step, time_end);
                for (uint16_t i = 0; i < out_n; i++) {
                    uprintf("reg[%u]=%u ", i, g_out[i]);
                }
                uprintf("\r\n");
            } else if (e == MB_M_EXCEPTION) {
                uprintf("[S%u] EXC %lums    slave exception: 0x%02X\r\n",
                        g_step, time_end, exc);
            } else {
                uprintf("[S%u] %s  %lums\r\n", g_step, mb_m_err_str(e), time_end);
            }
		  }
	  }
	  /* ---- 4) 下一轮 ---- */
	  g_step = (uint8_t)((g_step + 1u) % 5u);
      HAL_Delay(500);
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
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
