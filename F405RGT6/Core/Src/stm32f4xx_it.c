/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32f4xx_it.h"
#include "usart.h"   /* USART3_DMA_xxxCallback 声明 */
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */
static uint32_t step_divide_x2 = STEP_DIVIDE * 6;             //一个脉冲翻转两次GPIO
/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern UART_HandleTypeDef huart5;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 stream1 global interrupt.
  * @note  DMA1_Stream1 = USART3_RX(蓝牙) 的搬运通道。
  *        在调用 HAL_DMA_IRQHandler 之前，先把 HAL 的回调槽指向我们自己的
  *        USART3_DMA_xxxCallback，这样 HAL 的双缓冲分支就会回调到应用层，
  *        而不是默认的弱函数。
  */
void DMA1_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream1_IRQn 0 */
  hdma_usart3_rx.XferHalfCpltCallback     = USART3_DMA_HalfCallback;   /* Memory0 过半 */
  hdma_usart3_rx.XferCpltCallback         = USART3_DMA_FullCallback;   /* Memory0 全满 */
  hdma_usart3_rx.XferM1HalfCpltCallback   = USART3_DMA_FullCallback;   /* Memory1 过半(即后半段过半) */
  hdma_usart3_rx.XferM1CpltCallback       = USART3_DMA_HalfCallback;   /* Memory1 全满(回到前半段) */
  hdma_usart3_rx.XferErrorCallback        = USART3_DMA_ErrorCallback;  /* 传输错误 */
  /* USER CODE END DMA1_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
  /* USER CODE BEGIN DMA1_Stream1_IRQn 1 */

  /* USER CODE END DMA1_Stream1_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  if ((TIM2->SR & TIM_SR_UIF) != 0U)
  {
    TIM2->SR &= ~TIM_SR_UIF;              /* 清更新中断标志, 否则会反复进中断 */

	  if(HAL_GPIO_ReadPin(x_limit_r_grop,x_limit_r_pin) == x_limit_r_active){   /* X 右: 低有效 */
		  return;
	  }
	  if(HAL_GPIO_ReadPin(x_limit_l_grop,x_limit_l_pin) == x_limit_l_active){   /* X 左: 低有效 */
		  return;
	  }
	  if(HAL_GPIO_ReadPin(y_limit_d_grop,y_limit_d_pin) == y_limit_d_active){   /* Y 下: 高有效 */
		  return;
	  }
	  if(HAL_GPIO_ReadPin(y_limit_u_grop,y_limit_u_pin) == y_limit_u_active){   /* Y 上: 高有效 */
		  return;
	  }
	  
    if(y.lock == 1){
      HAL_GPIO_TogglePin(y_plus_grop, y_plus_pin);
      y.divide_step++;
      if( y.divide_step >= step_divide_x2){
        y.cur_point++;
        y.divide_step = 0;
        y.lock = 0;
      }
    }else{
	  HAL_GPIO_TogglePin(x_plus_grop, x_plus_pin);   /* 翻转 PC9 -> 输出一个步进脉冲 */
      x.divide_step ++;
      if (x.divide_step >= step_divide_x2)
      {
        x.cur_point++;
        x.divide_step = 0U;
        x.add_flag = 1;
      }
    }

    if (x.cur_point >= x.all_point)
    {
      y.lock = 1;                            /* 本行走完, 通知主循环换行 */
      x.cur_point = 0;
      HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
    }
  }
  /* USER CODE END TIM2_IRQn 0 */
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles UART5 global interrupt.
  */
void UART5_IRQHandler(void)
{
  /* USER CODE BEGIN UART5_IRQn 0 */

  /* USER CODE END UART5_IRQn 0 */
  HAL_UART_IRQHandler(&huart5);
  /* USER CODE BEGIN UART5_IRQn 1 */

  /* USER CODE END UART5_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
