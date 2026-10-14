/**
  ******************************************************************************
  * @file    tf_card.c
  * @brief   TF(microSD) 卡槽驱动 —— 纯寄存器版 SDIO，4 位总线，全轮询
  *
  * 详细说明见 tf_card.h 顶部的注释块。
  *
  * 【实现要点（都是容易踩坑的地方，逐条对应到代码）】
  *
  * 1) 为什么全轮询、不用中断 / DMA
  *    本驱动被要求"只新建 .c/.h、不碰其它文件"。中断服务函数要写在
  *    stm32f4xx_it.c 里，DMA 通道要在 dma.c 里申请，都会破坏这个约束。
  *    轮询模式下 512B/块 @16MHz 只要约 0.3ms，CPU 自己搬 128 个字绰绰有余。
  *
  * 2) 4 位模式的位定义
  *    SDIO_CLKCR.WIDBUS 是【两位】：00=1位, 01=4位, 10=8位。
  *    ★ 4 位只需置低那一位：SDIO_CLKCR_WIDBUS_0。
  *      （写成 WIDBUS_0|WIDBUS_1 就变成 8 位了，会完全不通信。）
  *
  * 3) 时钟
  *    SDIO_CK = SDIOCLK / (CLKDIV + 2)，SDIOCLK = PLL48CLK。
  *    SD 规范要求识别阶段 <=400kHz，所以先用 CLKDIV=118 认卡，
  *    认完再切到 CLKDIV=1（16MHz）。
  *    SDIOCLK 由 RCC->PLLCFGR 在运行时反推，不写死 48MHz。
  *
  * 4) 命令 CRC
  *    SDIO 外设会自动生成命令的 CRC7、数据的 CRC16，无需软件计算。
  *    所以这里只管把命令索引/参数/响应类型写进寄存器。
  *
  * 5) 扇区地址
  *    SDSC（标准容量）CMD17/24 的参数是【字节】地址；
  *    SDHC/SDXC（高容量）是【块】地址。由 g_tf.cardType 区分。
  *
  * 6) 接收 FIFO
  *    用 RXFIFOHF（FIFO 里至少 8 个字）批量搬 8 个 32 位字。
  *    因为 DLEN 恒为 512 的整数倍（32 字的整数倍），这样正好把数据搬完。
  *    按字节拆写进 buf，所以对 buf 的对齐没有任何要求。
  ******************************************************************************
  */

#include "tf_card.h"

#if TF_USE_OLED
#include "oled.h"
#endif

/* ==========================================================================
 * 私有常量（CMSIS 里没有、又必须用的几个）
 * ========================================================================== */

/* DCTRL.DBLOCKSIZE = 9 -> 2^9 = 512 字节。9 = 0b1001，即第 0、3 位 */
#define TF_DBLOCKSIZE_512B      (SDIO_DCTRL_DBLOCKSIZE_0 | SDIO_DCTRL_DBLOCKSIZE_3)

/* 一次清掉所有静态标志（命令响应 / 数据结果 / 超时 / CRC） */
#define TF_STATIC_FLAGS         (SDIO_ICR_CCRCFAILC | SDIO_ICR_DCRCFAILC | \
                                 SDIO_ICR_CTIMEOUTC | SDIO_ICR_DTIMEOUTC | \
                                 SDIO_ICR_TXUNDERRC | SDIO_ICR_RXOVERRC  | \
                                 SDIO_ICR_CMDRENDC  | SDIO_ICR_CMDSENTC  | \
                                 SDIO_ICR_DATAENDC  | SDIO_ICR_DBCKENDC  | \
                                 SDIO_ICR_STBITERRC)

/* 响应类型 */
#define TF_RESP_NONE            0U
#define TF_RESP_SHORT           1U       /* R1/R3/R6/R7，48 位，只要 RESP1 */
#define TF_RESP_LONG            2U       /* R2（CID/CSD），136 位，占 RESP1~4 */

/* 一次数据搬运用多少字（RXFIFOHF / TXFIFOHE 触发阈值就是 8） */
#define TF_FIFO_WORDS           8U

/* 命令超时兜底的循环上限（HAL 时基没跑起来时也不会死等） */
#define TF_SPIN_LIMIT           0x02000000U

/* 初始化最大重试：卡上电慢时，整条识别流程允许重来几次 */
#define TF_INIT_RETRY           3U

/* -------------------------------------------------------------------------
 * SD 命令号
 * ------------------------------------------------------------------------- */
#define TF_CMD_GO_IDLE          0U      /* CMD0  */
#define TF_CMD_SEND_IF_COND     8U      /* CMD8  */
#define TF_CMD_ALL_SEND_CID     2U      /* CMD2  */
#define TF_CMD_SEND_REL_ADDR    3U      /* CMD3  */
#define TF_CMD_SEND_CSD         9U      /* CMD9  */
#define TF_CMD_SELECT_CARD      7U      /* CMD7  */
#define TF_CMD_SET_BLOCKLEN     16U     /* CMD16 */
#define TF_CMD_READ_SINGLE      17U     /* CMD17 */
#define TF_CMD_READ_MULTI       18U     /* CMD18 */
#define TF_CMD_WRITE_SINGLE     24U     /* CMD24 */
#define TF_CMD_WRITE_MULTI      25U     /* CMD25 */
#define TF_CMD_STOP_TRANSMIT    12U     /* CMD12 */
#define TF_CMD_SEND_STATUS      13U     /* CMD13 */
#define TF_CMD_SWITCH_FUNC      6U      /* CMD6  */
#define TF_CMD_APP_CMD          55U     /* CMD55 */
#define TF_ACMD_SET_BUS_WIDTH   6U      /* ACMD6 */
#define TF_ACMD_SD_OP_COND      41U     /* ACMD41 */

/* CMD8 参数：电压 2.7~3.6V(0x1) + 校验图案 0xAA */
#define TF_CMD8_ARG             0x000001AAU

/* ACMD41 参数：HCS(bit30, 支持高容量) + 电压窗口 2.7~3.6V(bits 15..23)。
   ★ 不用 ST 那个 0x80100000，因为它的电压窗口只开了一位（3.2~3.3V），
     兼容性不如这个常用的全窗口值。 */
#define TF_ACMD41_ARG           0x40FF8000U

/* ---------------------------------------------------------------
 * CMD6 (SWITCH_FUNC) —— 把卡切到"高速模式"，这是跑 48MHz 的前提
 *   参数位域：
 *     bit31      = 1        模式：1 = 设置功能（0 = 只查询）
 *     bit30:24   = 0        保留
 *     bit23:20   = 0xF      功能组6：0xF = 不改
 *     bit19:16   = 0xF      功能组5：不改
 *     bit15:12   = 0xF      功能组4：不改
 *     bit11:8    = 0xF      功能组3：不改
 *     bit7:4     = 0xF      功能组2：不改
 *     bit3:0     = 0x1      功能组1(访问模式)：1 = 高速模式
 *   => 0x80FFFFF1
 *   ★ 这条命令带 64 字节的数据返回（512 位状态），要用数据路径读回来。
 * --------------------------------------------------------------- */
#define TF_CMD6_ARG             0x80FFFFF1U
#define TF_CMD6_STATUS_BYTES    64U

/* CMD6 返回数据的第 13 字节 = 功能组1(访问模式)的状态：
 *   bit0 = 支持 功能0(默认速度)，bit1 = 支持 功能1(高速速度)
 *   bit7:4 = 当前已选中的功能
 * => (status[13] & 0x02) 非 0 表示这张卡支持高速模式 */
#define TF_CMD6_FG1_BYTE        13U
#define TF_CMD6_FG1_HS_SUPPORT  0x02U

/* ---------------------------------------------------------------
 * DMA（STM32F405 的 SDIO 只能挂 DMA2）
 *   SDIO_RX -> DMA2_Stream3, Channel 4（卡 → 内存）
 *   SDIO_TX -> DMA2_Stream6, Channel 4（内存 → 卡）
 * ★ 两个流都是 Channel 4，但只有 Stream3 能收、Stream6 能发，
 *   这是硬件固定的路由，不能互换。已查 ST 官方社区与 CubeMX 生成代码确认。
 * --------------------------------------------------------------- */
#define TF_DMA              DMA2
#define TF_DMA_RX_STREAM    DMA2_Stream3    /* 卡 → 内存 */
#define TF_DMA_TX_STREAM    DMA2_Stream6    /* 内存 → 卡  */
#define TF_DMA_CHANNEL_BITS DMA_SxCR_CHSEL_2    /* Channel 4 = 0b100 */

/* 清一个流的全部中断标志：Stream3 在 LIFCR 的 bit27:22，Stream6 在 HIFCR 的 bit21:16 */
#define TF_DMA_CLR_RX       (0x3FU << 22U)
#define TF_DMA_CLR_TX       (0x3FU << 16U)

/* 每个流对应的中断标志寄存器里的 TCIF 位（轮询用，判断搬完了没） */
#define TF_DMA_RX_TCIF      DMA_LISR_TCIF3
#define TF_DMA_TX_TCIF      DMA_HISR_TCIF6
#define TF_DMA_RX_TEIF      DMA_LISR_TEIF3
#define TF_DMA_TX_TEIF      DMA_HISR_TEIF6


/* R1 响应（= 卡状态 32 位）里的错误位。
 *
 * ★ 老版本这里写的是 0x000000FF，是错的：SD 规范里卡状态的低 8 位
 *   (bit7:0) 除 bit7 在 CMD6 响应里表示 SWITCH_ERROR 外全是保留位，
 *   真正的错误位都在【高位】。所以老掩码既抓不到真错误，又可能被某些卡
 *   返回的非零保留位误触发 —— 症状就是莫名其妙报 TF_ERR_CMD_RESP
 *   （g_tfErrLine 停在 tf_check_r1 那一行，而 SDIO->STA 干干净净）。
 *
 * 下面这版按规范覆盖错误位：
 *   bit31:19  OUT_OF_RANGE / ADDRESS_ERROR / BLOCK_LEN_ERROR /
 *             ERASE_SEQ_ERROR / ERASE_PARAM / WP_VIOLATION /
 *             LOCK_UNLOCK_FAILED / COM_CRC_ERROR / ILLEGAL_COMMAND /
 *             CARD_ECC_FAILED / CC_ERROR / ERROR
 *   bit16     CSD_OVERWRITE
 *   bit15     WP_ERASE_SKIP
 *   bit7      SWITCH_ERROR（只在 CMD6 的响应里有意义，保持原来的行为）
 * 故意【不】当作错误的位：
 *   bit25 CARD_IS_LOCKED（卡锁了不等于这条命令失败）
 *   bit14 CARD_ECC_DISABLED / bit13 ERASE_RESET（是状态，不是错误）
 *   bit12:9 CURRENT_STATE / bit8 READY_FOR_DATA（正常状态位）
 */
#define TF_R1_ERR_MASK          0xFDF98080U

/* ==========================================================================
 * 全局状态
 * ========================================================================== */

TF_Info          g_tf;
volatile uint32_t g_tfStep    = TF_STEP_NONE;
volatile uint32_t g_tfLastSta = 0U;
volatile uint32_t g_tfErrLine = 0U;
volatile uint32_t g_tfLastResp = 0U;    /* 出错时那条命令的响应原值 */

/* 私有 */
static uint32_t  s_sdioClkSrcHz = 48000000U;   /* 反推出来的 PLL48CLK */

/* ==========================================================================
 * 小工具
 * ========================================================================== */

#define TF_MARK(line)   do { g_tfErrLine = (uint32_t)(line); } while (0)

/** 由 GPIO_PIN_x 反推位号 0~15（GPIO_PIN_8 = 0x0100 -> 8） */
static uint32_t tf_pin_index(uint32_t pin)
{
    uint32_t i = 0U;
    while ((i < 16U) && (((pin >> i) & 1U) == 0U))
    {
        ++i;
    }
    return i;
}

/** 等 SDIO->STA 里 mask 的任一位被置起。
 *  @retval 命中那一刻的 STA 值（调用方自己看是哪一位）
 *  ★ 双保险：既看 HAL 毫秒时基，也看纯循环计数 ——
 *    万一驱动被放到 HAL 时基还没跑起来的地方调用，也不会死等。 */
static uint32_t tf_wait_sta(uint32_t mask, uint32_t to_ms)
{
    const uint32_t t0   = HAL_GetTick();
    uint32_t       spin = 0U;
    uint32_t       sta;

    for (;;)
    {
        sta = SDIO->STA;
        if ((sta & mask) != 0U)
        {
            return sta;
        }
        if ((to_ms != 0U) && ((HAL_GetTick() - t0) > to_ms))
        {
            return sta;
        }
        if ((++spin) > TF_SPIN_LIMIT)
        {
            return sta;
        }
    }
}

/* ==========================================================================
 * GPIO / 时钟
 * ========================================================================== */

