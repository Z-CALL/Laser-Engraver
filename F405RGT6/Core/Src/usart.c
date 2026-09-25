/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   This file provides code for the configuration
  *          of the USART instances.
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
#include "usart.h"

/* USER CODE BEGIN 0 */
#include <string.h>   /* memcpy */
/* USER CODE END 0 */

UART_HandleTypeDef huart5;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart6;
DMA_HandleTypeDef hdma_usart3_rx;

/* UART5 init function */
void MX_UART5_Init(void)
{

  /* USER CODE BEGIN UART5_Init 0 */

  /* USER CODE END UART5_Init 0 */

  /* USER CODE BEGIN UART5_Init 1 */

  /* USER CODE END UART5_Init 1 */
  huart5.Instance = UART5;
  huart5.Init.BaudRate = 115200;
  huart5.Init.WordLength = UART_WORDLENGTH_8B;
  huart5.Init.StopBits = UART_STOPBITS_1;
  huart5.Init.Parity = UART_PARITY_NONE;
  huart5.Init.Mode = UART_MODE_TX_RX;
  huart5.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart5.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart5) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART5_Init 2 */

  /* USER CODE END UART5_Init 2 */

}
/* USART1 init function */

void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}
/* USART3 init function */

void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}
/* USART6 init function */

