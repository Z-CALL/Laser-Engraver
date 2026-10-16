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
#include "test.h"
#include "tf_card.h"
#include "tf_image.h"   /* ★ TF 卡分支用：读卡上的 IMG.BIN + 环形缓冲 */
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

uint32_t read_cusor = 0;
uint32_t write_cusor = 0;
conformation x, y;
volatile state sys_state;
uint8_t* alarm;

uint8_t ctrl_req[3] = {0x55, 0x44, 0xAA};   			 /* 换成你的 3 字节内容 */
uint8_t req_pending = 0;                          	/* 1 = 已发出，等 67 字节回包 */
uint32_t req_sent_ms = 0;
volatile uint8_t need_send_ctl = 0;   				      /* 1 = 该发控制信号了 */
uint8_t RX_number = 0;
uint8_t RX_data[8][64] = {0};
uint8_t RX_Ctl[8];
uint32_t RX_write_cusor = 0;
uint32_t work_cusor = 0;
uint32_t work_cusor_point = 0;
volatile uint8_t work_cusor_flag = 0;

uint32_t x_work_orgin = 0;
uint32_t y_work_orgin = 0;

uint8_t DataBack[3] = {0x33, 0x00, 0xEE};
uint8_t UART_Idle = 0;

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
  
  laser_close();
  
  //test_tf_card(); 
  
  //test_laser_PWM_fre_limit_24V();
  homing();
  
  
  //test_homing();
//  test_go_to_point();
//  test_laser_PWM_fre_limit_24V();
//  test_delay();

/* ★★ 数据源开关：宏定义在 configure.h 里。
       0 = 蓝牙(BT)下发   1 = TF 卡 IMG.BIN
   ⚠ 这里的宏名【必须】和 configure.h 里的一字不差 ——
     名字写错时，未定义的宏在 #if 里当 0，会【静默】走蓝牙那一臂，
     现象是"电机不动 / 一直等上位机"，非常难查。
     configure.h 里改过名字的话，这里也要跟着改。 */
