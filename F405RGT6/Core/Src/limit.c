#include "main.h"
#include "bsp_delay.h"

int homing(){
    uint32_t timeout = 0;
    sys_state = homing_1;
    x.lock = 0;
    y.lock = 0;

    //阶段1
    HAL_TIM_Base_Start_IT(&htim2);
    while(1)
    {
        if(HAL_GPIO_ReadPin(x_limit_l_grop,x_limit_l_pin) == x_limit_l_active){
            x.lock = 1;
        }
        if(HAL_GPIO_ReadPin(y_limit_d_grop,y_limit_d_pin) == y_limit_d_active){
            y.lock = 1;
        }
        BSP_DelayUs(10);
        if(x.lock == 1 && y.lock == 1){
            x.lock = 0;
            y.lock = 0;
            break;
        }
        timeout++;
        if(timeout >= HOMEING_TIMEOUT){
            HAL_TIM_Base_Stop_IT(&htim2);
            return -1;
        }
    }
    HAL_TIM_Base_Stop_IT(&htim2);
    BSP_DelayUs(1000);


    //阶段2
    sys_state = homing_2;
    RASTER_TIM2_PERIOD = 3 * RASTER_TIM2_PERIOD;
    __HAL_TIM_SET_AUTORELOAD(&htim2, RASTER_TIM2_PERIOD);
    HAL_GPIO_TogglePin(y_dir_grop, y_dir_pin);
    HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
    HAL_TIM_Base_Start_IT(&htim2);
    while(sys_state != homing_3);
    HAL_TIM_Base_Stop_IT(&htim2);
    BSP_DelayUs(1000);

    //阶段3
    timeout = 0;
    HAL_GPIO_TogglePin(y_dir_grop, y_dir_pin);
    HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
    HAL_TIM_Base_Start_IT(&htim2);
    while(1)
    {
        if(HAL_GPIO_ReadPin(x_limit_l_grop,x_limit_l_pin) == x_limit_l_active){
            x.lock = 1;
        }
        if(HAL_GPIO_ReadPin(y_limit_d_grop,y_limit_d_pin) == y_limit_d_active){
            y.lock = 1;
        }
        if(x.lock == 1 && y.lock == 1){
            x.lock = 0;
            y.lock = 0;
            break;
        }
        BSP_DelayUs(10);
        timeout++;
        if(timeout >= HOMEING_TIMEOUT){
            HAL_TIM_Base_Stop_IT(&htim2);
            BSP_DelayUs(1000);
            RASTER_TIM2_PERIOD = RASTER_TIM2_PERIOD / 3;
            __HAL_TIM_SET_AUTORELOAD(&htim2, RASTER_TIM2_PERIOD);
            return -1;
        }
    }
    HAL_TIM_Base_Stop_IT(&htim2);
    BSP_DelayUs(1000);

    //阶段4
    sys_state = homing_4;
    
    HAL_GPIO_TogglePin(y_dir_grop, y_dir_pin);
    HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
    HAL_TIM_Base_Start_IT(&htim2);
    while(sys_state != idle);
    HAL_TIM_Base_Stop_IT(&htim2);
    BSP_DelayUs(1000);
    RASTER_TIM2_PERIOD = RASTER_TIM2_PERIOD / 3;
    __HAL_TIM_SET_AUTORELOAD(&htim2, RASTER_TIM2_PERIOD);
    x.cur_point = 0;
    y.cur_point = 0;
    return 1;
}

int goto_point(int x_point,int y_point){
	int8_t x_dir = 0;
	int8_t y_dir = 0;
	uint32_t times = 0;
	x.lock = 0;
	y.lock = 0;
	if(x.cur_point == x_point){
		x.lock = 1;
	}
	if(y.cur_point == y_point){
		y.lock = 1;
	}
    if(x.cur_point > x_point){
        HAL_GPIO_WritePin(x_dir_grop, x_dir_pin, GPIO_PIN_RESET);
		x_dir = -1;
    }else{
        HAL_GPIO_WritePin(x_dir_grop, x_dir_pin, GPIO_PIN_SET);
		x_dir = 1;
	}
    if(y.cur_point > y_point){
        HAL_GPIO_WritePin(y_dir_grop, y_dir_pin, GPIO_PIN_RESET);
		y_dir = -1;
    }else{
        HAL_GPIO_WritePin(y_dir_grop, y_dir_pin, GPIO_PIN_SET);
		y_dir = 1;
	}
	
	while(1){
		if(HAL_GPIO_ReadPin(x_limit_r_grop,x_limit_r_pin) == x_limit_r_active){
			return -1;
		}
		if(HAL_GPIO_ReadPin(x_limit_l_grop,x_limit_l_pin) == x_limit_l_active){
			return -1;
		}
		if(HAL_GPIO_ReadPin(y_limit_d_grop,y_limit_d_pin) == y_limit_d_active){
			return -1;
		}
		if(HAL_GPIO_ReadPin(y_limit_u_grop,y_limit_u_pin) == y_limit_u_active){
			return -1;
		}
		if(x.lock == 0){
			HAL_GPIO_TogglePin(x_plus_grop, x_plus_pin);
			if(x.divide_step >= STEP_DIVIDE*6){
				if(x_dir == 1){
					x.cur_point++;
				}
				if(x_dir == -1){
					x.cur_point--;
				}
				x.divide_step = 0;
			}else{
				x.divide_step++;
			}
		}
		if(y.lock == 0){
			HAL_GPIO_TogglePin(y_plus_grop, y_plus_pin);
			if(y.divide_step >= STEP_DIVIDE*6){
				if(y_dir == 1){
					y.cur_point++;
				}
				if(y_dir == -1){
					y.cur_point--;
				}
				x.divide_step = 0;
				y.divide_step = 0;
			}else{
				y.divide_step++;
			}
		}
		times++;
		if(x.cur_point == x_point){
			x.lock = 1;
		}
		if(y.cur_point == y_point){
			y.lock = 1;
		}
		if(x.lock ==1 && y.lock ==1){
			return 1;
		}
		if(times > GOTO_POINT_TIMEOUT){
			return -1;
		}
		BSP_DelayUs(40);
	}
	
}