void MX_USART6_UART_Init(void)
{

  /* USER CODE BEGIN USART6_Init 0 */

  /* USER CODE END USART6_Init 0 */

  /* USER CODE BEGIN USART6_Init 1 */

  /* USER CODE END USART6_Init 1 */
  huart6.Instance = USART6;
  huart6.Init.BaudRate = 115200;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART6_Init 2 */

  /* USER CODE END USART6_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(uartHandle->Instance==UART5)
  {
  /* USER CODE BEGIN UART5_MspInit 0 */

  /* USER CODE END UART5_MspInit 0 */
    /* UART5 clock enable */
    __HAL_RCC_UART5_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    /**UART5 GPIO Configuration
    PC12     ------> UART5_TX
    PD2     ------> UART5_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_UART5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_UART5;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* UART5 interrupt Init */
    HAL_NVIC_SetPriority(UART5_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(UART5_IRQn);
  /* USER CODE BEGIN UART5_MspInit 1 */

  /* USER CODE END UART5_MspInit 1 */
  }
  else if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspInit 0 */

  /* USER CODE END USART1_MspInit 0 */
    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN USART1_MspInit 1 */

  /* USER CODE END USART1_MspInit 1 */
  }
  else if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspInit 0 */

  /* USER CODE END USART3_MspInit 0 */
    /* USART3 clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USART3 DMA Init */
    /* USART3_RX Init */
    hdma_usart3_rx.Instance = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority = DMA_PRIORITY_LOW;
    hdma_usart3_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmarx,hdma_usart3_rx);

  /* USER CODE BEGIN USART3_MspInit 1 */

  /* USER CODE END USART3_MspInit 1 */
  }
  else if(uartHandle->Instance==USART6)
  {
  /* USER CODE BEGIN USART6_MspInit 0 */

  /* USER CODE END USART6_MspInit 0 */
    /* USART6 clock enable */
    __HAL_RCC_USART6_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    /**USART6 GPIO Configuration
    PC6     ------> USART6_TX
    PC7     ------> USART6_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* USER CODE BEGIN USART6_MspInit 1 */

  /* USER CODE END USART6_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==UART5)
  {
  /* USER CODE BEGIN UART5_MspDeInit 0 */

  /* USER CODE END UART5_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_UART5_CLK_DISABLE();

    /**UART5 GPIO Configuration
    PC12     ------> UART5_TX
    PD2     ------> UART5_RX
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_12);

    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);

    /* UART5 interrupt Deinit */
    HAL_NVIC_DisableIRQ(UART5_IRQn);
  /* USER CODE BEGIN UART5_MspDeInit 1 */

  /* USER CODE END UART5_MspDeInit 1 */
  }
  else if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspDeInit 0 */

  /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9|GPIO_PIN_10);

  /* USER CODE BEGIN USART1_MspDeInit 1 */

  /* USER CODE END USART1_MspDeInit 1 */
  }
  else if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspDeInit 0 */

  /* USER CODE END USART3_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_10|GPIO_PIN_11);

    /* USART3 DMA DeInit */
    HAL_DMA_DeInit(uartHandle->hdmarx);
  /* USER CODE BEGIN USART3_MspDeInit 1 */

  /* USER CODE END USART3_MspDeInit 1 */
  }
  else if(uartHandle->Instance==USART6)
  {
  /* USER CODE BEGIN USART6_MspDeInit 0 */

  /* USER CODE END USART6_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART6_CLK_DISABLE();

    /**USART6 GPIO Configuration
    PC6     ------> USART6_TX
    PC7     ------> USART6_RX
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_6|GPIO_PIN_7);

  /* USER CODE BEGIN USART6_MspDeInit 1 */

  /* USER CODE END USART6_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* ============================================================================
 *  蓝牙串口(USART3) DMA 双缓冲接收 —— 数据通路
 * ============================================================================
 *
 *  数据流：
 *      蓝牙 -> USART3_DR -> DMA1_Stream1 -> dma_buffer[] -> row_buffer[] -> 激光
 *                              (硬件搬运)      (本文件)        (本文件)      (main.c)
 *
 *  为什么需要两个缓冲区：
 *      上位机是按像素点连续下发灰度值的，速度取决于蓝牙波特率(115200bps),
 *      一旦 USART3 的 RXNE 没有被及时读走就会 Overrun 丢数据。所以让 DMA
 *      全权负责"从串口寄存器搬到 RAM"，CPU 只在搬运告一段落时来做格式整理。
 *
 *  dma_buffer 被切成前后两半：
 *      +---------------------------+---------------------------+
 *      |   前半段 (4KB)            |   后半段 (4KB)            |
 *      +---------------------------+---------------------------+
 *         ^HT 中断：前半段已收满        ^TC 中断：后半段已收满
 *
 *      DMA 循环填满前半段 -> 触发 HT 中断 -> DMA 继续去填后半段，
 *      同时 CPU 把前半段搬进 row_buffer。两边各干各的，互不阻塞。
 *
 *  中断处理约定：
 *      中断里【只置标志】，不做 4096 字节的 memcpy —— 长拷贝会拉高中断延迟，
 *      可能让 TIM2 的步进脉冲时序抖动，进而影响雕刻精度。
 *      真正的搬运放在主循环 USART3_DMA_BufferTask() 里做。
 * ==========================================================================*/

/**
  * @brief  启动 USART3(蓝牙) 的 DMA 接收，把 dma_buffer 交给 DMA 循环填充
  * @note   必须在 MX_USART3_UART_Init() 之后调用，且只调用一次。
  * @retval HAL_OK 表示 DMA 已开始搬运
  */
HAL_StatusTypeDef USART3_StartReceiveDMA(void)
{
  HAL_StatusTypeDef status;

  dma_event         = 0;
  half_busy_flag    = 0;
  dma_rx_block_count = 0;

  /* 用 DMA 硬件双缓冲模式(Double Buffer Mode)：
     - Memory0 = dma_buffer            前半段
     - Memory1 = dma_buffer + HALF     后半段
     硬件会在两段之间自动切换 CT 位，DMA 永不停歇，天然规避 Overrun。
     注意：xfer 长度传的就是"半缓冲"长度。*/
  status = HAL_DMAEx_MultiBufferStart_IT(&hdma_usart3_rx,
                                         (uint32_t)&USART3->DR,
                                         (uint32_t)&dma_buffer[0],
                                         (uint32_t)&dma_buffer[ROW_BUFFER_SIZE],
                                         ROW_BUFFER_SIZE);

  if (status != HAL_OK)
  {
    return status;
  }

  /* 让 UART 侧把 USART3 的 DMAR 请求打开，并记录本次接收长度。
     这一步 HAL 内部会做：清 ORE 标志 -> 使能 EIE -> 置 CR3_DMAR。*/
  huart3.pRxBuffPtr = &dma_buffer[0];
  huart3.RxXferSize = ROW_BUFFER_SIZE;
  huart3.RxState    = HAL_UART_STATE_BUSY_RX;
  ATOMIC_SET_BIT(huart3.Instance->CR3, USART_CR3_DMAR);

  return HAL_OK;
}

/**
  * @brief  停止 USART3 的 DMA 接收
  */
void USART3_StopReceiveDMA(void)
{
  ATOMIC_CLEAR_BIT(huart3.Instance->CR3, USART_CR3_DMAR);
  ATOMIC_CLEAR_BIT(huart3.Instance->CR1, USART_CR1_PEIE);
  ATOMIC_CLEAR_BIT(huart3.Instance->CR3, USART_CR3_EIE);

  (void)HAL_DMA_Abort(&hdma_usart3_rx);

  huart3.RxState = HAL_UART_STATE_READY;
}

/**
  * @brief  DMA 把 dma_buffer 前半段(4KB)填满了 —— 立即提醒 CPU
  * @note   由 DMA1_Stream1 中断在本函数上下文中调用，务必保持简短。
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_HalfCallback(DMA_HandleTypeDef *hdma)
{
  (void)hdma;
  /* 告诉主循环：前半段数据齐了，可以搬走了 */
  dma_event |= DMA_EVENT_HALF;
  /* DMA 接下来会去写后半段，先打个招呼 */
  half_busy_flag = 1;
}