#ifndef BT_or_TFCard
#error "configure.h has no BT_or_TFCard - macro renamed? fix the name here too"
#endif
#if BT_or_TFCard

	/* ====================================================================
	 * TF 卡分支 —— 数据源从「蓝牙串口」换成「卡上的 IMG.BIN」
	 *
	 *   · 灰度数据：TFIMG_*（tf_image.c，32KB 环形缓冲，边读卡边消费）
	 *   · 行/列点数：直接取自图像头（x=宽, y=高），不再等上位机下发
	 *   · 回显：沿用原来的 DataBack（0x33 灰度 0xEE），一行没改
	 *   · 雕刻循环与下面 RT 分支同构，只有「取数」那一处不同
	 *
	 *   ★ 卡上放「导出TF卡图像(.bin)」生成的 IMG.BIN（8.3 短名，大写）。
	 *     数据本身就是雕刻顺序（弓字形/换行上位机已处理），所以这里
	 *     按顺序取字节即可，行尾不需要像 RT 那样跳包。
	 * ==================================================================== */

	if(TFIMG_Open(0) != TF_OK){        /* 0 = 用默认名 TFIMG_FILE_NAME(IMG.BIN) */
		laser_close();
		OLED_Clear();
		char tf_err[] = {"TF IMG OPEN FAIL"};
		OLED_ShowString(0, 0, tf_err, OLED_FONT_08, 1);
		OLED_ShowString(0, 16, TFIMG_ErrStr(), OLED_FONT_08, 1);
		OLED_Refresh();
		while(1);
	}

	/* 点数取自图像头：x = 宽（一行点数），y = 高（行数） */
	x.all_point = TFIMG_Width();
	y.all_point = TFIMG_Height();

	/* ★ 开雕前先把"读到的图像信息"显示清楚 —— 第一次跑就靠这几行判断
	   "卡上文件读到没有"。没有 OLED_Clear() 的话会叠在 homing 的字上，看不清。
	   （行数/每行点数不对 = 文件不对或读错了，别急着让它动） */
	OLED_Clear();
	OLED_ShowString(0,  0, "TF IMG OK", OLED_FONT_08, 1);
	OLED_ShowString(0, 16, "rows", OLED_FONT_08, 1);
	OLED_ShowNum(  40, 16, y.all_point, OLED_FONT_08, 1);
	OLED_ShowString(0, 32, "cols", OLED_FONT_08, 1);
	OLED_ShowNum(  40, 32, x.all_point, OLED_FONT_08, 1);
	OLED_Refresh();

	goto_point(x_work_orgin,y_work_orgin);

	/* ★ 加工前的额外定位：先回作业原点，再移到 (X=500, Y=10)，
	   然后从这里开始雕。
	   ★ 必须放在 HAL_TIM_Base_Start_IT / sys_state = working 【之前】——
	     goto_point() 自己一步步走电机，定时器一旦跑起来中断也在走电机，
	     两边会打架。
	   ★ 想直接去 (500,10)、不回原点，就把上面那句删掉；
	     想改目标点就改这两个数（单位 = 像素点，和作业原点同一套）。 */
	goto_point(500+x.all_point, 240+350 );
    HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);

	x.lock        = 0;
	y.lock        = 0;
	x.divide_step = 0;
	y.divide_step = 0;
	x.add_flag = 0;
	y.add_flag = 0;
	x.cur_point = 0;
	y.cur_point = 0;
	work_cusor = 0;
	work_cusor_point = 0;
	work_cusor_flag = 0;

	/* ★ 先把环形缓冲垫满，再让机器动。
	   Fill() 要读满 32KB（几十毫秒）；如果放在 sys_state = working 之后，
	   这段时间电机会先走、x.add_flag 会攒一堆，循环一起来就把开头的点
	   全挤在一起出光（工件上能看到起步一段糊掉）。
	   放在电机启动之前，读卡时间就完全不影响雕刻。 */
	TFIMG_Fill();

	htim2.Init.Period    = RASTER_TIM2_PERIOD;
	htim2.Init.Prescaler = 0U;
	(void)HAL_TIM_Base_Init(&htim2);
	__HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

	HAL_TIM_Base_Start_IT(&htim2);

	sys_state = working;

	while (1)
	{
		/* 卡 -> 环形缓冲：能塞多少塞多少；缓冲满时立刻返回，不阻塞 */
		TFIMG_Fill();

		if(y.cur_point >= y.all_point){
			HAL_TIM_Base_Stop_IT(&htim2);
			laser_close();
			OLED_Clear();
			char program_end[] = {"program end!"};
			OLED_ShowString(0, 0, program_end, OLED_FONT_08, 1);
			OLED_ShowNum(0,16,y.cur_point,OLED_FONT_08, 1);
			OLED_ShowNum(0,32,x.cur_point,OLED_FONT_08, 1);
			OLED_Refresh();
			while(1);
		}

		if(x.add_flag){
			uint8_t tf_gray = 0;

			x.add_flag = 0;

			/* ★ 与 RT 分支唯一的区别：这一字节来自 TF 卡的环形缓冲。
			   取不到 = 卡跟不上/数据读完 —— 先停激光再报错，
			   绝不能让激光悬在半空。 */
			if(TFIMG_GetByte(&tf_gray) == 0){
				HAL_TIM_Base_Stop_IT(&htim2);
				laser_close();
				OLED_Clear();
				char tf_empty[] = {"TF UNDERFLOW!!"};
				OLED_ShowString(0, 0, tf_empty, OLED_FONT_08, 1);
				OLED_ShowNum(0,16,x.cur_point,OLED_FONT_08, 1);
				OLED_Refresh();
				while(1);
			}

			laser_open();

#if TIME_DELAY

			alarm = Creat_alarm(tf_gray * LASER_POWER);
			if(alarm == NULL){
				HAL_TIM_Base_Stop_IT(&htim2);
				OLED_Clear();
				char program_error[] = {"program error!!!"};
				OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
				char program_error_1[] = {"delay_error!!!"};
				OLED_ShowString(0, 16, program_error_1, OLED_FONT_08, 1);
				OLED_Refresh();
				while(1);
			}

#else

			set_delay_time(tf_gray * LASER_POWER);

#endif

			DataBack[1] = tf_gray;
			if(UART_Idle == 0){
				USART3_SendDMA(DataBack, 3);
			}

			work_cusor_point++;
			if(work_cusor_point == 64 || work_cusor_flag == 1){
				/* 行尾不需要像 RT 那样跳包（数据已是雕刻顺序），只清标志 */
				work_cusor_flag = 0;
				work_cusor++;
				work_cusor_point = 0;
			}
		}

		delay_till();

#if TIME_DELAY

		if((alarm != NULL) && (*alarm)){
			laser_close();
			*alarm = 0;
			free(alarm);
			alarm = NULL;
		}

#endif
	}
}
	
	
	
