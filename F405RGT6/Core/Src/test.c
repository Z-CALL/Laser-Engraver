#include "test.h"
#include "tf_card.h"   /* TF_Init / TF_ReadBlocks / g_tf ... */
#include <string.h>    /* memcmp / memcpy */

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
 * ★ 为什么不用驱动自带的 TF_Test()：
 *   它的吞吐测试把 256 块 = 128KB 读进一个只有 512 字节的缓冲区
 *   （tf_card.c 里 s_tfBuf[TF_BLOCK_SIZE]），越界 128KB，会把 SRAM1 整片
 *   踩掉 —— OLED 显存和栈都在里面，表现就是"每隔一会儿花屏一次"。
 *   所以这里自己按正确尺寸做：单块用 512B，吞吐按 8 块(4KB)一次、分 32 次读。
 *
 * 检查项：
 *   1 init  TF_Init() 识别卡（含速度协商）
 *   2 mbr   读 0 号扇区，查 MBR 签名 55AA、取第一个分区起始 LBA
 *   3 fs    读该分区引导扇区，认 FAT32/exFAT/NTFS
 *   4 rep   0 号扇区重复读 4 次比对（抓偶发 CRC 错）
 *   5 spd   连读 128KB，用 DWT 计时算吞吐
 *
 * ★ 全程只读，绝不写卡；卡里不需要放任何文件，空卡也能跑。
 * ★ 结尾 while(1)：每 3 秒翻一页，一直循环：
 *     P0 结论    P1 检查项    P2 吞吐 / 卡信息 / 原始诊断
 *   本函数不返回，只能当独立调试模式用。
 *
 * ★ 要调用它：main.c 的 USER CODE BEGIN 2 里加一行 test_tf_card();
 *   并在 test.h 里补一句原型声明 void test_tf_card(void);
 * ========================================================================== */

/* ---- 测试用静态缓冲（不占栈）。4KB 那个给吞吐测试用 ---- */
static uint8_t t_blk[TF_BLOCK_SIZE]      __attribute__((aligned(4)));
static uint8_t t_ref[TF_BLOCK_SIZE]      __attribute__((aligned(4)));
static uint8_t t_big[TF_BLOCK_SIZE * 8U] __attribute__((aligned(4)));

/* ---- 结果暂存（给分页显示用）---- */
static uint8_t    s_fail;        /* 失败项个数 */
static uint8_t    s_fail_item;   /* 第一个失败的检查项编号，0 = 全过 */
static TF_Status  s_fail_st;     /* 第一个失败项返回的状态 */
static const char *s_init_txt;
static const char *s_mbr_txt;
static const char *s_fs_txt;
static const char *s_rep_txt;
static const char *s_type_txt;
static const char *s_bus_txt;
static const char *s_dma_txt;
static const char *s_hs_txt;
static uint32_t   s_kbps;
static uint32_t   s_tp_us;
static uint32_t   s_cap_mb;
static uint32_t   s_blk_cnt;
static uint32_t   s_clk_mhz;
static uint32_t   s_err_step;    /* 原始诊断：卡在哪一步 */
static uint32_t   s_err_sta;     /* 原始诊断：出错瞬间 SDIO->STA */
static uint32_t   s_err_resp;    /* 原始诊断：出错那条命令的 RESP1 */
static uint32_t   s_err_line;    /* 原始诊断：出错所在的源文件行号 */

/* ---- OLED 显存自检 ----
   用来判断"花屏"到底是谁干的：
     画完一页立刻算一次校验和当基准，静止 2.5 秒后再算一次；
     两次不一致 = 有东西在往显存里写（真被踩了）；
     一直一致但屏幕还是花 = 问题在 I2C / 屏那边，不是 RAM。 */
static uint32_t   s_oled_sum;    /* 画完时的校验和（基准） */
static uint32_t   s_oled_now;    /* 静止后再算一次的校验和 */
static uint16_t   s_oled_bad;    /* 校验和不一致的次数（显存被改） */
static uint16_t   s_oled_i2c_bad;/* OLED_Refresh() 传输失败的次数 */
static uint8_t    s_oled_wait;   /* 1 = 有一页等着校验 */

