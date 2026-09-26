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
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>   /* memcpy */
#include "oled.h"
#include "test.h"     /* test_laser_PWM_fre_limit() —— 里面还会带上 laser/bsp_delay/oled */
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
uint8_t  row_buffer[ROW_BUFFER_SIZE];
uint32_t read_cusor = 0;
uint32_t write_cusor = 0;
conformation x, y;
uint8_t* alarm;

uint8_t dma_buffer[DMA_BUFFER_SIZE];
volatile uint8_t  dma_event = 0;
volatile uint8_t  half_busy_flag = 0;
volatile uint32_t dma_rx_block_count = 0;
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
  MX_ADC1_Init();
  MX_UART5_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_USART6_UART_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_TIM3_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_TIM2_Init();

  /* USER CODE BEGIN 2 */
  
  OLED_Init();
  
  y.all_point = 1000;
  x.all_point = 522;

  HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
  HAL_GPIO_TogglePin(y_dir_grop, y_dir_pin);
  
  htim2.Init.Period    = RASTER_TIM2_PERIOD;
  htim2.Init.Prescaler = 0U;
  (void)HAL_TIM_Base_Init(&htim2);
  __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

  HAL_TIM_Base_Start_IT(&htim2);

//  test_laser_PWM_fre_limit_24V();
//  test_delay();
  


  /* 启动蓝牙串口(USART3)的 DMA 双缓冲接收：
     DMA 从此开始把上位机下发的像素灰度值循环搬进 dma_buffer，
     收满前半段/后半段时会在 DMA1_Stream1 中断里置 dma_event 提醒 CPU。*/
//  USART3_StartReceiveDMA();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
//      USART3_DMA_BufferTask();

      if(y.cur_point >= y.all_point){
        HAL_TIM_Base_Stop_IT(&htim2);
        laser_close();
        char program_end[] = {"program end!"};
        OLED_ShowString(0, 0, program_end, OLED_FONT_08, 1);
        OLED_Refresh();
        while(1);
      }

      if(x.add_flag){
        x.add_flag = 0;
        laser_open();
        //set_delay_time(((test_image[y.cur_point][x.cur_point]/25U) + 1) *240);
        alarm = Creat_alarm(test_image[y.cur_point][x.cur_point] * 9);
        if(alarm == NULL){
            HAL_TIM_Base_Stop_IT(&htim2);
            char program_error[] = {"program error!!!"};
            OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
            OLED_Refresh();
            while(1);
        }
      }

      delay_till();

      if((alarm != NULL) && (*alarm)){
        laser_close();
        *alarm = 0;
        free(alarm);
        alarm = NULL;
      }
    /* USER CODE BEGIN 3 */
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
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
/* ★ 注意: TIM2_IRQHandler() 已统一放到 Core/Src/stm32f4xx_it.c 中实现, 此处不要再写一份。
   中断向量只能有一个函数体, 若在此处再定义一份会立即与 stm32f4xx_it.c 冲突,
   链接时报 "Error: L6200E: Symbol TIM2_IRQHandler multiply defined"。
   (这个函数曾被覆盖操作还原回过 main.c, 若又出现, 删掉它即可。) */

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
