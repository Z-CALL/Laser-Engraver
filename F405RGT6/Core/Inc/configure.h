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

//串口接收数据包超时，这里是1S
#define REPLY_TIMEOUT_MS  1000U

//激光使用的定时方式，1代表使用链表定时（可实现多任务定时），0代表使用延时定时（只支持单任务）
#define TIME_DELAY 0






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
#define x_plus_pin GPIO_PIN_5
#define x_enable_grop GPIOC
#define x_enable_pin GPIO_PIN_4

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
#define x_limit_r_pin GPIO_PIN_3
#define x_limit_r_active 0
#define x_limit_l_grop GPIOC
#define x_limit_l_pin GPIO_PIN_2
#define x_limit_l_active 0

#define y_limit_d_grop GPIOC
#define y_limit_d_pin GPIO_PIN_0
#define y_limit_d_active 1
#define y_limit_u_grop GPIOC
#define y_limit_u_pin GPIO_PIN_1
#define y_limit_u_active 1

/*  
 *  激光引脚的的配置
 */
#define Laser_grop GPIOA
#define Laser_pin GPIO_PIN_15


/**************************错误码**************************/
#define BT_INIT_TIMEOUT 0x10				//蓝牙初始化超时


/**************************通信协议（不建议修改）**************************/

//RX_Ctl的控制位
#define RX_Gray_Data 	0x11
#define RX_NewLine		0x22
#define RX_Ctrl			0x33
#define TX_Next			0x44
#define TX_Ctrl			0x55
#define RX_waiting		0x66
#define TX_Ready 		0x77

//收到0x33后的信息位
#define X_ALL_POINT	0x44
#define Y_ALL_POINT	0x55
#define WorkSpeed	0x66

/* 	
 *	偏移	长		内容
 *  0		1		0x55 帧头
 *  1		1		0x33 控制位
 *  2		1		0x44 X 字段标签
 *  3-4		2		X 轴一行像素点（大端）
 *  5		1		0x55 Y 字段标签
 *  6-7		2		Y 轴一列像素点（大端）
 *  8		1		0x66 速度字段标签
 *  9		1		加工速度
 * 	10		1		0x77 X轴作业原点标签
 *	11-12	2		X轴作业原点
 *	13		1		0x88 Y轴作业原点标签
 *	14-15	2		Y轴作业原点
 *  16-65	56		0xFF 填充
 *  66-67	2		CRC16（大端）
 *  68		1		0xAA 帧尾
*/

#endif