/* DWT 周期计数器（Cortex-M4 内核自带，不占外设） */
#define T_DWT_DEMCR  (*(volatile uint32_t *)0xE000EDFCUL)
#define T_DWT_CTRL   (*(volatile uint32_t *)0xE0001000UL)
#define T_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)

/* 画字的小宏，省得到处写坐标换算。
   T8 = 8x16 字体（一屏 4 行，16 字符/行）
   T6 = 6x8  字体（一屏 8 行，21 字符/行） */
#define T8(row, x, s)      OLED_ShowString((x), (uint8_t)((row) * 16U), (s), OLED_FONT_08, 1U)
#define T8U(row, x, v, n)  OLED_ShowUNum((x), (uint8_t)((row) * 16U), (v), (n), OLED_FONT_08, 1U)
#define T8X(row, x, v, n)  OLED_ShowHexNum((x), (uint8_t)((row) * 16U), (v), (n), OLED_FONT_08, 1U)
#define T6(row, x, s)      OLED_ShowString((x), (uint8_t)((row) * 8U), (s), OLED_FONT_06, 1U)
#define T6U(row, x, v, n)  OLED_ShowUNum((x), (uint8_t)((row) * 8U), (v), (n), OLED_FONT_06, 1U)
#define T6X(row, x, v, n)  OLED_ShowHexNum((x), (uint8_t)((row) * 8U), (v), (n), OLED_FONT_06, 1U)

/** 短版状态名（OLED 一行放不下 TF_StatusStr 的长串） */
static const char *t_st_short(TF_Status st)
{
	switch (st)
	{
	case TF_OK:                 return "OK";
	case TF_ERR_CMD_TIMEOUT:    return "cTO";
	case TF_ERR_CMD_CRC:        return "cCRC";
	case TF_ERR_CMD_RESP:       return "cRSP";
	case TF_ERR_DATA_TIMEOUT:   return "dTO";
	case TF_ERR_DATA_CRC:       return "dCRC";
	case TF_ERR_DATA_FIFO:      return "fifo";
	case TF_ERR_V2_FAIL:        return "v1";
	case TF_ERR_VOLTAGE:        return "volt";
	case TF_ERR_NOT_SD:         return "notSD";
	case TF_ERR_ADDR_RANGE:     return "range";
	case TF_ERR_BUS_WIDTH:      return "bus";
	case TF_ERR_UNSUPPORTED_HS: return "noHS";
	default:                    return "?";
	}
}

/** 记一次失败，并把当时的原始诊断值抓下来（第一个失败项最有用） */
static void t_fail(uint8_t item, TF_Status st)
{
	if (s_fail_item == 0U)
	{
		s_fail_item = item;
		s_fail_st   = st;
		s_err_step  = g_tfStep;
		s_err_sta   = g_tfLastSta;
		s_err_resp  = g_tfLastResp;
		s_err_line  = g_tfErrLine;
	}
	++s_fail;
}

static void t_dwt_init(void)
{
	T_DWT_DEMCR  |= (1UL << 24);        /* TRCENA */
	T_DWT_CYCCNT  = 0U;
	T_DWT_CTRL   |= 1UL;                /* CYCCNTENA */
}

static uint32_t t_dwt_us(uint32_t start)
{
	/* 168MHz 下 CYCCNT 约 25.6s 回绕一次，单次测试远短于此 */
	return (T_DWT_CYCCNT - start) / (SystemCoreClock / 1000000U);
}

