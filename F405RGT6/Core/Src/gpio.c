/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* ---- 步进电机 / 限位引脚统一走 configure.h 的宏 ----
     配置项（x_dir / x_plus / x_enable、y 轴三根、x_limit 系列、y_limit 系列）
     都定义在 Core/Inc/configure.h，换接线时只改那一处，不必动本文件。
     注意：每个宏的 _grop 都是它自己的端口，本文件的物理端口名
     （GPIOA / GPIOC / GPIOB）与它们一一对应，分组保持与原来一致。 */

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(x_dir_grop, CS_Pin|x_dir_pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(x_enable_grop, DC_Pin|RST_Pin|x_enable_pin|x_plus_pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(y_dir_grop, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_12|y_enable_pin
                          |y_plus_pin|y_dir_pin|GPIO_PIN_3|GPIO_PIN_5, GPIO_PIN_RESET);

  /*Configure GPIO pins : x_limit_r x_limit_l y_limit_u y_limit_d */
  GPIO_InitStruct.Pin = x_limit_r_pin|x_limit_l_pin|y_limit_u_pin|y_limit_d_pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(x_limit_r_grop, &GPIO_InitStruct);

  /*Configure GPIO pins : CS_Pin x_dir_pin */
  GPIO_InitStruct.Pin = CS_Pin|x_dir_pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(x_dir_grop, &GPIO_InitStruct);

  /*Configure GPIO pins : DC_Pin RST_Pin x_enable_pin x_plus_pin */
  GPIO_InitStruct.Pin = DC_Pin|RST_Pin|x_enable_pin|x_plus_pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(x_enable_grop, &GPIO_InitStruct);

  /*Configure GPIO pins : PB0 PB1 PB12 y_enable_pin
                           y_plus_pin y_dir_pin PB3 PB5 */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_12|y_enable_pin
                          |y_plus_pin|y_dir_pin|GPIO_PIN_3|GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(y_dir_grop, &GPIO_InitStruct);

  /* ---- 激光输出单独一组 ----
     走 configure.h 的 Laser_grop / Laser_pin（当前是 PA15），
     换激光接线只改那一处，不必动本文件。 */
  HAL_GPIO_WritePin(Laser_grop, Laser_pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : Laser_pin */
  GPIO_InitStruct.Pin = Laser_pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Laser_grop, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