/**
  * @brief  DMA 把 dma_buffer 后半段(4KB)填满了 —— 立即提醒 CPU
  * @note   由 DMA1_Stream1 中断在本函数上下文中调用，务必保持简短。
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_FullCallback(DMA_HandleTypeDef *hdma)
{
  (void)hdma;
  /* 告诉主循环：后半段数据齐了，可以搬走了 */
  dma_event |= DMA_EVENT_FULL;
  dma_rx_block_count++;
}

/**
  * @brief  DMA 出错（传输错误 / FIFO 错误 / 直接模式错误）
  * @param  hdma DMA 句柄，由 HAL 传入
  */
void USART3_DMA_ErrorCallback(DMA_HandleTypeDef *hdma)
{
  (void)hdma;
  /* 出错后 DMA 已停，重新拉起来，尽量不丢帧；具体错误码可看 hdma_usart3_rx.ErrorCode */
  (void)HAL_DMAEx_MultiBufferStart_IT(&hdma_usart3_rx,
                                      (uint32_t)&USART3->DR,
                                      (uint32_t)&dma_buffer[0],
                                      (uint32_t)&dma_buffer[ROW_BUFFER_SIZE],
                                      ROW_BUFFER_SIZE);
  ATOMIC_SET_BIT(huart3.Instance->CR3, USART_CR3_DMAR);
}

/**
  * @brief  主循环任务：把 DMA 已经收好的数据搬到 row_buffer
  * @note   请放在 while(1) 里反复调用。它会检查 dma_event，
  *         只有 DMA 真的搬完一段才做拷贝，平时几乎零开销。
  */
void USART3_DMA_BufferTask(void)
{
  uint8_t event = dma_event;

  if (event == 0U)
  {
    return;                     /* 没有新数据，直接返回 */
  }

  /* DMA 硬件双缓冲下，CT 位指示当前 DMA 正在写哪一段。
     CT == 0 -> 正在写 Memory0(前半段)，说明后半段是安全可读的
     CT == 1 -> 正在写 Memory1(后半段)，说明前半段是安全可读的 */
  if ((hdma_usart3_rx.Instance->CR & DMA_SxCR_CT) == 0U)
  {
    /* 后半段安全：搬运后半段 -> row_buffer 的后半部分 */
    if ((event & DMA_EVENT_FULL) != 0U)
    {
      memcpy(&row_buffer[0], &dma_buffer[ROW_BUFFER_SIZE], ROW_BUFFER_SIZE);
      dma_event &= (uint8_t)(~DMA_EVENT_FULL);
    }
  }
  else
  {
    /* 前半段安全：搬运前半段 -> row_buffer 的前半部分 */
    if ((event & DMA_EVENT_HALF) != 0U)
    {
      memcpy(&row_buffer[0], &dma_buffer[0], ROW_BUFFER_SIZE);
      dma_event &= (uint8_t)(~DMA_EVENT_HALF);
    }
  }

  /* 两段都处理完了，把"正在被 DMA 写"的标记撤掉 */
  if (dma_event == 0U)
  {
    half_busy_flag = 0;
  }
}

/* USER CODE END 1 */