/** 初始化进度（g_tfStep）的可读名字。失败时看这个比看数字直观得多 */
static const char *t_step_name(uint32_t s)
{
	switch (s)
	{
	case TF_STEP_NONE:     return "-";
	case TF_STEP_GPIO:     return "gpio";
	case TF_STEP_POWER:    return "power";
	case TF_STEP_CLK_INIT: return "clk init";
	case TF_STEP_CMD0:     return "cmd0";
	case TF_STEP_CMD8:     return "cmd8";
	case TF_STEP_ACMD41:   return "acmd41";
	case TF_STEP_CMD2:     return "cmd2";
	case TF_STEP_CMD3:     return "cmd3";
	case TF_STEP_CMD9:     return "cmd9";
	case TF_STEP_CMD7:     return "cmd7";
	case TF_STEP_CMD16:    return "cmd16";
	case TF_STEP_ACMD6:    return "acmd6";
	case TF_STEP_CLK_RUN:  return "clk run";
	case TF_STEP_READY:    return "ready";
	case TF_STEP_READ:     return "read";
	case TF_STEP_WRITE:    return "write";
	case TF_STEP_SPEED:    return "speed";
	default:               return "?";
	}
}

/** 对 OLED 显存采样算校验和。
 *  OLED_GetPoint() 读的是本地显存（不走 I2C），512 个采样点很快。 */
static uint32_t t_oled_sum(void)
{
	uint32_t s = 0U;
	uint8_t  x, y;

	for (y = 0U; y < OLED_HEIGHT; y += 4U)
	{
		for (x = 0U; x < OLED_WIDTH; x += 4U)
		{
			s = s * 33U + OLED_GetPoint(x, y);
		}
	}
	return s;
}

/** 认文件系统（看分区引导扇区里的标志串） */
static const char *t_fs_name(const uint8_t *bs)
{
	if (memcmp(bs + 3U, "EXFAT   ", 8U) == 0)  { return "exFAT"; }
	if (memcmp(bs + 3U, "NTFS    ", 8U) == 0)  { return "NTFS"; }
	if (memcmp(bs + 0x52U, "FAT32", 5U) == 0)  { return "FAT32"; }
	if (memcmp(bs + 0x36U, "FAT", 3U) == 0)    { return "FAT16/12"; }
	return "unknown";
}

/** 画一页到 OLED。
 *  ★ 所有页都用同一套版式（8x16 字体、4 行、行高 16），这样翻页时
 *    新旧内容在同一位置互相替换，不会出现"整屏重排"那种花屏感。 */
static void t_page(uint8_t p)
{
	OLED_Clear();

	if (p == 0U)
	{
		/* ---------------- P0 结论 ---------------- */
		if (s_fail == 0U)
		{
			T8(0, 0, "RESULT: PASS");
			T8(1, 0, "cap");
			T8U(1, 32, s_cap_mb, 6U);
			T8(1, 88, "MB");
			T8(2, 0, s_bus_txt);
			T8U(2, 40, s_clk_mhz, 2U);
			T8(2, 56, "MHz");
			T8(3, 0, s_dma_txt);
			T8(3, 48, s_hs_txt);
		}
		else
		{
			T8(0, 0, "RESULT: FAIL");
			T8(1, 0, "fail");
			T8U(1, 40, s_fail, 2U);
			T8(1, 64, "item");
			T8U(1, 104, s_fail_item, 2U);
			T8(2, 0, t_st_short(s_fail_st));
			T8(2, 48, "step");
			T8U(2, 88, s_err_step, 2U);
			/* 卡在哪一步，直接用文字写出来 —— 这一行最有用 */
			T8(3, 0, t_step_name(s_err_step));
		}
	}
	else if (p == 1U)
	{
		/* ---------------- P1 各项检查的结果 ---------------- */
		T8(0, 0, "1 init"); T8(0, 64, s_init_txt);
		T8(1, 0, "2 mbr "); T8(1, 64, s_mbr_txt);
		T8(2, 0, "3 fs  "); T8(2, 64, s_fs_txt);
		T8(3, 0, "4 rep "); T8(3, 64, s_rep_txt);
	}
	else if (p == 2U)
	{
		/* ---------------- P2 吞吐 / 卡信息 ---------------- */
		T8(0, 0, "spd");   T8U(0, 32, s_kbps, 5U);  T8(0, 72, "KB/s");
		T8(1, 0, "128KB"); T8U(1, 48, s_tp_us, 7U); T8(1, 104, "us");
		T8(2, 0, "type");  T8(2, 40, s_type_txt);
		T8(3, 0, "blk");   T8U(3, 32, s_blk_cnt, 10U);
	}
	else if (p == 3U)
	{
		/* ---------------- P3 总线 / 原始诊断 ---------------- */
		T8(0, 0, "sta");  T8X(0, 32, s_err_sta, 8U);
		T8(1, 0, "bus");  T8(1, 32, s_bus_txt);
		T8(1, 72, s_dma_txt);
		T8(1, 104, s_hs_txt);
		T8(2, 0, "line"); T8U(2, 40, s_err_line, 4U);
		T8(3, 0, "resp"); T8X(3, 40, s_err_resp, 8U);
	}
	else
	{
		/* ---------------- P4 OLED 自检 ----------------
		   ram bad 一直是 0 -> 显存没被人写，屏幕还花就是 I2C / 屏那边的事；
		   ram bad 往上涨   -> 有东西在往显存里写（真被踩了）；
		   i2c bad 往上涨   -> OLED_Refresh() 的 I2C 传输在失败（线/上拉/干扰）。 */
		T8(0, 0, "OLED RAM");
		T8(1, 0, "ram bad"); T8U(1, 64, s_oled_bad, 4U);
		T8(2, 0, "i2c bad"); T8U(2, 64, s_oled_i2c_bad, 4U);
		T8(3, 0, "sum");     T8X(3, 32, s_oled_sum, 8U);
	}

	if (OLED_Refresh() != HAL_OK)
	{
		++s_oled_i2c_bad;
	}
}

