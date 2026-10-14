#include "test.h"
#include "tf_card.h"   /* TF_Test / TF_ShowLog / g_tf ... */

void test_laser_PWM_fre_limit(void)
{

    for(int j = 0 ; j < 5; j++){
        for(int i = 0 ; i < 2000 ; i++){
            laser_open();
            BSP_DelayUs((j+1) * 100);
            laser_close();
            BSP_DelayUs((9-j) * 100);
        }
        BSP_DelayMs(200);
    }

    while(1);
}


void test_laser_PWM_fre_limit_12V(void)
{

    for(int j = 0 ; j < 10; j++){
        for(int i = 0 ; i < 200 ; i++){             //1K PWM
            laser_open();
            BSP_DelayUs(500 - 50*j);
            laser_close();
            BSP_DelayUs(500 + 50 * j);
        }
        BSP_DelayMs(200);
    }

    while(1);
}


void test_laser_PWM_fre_limit_24V(void)
{
	x.all_point = 1000000;
	y.all_point = 1000000;
    HAL_GPIO_TogglePin(x_dir_grop, x_dir_pin);
	sys_state = working;
	HAL_TIM_Base_Start_IT(&htim2);
    for(int j = 0 ; j < 30; j++){
        for(int i = 0 ; i < 50 ; i++){             //1K PWM
            laser_open();
            BSP_DelayUs(300 - 10*j);
            laser_close();
            BSP_DelayUs(700 + 10 * j);
        }
        BSP_DelayMs(50);
    }
    HAL_TIM_Base_Stop_IT(&htim2);
    while(1);
}

void test_delay(){
	BSP_DelayMs(10000);
	OLED_ShowNum(0,0,1,OLED_FONT_16,1);
	OLED_Refresh();
	while(1);
}

void test_homing(){
    homing();
    while(1);
}

void test_go_to_point(){
	goto_point(100,100);
	goto_point(1000,100);
	goto_point(1000,1000);
	goto_point(100,1000);
	goto_point(500,500);
	goto_point(100,100);
	goto_point(1000,100);
	goto_point(1000,1000);
	goto_point(100,1000);
	goto_point(500,500);
	while(1);
}

/* ==========================================================================
 * TF(microSD) 卡测试 —— 结果全部走 OLED
 *
 * 依赖 tf_card.c / tf_card.h（纯寄存器 SDIO 驱动，4 位总线）
 *   CLK=PC12  CMD=PD2  D0~D3=PC8~PC11
 * 调用前必须先 OLED_Init()（main.c 里已经有了）。
 *
 * 流程：
 *   [1] 先刷一屏提示，告诉人"在测了"（识别+吞吐测试要跑几秒）
 *   [2] TF_Test(0) 跑完整自检 —— **只读，绝不写卡**：
 *         识别卡 -> 读 0 号扇区查 MBR 签名 55AA -> 读分区引导扇区认文件系统
 *         -> 同一扇区重复读 4 次比对 -> 连读 256 扇区(128KB) x3 用 DWT 测吞吐
 *       报告会自动攒进驱动内部的日志缓冲，结束时驱动自己会刷一次结论页。
 *   [3] 之后进死循环：先显示一屏"结论速览"（8x16 大字，一眼能看清），
 *       再把完整报告用 6x8 小字体分页轮播（每页 2 秒），一直循环，
 *       方便凑近了慢慢翻，不会一闪而过。
 *
 * ★ 卡里不需要放任何文件，空卡也能跑（驱动是扇区级的）。
 *
 * ★ 想看写测试：把下面的 TF_Test(0U) 改成 TF_Test(1U)。
 *   写测试只动【卡的最后一个扇区】，先备份、写完校验、再把原内容写回，
 *   不会破坏卡上数据。
 *
 * ★ 本函数不返回（结尾 while(1)），只能当独立调试模式用；
 *   要调用它，在 main.c 的 USER CODE BEGIN 2 里加一行
 *   test_tf_card();   并在 test.h 里补一句原型声明。
 * ========================================================================== */
void test_tf_card(void)
{
	uint8_t fail;

	/* ---- [1] 提示页 ---- */
	OLED_Clear();
	OLED_ShowString(0U,  0U, "TF CARD TEST",  OLED_FONT_08, 1U);
	OLED_ShowString(0U, 16U, "read only",     OLED_FONT_08, 1U);
	OLED_ShowString(0U, 32U, "please wait...", OLED_FONT_06, 1U);
	OLED_Refresh();

	/* ---- [2] 完整自检（只读）。内部结束时会自动刷一次结论页 ---- */
	fail = TF_Test(0U);

	/* ---- [3] 结论速览 + 完整报告轮播，死循环 ---- */
	while (1)
	{
		OLED_Clear();
		OLED_ShowString(0U, 0U, (fail == 0U) ? "RESULT: PASS" : "RESULT: FAIL",
		                OLED_FONT_08, 1U);

		if (fail != 0U)
		{
			/* 失败：最该看的三个数是 失败项数 / 卡在哪一步 / SDIO->STA */
			OLED_ShowString(0U, 16U, "fail:", OLED_FONT_08, 1U);
			OLED_ShowUNum(40U, 16U, fail, 2U, OLED_FONT_08, 1U);
			OLED_ShowString(64U, 16U, "step:", OLED_FONT_08, 1U);
			OLED_ShowUNum(104U, 16U, g_tfStep, 2U, OLED_FONT_08, 1U);

			OLED_ShowString(0U, 32U, "sta:", OLED_FONT_08, 1U);
			OLED_ShowHexNum(32U, 32U, g_tfLastSta, 8U, OLED_FONT_08, 1U);

			OLED_ShowString(0U, 48U, "line:", OLED_FONT_08, 1U);
			OLED_ShowUNum(40U, 48U, g_tfErrLine, 4U, OLED_FONT_08, 1U);
		}
		else
		{
			/* 成功：容量 / 位宽 / 实测频率 / 数据通道 / 是否高速模式 */
			OLED_ShowString(0U, 16U, "cap:", OLED_FONT_08, 1U);
			OLED_ShowUNum(32U, 16U, g_tf.capacityKB / 1024U, 5U, OLED_FONT_08, 1U);
			OLED_ShowString(72U, 16U, "MB", OLED_FONT_08, 1U);

			OLED_ShowString(0U, 32U, g_tf.bus4bit ? "4bit" : "1bit",
			                OLED_FONT_08, 1U);
			OLED_ShowUNum(40U, 32U, g_tf.sdioClkHz / 1000000U, 2U,
			              OLED_FONT_08, 1U);
			OLED_ShowString(56U, 32U, "MHz", OLED_FONT_08, 1U);

			OLED_ShowString(0U, 48U, g_tf.dmaUsed ? "DMA" : "CPU",
			                OLED_FONT_08, 1U);
			OLED_ShowString(48U, 48U, g_tf.highSpeedMode ? "HS" : "--",
			                OLED_FONT_08, 1U);
		}
		OLED_Refresh();
		BSP_DelayMs(3000U);

		/* 完整报告分页轮播一遍（阻塞），转完回到循环开头继续 */
		TF_ShowLog(2000U, 1U);
	}
}