#else

	if(BT_Send_Ctrl() != 1){
		for(int i = 0; i < 8; i++){
			memset(RX_data[i], 0, 64);
		}
		char program_error[] = {"BT Try again..."};
		OLED_ShowString(0, 16, program_error, OLED_FONT_08, 1);
		OLED_Refresh();
		if(BT_Send_Ctrl() != 1){
            char program_error[] = {"USART error!!!"};
            OLED_ShowString(0, 32, program_error, OLED_FONT_08, 1);
            OLED_Refresh();
			while(1);
		}
	}
	
	OLED_ShowNum(0,16,y.all_point,OLED_FONT_08, 1);
	OLED_ShowNum(0,32,x.all_point,OLED_FONT_08, 1);
    OLED_Refresh();
	waiting();
	USART3_RxStart();
	goto_point(x_work_orgin,y_work_orgin);
    x.lock        = 0;
    y.lock        = 0;
    x.divide_step = 0;
    y.divide_step = 0;
	x.add_flag = 0;
	y.add_flag = 0;
	x.cur_point = 0;
	y.cur_point = 0;
	work_cusor = 0;

	htim2.Init.Period    = RASTER_TIM2_PERIOD;
	htim2.Init.Prescaler = 0U;
	(void)HAL_TIM_Base_Init(&htim2);
	__HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

	HAL_TIM_Base_Start_IT(&htim2);
	
	sys_state = working;
	
	RX_write_cusor = 0;
	
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

      RX_number = USART3_RxPoll();
      if(RX_number == USART3_CMD_BUF_SIZE){
		if(usart3_cmd_buf[0] == 0x55 && usart3_cmd_buf[68] == 0xAA){
			memcpy(RX_data[(RX_write_cusor % 8)], usart3_cmd_buf + 2, 64);
			usart3_cmd_buf[0] = 0x00;
			usart3_cmd_buf[68] = 0x00;
			RX_Ctl[(RX_write_cusor % 8)] = usart3_cmd_buf[1];
			RX_write_cusor++;
		}
	    RX_number = 0;
		USART3_RxStart();
      }
	  
      if(y.cur_point >= y.all_point){
        HAL_TIM_Base_Stop_IT(&htim2);
        laser_close();
		OLED_Clear();
        char program_end[] = {"program end!"};
        OLED_ShowString(0, 0, program_end, OLED_FONT_08, 1);
		OLED_ShowNum(0,16,y.cur_point,OLED_FONT_08, 1);
		OLED_ShowNum(0,32,x.cur_point,OLED_FONT_08, 1);
        OLED_ShowString(0, 0, program_end, OLED_FONT_08, 1);
        OLED_Refresh();
        while(1);
      }

      if(x.add_flag){
        x.add_flag = 0;
        laser_open();
		  
#if TIME_DELAY
		  
        alarm = Creat_alarm(RX_data[work_cusor % 8][work_cusor_point] * LASER_POWER);
        if(alarm == NULL){
            HAL_TIM_Base_Stop_IT(&htim2);
			OLED_Clear();
            char program_error[] = {"program error!!!"};
            OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
            char program_error_1[] = {"delay_error!!!"};
            OLED_ShowString(0, 16, program_error_1, OLED_FONT_08, 1);
            OLED_Refresh();
            while(1);
        }
		  
#else
		  
        set_delay_time(RX_data[(work_cusor % 8)][work_cusor_point] * LASER_POWER);
		  
#endif
		
		DataBack[1] = RX_data[(work_cusor % 8)][work_cusor_point];
		if(UART_Idle == 0){
			USART3_SendDMA(DataBack, 3);
		}
		
		
		work_cusor_point++;
		if(work_cusor_point == 64 || work_cusor_flag == 1){
			work_cusor_flag = 0;
			work_cusor++;
			work_cusor_point = 0;
		}
      }
	  

      delay_till();
	  
#if TIME_DELAY
	  
      if((alarm != NULL) && (*alarm)){
        laser_close();
        *alarm = 0;
        free(alarm);
        alarm = NULL;
      }
	  
#endif
	  
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}



#endif   /* BT_or_TFCard: 0 = BT/Bluetooth, 1 = TF card */

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