void test_tf_card(void)
{
	TF_Status st;
	uint32_t  t0;
	uint32_t  partLba;
	uint8_t   i;

	s_fail      = 0U;
	s_fail_item = 0U;
	s_fail_st   = TF_OK;
	s_init_txt  = "-";
	s_mbr_txt   = "-";
	s_fs_txt    = "-";
	s_rep_txt   = "-";
	s_type_txt  = "-";
	s_bus_txt   = "-";
	s_dma_txt   = "-";
	s_hs_txt    = "-";
	s_kbps      = 0U;
	s_tp_us     = 0U;
	s_cap_mb    = 0U;
	s_blk_cnt   = 0U;
	s_clk_mhz   = 0U;
	s_err_step  = 0U;
	s_err_sta   = 0U;
	s_err_resp  = 0U;
	s_err_line  = 0U;
	partLba     = 0U;

	s_oled_sum     = 0U;
	s_oled_now     = 0U;
	s_oled_bad     = 0U;
	s_oled_i2c_bad = 0U;
	s_oled_wait    = 0U;

	/* ---- 开机提示：识别 + 测试要跑几百毫秒到几秒，先给个反馈 ---- */
	OLED_Clear();
	OLED_ShowString(0U,  0U, "TF CARD TEST", OLED_FONT_08, 1U);
	OLED_ShowString(0U, 16U, "read only",    OLED_FONT_08, 1U);
	OLED_ShowString(0U, 32U, "please wait",  OLED_FONT_06, 1U);
	OLED_Refresh();

	t_dwt_init();

	/* ================= [1] 初始化（含速度协商） ================= */
	st = TF_Init();
	if (st != TF_OK)
	{
		s_init_txt = t_st_short(st);
		t_fail(1U, st);
		goto show;              /* 卡都没认出来，后面几项没意义 */
	}
	s_init_txt = "OK";

	s_type_txt = (g_tf.cardType == TF_CARD_SDHC) ? "SDHC/SDXC"
	           : (g_tf.cardType == TF_CARD_SDSC) ? "SDSC" : "unknown";
	s_cap_mb   = g_tf.capacityKB / 1024U;
	s_blk_cnt  = g_tf.blockCount;
	s_bus_txt  = g_tf.bus4bit ? "4bit" : "1bit";
	s_dma_txt  = g_tf.dmaUsed ? "DMA" : "CPU";
	s_hs_txt   = g_tf.highSpeedMode ? "HS" : "--";
	s_clk_mhz  = g_tf.sdioClkHz / 1000000U;

	/* ================= [2] 读 0 号扇区，查 MBR ================= */
	st = TF_ReadBlockRetry(0U, t_blk, 3U);
	if (st != TF_OK)
	{
		s_mbr_txt = t_st_short(st);
		t_fail(2U, st);
		goto show;              /* 连 0 号扇区都读不出来，后面别跑了 */
	}

	partLba = ((uint32_t)t_blk[446U + 8U])
	        | ((uint32_t)t_blk[446U + 9U]  << 8U)
	        | ((uint32_t)t_blk[446U + 10U] << 16U)
	        | ((uint32_t)t_blk[446U + 11U] << 24U);

	/* 没有 55AA 签名不算失败（裸卡/空卡也这样），读取本身成功就算通 */
	s_mbr_txt = ((t_blk[510] == 0x55U) && (t_blk[511] == 0xAAU)) ? "55AA" : "noMBR";

	/* ================= [3] 认文件系统 ================= */
	if ((partLba == 0U) || (partLba >= g_tf.blockCount))
	{
		s_fs_txt = "skip";
	}
	else
	{
		st = TF_ReadBlockRetry(partLba, t_ref, 3U);
		if (st != TF_OK)
		{
			s_fs_txt = t_st_short(st);
			t_fail(3U, st);
		}
		else
		{
			s_fs_txt = t_fs_name(t_ref);
		}
	}

	/* ================= [4] 重复读一致性 ================= */
	st = TF_ReadBlockRetry(0U, t_blk, 3U);
	if (st != TF_OK)
	{
		s_rep_txt = t_st_short(st);
		t_fail(4U, st);
	}
	else
	{
		for (i = 0U; i < 3U; ++i)
		{
			if (TF_ReadBlockRetry(0U, t_ref, 3U) != TF_OK) { break; }
			if (memcmp(t_blk, t_ref, TF_BLOCK_SIZE) != 0)  { break; }
		}
		if (i == 3U)
		{
			s_rep_txt = "OK";
		}
		else
		{
			s_rep_txt = "BAD";
			t_fail(4U, TF_ERR_DATA_CRC);    /* 内容对不上 = 数据不可信 */
		}
	}

	/* ================= [5] 吞吐：连读 128KB =================
	   8 块 x 512B = 4KB 一次（正好装进 t_big），读 32 次 = 128KB。
	   一次读多块能摊掉命令开销，比逐块读更接近真实吞吐。 */
	t0 = T_DWT_CYCCNT;
	st = TF_OK;
	for (i = 0U; i < 32U; ++i)
	{
		st = TF_ReadBlocks(0U, t_big, 8U);
		if (st != TF_OK) { break; }         /* 失败立刻停，别耗时间 */
	}
	s_tp_us = t_dwt_us(t0);

	if (st != TF_OK)
	{
		t_fail(5U, st);
	}
	else if (s_tp_us > 0U)
	{
		/* 32 次 x 8 块 x 512B = 131072 字节； KB/s = 字节 x 1000 / 微秒 */
		s_kbps = (32U * 8U * TF_BLOCK_SIZE * 1000U) / s_tp_us;
	}

show:
	/* 全部通过时也抓一份原始诊断值 —— 这是驱动"最后一次见到的错"，
	   多半是速度协商时试高档失败留下的（已被降档处理，不是故障）。 */
	if (s_fail == 0U)
	{
		s_err_step = g_tfStep;
		s_err_sta  = g_tfLastSta;
		s_err_resp = g_tfLastResp;
		s_err_line = g_tfErrLine;
	}

	/* ================= 分页轮播，死循环 ================= */
	while (1)
	{
		uint8_t p;

		for (p = 0U; p < 6U; ++p)
		{
			/* 上一页已经静止了 2.5 秒，先验一下显存有没有被谁踩过 */
			if (s_oled_wait != 0U)
			{
				s_oled_now = t_oled_sum();
				if (s_oled_now != s_oled_sum)
				{
					++s_oled_bad;
				}
				s_oled_wait = 0U;
			}

			t_page(p);

			s_oled_sum  = t_oled_sum();     /* 画完立刻存一份基准 */
			s_oled_wait = 1U;

			BSP_DelayMs(2500U);
		}
	}
}