/** 把一个引脚配成 AF12(SDIO)、推挽、上拉、超高速 */
static void tf_pin_to_af12(GPIO_TypeDef *port, uint32_t pin)
{
    const uint32_t i = tf_pin_index(pin);
    const uint32_t m = 3UL << (i * 2U);

    /* 复用功能 */
    port->MODER   = (port->MODER   & ~m) | (2UL << (i * 2U));
    /* 推挽输出（SDIO 是推挽驱动） */
    port->OTYPER &= ~(1UL << i);
    /* 超高速：SDIO 在 16MHz 下边沿要够陡 */
    port->OSPEEDR = (port->OSPEEDR & ~m) | (3UL << (i * 2U));
    /* 上拉：DAT/CMD 空闲时必须为高；卡座那侧一般也有上拉，双保险 */
    port->PUPDR   = (port->PUPDR   & ~m) | (1UL << (i * 2U));

    /* AF12 = SDIO */
    if (i < 8U)
    {
        port->AFR[0] = (port->AFR[0] & ~(0xFUL << (i * 4U))) | (12UL << (i * 4U));
    }
    else
    {
        port->AFR[1] = (port->AFR[1] & ~(0xFUL << ((i - 8U) * 4U)))
                     | (12UL << ((i - 8U) * 4U));
    }
}

/** 使能时钟 + 配好六个引脚 */
static void tf_gpio_init(void)
{
    /* ★★★ 必须先关掉 UART5 —— 这是个真会出事的地方，不是洁癖 ★★★
       UART5 的 TX = PC12、RX = PD2（见 usart.c 的 UART5_MspInit），
       而 PC12/PD2 正是 TF 卡的 CLK 和 CMD！
       本工程 UART5 只被 CubeMX 初始化过、没有任何功能使用它，
       但 usart.c:184 明明开着 HAL_NVIC_EnableIRQ(UART5_IRQn)。
       一旦把 PD2 改成 SDIO_CMD，UART5 的接收端就会把 SDIO 命令线上的
       电平当成串口数据，疯狂产生 FE(帧错误)/RXNE 中断 ——
       既白烧 CPU，也可能干扰 SDIO 的时序。
       所以这里把它彻底停掉：先断中断线，再停 UART 本体。 */
    NVIC_DisableIRQ(UART5_IRQn);
    NVIC_ClearPendingIRQ(UART5_IRQn);
    UART5->CR1 = 0U;                    /* 关掉 UART 本体，它不会再产生任何事件 */
    (void)UART5->SR;
    (void)UART5->DR;

    /* GPIOC 一定已经开了（现有代码在用），GPIOD 可能没开，开着也无害 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIODEN;
    (void)RCC->AHB1ENR;                 /* 读回来，确保写生效再动 GPIO */

    tf_pin_to_af12(TF_PORT_CLK, TF_PIN_CLK);
    tf_pin_to_af12(TF_PORT_CMD, TF_PIN_CMD);
    tf_pin_to_af12(TF_PORT_D0,  TF_PIN_D0);
    tf_pin_to_af12(TF_PORT_D1,  TF_PIN_D1);
    tf_pin_to_af12(TF_PORT_D2,  TF_PIN_D2);
    tf_pin_to_af12(TF_PORT_D3,  TF_PIN_D3);
}

