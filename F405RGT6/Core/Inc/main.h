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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "pin_configure.h"
#include "bsp_delay.h"
#include "laser.h"
#include "step_motor.h"
#include "limit.h"
#include "image.h"
#include "stdlib.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
//雕刻速度：单位mm/s
#define SPEED 60
//步进电机的细分
#define STEP_DIVIDE 16
//buffer缓冲区大小
#define ROW_BUFFER_SIZE 4096

/* ===================== ★ 扫描中断频率(唯一旋钮) =====================
   由 TIM2 的自动重装值决定(定时器时钟 84MHz):
       中断频率 = 84e6 / (RASTER_TIM2_PERIOD + 1)
       一个"点" = STEP_DIVIDE x 2 次 STEP 翻转

       PERIOD 2624 -> 32.0 kHz -> 302 点/s -> 30 mm/s   (原值)
       PERIOD 5249 -> 16.0 kHz -> 151 点/s -> 15 mm/s   <-- 当前: 用户要求频率减半
       PERIOD 10499 ->  8.0 kHz ->  75 点/s ->  7.5 mm/s

   ★ 中断频率减半 = 每个点的时间翻倍 -> **扫描速度也减半**(30 -> 15mm/s),
     作业时长翻倍。要"只降中断频率不降速度"是做不到的:
       点率 = 中断频率 / (STEP_DIVIDE x 2), 而 STEP_DIVIDE 同时决定点距。
   ★ 为什么在这里设而不是改 tim.c: tim.c 是 CubeMX 生成的, 重新生成就被覆盖;
     放在 main.h + main.c 的 USER CODE 区里就一直是我们的。
   ⚠ 顺带: 点周期变长(3312 -> 6625us), 激光脉宽的上限也必须小于它
     (脉宽 = 灰度 x LASER_POWER, 见 main.c; 6625us 下 LASER_POWER 别超过 20)。 */
#define RASTER_TIM2_PERIOD      2625U

/* ===================== 蓝牙串口 DMA 双缓冲配置 ===================== */
/* DMA 缓冲区(第二个缓冲区)：DMA 把 USART3 收到的灰度值直接搬到这里，CPU 不参与 */
#define DMA_BUFFER_SIZE (ROW_BUFFER_SIZE * 2)
/* DMA 搬运完成事件：由 DMA 中断置位，主循环消费，用于把数据搬进 row_buffer */
#define DMA_EVENT_HALF  0x01U   /* DMA 搬完了 dma_buffer 的前半段 */
#define DMA_EVENT_FULL  0x02U   /* DMA 搬完了 dma_buffer 的后半段 */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

typedef struct conformation{
   uint32_t all_point;
   uint32_t cur_point;
   uint8_t add_flag;
   uint8_t lock;
   uint8_t divide_step;
}conformation;

   extern conformation x, y; 
   extern uint8_t  row_buffer[ROW_BUFFER_SIZE];
   extern uint32_t read_cusor;         //图像灰度值的读游标
   extern uint32_t write_cusor;        //图像灰度值的写游标
   extern int      cur_count;
   extern int      row_count;          //图像总共有多少列
   extern int      line_count;         //图像总共有多少行
   extern int      row_count_2;        //直接算出GPIO需要翻转的次数
   extern uint8_t  step_count;
   extern int      line_couant_2;
   extern uint8_t  row_add;            //被置1了就代表已经到达下一个像素点了
   extern uint8_t  line_add;           //被置1了就是要换行了

  /* ---------------- DMA 双缓冲区相关变量 ---------------- */
  /* 第二个缓冲区：DMA(USART3_RX) 的目的地。
     长度是 row_buffer 的 2 倍，前半段和后半段轮流被 DMA 填充，
     每次填满一段就产生一次"过半/全满"事件通知 CPU 去搬运。*/
  extern uint8_t dma_buffer[DMA_BUFFER_SIZE];
  /* DMA 事件标志：0=空闲, DMA_EVENT_HALF=前半段好了, DMA_EVENT_FULL=后半段好了。
     中断里只置位，主循环里清除并搬运，避免在中断里做 4096 字节的长拷贝。*/
  extern volatile uint8_t dma_event;
  /* 保护 dma_buffer 后半段：DMA 正在写它时，主循环不要去读它 */
  extern volatile uint8_t half_busy_flag;
  /* 累计收到的 DMA 完整缓冲块数，便于观察蓝牙数据是否在持续进来 */
  extern volatile uint32_t dma_rx_block_count;
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
