/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include "tim.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "configure.h"
#include "bsp_delay.h"
#include "laser.h"
#include "step_motor.h"
#include "limit.h"
#include "image.h"
#include "stdlib.h"
#include "Bluetooth.h"
#include <string.h>
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */


/* ===================== 蓝牙串口 DMA 双缓冲配置 ===================== */
/* DMA 缓冲区(第二个缓冲区)：DMA 把 USART3 收到的灰度值直接搬到这里，CPU 不参与 */
#define DMA_BUFFER_SIZE (ROW_BUFFER_SIZE * 2)
/* DMA 搬运完成事件：由 DMA 中断置位，主循环消费，用于把数据搬进 row_buffer */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

typedef struct conformation{
   uint32_t all_point;
   uint32_t cur_point;
   volatile uint8_t add_flag;
   volatile uint8_t lock;
   volatile uint8_t divide_step;
}conformation;

typedef enum state{
   homing_1,
   homing_2,
   homing_3,
   homing_4,
   idle,
   working,
   edge,
   speed_release,
   speed_add,
}state;

   extern conformation x, y; 
   extern volatile state sys_state;
   extern uint32_t read_cusor;
   extern uint32_t write_cusor;
   extern int      cur_count;
   extern int      row_count;
   extern int      line_count;
   extern int      row_count_2;
   extern uint8_t  step_count;
   extern int      line_couant_2;
   extern uint8_t  row_add;
   extern uint8_t  line_add;

	extern uint8_t ctrl_req[3];   			/* 换成你的 3 字节内容 */
	extern uint8_t  req_pending;                          	/* 1 = 已发出，等 67 字节回包 */
	extern uint32_t req_sent_ms;
	extern volatile uint8_t need_send_ctl;   				/* 1 = 该发控制信号了 */
	extern uint8_t RX_number;
	extern uint8_t RX_data[8][64];
	extern uint8_t RX_Ctl[8];
	extern uint8_t RX_write_cusor;
	extern uint8_t work_cusor;
	extern uint8_t work_cusor_point;
	extern volatile uint8_t work_cusor_flag;

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define CS_Pin GPIO_PIN_6
#define CS_GPIO_Port GPIOA
#define DC_Pin GPIO_PIN_4
#define DC_GPIO_Port GPIOC
#define RST_Pin GPIO_PIN_5
#define RST_GPIO_Port GPIOC
#define Jiangguang_Pin GPIO_PIN_9
#define Jiangguang_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
