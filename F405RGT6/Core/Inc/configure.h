#ifndef CONFIGURE_H
#define CONFIGURE_H


/*引脚配置在最下面*/

/**************************功能参数配置**************************/

//激光强弱，数字越大越强，但是得配合自己得实际情况而定
#define LASER_POWER 4

#if 0
//速度配置，目前通过提升中断频率来提升雕刻速度
#define SPEED 60
#endif

//步进电机的细分
#define STEP_DIVIDE 16

//DMA缓冲区大小
#define ROW_BUFFER_SIZE 4096

//回零超时时间,这里是5s
#define HOMEING_TIMEOUT 5*100000

//去原点超时时间，这里是5s
#define GOTO_POINT_TIMEOUT 5*25000

//定时时间：2625对应32K Hz的中断频率
extern uint16_t RASTER_TIM2_PERIOD;






/**************************引脚配置**************************/
/*
 *  命名规则：
 *  grop表该引脚对应的GPIO组别
 *  pin表该引脚对应的组号
 */

/*
 *  步进电机的引脚配置
 *  dir表方向引脚，plus表脉冲引脚，enable表使能引脚
 *  grop表该引脚对应的GPIO组
 *  pin表该引脚对应的编号
 */
#define x_dir_grop GPIOA
#define x_dir_pin GPIO_PIN_8
#define x_plus_grop GPIOC
#define x_plus_pin GPIO_PIN_9
#define x_enable_grop GPIOC
#define x_enable_pin GPIO_PIN_8

#define y_dir_grop GPIOB
#define y_dir_pin GPIO_PIN_15
#define y_plus_grop GPIOB
#define y_plus_pin GPIO_PIN_14
#define y_enable_grop GPIOB
#define y_enable_pin GPIO_PIN_13

/*  
 *  限位引脚的配置
 *  r、l、u、d分别对应X轴的左右限位、Y轴的上下限位
 *  active分别表示触发限位时限位开关的电平，0 表触发时为低电平，1 表触发时为高电平
 */
#define x_limit_r_grop GPIOC
#define x_limit_r_pin GPIO_PIN_0
#define x_limit_r_active 0
#define x_limit_l_grop GPIOC
#define x_limit_l_pin GPIO_PIN_1
#define x_limit_l_active 0

#define y_limit_d_grop GPIOC
#define y_limit_d_pin GPIO_PIN_3
#define y_limit_d_active 1
#define y_limit_u_grop GPIOC
#define y_limit_u_pin GPIO_PIN_2
#define y_limit_u_active 1


#endif
