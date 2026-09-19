/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mb_master.h"
#include "mb_port_f4.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
extern UART_HandleTypeDef huart1;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

typedef struct {
    uint8_t  step_index;       /* 状态机当前步骤 */
    uint8_t  slave_id;         /* 从站地址 */
    uint16_t start_reg_addr;   /* 本次访问的起始寄存器地址 */
    mb_m_err_t error;          /* MB_M_OK / MB_M_EXCEPTION / MB_M_TIMEOUT ... */
    uint8_t  exception_code;   /* 从站异常码，仅 error == MB_M_EXCEPTION 时有效 */
    uint32_t elapsed_ms;       /* 本步耗时，单位 ms */

    uint8_t  tx_len;
    uint8_t  rx_len;
    uint8_t  tx_buf[8];        /* 发送帧缓冲 */
    uint8_t  rx_buf[16];       /* 接收帧缓冲 */

    uint16_t reg_count;        /* 解析出的寄存器个数 */
    uint16_t reg_values[8];    /* 解析出的寄存器值 */
} mb_m_transaction_t;

static osMessageQueueId_t s_q_sample = NULL;
static uint32_t s_dropped = 0;	/* 队列满被丢掉的条数 */


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
 
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for taskModbus */
osThreadId_t taskModbusHandle;
const osThreadAttr_t taskModbus_attributes = {
  .name = "taskModbus",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for taskReport */
osThreadId_t taskReportHandle;
const osThreadAttr_t taskReport_attributes = {
  .name = "taskReport",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for taskLed */
osThreadId_t taskLedHandle;
const osThreadAttr_t taskLed_attributes = {
  .name = "taskLed",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartTaskModbus(void *argument);
void StartTaskReport(void *argument);
void StartTaskLed(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
	(void)xTask;
	(void)pcTaskName;
	
	taskDISABLE_INTERRUPTS();
	HAL_GPIO_WritePin(GPIOF,GPIO_PIN_9,GPIO_PIN_RESET);
	while(1)
	{}
}
/* USER CODE END 4 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
	s_q_sample = osMessageQueueNew(8U, sizeof(mb_m_transaction_t), NULL);  // 创建一个消息队列，最多存放 8 条 mb_sample_t 类型的消息
  if(s_q_sample == NULL)
  {
    uprintf("任务队列创建失败....\r\n");
    HAL_GPIO_WritePin(GPIOF,GPIO_PIN_9,GPIO_PIN_RESET);
    while(1)
    {}
  }
  else
  uprintf("任务队列创建成功\r\n");
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of taskModbus */
  taskModbusHandle = osThreadNew(StartTaskModbus, NULL, &taskModbus_attributes);

  /* creation of taskReport */
  taskReportHandle = osThreadNew(StartTaskReport, NULL, &taskReport_attributes);

  /* creation of taskLed */
  taskLedHandle = osThreadNew(StartTaskLed, NULL, &taskLed_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_StartTaskModbus */
/**
* @brief Function implementing the taskModbus thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskModbus */
void StartTaskModbus(void *argument)
{
  /* USER CODE BEGIN StartTaskModbus */
  /* Infinite loop */
  (void)argument; // 避免编译器警告：未使用的参数
  uint8_t step = 0;
  while(1)
  {
    mb_m_transaction_t s;
    uint16_t req_len = 0;
    uint16_t rsp_len = 0;
    uint16_t out_n = 0;
    uint16_t out[8];
    uint8_t exc = 0;
    mb_m_err_t e;
    uint32_t ms = 0;

    memset(&s, 0, sizeof(s));   //
    s.step_index = (uint8_t)step;

    /* 组帧 */
    switch(step)
    {
      case 0:
        s.slave_id = 0x01;
        s.start_reg_addr = 0;
        req_len =  mb_master_build_read(s.tx_buf, sizeof(s.tx_buf),
                            s.slave_id, s.start_reg_addr, 2);
        break;
      
      case 1:
        s.slave_id = 0x01;
        s.start_reg_addr = 4;
        req_len = mb_master_build_write_single(s.tx_buf,sizeof(s.tx_buf),
                                          s.slave_id,s.start_reg_addr,0x037F);
        break;

      case 2:
        s.slave_id = 0x01;
        s.start_reg_addr = 4;
        req_len = mb_master_build_read(s.tx_buf,sizeof(s.tx_buf),
                                  s.slave_id,s.start_reg_addr,1);
        break;
      
      case 3:
        s.slave_id = 0x01;
        s.start_reg_addr = 200;
        req_len = mb_master_build_read(s.tx_buf,sizeof(s.tx_buf),
                                  s.slave_id,s.start_reg_addr,1);
        break;

      case 4:
        s.slave_id = 0x02;
        s.start_reg_addr = 0;
        req_len = mb_master_build_read(s.tx_buf,sizeof(s.tx_buf),
                                  s.slave_id,s.start_reg_addr,1);
        break;

      default:
        step = 0;
        continue;
    }
    s.tx_len = (uint8_t)req_len;

    /* 收发+计时*/
    ms = HAL_GetTick();
    e = mb_port_transfer(s.tx_buf,s.tx_len,s.rx_buf,sizeof(s.rx_buf),&rsp_len,200);
    s.elapsed_ms = HAL_GetTick() - ms;
    if(e == MB_M_OK || e == MB_M_EXCEPTION)
    s.rx_len = (uint8_t)rsp_len;
    else
    s.rx_len = 0;


    /* 解析 */
    if(e == MB_M_OK)
    {
      e = mb_master_parse(s.tx_buf,s.tx_len,
      s.rx_buf,s.rx_len,out,sizeof(out)/sizeof(out[0]),&out_n,&exc);
      if(e == MB_M_OK)
      {
        for(uint16_t i = 0; i < out_n; i++)
        {
          s.reg_values[i] = out[i];
        }
        s.reg_count = out_n;
      }
      s.exception_code = exc;
    }
    s.error = e;

    /* ---- 入队 ---- */
    if(s_q_sample != NULL)
    {
      if(osMessageQueuePut(s_q_sample,&s,0U,0U) != osOK)
      s_dropped++;
    }
    step = (uint8_t)((step + 1u) % 5u);
    osDelay(500);
  }
  /* USER CODE END StartTaskModbus */
}

/* USER CODE BEGIN Header_StartTaskReport */
/**
* @brief Function implementing the taskReport thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskReport */
void StartTaskReport(void *argument)
{
  /* USER CODE BEGIN StartTaskReport */
  (void)argument;
  mb_m_transaction_t s;
  /* Infinite loop */
  while(1)
  {
    if(s_q_sample == NULL)
    {
      osDelay(100);
      continue;
    }
    if(osMessageQueueGet(s_q_sample,&s,NULL,osWaitForever) != osOK)
    {
      continue;
    }

    switch (s.step_index) {
        case 0:
            uprintf("[RPT][S0] read  slave=%02X start=%u qty=2\r\n",
                    (unsigned)s.slave_id, (unsigned)s.start_reg_addr);
            break;
        case 1:
            uprintf("[RPT][S1] write slave=%02X reg[%u]=0x037F\r\n",
                    (unsigned)s.slave_id, (unsigned)s.start_reg_addr);
            break;
        case 2:
            uprintf("[RPT][S2] read  slave=%02X start=%u qty=1\r\n",
                    (unsigned)s.slave_id, (unsigned)s.start_reg_addr);
            break;
        case 3:
            uprintf("[RPT][S3] read  slave=%02X start=%u qty=1 (expect exception)\r\n",
                    (unsigned)s.slave_id, (unsigned)s.start_reg_addr);
        case 4:
            uprintf("[RPT][S4] read  slave=%02X start=%u qty=1 (expect timeout)\r\n",
                    (unsigned)s.slave_id, (unsigned)s.start_reg_addr);
            break;
        default:
            uprintf("[RPT][S?] unknown step\r\n");
            break;
        }
        /* ---- TX 回显 ---- */
        uprintf("[RPT][S%u] TX:", (unsigned)s.step_index);
        for(uint16_t i = 0; i < s.tx_len; i++)
        {
          uprintf(" %02X", s.tx_buf[i]);          
        }
        /* 按 s.err 分三种情况打印 */
        if (s.error == MB_M_OK) {
            uprintf("[RPT][S%u] RX:", (unsigned)s.step_index);
            for (uint8_t i = 0; i < s.rx_len; i++) {
                uprintf(" %02X", s.rx_buf[i]);
            }
            uprintf("\r\n");

            uprintf("[RPT][S%u] OK  %lums",
                    (unsigned)s.step_index, (unsigned long)s.elapsed_ms);
            for (uint16_t i = 0; i < s.reg_count; i++) {
                uprintf(" reg[%u]=%u", (unsigned)i, (unsigned)s.reg_values[i]);
            }
            uprintf("\r\n");
        }
        else if (s.error == MB_M_EXCEPTION) {
            uprintf("[RPT][S%u] RX:", (unsigned)s.step_index);
            for (uint8_t i = 0; i < s.rx_len; i++) {
                uprintf(" %02X", s.rx_buf[i]);
            }
            uprintf("\r\n");

            uprintf("[RPT][S%u] EXC %lums slave exception: 0x%02X\r\n",
                    (unsigned)s.step_index, (unsigned long)s.elapsed_ms, (unsigned)s.exception_code);
        }
        else {
            /* 超时/CRC 等：不打 RX 字节，避免垃圾。 */
            /* mb_m_err_str 若不存在，改成 (int)s.err 或你自己的错误字符串函数 */
            uprintf("[RPT][S%u] %s %lums\r\n",
                    (unsigned)s.step_index, mb_m_err_str(s.error), (unsigned long)s.elapsed_ms);
        }

        /* TODO-7：留一行注释占位 */
        /* CP2: MQTT publish 就写在这里 —— 采集侧一个字都不用改 */
  }
  /* USER CODE END StartTaskReport */
}

/* USER CODE BEGIN Header_StartTaskLed */
/**
* @brief Function implementing the taskLed thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskLed */
void StartTaskLed(void *argument)
{
  /* USER CODE BEGIN StartTaskLed */
  (void)argument;
  /* Infinite loop */
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10);
    osDelay(500);
  }
  /* USER CODE END StartTaskLed */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