/** 从 RCC->PLLCFGR 反推 SDIOCLK(PLL48CLK)，不写死 48MHz */
static void tf_calc_clk_src(void)
{
    const uint32_t pllcfgr = RCC->PLLCFGR;
    const uint32_t pllq    = ((pllcfgr & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos);
    const uint32_t pllm    = (pllcfgr & RCC_PLLCFGR_PLLM);
    const uint32_t plln    = ((pllcfgr & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos);
    uint32_t       src     = HSE_VALUE;   /* 本工程 PLL 源是 HSE */

    /* PLL 源若不是 HSE（异常情况），退回 HSI，避免算出离谱的值 */
    if ((pllcfgr & RCC_PLLCFGR_PLLSRC) == 0U)
    {
        src = HSI_VALUE;
    }

    if ((pllm != 0U) && (plln != 0U) && (pllq != 0U))
    {
        const uint64_t vco = ((uint64_t)src * (uint64_t)plln) / (uint64_t)pllm;
        s_sdioClkSrcHz = (uint32_t)(vco / (uint64_t)pllq);
    }
    else
    {
        s_sdioClkSrcHz = 48000000U;
    }

    g_tf.sdioClkSrcHz = s_sdioClkSrcHz;
}

/** 档位 -> CLKDIV。返回 0xFFFFFFFF 表示这一档要用 BYPASS */
static uint32_t tf_level_to_div(uint8_t level)
{
    switch (level)
    {
    case TF_SPEED_48M:   return 0xFFFFFFFFU;    /* BYPASS */
    case TF_SPEED_24M:   return 0U;             /* 48/(0+2) = 24MHz */
    case TF_SPEED_16M:   return 1U;
    case TF_SPEED_12M:   return 2U;
    case TF_SPEED_8M:    return 4U;
    case TF_SPEED_4M:    return 10U;
    case TF_SPEED_400K:  return TF_CLKDIV_INIT;
    default:             return 0U;
    }
}

/** 设 SDIO 时钟档位。level = TF_SPEED_xxx。
 *  ★ BYPASS 一档（48MHz）与其它档的取法完全不同：
 *      BYPASS=1 时 CLKDIV 被忽略，SDIO_CK = SDIOCLK 直通 = 48MHz；
 *      其余档 SDIO_CK = SDIOCLK / (CLKDIV + 2)，最高只能到 24MHz。
 *  ★ 改时钟时先把 CLKEN 关掉再改，避免切换瞬间产生畸形时钟打乱卡的状态。 */
static void tf_set_clk_level(uint8_t level)
{
    const uint32_t div = tf_level_to_div(level);
    uint32_t       tmp;

    tmp = SDIO->CLKCR;
    SDIO->CLKCR = tmp & ~SDIO_CLKCR_CLKEN;      /* 先停卡时钟 */

    tmp &= ~(SDIO_CLKCR_CLKDIV | SDIO_CLKCR_BYPASS | SDIO_CLKCR_NEGEDGE);

    if (div == 0xFFFFFFFFU)
    {
        tmp |= SDIO_CLKCR_BYPASS;
        /* BYPASS 下 SDIOCLK 直通，走线路径本身有延迟，
           相位要靠 NEGEDGE 补偿（这也是 ST 手册里推荐的做法）。 */
        tmp |= SDIO_CLKCR_NEGEDGE;
        g_tf.clkDiv    = 0U;
        g_tf.sdioClkHz = s_sdioClkSrcHz;        /* 48MHz */
        g_tf.bypass    = 1U;
        g_tf.negedge   = 1U;
    }
    else
    {
        tmp |= (div & SDIO_CLKCR_CLKDIV);
        g_tf.clkDiv    = div;
        g_tf.sdioClkHz = s_sdioClkSrcHz / (div + 2U);
        g_tf.bypass    = 0U;
        g_tf.negedge   = 0U;
    }

    /* ★ 4 位模式只置 WIDBUS 低位（00=1位 01=4位 10=8位）。
       写成 WIDBUS_0|WIDBUS_1 就变 8 位，会完全不通信。 */
    if (g_tf.bus4bit != 0U)
    {
        tmp |= SDIO_CLKCR_WIDBUS_0;
    }

    SDIO->CLKCR = tmp | SDIO_CLKCR_CLKEN;

    g_tf.speedLevel = level;
}

/** 老接口：直接按 CLKDIV 设（会退出 BYPASS） */
uint32_t TF_SetClockDiv(uint32_t div)
{
    uint8_t lv;

    if (div > 255U)
    {
        div = 255U;
    }

    switch (div)
    {
    case 0U:  lv = TF_SPEED_24M; break;
    case 1U:  lv = TF_SPEED_16M; break;
    case 2U:  lv = TF_SPEED_12M; break;
    case 4U:  lv = TF_SPEED_8M;  break;
    case 10U: lv = TF_SPEED_4M;  break;
    default:
        /* 其它值直接算，但不改档位语义 */
        {
            uint32_t tmp = SDIO->CLKCR;
            SDIO->CLKCR = tmp & ~SDIO_CLKCR_CLKEN;
            tmp &= ~(SDIO_CLKCR_CLKDIV | SDIO_CLKCR_BYPASS);
            tmp |= (div & SDIO_CLKCR_CLKDIV);
            SDIO->CLKCR = tmp | SDIO_CLKCR_CLKEN;
            g_tf.clkDiv    = div;
            g_tf.sdioClkHz = s_sdioClkSrcHz / (div + 2U);
            g_tf.bypass    = 0U;
            g_tf.speedLevel = TF_SPEED_AUTO;
            return div;
        }
    }

    tf_set_clk_level(lv);
    return g_tf.clkDiv;
}

const char *TF_SpeedStr(uint8_t level)
{
    switch (level)
    {
    case TF_SPEED_AUTO: return "auto";
    case TF_SPEED_48M:  return "48MHz";
    case TF_SPEED_24M:  return "24MHz";
    case TF_SPEED_16M:  return "16MHz";
    case TF_SPEED_12M:  return "12MHz";
    case TF_SPEED_8M:   return "8MHz";
    case TF_SPEED_4M:   return "4MHz";
    case TF_SPEED_400K: return "400kHz";
    default:            return "?";
    }
}


/* ==========================================================================
 * 命令收发
 * ========================================================================== */

/** 发一条命令并等响应。
 *  @param idx     命令号
 *  @param arg     参数
 *  @param resp    TF_RESP_NONE / _SHORT / _LONG
 *  @param to_ms   超时毫秒，0 = 不限时（靠循环兜底）
 *  @param no_crc  1 = 不把响应 CRC 错当失败（R3 用，见下面的说明）
 *  @retval TF_OK 或错误码
 *  ★ 响应值放在 SDIO->RESP1..4，调用方自己读。
 *
 *  ★★ 关于 CCRCFAIL（"1 init cCRC" 就是栽在这里）：
 *     - R1/R6（短响应）里带 CRC7，校验不过确实是真错，照旧报 TF_ERR_CMD_CRC；
 *     - R2（CMD2/CMD9 的长响应）和 R3（ACMD41 返回的 OCR）按规范
 *       【没有可校验的 CRC7】：R3 的 48 位里根本没有 CRC 字段，
 *       最后 8 位其实是 OCR 的低位；SDIO 外设照它的"CRC"去比，必然置
 *       CCRCFAIL。ST 的 HAL 在 SDMMC_GetCmdResp2() / SDMMC_GetCmdResp3()
 *       里也是直接清掉这个标志、不当错误处理。
 *     所以这里对 R2 自动放宽，对 R3 用 no_crc 显式放宽；
 *     放宽后响应内容照样读得到，由调用方自己判断内容对不对。 */
static TF_Status tf_cmd_ex(uint8_t idx, uint32_t arg, uint32_t resp, uint32_t to_ms,
                           uint8_t no_crc)
{
    uint32_t sta;
    uint32_t cmdreg;

    /* 先清标志，免得把上一条命令的结果当成这条的 */
    SDIO->ICR = TF_STATIC_FLAGS;

    SDIO->ARG = arg;

    cmdreg = (uint32_t)idx | SDIO_CMD_CPSMEN;
    if (resp == TF_RESP_SHORT)
    {
        cmdreg |= SDIO_CMD_WAITRESP_0;      /* 只等 1 位宽（短响应） */
    }
    else if (resp == TF_RESP_LONG)
    {
        cmdreg |= SDIO_CMD_WAITRESP;        /* 两位都置 = 长响应 */
    }
    SDIO->CMD = cmdreg;

    /* 等命令路径状态机给出结果 */
    if (resp == TF_RESP_NONE)
    {
        sta = tf_wait_sta(SDIO_STA_CMDSENT | SDIO_STA_CTIMEOUT, to_ms);
        if ((sta & SDIO_STA_CTIMEOUT) != 0U)
        {
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_CMD_TIMEOUT;
        }
        if ((sta & SDIO_STA_CMDSENT) == 0U)
        {
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_CMD_TIMEOUT;
        }
        return TF_OK;
    }

    sta = tf_wait_sta(SDIO_STA_CMDREND | SDIO_STA_CTIMEOUT | SDIO_STA_CCRCFAIL, to_ms);

    if ((sta & SDIO_STA_CTIMEOUT) != 0U)
    {
        TF_MARK(__LINE__);
        g_tfLastSta = sta;      /* 超时的话 RESP1 没意义，不记 */
        return TF_ERR_CMD_TIMEOUT;
    }

    if ((sta & SDIO_STA_CCRCFAIL) != 0U)
    {
        g_tfLastResp = SDIO->RESP1;     /* 诊断：把当时的响应原值留下来 */

        /* ★★ R2（CMD2/CMD9 的长响应）和 R3（ACMD41 返回的 OCR）按规范
           没有可校验的 CRC7。这种情况外设【只置 CCRCFAIL，不置 CMDREND】，
           所以 CCRCFAIL 本身就是"响应已经到了"的信号 ——
           既不能当成错误，也不能清完标志再回头去等 CMDREND
           （那样会误报 cTO，见 line 525 那一支）。
           ST 的 HAL 在 SDMMC_GetCmdResp2()/GetCmdResp3() 里也是直接清掉
           这个标志、当成功处理。响应内容照样在 RESP1..4 里，
           由调用方自己判断内容对不对（ACMD41 那边看 OCR 的 bit31/bit30）。 */
        if ((no_crc == 0U) && (resp != TF_RESP_LONG))
        {
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_CMD_CRC;      /* R1/R6/R7 带真 CRC7：这确实是错 */
        }

        SDIO->ICR = SDIO_STA_CCRCFAIL;
        return TF_OK;                   /* 放宽的场合：到这里就算收到了 */
    }

    if ((sta & SDIO_STA_CMDREND) == 0U)
    {
        TF_MARK(__LINE__);
        g_tfLastSta = sta;
        return TF_ERR_CMD_TIMEOUT;
    }

    return TF_OK;
}

/** 普通命令（R1/R6/R7 这类带有效 CRC7 的）：不放宽 CRC 检查 */
static TF_Status tf_cmd(uint8_t idx, uint32_t arg, uint32_t resp, uint32_t to_ms)
{
    return tf_cmd_ex(idx, arg, resp, to_ms, 0U);
}

/** 读回 4 个响应寄存器（长响应用） */
static void tf_get_resp4(uint32_t *out)
{
    out[0] = SDIO->RESP1;
    out[1] = SDIO->RESP2;
    out[2] = SDIO->RESP3;
    out[3] = SDIO->RESP4;
}

/** 读回 RESP1（短响应用） */
static uint32_t tf_get_resp1(void)
{
    return SDIO->RESP1;
}

/** 检查 R1 响应里的错误位 */
static TF_Status tf_check_r1(uint32_t resp1)
{
    if ((resp1 & TF_R1_ERR_MASK) != 0U)
    {
        TF_MARK(__LINE__);
        g_tfLastResp = resp1;
        return TF_ERR_CMD_RESP;
    }
    return TF_OK;
}

/* ==========================================================================
 * 数据路径配置
 * ========================================================================== */

/** 配好数据路径。
 *  @param len_bytes       本次要传的字节数（= 块数 x 512，或 CMD6 的 64）
 *  @param read_from_card  1 = 从卡读（卡→控制器），0 = 写卡（控制器→卡）
 *  @param to_ms           预留
 *  ★ SDIO_DCTRL_DTDIR 的方向定义是：1 = 卡→控制器（读），0 = 控制器→卡（写）。
 *    别被名字骗了，这个位不是"发给卡"。 */
static void tf_data_config(uint32_t len_bytes, uint32_t read_from_card, uint32_t use_dma)
{
    /* 每次传输前先关 DPSM，再重配 */
    SDIO->DCTRL   = 0U;
    SDIO->DTIMER  = 0xFFFFFFFFU;        /* 约 89s @48MHz，足够 */
    SDIO->DLEN    = len_bytes;

    SDIO->DCTRL = TF_DBLOCKSIZE_512B
                | (read_from_card ? SDIO_DCTRL_DTDIR : 0U)
                | (use_dma ? SDIO_DCTRL_DMAEN : 0U)
                | SDIO_DCTRL_DTEN;
}

/** CMD6 的状态数据是 64 字节一块 */
static void tf_data_config64(uint32_t read_from_card)
{
    SDIO->DCTRL  = 0U;
    SDIO->DTIMER = 0xFFFFFFFFU;
    SDIO->DLEN   = TF_CMD6_STATUS_BYTES;
    /* DBLOCKSIZE = 6 -> 2^6 = 64。 6 = 0b0110 -> 第 1、2 位 */
    SDIO->DCTRL  = (SDIO_DCTRL_DBLOCKSIZE_1 | SDIO_DCTRL_DBLOCKSIZE_2)
                 | (read_from_card ? SDIO_DCTRL_DTDIR : 0U)
                 | SDIO_DCTRL_DTEN;
}

/* ==========================================================================
 * DMA
 * ========================================================================== */

/**
 * 配好并启动一条 DMA 流。
 *   mem_to_periph = 0 -> 用 Stream3（卡 → 内存，读卡）
 *   mem_to_periph = 1 -> 用 Stream6（内存 → 卡，写卡）
 *   words = 要搬的 32 位字数（= 总字节数 / 4）
 *
 * ★ 用直通模式（DMA_SxFCR_DMDIS 关掉 FIFO、不做突发）：
 *   32 位一次搬一个字，最保守也最不容易出错。对 24~48MHz 的 SDIO 来说
 *   DMA 侧（AHB 168MHz）完全不是瓶颈，瓶颈永远在 SDIO_CK 上。
 *
 * ★ 不用 PFCTRL（外设流控）：用普通模式 + 精确 NDTR 更直观，
 *   而且完成判据交给 SDIO 自己的 DATAEND，比看 DMA 标志更贴近协议层。
 *
 * ★ 不注册 DMA 中断 —— 中断服务函数要写进 stm32f4xx_it.c，
 *   本驱动的约定是不动那个文件。轮询 SDIO 标志一样准确。
 */
static void tf_dma_start(uint32_t mem_to_periph, const void *buf, uint32_t words)
{
    DMA_Stream_TypeDef *stream = mem_to_periph ? TF_DMA_TX_STREAM : TF_DMA_RX_STREAM;
    uint32_t            cr;

    /* 1) 先关流，等 EN 位真的落下去 */
    stream->CR = 0U;
    while ((stream->CR & DMA_SxCR_EN) != 0U)
    {
        /* 空转等硬件清掉 EN */
    }

    /* 2) 清掉这条流的全部中断标志（避免残留的 TCIF 干扰判断） */
    if (mem_to_periph != 0U)
    {
        TF_DMA->HIFCR = TF_DMA_CLR_TX;
    }
    else
    {
        TF_DMA->LIFCR = TF_DMA_CLR_RX;
    }

    /* 3) 地址与长度 */
    stream->PAR  = (uint32_t)(&SDIO->FIFO);
    stream->M0AR = (uint32_t)buf;
    stream->NDTR = words;

    /* 4) 直通模式，不做突发 */
    stream->FCR = DMA_SxFCR_DMDIS;

    /* 5) 控制字
          CHSEL = 4（SDIO 固定通道）
          源/目的都是 32 位、内存地址自增、外设地址不自增（默认）
          DIR: 00 = 外设→内存(读卡)，01 = 内存→外设(写卡) */
    cr  = TF_DMA_CHANNEL_BITS
        | ((TF_DMA_PRIORITY & 3U) << DMA_SxCR_PL_Pos)
        | DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1
        | DMA_SxCR_MINC
        | (mem_to_periph ? DMA_SxCR_DIR_0 : 0U);

    stream->CR = cr | DMA_SxCR_EN;
}

/** 关掉一条 DMA 流并清标志 */
static void tf_dma_stop(uint32_t mem_to_periph)
{
    DMA_Stream_TypeDef *stream = mem_to_periph ? TF_DMA_TX_STREAM : TF_DMA_RX_STREAM;

    stream->CR = 0U;
    while ((stream->CR & DMA_SxCR_EN) != 0U)
    {
        /* 等 */
    }

    if (mem_to_periph != 0U)
    {
        TF_DMA->HIFCR = TF_DMA_CLR_TX;
    }
    else
    {
        TF_DMA->LIFCR = TF_DMA_CLR_RX;
    }
}

/**
 * 等一条 DMA 流真的把 NDTR 走完（返回 1 = 走完，0 = 超时）。
 *
 * ★★ 为什么必须等 —— 这是个非常阴的坑：
 *    SDIO 的 DATAEND 是按"SDIDCOUNT 归零"置位的，而 DCOUNT 归零只说明
 *    数据【进了 SDIO 自己的 FIFO】，不代表 DMA 已经把它写进内存。
 *    此刻若立刻 tf_dma_stop()（往 CR 写 0），就会把还在跑的流【掐掉】，
 *    块尾最后几个字永远留在 FIFO 里丢掉。
 *
 * ★★ 丢了尾巴【不会报任何错】，所以极难查：
 *    - DMA 模式下 remaining 被无条件置 0，长度检查形同虚设；
 *    - 数据 CRC 是硬件按"进了 FIFO 的字节"算的，照样通过；
 *    - 反复读同一块，每次都丢同样多，一致性测试也发现不了。
 *    表现就是"块尾是缓冲区里的旧数据"。
 *    典型症状：MBR 的 55AA 签名没了（块 0 的 510/511 正好是最后 2 字节），
 *    但同一块靠前的分区表项又是对的 —— 看着像"卡没有 MBR"。
 */
static uint8_t tf_dma_wait_done(uint32_t mem_to_periph, uint32_t to_ms)
{
    DMA_Stream_TypeDef *stream = mem_to_periph ? TF_DMA_TX_STREAM : TF_DMA_RX_STREAM;
    const uint32_t      t0     = HAL_GetTick();

    while (stream->NDTR != 0U)
    {
        if ((HAL_GetTick() - t0) > to_ms)
        {
            return 0U;
        }
    }
    return 1U;
}

/** 开一次 DMA2 时钟 + 复位两条流（初始化时调一次） */
static void tf_dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    (void)RCC->AHB1ENR;

    tf_dma_stop(0U);
    tf_dma_stop(1U);

    g_tf.dmaUsed = (uint8_t)TF_USE_DMA;
}

/**
 * 传输中途出错后的收尾 —— 让卡从"还在往外吐数据"的状态里退出来。
 *
 * ★ 为什么必须做：多块读（CMD18）是开放式传输，读到一半出错时
 *   卡那边以为还在传，这时直接改时钟或发下一条命令，卡会完全不响应，
 *   表现为"一次 CRC 错之后整张卡就死了"。补一条 CMD12 就能拉回来。
 *
 * ★ 只在已经知道 RCA 的阶段调用（即读写路径），识别阶段不走这里。
 */
static void tf_abort_transfer(void)
{
    SDIO->DCTRL = 0U;
    (void)tf_cmd(TF_CMD_STOP_TRANSMIT, 0U, TF_RESP_SHORT, 100U);
    (void)tf_cmd(TF_CMD_SEND_STATUS, ((uint32_t)g_tf.rca << 16U),
                 TF_RESP_SHORT, 100U);
    SDIO->ICR = TF_STATIC_FLAGS;
}

/* ==========================================================================
 * 速度：CMD6 切高速模式 + 自动协商
 * ========================================================================== */

/**
 * 用 CMD6(SWITCH_FUNC) 把卡切到高速模式。
 *
 * ★ 这是跑 48MHz 的硬前提：SD 规范的 Default Speed 模式上限只有 25MHz，
 *   想上 48MHz 必须先进 High Speed 模式（允许到 50MHz）。
 *
 * CMD6 和其他命令不同 —— 它要连 64 字节的状态数据一起读回来，
 * 所以不能只发命令，要配好数据路径再读。
 *
 * 返回 TF_OK 表示卡确实切过去了（不是"只是支持"）。
 */
static TF_Status tf_switch_high_speed(void)
{
    TF_Status st;
    uint8_t   status[TF_CMD6_STATUS_BYTES] __attribute__((aligned(4)));
    uint32_t  i;
    uint32_t  remaining;
    uint32_t  sta;
    uint32_t  t0;
    uint32_t  spin;
    uint32_t  w;
    uint8_t  *p = status;

    for (i = 0U; i < TF_CMD6_STATUS_BYTES; ++i)
    {
        status[i] = 0U;
    }

    /* CMD6 是读方向的 64 字节数据 */
    tf_data_config64(1U);

    st = tf_cmd(TF_CMD_SWITCH_FUNC, TF_CMD6_ARG, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        SDIO->DCTRL = 0U;
        return st;
    }
    st = tf_check_r1(tf_get_resp1());
    if (st != TF_OK)
    {
        SDIO->DCTRL = 0U;
        return st;
    }

    /* 把 64 字节搬回来。这里数据量极小，直接 CPU 轮询，不用惊动 DMA */
    remaining = TF_CMD6_STATUS_BYTES;
    t0        = HAL_GetTick();
    spin      = 0U;

    for (;;)
    {
        sta = SDIO->STA;

        if ((sta & (SDIO_STA_RXOVERR | SDIO_STA_DCRCFAIL | SDIO_STA_DTIMEOUT |
                    SDIO_STA_STBITERR | SDIO_STA_DATAEND)) != 0U)
        {
            break;
        }

        if ((sta & SDIO_STA_RXFIFOHF) != 0U)
        {
            for (i = 0U; i < TF_FIFO_WORDS; ++i)
            {
                w = SDIO->FIFO;
                if (remaining >= 4U)
                {
                    *p++ = (uint8_t)(w);
                    *p++ = (uint8_t)(w >> 8U);
                    *p++ = (uint8_t)(w >> 16U);
                    *p++ = (uint8_t)(w >> 24U);
                    remaining -= 4U;
                }
            }
        }

        if (((HAL_GetTick() - t0) > 500U) || ((++spin) > TF_SPIN_LIMIT))
        {
            SDIO->DCTRL = 0U;
            return TF_ERR_DATA_TIMEOUT;
        }
    }

    /* 排空 FIFO 尾部（和读块时同一个道理） */
    while (((SDIO->STA & SDIO_STA_RXDAVL) != 0U) && (remaining >= 4U))
    {
        w = SDIO->FIFO;
        *p++ = (uint8_t)(w);
        *p++ = (uint8_t)(w >> 8U);
        *p++ = (uint8_t)(w >> 16U);
        *p++ = (uint8_t)(w >> 24U);
        remaining -= 4U;
    }
    while ((SDIO->STA & SDIO_STA_RXDAVL) != 0U)
    {
        (void)SDIO->FIFO;
    }

    SDIO->DCTRL = 0U;

    if ((sta & (SDIO_STA_DCRCFAIL | SDIO_STA_DTIMEOUT | SDIO_STA_RXOVERR)) != 0U)
    {
        return TF_ERR_DATA_CRC;
    }

    /* 第 13 字节 = 功能组1(访问模式)：
         bit0 = 支持默认速度，bit1 = 支持高速速度
         bit7:4 = 当前已选中的功能
       切成功的话 bit1 应该为 1，且高 4 位应该是 1（已选高速）。 */
    if ((status[TF_CMD6_FG1_BYTE] & TF_CMD6_FG1_HS_SUPPORT) == 0U)
    {
        return TF_ERR_UNSUPPORTED_HS;       /* 这张卡不支持高速模式 */
    }

    return TF_OK;
}

/**
 * 速度协商：从快到慢依次试，每一档都【真读一次 0 号扇区】来验证。
 *
 * ★ 为什么要真读：SDIO 硬件会对数据做 CRC16 校验，
 *   CRC 不过会置 DCRCFAIL —— 所以"读成功"就是这一档可靠的硬证据，
 *   比看手册算理论值靠谱得多。
 *
 * ★ 永远不会失败：最差落到 4MHz，绝不会把卡弄成不可用。
 *   试过哪几档记录在 g_tf.speedTriedMask 里，方便排查。
 */
static uint8_t tf_negotiate_speed(void)
{
    /* 从快到慢。48M 放在最前面 */
    static const uint8_t order[6] = {
        TF_SPEED_48M, TF_SPEED_24M, TF_SPEED_16M,
        TF_SPEED_12M, TF_SPEED_8M,  TF_SPEED_4M
    };
    uint8_t buf[TF_BLOCK_SIZE] __attribute__((aligned(4)));
    uint8_t i;
    uint8_t hs_ok = 0U;

    g_tf.speedTriedMask = 0U;

    /* 先把卡切到高速模式 —— 不然后面 48MHz 一定失败 */
    if (tf_switch_high_speed() == TF_OK)
    {
        hs_ok = 1U;
    }
    g_tf.highSpeedMode = hs_ok;

    for (i = 0U; i < 6U; ++i)
    {
        /* 48MHz 只有高速模式的卡才允许，没切成就跳过 */
        if ((order[i] == TF_SPEED_48M) && (hs_ok == 0U))
        {
            continue;
        }

        g_tf.speedTriedMask |= (1UL << order[i]);

        tf_set_clk_level(order[i]);
        HAL_Delay(1U);                      /* 给卡几个时钟稳定 */

        if (TF_ReadBlocks(0U, buf, 1U) == TF_OK)
        {
            return order[i];                /* 这一档能稳，就用它 */
        }

        /* ★ 这一档读失败了。先让卡退出可能残留的数据状态，
           否则下一档会带着"卡还在传"的烂摊子继续，必然一起失败。 */
        tf_abort_transfer();
        HAL_Delay(2U);
    }

    /* 全不行：退回最保守的档 */
    tf_set_clk_level(TF_SPEED_4M);
    HAL_Delay(1U);
    (void)TF_ReadBlocks(0U, buf, 1U);
    return TF_SPEED_4M;
}

uint8_t TF_SetSpeed(uint8_t level)
{
    if (g_tf.inited == 0U)
    {
        /* 还没初始化就先不管，等 TF_Init 里统一协商 */
        return TF_SPEED_AUTO;
    }

    if (level == TF_SPEED_AUTO)
    {
        g_tf.speedAuto = 1U;
        return tf_negotiate_speed();
    }

    if (level >= TF_SPEED_LEVELS)
    {
        return g_tf.speedLevel;
    }

    /* 指定档位：48M 需要高速模式，先确保切过 */
    if ((level == TF_SPEED_48M) && (g_tf.highSpeedMode == 0U))
    {
        if (tf_switch_high_speed() == TF_OK)
        {
            g_tf.highSpeedMode = 1U;
        }
        else
        {
            /* 切不了就退到 24MHz，不硬闯 */
            level = TF_SPEED_24M;
        }
    }

    g_tf.speedAuto = 0U;
    tf_set_clk_level(level);
    return level;
}

/* ==========================================================================
 * 识别流程
 * ========================================================================== */

/** ACMD41 轮询，直到卡 ready。
 *  返回后 SDIO->RESP1 里是最后一次的 OCR。 */
static TF_Status tf_acmd41(uint32_t *ocr_out)
{
    uint32_t i;
    TF_Status st;

    for (i = 0U; i < TF_ACMD41_TRIES; ++i)
    {
        /* ★ 每条 ACMD 前必须来一条 CMD55（RCA=0，因为还在识别阶段） */
        st = tf_cmd(TF_CMD_APP_CMD, 0U, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
        if (st != TF_OK)
        {
            return st;
        }
        st = tf_check_r1(tf_get_resp1());
        if (st != TF_OK)
        {
            return st;
        }

        /* ★ 注意用 tf_cmd_ex(..., 1U)：ACMD41 的 R3 响应里没有 CRC7，
           SDIO 外设必然置 CCRCFAIL，必须放宽，否则识别永远卡死在这一步
           （现象就是 TF_Init() 返回 TF_ERR_CMD_CRC，即 OLED 上的 "1 init cCRC"）。
           响应内容就是 OCR，下面用它的 bit31/bit30 自己判断。 */
        st = tf_cmd_ex(TF_ACMD_SD_OP_COND, TF_ACMD41_ARG, TF_RESP_SHORT,
                       TF_CMD_TIMEOUT_MS, 1U);
        if (st != TF_OK)
        {
            return st;
        }

        *ocr_out = tf_get_resp1();

        /* bit31 = busy。为 1 表示卡还在上电初始化，继续等 */
        if ((*ocr_out & 0x80000000U) != 0U)
        {
            return TF_OK;
        }

        HAL_Delay(1U);
    }

    TF_MARK(__LINE__);
    return TF_ERR_VOLTAGE;
}

/** 从 CSD 算出容量与块数
 *
 *  ★★ 最容易踩的坑：CSD v1.0 和 v2.0 的 C_SIZE 位置【不一样】！
 *       v1.0: C_SIZE = bits 73:62（12 位）、C_SIZE_MULT = bits 49:47
 *       v2.0: C_SIZE = bits 69:48（22 位）、没有 C_SIZE_MULT
 *     两个版本只差几位，写错了照样能算出个"看起来像容量"的数，
 *     所以必须先用 CSD_STRUCTURE 分版本，再套对应的位域。
 *     READ_BL_LEN 两个版本都是 bits 83:80（这个不变）。
 */
static void tf_parse_csd(const uint32_t *csd)
{
    /* ★ 版本直接看 CSD 自己的 bits 127:126（CSD_STRUCTURE）：
         0 = v1.0（标准容量 SDSC）、1 = v2.0（高容量 SDHC/SDXC）、
         2/3 = 更新版本，按 v2.0 处理。
       ★ 以 CSD 为准、而不是只看 OCR 的 CCS 位：两者不一致时 CSD 更可靠，
         顺手把 cardType 也纠正过来 —— 后面读写的"块地址 / 字节地址"
         是照 cardType 走的，错了就全错。 */
    const uint32_t csd_ver = (csd[0] >> 30U) & 0x3U;

    if (csd_ver >= 1U)
    {
        /* ---- CSD v2.0：C_SIZE 22 位，块大小固定 512B，没有 C_SIZE_MULT ----
           C_SIZE = CSD bits 69:48
                  = ((CSD[1] & 0x3F) << 16) | ((CSD[2] & 0xFFFF0000) >> 16)
           容量 = (C_SIZE + 1) * 512KB（512KB = 1024 个 512B 块） */
        const uint32_t c_size = ((csd[1] & 0x0000003FU) << 16U)
                              | ((csd[2] & 0xFFFF0000U) >> 16U);

        g_tf.cardType   = TF_CARD_SDHC;
        g_tf.blockCount = (c_size + 1U) * 1024U;
    }
    else
    {
        /* ---- CSD v1.0 ----
           C_SIZE      = CSD bits 73:62 = ((CSD[1] & 0x3FF) << 2) | (CSD[2] >> 30)
           C_SIZE_MULT = CSD bits 49:47 = (CSD[2] >> 15) & 0x7
           READ_BL_LEN = CSD bits 83:80 = (CSD[1] >> 16) & 0xF
           块数   = (C_SIZE+1) * 2^(C_SIZE_MULT+2)
           块大小 = 2^READ_BL_LEN

           ★★ 这里原来是 `csd[1] & 0xF`，取的是 CSD bits 67:64 ——
              那几位其实属于 C_SIZE（v1.0 里 C_SIZE = bits 73:62），
              根本不是 READ_BL_LEN。于是算出的块大小 < 512B，
              折算成 512B 逻辑块时变成 0 -> blockCount = 0 ->
              TF_Init() 报 TF_ERR_NOT_SD（现象：step 9 / cmd9）。 */
        const uint32_t c_size  = ((csd[1] & 0x000003FFU) << 2U)
                               | ((csd[2] & 0xC0000000U) >> 30U);
        const uint32_t mult    = (csd[2] & 0x00038000U) >> 15U;
        const uint32_t bl_len  = (csd[1] >> 16U) & 0x0000000FU;
        const uint32_t n_blk   = (c_size + 1U) * (1UL << (mult + 2U));
        const uint32_t bl_size = (1UL << bl_len);

        g_tf.cardType = TF_CARD_SDSC;

        /* 统一折算成 512 字节的"逻辑块"。
           万一 READ_BL_LEN < 9（块比 512B 还小，规范上不该出现），
           按 1 个逻辑块算 —— 别让结果变成 0 又被判成"不是 SD 卡"。 */
        g_tf.blockCount = n_blk * ((bl_size >= TF_BLOCK_SIZE)
                                   ? (bl_size / TF_BLOCK_SIZE) : 1U);
    }

    g_tf.capacityKB = (g_tf.blockCount / 2U);   /* 2 个 512B 块 = 1KB */
}

/** 一次完整的识别流程（从 CMD0 到切完 4 位） */
static TF_Status tf_identify(void)
{
    TF_Status st;
    uint32_t  resp1;
    uint32_t  csd[4];
    uint32_t  ocr = 0U;
    uint8_t   is_v2 = 0U;

    /* ---- 上电、配 GPIO、先用 400kHz ---- */
    g_tfStep = TF_STEP_GPIO;
    tf_gpio_init();

    g_tfStep = TF_STEP_POWER;
    RCC->APB2ENR |= RCC_APB2ENR_SDIOEN;
    (void)RCC->APB2ENR;
    tf_calc_clk_src();

    SDIO->POWER = 0U;                   /* 先彻底断电 */
    HAL_Delay(2U);
    SDIO->POWER = SDIO_POWER_PWRCTRL;   /* 再上电 */
    HAL_Delay(2U);

    g_tfStep = TF_STEP_CLK_INIT;
    /* 复位成已知状态：1 位总线、上升沿、无关断、无硬件流控 */
    SDIO->CLKCR = 0U;
    g_tf.bus4bit = 0U;
    tf_set_clk_level(TF_SPEED_400K);

    /* 给卡至少 74 个时钟（这里是 3ms，远超） */
    HAL_Delay(3U);

    /* ---- CMD0：进 IDLE ---- */
    g_tfStep = TF_STEP_CMD0;
    st = tf_cmd(TF_CMD_GO_IDLE, 0U, TF_RESP_NONE, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return st;                      /* 没响应：多半没插卡 / 没供电 */
    }

    /* ---- CMD8：探 SD 2.0 与电压 ---- */
    g_tfStep = TF_STEP_CMD8;
    st = tf_cmd(TF_CMD_SEND_IF_COND, TF_CMD8_ARG, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st == TF_OK)
    {
        resp1 = tf_get_resp1();
        /* 低 12 位应该把我们发的 0x1AA 原样回显 */
        if ((resp1 & 0x00000FFFU) == (TF_CMD8_ARG & 0x00000FFFU))
        {
            is_v2 = 1U;
        }
        else
        {
            /* 回显不对：可能不是 SD 卡 */
            TF_MARK(__LINE__);
            g_tfLastResp = resp1;
            return TF_ERR_CMD_RESP;
        }
    }
    else
    {
        /* CMD8 超时/CRC 错：V1.x 卡或 MMC。重新 CMD0 再按 V1 走一遍 */
        st = tf_cmd(TF_CMD_GO_IDLE, 0U, TF_RESP_NONE, TF_CMD_TIMEOUT_MS);
        if (st != TF_OK)
        {
            return st;
        }
        is_v2 = 0U;
    }
    g_tf.isV2 = is_v2;

    /* ---- ACMD41：等卡上电完成，同时确定容量类型 ---- */
    g_tfStep = TF_STEP_ACMD41;
    st = tf_acmd41(&ocr);
    if (st != TF_OK)
    {
        return st;
    }

    /* OCR bit30 = CCS：1 = 高容量(SDHC/SDXC)，0 = 标准容量(SDSC)。
       ★ 只有 V2 卡才有这个位；V1 卡一定是 SDSC。 */
    if ((is_v2 != 0U) && ((ocr & 0x40000000U) != 0U))
    {
        g_tf.cardType = TF_CARD_SDHC;
    }
    else
    {
        g_tf.cardType = TF_CARD_SDSC;
    }

    /* ---- CMD2：取 CID（这里只取一下，内容不用） ---- */
    g_tfStep = TF_STEP_CMD2;
    st = tf_cmd(TF_CMD_ALL_SEND_CID, 0U, TF_RESP_LONG, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return st;
    }

    /* ---- CMD3：取 RCA ---- */
    g_tfStep = TF_STEP_CMD3;
    st = tf_cmd(TF_CMD_SEND_REL_ADDR, 0U, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return TF_ERR_NOT_SD;
    }
    resp1 = tf_get_resp1();
    /* R6 里 RCA 在高 16 位；低 16 位是状态 */
    g_tf.rca = (uint16_t)(resp1 >> 16U);
    if (g_tf.rca == 0U)
    {
        TF_MARK(__LINE__);
        return TF_ERR_NOT_SD;
    }

    /* ---- CMD9：取 CSD，算容量 ---- */
    g_tfStep = TF_STEP_CMD9;
    st = tf_cmd(TF_CMD_SEND_CSD, ((uint32_t)g_tf.rca << 16U),
                TF_RESP_LONG, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return st;
    }
    tf_get_resp4(csd);
    /* 诊断：原始 CSD 留一份，容量不对时可以手工解码核对位域 */
    g_tf.csd[0] = csd[0];
    g_tf.csd[1] = csd[1];
    g_tf.csd[2] = csd[2];
    g_tf.csd[3] = csd[3];
    tf_parse_csd(csd);

    if (g_tf.blockCount == 0U)
    {
        TF_MARK(__LINE__);
        return TF_ERR_NOT_SD;
    }

    /* ---- CMD7：选中这张卡 ---- */
    g_tfStep = TF_STEP_CMD7;
    st = tf_cmd(TF_CMD_SELECT_CARD, ((uint32_t)g_tf.rca << 16U),
                TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return st;
    }
    st = tf_check_r1(tf_get_resp1());
    if (st != TF_OK)
    {
        return st;
    }

    /* ---- CMD16：设块长 512。
       SDHC 会忽略它，但 SDSC 必须设，否则读写长度不对。 ---- */
    g_tfStep = TF_STEP_CMD16;
    st = tf_cmd(TF_CMD_SET_BLOCKLEN, TF_BLOCK_SIZE, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st != TF_OK)
    {
        return st;
    }
    st = tf_check_r1(tf_get_resp1());
    if (st != TF_OK)
    {
        return st;
    }

    /* ---- ACMD6：切 4 位总线。
       ★ 必须在【还是 1 位】的时候发这条命令，
         等卡答应了再把 SDIO 的 WIDBUS 改掉。顺序反了就通不了。 ---- */
    g_tfStep = TF_STEP_ACMD6;
    st = tf_cmd(TF_CMD_APP_CMD, ((uint32_t)g_tf.rca << 16U),
                TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    if (st == TF_OK)
    {
        st = tf_check_r1(tf_get_resp1());
    }
    if (st == TF_OK)
    {
        /* ACMD6 的参数：0 = 1 位，2 = 4 位 */
        st = tf_cmd(TF_ACMD_SET_BUS_WIDTH, 2U, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }
    if (st == TF_OK)
    {
        st = tf_check_r1(tf_get_resp1());
    }

    if (st == TF_OK)
    {
        g_tf.bus4bit = 1U;
    }
    else
    {
        /* 切 4 位失败不算致命：退回 1 位继续用，只是慢一半 */
        g_tf.bus4bit = 0U;
        st = TF_OK;
    }

    /* ---- 先落在一个稳妥的中间速度。
           真正的提速交给 TF_Init() 里的速度协商（会试 48M/24M/16M...
           并逐档真读验证），这里不直接上高速。 ---- */
    g_tfStep = TF_STEP_CLK_RUN;
    tf_set_clk_level(TF_SPEED_12M);

    g_tfStep    = TF_STEP_READY;
    g_tf.inited = 1U;
    return TF_OK;
}

/* ==========================================================================
 * 对外的初始化接口
 * ========================================================================== */

void TF_DeInit(void)
{
    SDIO->DCTRL  = 0U;
    SDIO->CLKCR  = 0U;
    SDIO->POWER  = 0U;
    RCC->APB2ENR &= ~RCC_APB2ENR_SDIOEN;
    (void)RCC->APB2ENR;

    g_tf.inited  = 0U;
    g_tf.bus4bit = 0U;
    g_tf.cardType = TF_CARD_UNKNOWN;
    g_tfStep = TF_STEP_NONE;
}

TF_Status TF_Init(void)
{
    TF_Status st;
    uint8_t   i;

    if (g_tf.inited != 0U)
    {
        return TF_OK;
    }

    /* 清空状态 */
    g_tf.inited      = 0U;
    g_tf.cardType    = TF_CARD_UNKNOWN;
    g_tf.bus4bit     = 0U;
    g_tf.isV2        = 0U;
    g_tf.rca         = 0U;
    g_tf.blockCount  = 0U;
    g_tf.capacityKB  = 0U;
    g_tf.speedLevel  = TF_SPEED_AUTO;
    g_tf.speedAuto   = 1U;
    g_tf.highSpeedMode = 0U;
    g_tf.bypass      = 0U;
    g_tf.csd[0]      = 0U;
    g_tf.csd[1]      = 0U;
    g_tf.csd[2]      = 0U;
    g_tf.csd[3]      = 0U;

    g_tfLastSta  = 0U;
    g_tfLastResp = 0U;
    g_tfErrLine  = 0U;

    /* DMA2 先准备好（SDIO 只能挂 DMA2） */
    tf_dma_init();

    /* 识别会失败于"卡刚上电还没 ready"，允许整条流程重来几次 */
    for (i = 0U; i < TF_INIT_RETRY; ++i)
    {
        st = tf_identify();
        if (st == TF_OK)
        {
            /* ★ 识别成功。先把 inited 置起来 ——
               下面的速度协商和验证会走 TF_ReadBlocks()，
               而那个函数内部会调 TF_EnsureInit()，不置就是死递归。 */
            g_tf.inited = 1U;

            /* ★ 速度协商：自动从 48MHz 往下试，取能稳定读的那一档。
               内部自己兜底，永远不会失败。 */
            g_tfStep = TF_STEP_SPEED;
            (void)tf_negotiate_speed();
            g_tfStep = TF_STEP_READY;

            g_tf.inited = 1U;
            return TF_OK;
        }
#if defined(TF_TRACE_INIT) && TF_USE_OLED
        /* 打开了追踪就把每次重试的结果打出来（走 OLED） */
        TF_OledShow(st, (uint8_t)(16U + i * 16U));
#endif
        HAL_Delay(50U);
    }

    /* 全失败了：把 SDIO 彻底关掉。
       ★ 只清 POWER 是不够的 —— CLKCR 里的 CLKEN 还开着，SDIO_CK 会继续
         在 PC12 上按 400kHz 一直跑，白白往外辐射（对旁边那条 OLED 的
         I2C 就是干扰源）。所以时钟、数据通道、卡电一起断。 */
    SDIO->DCTRL = 0U;
    SDIO->CLKCR = 0U;
    SDIO->POWER = 0U;
    return st;
}

TF_Status TF_EnsureInit(void)
{
    if (g_tf.inited != 0U)
    {
        return TF_OK;
    }
    return TF_Init();
}

/* ==========================================================================
 * 读块
 * ========================================================================== */

/** 参数换算：把"块号"变成命令里该填的地址 */
static uint32_t tf_addr_of(uint32_t block)
{
    /* SDHC/SDXC 直接用块号；SDSC 要用字节地址 */
    return (g_tf.cardType == TF_CARD_SDHC) ? block : (block * TF_BLOCK_SIZE);
}

TF_Status TF_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count)
{
    TF_Status st;
    uint32_t  total;
    uint32_t  remaining;
    uint32_t  to_ms;
    uint32_t  sta;
    uint32_t  t0;
    uint32_t  spin;
    uint32_t  i;
    uint32_t  w;
    uint32_t  use_dma;
    uint32_t  dma_bad;
    uint8_t  *p = buf;

    if ((buf == 0) || (count == 0U))
    {
        return TF_ERR_PARAM;
    }

    st = TF_EnsureInit();
    if (st != TF_OK)
    {
        return st;
    }

    if ((block >= g_tf.blockCount) || ((block + count) > g_tf.blockCount))
    {
        return TF_ERR_ADDR_RANGE;
    }

    g_tfStep = TF_STEP_READ;

    total     = count * TF_BLOCK_SIZE;
    remaining = total;

    /* 超时按块数放大，免得大块读被误判超时 */
    to_ms = count * TF_DATA_TIMEOUT_PER_BLOCK_MS;
    if (to_ms < TF_DATA_TIMEOUT_MIN_MS)
    {
        to_ms = TF_DATA_TIMEOUT_MIN_MS;
    }

    /* ---- 选搬运方式 ----
       ★ DMA 的 MSIZE 是 32 位，要求内存地址 4 字节对齐。
         这里不报错，而是静默退回 CPU 轮询路径 —— 调用方不用关心对齐。 */
    use_dma = ((TF_USE_DMA != 0) && (((uint32_t)buf & 3U) == 0U)) ? 1U : 0U;
    dma_bad = 0U;

    /* ---- 配数据路径（读：read_from_card=1 -> DTDIR=1）---- */
    tf_data_config(total, 1U, use_dma);

    /* ---- 先武装 DMA，再发命令 ----
       ★ 顺序很重要：命令一出去，卡可能很快就开始回数据，
         那时 DMA 必须已经就绪，否则 FIFO 直接溢出（RXOVERR）。 */
    if (use_dma != 0U)
    {
        tf_dma_start(0U, buf, total / 4U);      /* 0 = 卡 → 内存 */
    }

    /* ---- 再发命令 ---- */
    if (count == 1U)
    {
        st = tf_cmd(TF_CMD_READ_SINGLE, tf_addr_of(block),
                    TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }
    else
    {
        st = tf_cmd(TF_CMD_READ_MULTI, tf_addr_of(block),
                    TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }
    if (st != TF_OK)
    {
        if (use_dma != 0U) { tf_dma_stop(0U); }
        SDIO->DCTRL = 0U;
        return st;
    }
    st = tf_check_r1(tf_get_resp1());
    if (st != TF_OK)
    {
        if (use_dma != 0U) { tf_dma_stop(0U); }
        SDIO->DCTRL = 0U;
        return st;
    }

    /* ---- 等搬完 ---- */
    if (use_dma != 0U)
    {
        /* 数据由 DMA 在后台搬，CPU 只等 SDIO 的结束/错误标志 */
        sta = tf_wait_sta(SDIO_STA_RXOVERR | SDIO_STA_DCRCFAIL |
                          SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR |
                          SDIO_STA_DATAEND, to_ms);
        /* ★★ DATAEND 只说明数据进了 SDIO 的 FIFO。必须再等 DMA 把 NDTR
           走完才能停流，否则块尾几个字会被 tf_dma_stop() 掐掉丢掉
           （丢尾巴不报错，见 tf_dma_wait_done 的说明）。 */
        if (tf_dma_wait_done(0U, to_ms) == 0U)
        {
            dma_bad = 1U;
        }
        tf_dma_stop(0U);
        remaining = 0U;             /* DMA 模式不看这个计数 */
    }
    else
    {
    /* ---- CPU 轮询版：一边等一边搬 ---- */
    t0   = HAL_GetTick();
    spin = 0U;
    for (;;)
    {
        sta = SDIO->STA;

        /* 先看错误和结束 */
        if ((sta & (SDIO_STA_RXOVERR | SDIO_STA_DCRCFAIL |
                    SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR |
                    SDIO_STA_DATAEND)) != 0U)
        {
            break;
        }

        /* FIFO 里攒够 8 个字就搬走。DLEN 是 32 字节的整数倍，
           所以这条路径正好把全部数据搬完。 */
        if ((sta & SDIO_STA_RXFIFOHF) != 0U)
        {
            for (i = 0U; i < TF_FIFO_WORDS; ++i)
            {
                w = SDIO->FIFO;
                if (remaining >= 4U)
                {
                    /* 按字节写，这样对 buf 的对齐没有任何要求 */
                    *p++ = (uint8_t)(w);
                    *p++ = (uint8_t)(w >> 8U);
                    *p++ = (uint8_t)(w >> 16U);
                    *p++ = (uint8_t)(w >> 24U);
                    remaining -= 4U;
                }
            }
        }

        if ((HAL_GetTick() - t0) > to_ms)
        {
            SDIO->DCTRL = 0U;
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_DATA_TIMEOUT;
        }
        if ((++spin) > TF_SPIN_LIMIT)
        {
            SDIO->DCTRL = 0U;
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_DATA_TIMEOUT;
        }
    }
    }   /* ---- CPU 轮询版结束 ---- */

    /* ---- 收尾：把 FIFO 里剩下的字排空 ----
       ★ 这一步不能省。DATAEND 是按"DCOUNT 归零"置位的，而 DCOUNT 归零
         发生在数据进了 FIFO 之后 —— 也就是说 DATAEND 有可能和最后一批
         RXFIFOHF 同时置起，上面那个循环先看到 DATAEND 就跳出来了，
         最后几个字还留在 FIFO 里。不排空就会丢掉块尾的几十字节。
       ★ DMA 模式下 FIFO 由 DMA 接管，CPU 绝对不能再碰它。 */
    if (use_dma == 0U)
    {
    while (((SDIO->STA & SDIO_STA_RXDAVL) != 0U) && (remaining >= 4U))
    {
        w = SDIO->FIFO;
        *p++ = (uint8_t)(w);
        *p++ = (uint8_t)(w >> 8U);
        *p++ = (uint8_t)(w >> 16U);
        *p++ = (uint8_t)(w >> 24U);
        remaining -= 4U;
    }
    /* FIFO 里若还有残留（说明长度对不上），读掉丢弃，别留给下一次传输 */
    while ((SDIO->STA & SDIO_STA_RXDAVL) != 0U)
    {
        (void)SDIO->FIFO;
    }
    }   /* ---- FIFO 排空结束 ---- */

    /* ---- 判定结果 ---- */
    SDIO->DCTRL = 0U;

    /* ★ 数据阶段只要出过错，先补一条 CMD12 把卡从"还在往外吐数据"的
       状态里拉出来。不做这一步的话，多块读失败之后卡会彻底不响应，
       表现为"一次 CRC 错之后整张卡就死了"。 */
    if ((sta & (SDIO_STA_RXOVERR | SDIO_STA_DCRCFAIL |
                SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR)) != 0U)
    {
        tf_abort_transfer();
    }

    if ((remaining != 0U) || (dma_bad != 0U))
    {
        /* 少收了字节 / DMA 没搬完：即便 DATAEND 置了也不能算成功 */
        if (dma_bad != 0U)
        {
            tf_abort_transfer();
        }
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }

    if ((sta & SDIO_STA_RXOVERR) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_FIFO;
    }
    if ((sta & SDIO_STA_STBITERR) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_FIFO;
    }
    if ((sta & SDIO_STA_DCRCFAIL) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_CRC;
    }
    if ((sta & SDIO_STA_DTIMEOUT) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }
    if ((sta & SDIO_STA_DATAEND) == 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }

    /* ---- 多块读要用 CMD12 收尾（CMD18 是开放式传输）---- */
    if (count > 1U)
    {
        (void)tf_cmd(TF_CMD_STOP_TRANSMIT, 0U, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
        /* ★ CMD12 的响应是"前一条数据命令"的尾巴，按规范要丢一次响应。
           这里再读一次 RESP1 只是把状态清干净，不判错。 */
    }

    SDIO->ICR = TF_STATIC_FLAGS;
    return TF_OK;
}

TF_Status TF_ReadBlockRetry(uint32_t block, uint8_t *buf, uint32_t retry)
{
    TF_Status st = TF_ERR_DATA_CRC;
    uint32_t  i;

    if (retry == 0U)
    {
        retry = 1U;
    }

    for (i = 0U; i < retry; ++i)
    {
        st = TF_ReadBlocks(block, buf, 1U);
        if (st == TF_OK)
        {
            return TF_OK;
        }
    }
    return st;
}

/* ==========================================================================
 * 写块
 * ========================================================================== */

/** 等卡内部编程结束。
 *  写完之后卡会拉低 DAT0 表示 busy（最长几百 ms），
 *  用 CMD13 查状态里的 READY_FOR_DATA(bit8) 最稳妥。 */
static void tf_wait_write_done(void)
{
    uint32_t i;
    TF_Status st;

    for (i = 0U; i < 200U; ++i)
    {
        st = tf_cmd(TF_CMD_SEND_STATUS, ((uint32_t)g_tf.rca << 16U),
                    TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
        if (st == TF_OK)
        {
            const uint32_t r = tf_get_resp1();
            /* bit8 = READY_FOR_DATA；bit9~12 是卡当前状态，4 = tran */
            if ((r & 0x00000100U) != 0U)
            {
                return;
            }
        }
        HAL_Delay(1U);
    }
}

TF_Status TF_WriteBlocks(uint32_t block, const uint8_t *buf, uint32_t count)
{
    TF_Status st;
    uint32_t  total;
    uint32_t  words_total;
    uint32_t  words_sent;
    uint32_t  to_ms;
    uint32_t  sta;
    uint32_t  t0;
    uint32_t  spin;
    uint32_t  i;
    uint32_t  w;
    uint32_t  use_dma;
    uint32_t  dma_bad;
    const uint8_t *p = buf;

    if ((buf == 0) || (count == 0U))
    {
        return TF_ERR_PARAM;
    }

    st = TF_EnsureInit();
    if (st != TF_OK)
    {
        return st;
    }

    if ((block >= g_tf.blockCount) || ((block + count) > g_tf.blockCount))
    {
        return TF_ERR_ADDR_RANGE;
    }

    g_tfStep = TF_STEP_WRITE;

    total       = count * TF_BLOCK_SIZE;
    words_total = total / 4U;           /* 512 的整数倍，一定是 4 的整数倍 */
    words_sent  = 0U;

    to_ms = count * TF_DATA_TIMEOUT_PER_BLOCK_MS + 500U;
    if (to_ms < TF_DATA_TIMEOUT_MIN_MS)
    {
        to_ms = TF_DATA_TIMEOUT_MIN_MS;
    }

    /* ---- 选搬运方式（DMA 要 4 字节对齐）---- */
    use_dma = ((TF_USE_DMA != 0) && (((uint32_t)buf & 3U) == 0U)) ? 1U : 0U;
    dma_bad = 0U;

    /* ---- 配数据路径（写：read_from_card=0 -> DTDIR=0，控制器→卡）---- */
    tf_data_config(total, 0U, use_dma);

    /* ---- 先武装 DMA，再发命令 ----
       写方向更要先就绪：卡一进接收状态就会不停要数据，
       DMA 没准备好立刻就是 TXUNDERR（FIFO 欠载）。 */
    if (use_dma != 0U)
    {
        tf_dma_start(1U, buf, total / 4U);      /* 1 = 内存 → 卡 */
    }

    if (count == 1U)
    {
        st = tf_cmd(TF_CMD_WRITE_SINGLE, tf_addr_of(block),
                    TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }
    else
    {
        st = tf_cmd(TF_CMD_WRITE_MULTI, tf_addr_of(block),
                    TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }
    if (st != TF_OK)
    {
        if (use_dma != 0U) { tf_dma_stop(1U); }
        SDIO->DCTRL = 0U;
        return st;
    }
    st = tf_check_r1(tf_get_resp1());
    if (st != TF_OK)
    {
        if (use_dma != 0U) { tf_dma_stop(1U); }
        SDIO->DCTRL = 0U;
        return st;
    }

    /* ---- 等搬完 ---- */
    if (use_dma != 0U)
    {
        sta = tf_wait_sta(SDIO_STA_TXUNDERR | SDIO_STA_DCRCFAIL |
                          SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR |
                          SDIO_STA_DATAEND, to_ms);
        /* 和读路径同理：确认 DMA 真的把 NDTR 走完再停流 */
        if (tf_dma_wait_done(1U, to_ms) == 0U)
        {
            dma_bad = 1U;
        }
        tf_dma_stop(1U);
        words_sent = words_total;       /* DMA 搬完就是全部搬完 */
    }
    else
    {
    /* ---- CPU 轮询版：一边等一边灌 ---- */
    t0   = HAL_GetTick();
    spin = 0U;
    for (;;)
    {
        sta = SDIO->STA;

        if ((sta & (SDIO_STA_TXUNDERR | SDIO_STA_DCRCFAIL |
                    SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR |
                    SDIO_STA_DATAEND)) != 0U)
        {
            break;
        }

        /* FIFO 至少空出 8 个字就灌最多 8 个。
           ★ 用"已灌字数"计数，而不是靠 remaining 推条件 ——
             FIFO 的写入次数必须是精确的 (total/4) 次，多灌一个字
             都会让卡那边收到错位的数据。 */
        if ((sta & SDIO_STA_TXFIFOHE) != 0U)
        {
            for (i = 0U; i < TF_FIFO_WORDS; ++i)
            {
                if (words_sent >= words_total)
                {
                    break;                  /* 灌完了，剩下的等 DATAEND */
                }
                w  = (uint32_t)p[0];
                w |= (uint32_t)p[1] << 8U;
                w |= (uint32_t)p[2] << 16U;
                w |= (uint32_t)p[3] << 24U;
                p += 4;
                SDIO->FIFO = w;
                ++words_sent;
            }
        }

        if ((HAL_GetTick() - t0) > to_ms)
        {
            SDIO->DCTRL = 0U;
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_DATA_TIMEOUT;
        }
        if ((++spin) > TF_SPIN_LIMIT)
        {
            SDIO->DCTRL = 0U;
            TF_MARK(__LINE__);
            g_tfLastSta = sta;
            return TF_ERR_DATA_TIMEOUT;
        }
    }
    }   /* ---- CPU 轮询版结束 ---- */

    SDIO->DCTRL = 0U;

    /* ★ 数据阶段出错就先补 CMD12 让卡退出传输状态（同读路径的理由） */
    if ((sta & (SDIO_STA_TXUNDERR | SDIO_STA_DCRCFAIL |
                SDIO_STA_DTIMEOUT | SDIO_STA_STBITERR)) != 0U)
    {
        tf_abort_transfer();
    }

    /* 灌的字数必须正好对上，少一个字都说明数据不完整 */
    if ((words_sent != words_total) || (dma_bad != 0U))
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }

    if ((sta & SDIO_STA_TXUNDERR) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_FIFO;
    }
    if ((sta & SDIO_STA_STBITERR) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_FIFO;
    }
    if ((sta & SDIO_STA_DCRCFAIL) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_CRC;
    }
    if ((sta & SDIO_STA_DTIMEOUT) != 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }
    if ((sta & SDIO_STA_DATAEND) == 0U)
    {
        TF_MARK(__LINE__); g_tfLastSta = sta;
        return TF_ERR_DATA_TIMEOUT;
    }

    if (count > 1U)
    {
        (void)tf_cmd(TF_CMD_STOP_TRANSMIT, 0U, TF_RESP_SHORT, TF_CMD_TIMEOUT_MS);
    }

    /* 等卡把数据真正写进 Flash */
    tf_wait_write_done();

    SDIO->ICR = TF_STATIC_FLAGS;
    return TF_OK;
}

/* ==========================================================================
 * 查询接口
 * ========================================================================== */

uint32_t TF_BlockCount(void)  { return g_tf.blockCount; }
uint32_t TF_CapacityKB(void)  { return g_tf.capacityKB; }
uint8_t  TF_IsReady(void)     { return g_tf.inited; }
uint8_t  TF_IsBus4Bit(void)   { return g_tf.bus4bit; }
uint32_t TF_SdioClockHz(void) { return g_tf.sdioClkHz; }

uint8_t TF_GetSpeedLevel(void) { return g_tf.speedLevel; }
uint8_t TF_IsHighSpeed(void)   { return g_tf.highSpeedMode; }
uint8_t TF_IsDma(void)         { return g_tf.dmaUsed; }

const char *TF_StatusStr(TF_Status st)
{
    switch (st)
    {
    case TF_OK:                return "OK";
    case TF_ERR_PARAM:         return "param";
    case TF_ERR_NOT_INIT:      return "not init";
    case TF_ERR_CMD_TIMEOUT:   return "cmd timeout";
    case TF_ERR_CMD_CRC:       return "cmd crc";
    case TF_ERR_CMD_RESP:      return "cmd resp err";
    case TF_ERR_DATA_TIMEOUT:  return "data timeout";
    case TF_ERR_DATA_CRC:      return "data crc";
    case TF_ERR_DATA_FIFO:     return "fifo/startbit";
    case TF_ERR_V2_FAIL:       return "cmd8 v1 card";
    case TF_ERR_VOLTAGE:       return "acmd41 no ready";
    case TF_ERR_NOT_SD:        return "not sd card";
    case TF_ERR_ADDR_RANGE:    return "addr range";
    case TF_ERR_BUS_WIDTH:     return "bus width";
    case TF_ERR_UNSUPPORTED_HS:return "no high speed";
    default:                   return "unknown";
    }
}

/* ==========================================================================
 * 自检
 * ========================================================================== */

TF_Status TF_SelfTest(TF_SelfTestResult *res)
{
    TF_Status st;
    /* ★ 必须 4 字节对齐：DMA 模式下内存地址不对齐会走不通
       （函数内部会自动退回 CPU 路径，但这里直接对齐省事） */
    uint8_t   buf[TF_BLOCK_SIZE] __attribute__((aligned(4)));
    uint32_t  i;
    uint32_t  zero_words = 0U;
    uint8_t   all_ff = 1U;
    uint8_t   all_00 = 1U;

    if (res != 0)
    {
        res->mbrOk       = 0U;
        res->partType    = 0U;
        res->firstByteFF = 0U;
        res->firstByte00 = 0U;
        res->partLba     = 0U;
        res->zeroWords   = 0U;
    }

    st = TF_Init();
    if (st != TF_OK)
    {
        return st;
    }

    /* 读 0 号扇区（MBR） */
    for (i = 0U; i < TF_BLOCK_SIZE; ++i)
    {
        buf[i] = 0U;
    }

    st = TF_ReadBlockRetry(0U, buf, 3U);
    if (st != TF_OK)
    {
        return st;
    }

    /* 统计特征字节，用来区分"卡没问题但内容全是 0xFF/0x00"
       （那通常是接触不良或没插好，而不是驱动错） */
    for (i = 0U; i < TF_BLOCK_SIZE; ++i)
    {
        if (buf[i] != 0xFFU) { all_ff = 0U; }
        if (buf[i] != 0x00U) { all_00 = 0U; }
    }
    for (i = 0U; i < (TF_BLOCK_SIZE / 4U); ++i)
    {
        const uint32_t w = ((uint32_t)buf[i * 4U])
                         | ((uint32_t)buf[i * 4U + 1U] << 8U)
                         | ((uint32_t)buf[i * 4U + 2U] << 16U)
                         | ((uint32_t)buf[i * 4U + 3U] << 24U);
        if (w == 0U) { ++zero_words; }
    }

    if (res != 0)
    {
        res->firstByteFF = all_ff;
        res->firstByte00 = all_00;
        res->zeroWords   = zero_words;
    }

    /* MBR 签名在最后两字节：0x55 0xAA */
    if ((buf[510] == 0x55U) && (buf[511] == 0xAAU))
    {
        if (res != 0)
        {
            res->mbrOk    = 1U;
            res->partType = buf[446U + 4U];        /* 第一个分区表的类型字节 */
            res->partLba  = ((uint32_t)buf[446U + 8U])
                          | ((uint32_t)buf[446U + 9U]  << 8U)
                          | ((uint32_t)buf[446U + 10U] << 16U)
                          | ((uint32_t)buf[446U + 11U] << 24U);
        }
        return TF_OK;
    }

    /* 没签名：可能这张卡没分区表（裸卡），但读取本身是成功的。
       这种情况仍然算"驱动通了"，只是 res->mbrOk = 0。 */
    return TF_OK;
}

/* ==========================================================================
 * OLED 简报
 * ========================================================================== */

#if TF_USE_OLED
void TF_OledShow(TF_Status st, uint8_t y)
{
    if (st != TF_OK)
    {
        /* 失败：打错误名 + 卡在哪一步（最有用的两个数） */
        OLED_ShowString(0U, y, "TF:", OLED_FONT_08, 1U);
        OLED_ShowString(24U, y, TF_StatusStr(st), OLED_FONT_08, 1U);
        OLED_ShowString(0U, (uint8_t)(y + 16U), "step:", OLED_FONT_08, 1U);
        OLED_ShowUNum(48U, (uint8_t)(y + 16U), g_tfStep, 2U, OLED_FONT_08, 1U);
    }
    else
    {
        /* 成功：容量(MB) + 总线位宽 + 实测速度 + 是否高速模式/DMA */
        OLED_ShowString(0U, y, "TF ok ", OLED_FONT_08, 1U);
        OLED_ShowUNum(48U, y, g_tf.capacityKB / 1024U, 5U, OLED_FONT_08, 1U);
        OLED_ShowString(88U, y, "MB", OLED_FONT_08, 1U);

        OLED_ShowString(0U, (uint8_t)(y + 16U), g_tf.bus4bit ? "4bit" : "1bit",
                        OLED_FONT_08, 1U);
        OLED_ShowUNum(40U, (uint8_t)(y + 16U), g_tf.sdioClkHz / 1000000U,
                      2U, OLED_FONT_08, 1U);
        OLED_ShowString(56U, (uint8_t)(y + 16U), "MHz", OLED_FONT_08, 1U);

        /* H = 已进高速模式，D = 走 DMA */
        OLED_ShowString(0U, (uint8_t)(y + 32U), g_tf.highSpeedMode ? "HS" : "--",
                        OLED_FONT_08, 1U);
        OLED_ShowString(24U, (uint8_t)(y + 32U), g_tf.dmaUsed ? "DMA" : "CPU",
                        OLED_FONT_08, 1U);
    }
}
#endif

/* ==========================================================================
 *                     测试 / 调试工具（OLED 版）
 *
 * ★ 报告输出到 OLED（128x64）。
 *   OLED 用的是 6x8 小字体：一行 21 个字符、一屏 8 行。
 *   报告一行行攒进日志缓冲，然后**分页轮播** —— 每页停几秒自动翻，
 *   转完设定的圈数再返回。
 *
 * ★ 同时也往 USART1(PA9) 打一份同样的内容（如果你以后把 PA9 引出来了，
 *   接个 USB-TTL 就能在串口助手里看到一样的报告）。
 *   不想要就把 TF_DBG_UART_EN 设成 0，那时完全不碰 USART1。
 *
 * ★ 整段可以用 TF_INCLUDE_TEST 定义成 0 关掉，不影响驱动本体。
 * ========================================================================== */

#include <string.h>

#ifndef TF_INCLUDE_TEST
#define TF_INCLUDE_TEST     1
#endif

/* 串口镜像输出（可选）。设 0 = 完全不碰 USART1 */
#ifndef TF_DBG_UART_EN
#define TF_DBG_UART_EN      1
#endif

#ifndef TF_DBG_UART
#define TF_DBG_UART         USART1
#endif

/* OLED 文字控制台的尺寸（6x8 字体） */
#define TF_LOG_ROWS         8U      /* 一屏行数 */
#define TF_LOG_COLS         22U     /* 缓冲字节数；6x8 字体一行能放 21 个字符，
                                       第 22 字节留给结尾的 NUL —— 所以实际
                                       最多 21 个字符可见 */
#define TF_LOG_MAXLINES     80U     /* 日志最多攒多少行 */

/* DWT 周期计数器（Cortex-M4 内核自带，不占外设） */
#define TF_DWT_DEMCR        (*(volatile uint32_t *)0xE000EDFCUL)
#define TF_DWT_CTRL         (*(volatile uint32_t *)0xE0001000UL)
#define TF_DWT_CYCCNT       (*(volatile uint32_t *)0xE0001004UL)

#if TF_INCLUDE_TEST

/* ---------------- 日志缓冲 ---------------- */

static char     s_tfLog[TF_LOG_MAXLINES][TF_LOG_COLS];
static uint16_t s_tfLogN;               /* 已攒了多少行 */

static char     s_lb[TF_LOG_COLS];      /* 正在拼的这一行 */
static uint8_t  s_lbLen;

static void lb_reset(void)
{
    s_lbLen  = 0U;
    s_lb[0]  = '\0';
}

static void lb_ch(char c)
{
    if (s_lbLen < (TF_LOG_COLS - 1U))
    {
        s_lb[s_lbLen++] = c;
        s_lb[s_lbLen]   = '\0';
    }
}

static void lb_puts(const char *s)
{
    while (*s != '\0')
    {
        lb_ch(*s++);
    }
}

static void lb_putu(uint32_t v)
{
    char     tmp[12];
    uint32_t n = 0U;

    if (v == 0U)
    {
        lb_ch('0');
        return;
    }
    while ((v > 0U) && (n < 11U))
    {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0U)
    {
        lb_ch(tmp[--n]);
    }
}

/** 8 位十六进制（不带 0x 前缀，省地方） */
static void lb_puthex(uint32_t v)
{
    static const char hexd[16] = {'0','1','2','3','4','5','6','7',
                                  '8','9','A','B','C','D','E','F'};
    int32_t i;

    for (i = 7; i >= 0; --i)
    {
        lb_ch(hexd[(v >> (uint32_t)(i * 4)) & 0xFU]);
    }
}

/** 拼一个 "名字       值" 的行：名字占 11 个字符宽，左对齐 */
static void lb_kv(const char *name, const char *val)
{
    uint8_t n = 0U;

    lb_reset();
    while ((name[n] != '\0') && (n < 11U))
    {
        lb_ch(name[n]);
        ++n;
    }
    while (n < 11U)
    {
        lb_ch(' ');
        ++n;
    }
    lb_puts(val);
    /* 值如果是空的，就到这里为止 */
}

/** 把当前拼好的行落进日志（并镜像到串口） */
static void lb_emit(void)
{
#if TF_DBG_UART_EN
    {
        /* 先把串口打开（幂等），再整行打过去 + 回车换行 */
        uint32_t guard;
        const char *p = s_lb;

        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
        (void)RCC->APB2ENR;
        TF_DBG_UART->CR1 = USART_CR1_TE | USART_CR1_UE;

        while (*p != '\0')
        {
            guard = 0U;
            while (((TF_DBG_UART->SR & USART_SR_TXE) == 0U) && (++guard < 500000U))
            {
            }
            TF_DBG_UART->DR = (uint16_t)(*p++);
        }
        for (guard = 0U; guard < 2U; ++guard)
        {
            uint32_t g2 = 0U;
            while (((TF_DBG_UART->SR & USART_SR_TXE) == 0U) && (++g2 < 500000U))
            {
            }
            TF_DBG_UART->DR = (uint16_t)((guard == 0U) ? '\r' : '\n');
        }
    }
#endif

    if (s_tfLogN < TF_LOG_MAXLINES)
    {
        uint8_t i;
        for (i = 0U; i < TF_LOG_COLS; ++i)
        {
            s_tfLog[s_tfLogN][i] = s_lb[i];
            if (s_lb[i] == '\0')
            {
                break;
            }
        }
        s_tfLog[s_tfLogN][TF_LOG_COLS - 1U] = '\0';
        ++s_tfLogN;
    }

    lb_reset();
}

/** 换行：把当前行发出，另起一行空的开头（方便后面继续拼） */
static void lb_nl(void)
{
    lb_emit();
    lb_reset();
}

/** 整行输出一个常量字符串 */
static void lb_str(const char *s)
{
    lb_reset();
    lb_puts(s);
    lb_emit();
}

/** 名字 + 一个十进制数 */
static void lb_kv_u(const char *name, uint32_t v)
{
    lb_kv(name, "");
    lb_putu(v);
    lb_emit();
}

/** 名字 + 一个十六进制数 */
static void lb_kv_x(const char *name, uint32_t v)
{
    lb_kv(name, "");
    lb_puthex(v);
    lb_emit();
}

/* ---------------- DWT 计时 ---------------- */

static void tf_dwt_init(void)
{
    TF_DWT_DEMCR |= (1UL << 24);        /* TRCENA */
    TF_DWT_CYCCNT = 0U;
    TF_DWT_CTRL  |= 1UL;                /* CYCCNTENA */
}

static uint32_t tf_dwt_now(void)
{
    return TF_DWT_CYCCNT;
}

static uint32_t tf_dwt_us(uint32_t start)
{
    /* 168MHz 下 CYCCNT 约 25.6s 回绕一次，单次测试远短于此 */
    return (TF_DWT_CYCCNT - start) / (SystemCoreClock / 1000000U);
}

/* ---------------- 文件系统识别 ---------------- */

static const char *tf_fs_name(const uint8_t *bs)
{
    if (memcmp(bs + 3U, "EXFAT   ", 8U) == 0) { return "exFAT"; }
    if (memcmp(bs + 3U, "NTFS    ", 8U) == 0)  { return "NTFS"; }
    if (memcmp(bs + 0x52U, "FAT32", 5U) == 0)  { return "FAT32"; }
    if (memcmp(bs + 0x36U, "FAT", 3U) == 0)    { return "FAT16/12"; }
    if ((bs[510] == 0x55U) && (bs[511] == 0xAAU)) { return "MBR?"; }
    return "unknown";
}

/** 短版错误名。OLED 一行只有 21 个字符，TF_StatusStr() 太长会撑破行宽 */
static const char *tf_status_short(TF_Status st)
{
    switch (st)
    {
    case TF_OK:                 return "OK";
    case TF_ERR_PARAM:          return "param";
    case TF_ERR_NOT_INIT:       return "not init";
    case TF_ERR_CMD_TIMEOUT:    return "cmd TO";
    case TF_ERR_CMD_CRC:        return "cmd crc";
    case TF_ERR_CMD_RESP:       return "cmd resp";
    case TF_ERR_DATA_TIMEOUT:   return "data TO";
    case TF_ERR_DATA_CRC:       return "data crc";
    case TF_ERR_DATA_FIFO:      return "fifo/stb";
    case TF_ERR_V2_FAIL:        return "v1 card";
    case TF_ERR_VOLTAGE:        return "no ready";
    case TF_ERR_NOT_SD:         return "not SD";
    case TF_ERR_ADDR_RANGE:     return "range";
    case TF_ERR_BUS_WIDTH:      return "bus width";
    case TF_ERR_UNSUPPORTED_HS: return "no HS";
    default:                    return "?";
    }
}

/* ---------------- 工作缓冲（静态，不占栈）---------------- */

static uint8_t s_tfBuf[TF_BLOCK_SIZE]   __attribute__((aligned(4)));
static uint8_t s_tfRef[TF_BLOCK_SIZE]   __attribute__((aligned(4)));
static uint8_t s_tfSaved[TF_BLOCK_SIZE] __attribute__((aligned(4)));

/* ★ 吞吐测试专用的大缓冲。
   注意别把它省了：以前这里没有它，吞吐测试直接把 256 块(128KB) 读进
   512 字节的 s_tfBuf，越界 128KB，会把 SRAM1 整片踩掉
   （OLED 显存、栈都在里面）—— 表现就是屏幕周期性花屏。
   一次读 8 块(4KB) 正好装进本缓冲，读 32 次就是 128KB。 */
static uint8_t s_tfBig[TF_BLOCK_SIZE * 8U] __attribute__((aligned(4)));

/* 吞吐测试一次读多少块（= sizeof(s_tfBig) / 512） */
#define TF_PERF_BLOCKS      8U
/* 一轮吞吐测试总共读多少块（32 x 8 = 256 块 = 128KB，和原来一样） */
#define TF_PERF_ROUNDS      32U

/* ---------------- 日志显示 ---------------- */

/**
 * 把日志分页显示到 OLED。
 *
 * @param pageMs 每页停留多少毫秒（0 会被当成 1000）
 * @param loops  轮播几圈（0 会被当成 1）
 *
 * ★ 这是个**阻塞**函数，会一直占到轮播结束才返回。
 *   所以别放在需要立刻响应的流程中间（比如等上位机配置握手的那段）。
 */
void TF_ShowLog(uint16_t pageMs, uint8_t loops)
{
#if TF_USE_OLED
    uint16_t total;
    uint16_t p;
    uint8_t  i;
    uint8_t  n;

    if (s_tfLogN == 0U)
    {
        OLED_Clear();
        OLED_ShowString(0U, 0U, "TF: no log yet", OLED_FONT_06, 1U);
        OLED_Refresh();
        HAL_Delay(1000U);
        return;
    }

    if (pageMs == 0U) { pageMs = 1000U; }
    if (loops  == 0U) { loops  = 1U; }

    total = (uint16_t)((s_tfLogN + TF_LOG_ROWS - 1U) / TF_LOG_ROWS);

    for (n = 0U; n < loops; ++n)
    {
        for (p = 0U; p < total; ++p)
        {
            OLED_Clear();

            /* 右上角显示页码，方便知道有没有翻完 */
            {
                char pg[6];
                pg[0] = (char)('0' + (uint8_t)((p + 1U) / 10U % 10U));
                pg[1] = (char)('0' + (uint8_t)((p + 1U) % 10U));
                pg[2] = '/';
                pg[3] = (char)('0' + (uint8_t)(total / 10U % 10U));
                pg[4] = (char)('0' + (uint8_t)(total % 10U));
                pg[5] = '\0';
                OLED_ShowString((uint8_t)(OLED_WIDTH - 30U), 0U, pg,
                                OLED_FONT_06, 1U);
            }

            for (i = 0U; i < TF_LOG_ROWS; ++i)
            {
                uint16_t li = (uint16_t)(p * TF_LOG_ROWS + i);
                if (li < s_tfLogN)
                {
                    OLED_ShowString(0U, (uint8_t)(i * 8U), s_tfLog[li],
                                    OLED_FONT_06, 1U);
                }
            }

            OLED_Refresh();
            HAL_Delay(pageMs);
        }
    }
#else
    (void)pageMs;
    (void)loops;
#endif
}

/** 只显示最后一页（结论页），不占用时间。测试完会自动调一次 */
void TF_ShowLastPage(void)
{
#if TF_USE_OLED
    uint16_t total;
    uint16_t p;
    uint8_t  i;

    if (s_tfLogN == 0U)
    {
        return;
    }

    total = (uint16_t)((s_tfLogN + TF_LOG_ROWS - 1U) / TF_LOG_ROWS);
    p     = (uint16_t)(total - 1U);

    OLED_Clear();
    for (i = 0U; i < TF_LOG_ROWS; ++i)
    {
        uint16_t li = (uint16_t)(p * TF_LOG_ROWS + i);
        if (li < s_tfLogN)
        {
            OLED_ShowString(0U, (uint8_t)(i * 8U), s_tfLog[li],
                            OLED_FONT_06, 1U);
        }
    }
    OLED_Refresh();
#endif
}

/** 取日志行数 / 取第 n 行文本（自己想在别处显示时用） */
uint16_t TF_LogLines(void)
{
    return s_tfLogN;
}

const char *TF_LogLine(uint16_t n)
{
    if (n >= s_tfLogN)
    {
        return "";
    }
    return s_tfLog[n];
}

/* ---------------- 主测试 ---------------- */

/**
 * 完整测试。报告攒进日志缓冲，结束后在 OLED 上显示结论页。
 *
 * @param doWriteTest 0 = 只读测试（**绝不写卡，绝对安全**）
 *                    1 = 额外做写回测试：只写【卡的最后一个扇区】，
 *                        且先备份原内容、验证完再写回，不会破坏数据。
 * @return 0 = 全部通过；非 0 = 失败的检查项个数
 *
 * ★ 卡里**不需要放任何文件**，也不需要特定格式 —— 驱动是扇区级的。
 *   第 [2][3] 步是去读卡上本来就有的 MBR 和分区引导扇区，
 *   用来证明"读回来的数据真实有效"。空卡也能跑，只是没有 MBR 而已。
 *
 * ★ 想看完整报告：测试完调一次 TF_ShowLog(2500, 2) 就会分页轮播。
 *   注意那是阻塞的，别放在等上位机配置握手的那段流程里。
 */
uint8_t TF_Test(uint8_t doWriteTest)
{
    uint8_t   fail = 0U;
    uint8_t   i;
    uint32_t  t0;
    uint32_t  us;
    uint32_t  usSum;
    uint32_t  kbps;
    uint32_t  blk;
    uint32_t  k;
    uint32_t  partLba;
    TF_Status st;

    s_tfLogN = 0U;
    lb_reset();
    tf_dwt_init();

    lb_str("=== TF CARD TEST ===");
    lb_str("--- card ---");

    /* ---------------- [1] 初始化 ---------------- */
    t0 = tf_dwt_now();
    st = TF_Init();
    us = tf_dwt_us(t0);

    if (st != TF_OK)
    {
        lb_kv("1 init", "FAIL!");
        lb_kv("  result", tf_status_short(st));
        lb_kv_u("  step", g_tfStep);
        lb_kv_x("  SDIO STA", g_tfLastSta);
        lb_kv_x("  resp", g_tfLastResp);
        lb_kv_u("  srcline", g_tfErrLine);
        lb_str("--- result ---");
        lb_str("RESULT: FAIL");
        TF_ShowLastPage();
        return 1U;
    }

    lb_kv("1 init", "OK");
    lb_kv("type", (g_tf.cardType == TF_CARD_SDHC) ? "SDHC/SDXC" :
                  (g_tf.cardType == TF_CARD_SDSC) ? "SDSC" : "unknown");
    lb_kv_u("size(MB)", g_tf.capacityKB / 1024U);
    lb_kv_u("blocks", g_tf.blockCount);
    lb_kv("bus", g_tf.bus4bit ? "4bit" : "1bit");
    lb_kv_u("clock MHz", g_tf.sdioClkHz / 1000000U);
    lb_kv("highspeed", g_tf.highSpeedMode ? "yes" : "no");
    lb_kv("datapath", g_tf.dmaUsed ? "DMA" : "CPU");
    lb_kv_u("inittime us", us);
    lb_str("--- test ---");

    /* ---------------- [2] 读 0 号扇区 ---------------- */
    st = TF_ReadBlockRetry(0U, s_tfBuf, 3U);
    if (st != TF_OK)
    {
        lb_kv("2 mbr", "FAIL!");
        ++fail;
        partLba = 0U;
    }
    else
    {
        partLba = ((uint32_t)s_tfBuf[446U + 8U])
                | ((uint32_t)s_tfBuf[446U + 9U]  << 8U)
                | ((uint32_t)s_tfBuf[446U + 10U] << 16U)
                | ((uint32_t)s_tfBuf[446U + 11U] << 24U);

        if ((s_tfBuf[510] == 0x55U) && (s_tfBuf[511] == 0xAAU))
        {
            lb_kv("2 mbr", "OK 55AA");
        }
        else
        {
            lb_kv("2 mbr", "OK no MBR");
        }
        lb_kv_u("  part LBA", partLba);
    }

    /* ---------------- [3] 分区引导扇区 ---------------- */
    if ((partLba == 0U) || (partLba >= g_tf.blockCount))
    {
        lb_kv("3 fs", "skip");
    }
    else
    {
        st = TF_ReadBlockRetry(partLba, s_tfRef, 3U);
        if (st == TF_OK)
        {
            lb_kv("3 fs", tf_fs_name(s_tfRef));
        }
        else
        {
            lb_kv("3 fs", "FAIL!");
            ++fail;
        }
    }

    /* ---------------- [4] 重复读一致性 ---------------- */
    st = TF_ReadBlockRetry(0U, s_tfBuf, 3U);
    if (st == TF_OK)
    {
        for (i = 0U; i < 3U; ++i)
        {
            if (TF_ReadBlockRetry(0U, s_tfRef, 3U) != TF_OK)
            {
                break;
            }
            if (memcmp(s_tfBuf, s_tfRef, TF_BLOCK_SIZE) != 0)
            {
                break;
            }
        }
        if (i == 3U)
        {
            lb_kv("4 repeat", "OK");
        }
        else
        {
            lb_kv("4 repeat", "MISMATCH");
            ++fail;
        }
    }
    else
    {
        lb_kv("4 repeat", "FAIL!");
        ++fail;
    }

    /* ---------------- [5] 吞吐测试 ----------------
       ★ 分块读：一次 TF_PERF_BLOCKS 块，读 TF_PERF_ROUNDS 次 = 128KB。
         别改成一次读 256 块 —— 那要 128KB 缓冲，而 s_tfBuf 只有 512B。 */
    usSum = 0U;
    for (i = 0U; i < 3U; ++i)
    {
        t0 = tf_dwt_now();
        st = TF_OK;
        for (k = 0U; k < TF_PERF_ROUNDS; ++k)
        {
            st = TF_ReadBlocks(0U, s_tfBig, TF_PERF_BLOCKS);
            if (st != TF_OK) { break; }
        }
        us = tf_dwt_us(t0);
        usSum += us;

        if (st != TF_OK)
        {
            ++fail;
        }
    }

    if (usSum == 0U)
    {
        usSum = 1U;
    }
    /* 一轮 = TF_PERF_ROUNDS x TF_PERF_BLOCKS x 512B。 KB/s = 字节*1000/微秒 */
    kbps = ((uint32_t)TF_PERF_ROUNDS * TF_PERF_BLOCKS * TF_BLOCK_SIZE * 1000U)
         / (usSum / 3U);

    if (fail == 0U)
    {
        lb_kv("5 speed", "");
        lb_putu(kbps);
        lb_puts("KB/s");
        lb_emit();
    }
    else
    {
        lb_kv("5 speed", "FAIL!");
    }

    /* ---------------- [6] 写回测试（可选）---------------- */
    if (doWriteTest == 0U)
    {
        lb_kv("6 write", "skip");
    }
    else
    {
        uint8_t wfail = 0U;
        uint32_t j;

        blk = g_tf.blockCount - 1U;

        st = TF_ReadBlockRetry(blk, s_tfSaved, 3U);
        if (st != TF_OK)
        {
            lb_kv("6 write", "bkup FAIL");
            ++fail;
            ++wfail;
        }

        if (wfail == 0U)
        {
            for (j = 0U; j < TF_BLOCK_SIZE; ++j)
            {
                s_tfBuf[j] = (uint8_t)((j * 7U + 0xA5U) & 0xFFU);
            }

            st = TF_WriteBlocks(blk, s_tfBuf, 1U);
            if (st != TF_OK)
            {
                lb_kv("6 write", "w FAIL!");
                ++fail;
                ++wfail;
            }
        }

        if (wfail == 0U)
        {
            memset(s_tfRef, 0, TF_BLOCK_SIZE);
            st = TF_ReadBlockRetry(blk, s_tfRef, 3U);
            if ((st == TF_OK) && (memcmp(s_tfBuf, s_tfRef, TF_BLOCK_SIZE) == 0))
            {
                lb_kv("6 write", "OK");
            }
            else
            {
                lb_kv("6 write", "VFY FAIL");
                ++fail;
            }
        }

        /* 无论成败都把原内容写回去 */
        if (TF_WriteBlocks(blk, s_tfSaved, 1U) == TF_OK)
        {
            lb_kv("  restore", "OK");
        }
        else
        {
            lb_kv("  restore", "FAIL!");
            ++fail;
        }
    }

    /* ---------------- 汇总 ---------------- */
    lb_str("--- result ---");
    if (fail == 0U)
    {
        lb_str("RESULT: ALL PASS");
    }
    else
    {
        lb_str("RESULT: FAIL");
        lb_kv_u("fail items", fail);
    }

    TF_ShowLastPage();
    return fail;
}

#endif /* TF_INCLUDE_TEST */
