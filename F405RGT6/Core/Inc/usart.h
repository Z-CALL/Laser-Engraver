/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   This file contains all the function prototypes for
  *          the usart.c file
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
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern UART_HandleTypeDef huart5;

extern UART_HandleTypeDef huart1;

extern UART_HandleTypeDef huart3;

extern UART_HandleTypeDef huart6;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_UART5_Init(void);
void MX_USART1_UART_Init(void);
void MX_USART3_UART_Init(void);
void MX_USART6_UART_Init(void);

/* USER CODE BEGIN Prototypes */

/* ---------------- 蓝牙串口(USART3) DMA 双缓冲接收接口 ---------------- */

/**
  * @brief  启动 USART3(蓝牙) 的 DMA 双缓冲接收
  * @note   在 MX_USART3_UART_Init() 之后调用一次即可，之后 DMA 会一直循环搬运。
  * @retval HAL_OK 表示启动成功
  */
HAL_StatusTypeDef USART3_StartReceiveDMA(void);

/**
  * @brief  停止 USART3 的 DMA 接收
  */
void USART3_StopReceiveDMA(void);

/**
  * @brief  主循环任务：把 DMA 收好的数据搬到 row_buffer
  * @note   放在 while(1) 里反复调用；无新数据时几乎零开销。
  */
void USART3_DMA_BufferTask(void);

/**
  * @brief  DMA 收满前半段(4KB)时的提醒（在 DMA1_Stream1 中断里被调用）
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_HalfCallback(DMA_HandleTypeDef *hdma);

/**
  * @brief  DMA 收满后半段(4KB)时的提醒（在 DMA1_Stream1 中断里被调用）
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_FullCallback(DMA_HandleTypeDef *hdma);

/**
  * @brief  DMA 出错回调
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_ErrorCallback(DMA_HandleTypeDef *hdma);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

