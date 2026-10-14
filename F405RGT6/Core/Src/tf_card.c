/* ============================================================================
 * tf_card.c —— TF(SD) 卡驱动（HAL_SD 版）
 *
 * ★ 这一版是 2026-10-14 从"纯寄存器版 SDIO 驱动"改写过来的：
 *   原来那版自己发 CMD0/8/41/2/3/9/7/16/6、自己解析 CSD、自己配 DMA，
 *   踩了一串坑（R2/R3 没有 CRC7、CSD v1.0 的位域、DMA 的 FIFO 残余……）。
 *   现在改成直接用 ST 的 HAL_SD，那些坑由 HAL + CubeMX 生成的 MSP 兜住。
 *   旧版可以从 git 取回：git show b0e6c05:F405RGT6/Core/Src/tf_card.c
 *
 * ★ 对外接口（tf_card.h）基本没变，所以 test.c 的测试台 / OLED 页面
 *   不需要大改，只是部分诊断量的含义变了（见下面）。
 *
 * ★ 三个 HAL_SD 使用要点（踩过才知道）：
 *   1. HAL_SD_Init() 一口气把识别全做完（CMD0→…→CMD16），
 *      结果放在 hsd.SdCard 里（CardType / LogBlockNbr / CSD[] …）。
 *      ★ 所以 g_tfStep 的粒度变粗了：识别阶段只能标到 CLK_INIT，
 *        想看"卡在哪一步"要看 g_tfLastSta（= HAL_SD 的 ErrorCode）。
 *   2. HAL_SD_ReadBlocks() 是【阻塞轮询】版；HAL_SD_ReadBlocks_DMA() 是
 *      【异步】版，靠 SDIO/DMA 中断收尾。本文件用 TF_USE_HAL_DMA 切换，
 *      默认走 DMA（因为工程里 DMA2 + 两个中断都已经配好了）。
 *   3. 4 位总线要另外调 HAL_SD_ConfigWideBusOperation()，
 *      HAL_SD_Init() 本身不会切。
 *
 * ★ SDIO_CK 速度：HAL_SD 没有"运行时改时钟"的 API，本文件直接改
 *   SDIO->CLKCR 的 CLKDIV（保留 WIDBUS/NEGEDGE/PWRSAV 位）。
 *   ★ 本版【不做】CMD6 高速模式切换，所以最高只到 24MHz
 *     （Default Speed 的上限就是 25MHz，不切模式跑 48MHz 不合规范）。
 *     g_tf.highSpeedMode 因此恒为 0，报告里 HS 显示 "--"。
 * ========================================================================== */

#include "tf_card.h"
#include "sdio.h"       /* hsd；HAL_SD_MspInit（时钟/GPIO/DMA/中断）都在 sdio.c 里 */
#if TF_USE_OLED
#include "oled.h"
#endif
#include <string.h>

/* ==========================================================================
 * 诊断量（名字和寄存器版保持一致，test.c / OLED 直接读）
 * ========================================================================== */

TF_Info           g_tf;
volatile uint32_t g_tfStep     = TF_STEP_NONE;
volatile uint32_t g_tfLastSta  = 0U;    /* ★ HAL_SD 版：这里是 hsd.ErrorCode，不是 SDIO->STA */
volatile uint32_t g_tfErrLine  = 0U;
volatile uint32_t g_tfLastResp = 0U;    /* SDIO->RESP1 原值（HAL 不暴露，直接读寄存器） */
volatile uint32_t g_tfDmaLeft  = 0U;    /* HAL_SD 版不用：DMA 由 HAL 管，没有残余补数这回事 */
volatile uint32_t g_tfDmaFifo  = 0U;
volatile uint32_t g_tfDmaIsr   = 0U;

#define TF_MARK(line)   do { g_tfErrLine = (uint32_t)(line); } while (0)

/* SDIOCLK（F4 上 SDIO 的时钟源只能是 PLL48CLK），开机算一次 */
static uint32_t s_sdioClkSrcHz = 48000000U;

/* 自动协商的顺序：从快到慢。★ 不含 48MHz，理由见文件头。 */
static const uint8_t s_autoOrder[5] = {
    TF_SPEED_24M, TF_SPEED_16M, TF_SPEED_12M, TF_SPEED_8M, TF_SPEED_4M
};

/* ==========================================================================
 * 小工具
 * ========================================================================== */

/** 推算 SDIOCLK = PLL48CLK = VCO / PLLQ，VCO = 源 / PLLM * PLLN */
static uint32_t tf_sdioclk_hz(void)
{
    const uint32_t cfgr = RCC->PLLCFGR;
    uint32_t       src_hz;
    uint32_t       pllm;
    uint32_t       plln;
    uint32_t       pllq;
    uint32_t       vco;

#if defined(HSE_VALUE) && (HSE_VALUE > 0U)
    src_hz = ((cfgr & RCC_PLLCFGR_PLLSRC_HSE) != 0U) ? (uint32_t)HSE_VALUE : 16000000U;
#else
    src_hz = ((cfgr & RCC_PLLCFGR_PLLSRC_HSE) != 0U) ? 8000000U : 16000000U;
#endif

    pllm = (cfgr & RCC_PLLCFGR_PLLM) >> RCC_PLLCFGR_PLLM_Pos;
    plln = (cfgr & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
    pllq = (cfgr & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos;

    if ((pllm == 0U) || (plln == 0U))
    {
        return 48000000U;               /* 说不通就按常规值兜底 */
    }
    if (pllq == 0U)
    {
        pllq = 1U;
    }

    vco = (src_hz / pllm) * plln;

    return vco / pllq;
}

/** 档位 -> CLKDIV（bypass=1 表示走 BYPASS，此时 CLKDIV 无意义） */
static uint32_t tf_div_of(uint8_t level, uint8_t *bypass)
{
    *bypass = 0U;

    switch (level)
    {
    case TF_SPEED_24M:   return 0U;
    case TF_SPEED_16M:   return 1U;
    case TF_SPEED_12M:   return 2U;
    case TF_SPEED_8M:    return 4U;
    case TF_SPEED_4M:    return 10U;
    case TF_SPEED_400K:  return TF_CLKDIV_INIT;
    default:             return TF_CLKDIV_INIT;
    }
}

/**
 * 直接改 SDIO->CLKCR 里的时钟分频。
 * ★ HAL_SD 没有"运行时改 SDIO_CK"的 API，只能自己动寄存器。
 *   必须【保留】其它位（WIDBUS 总线宽度 / NEGEDGE 相位 / PWRSAV / CLKEN），
 *   只替换 CLKDIV —— 不然会把 4 位总线配置顺手清掉。
 */
static void tf_apply_clk(uint32_t div, uint8_t bypass)
{
    uint32_t cr = SDIO->CLKCR;

    cr &= ~(SDIO_CLKCR_CLKDIV | SDIO_CLKCR_BYPASS);
    cr |= (div & 0xFFU);
    if (bypass != 0U)
    {
        cr |= SDIO_CLKCR_BYPASS;
    }
    SDIO->CLKCR = cr;

    /* 让 HAL 的实例也跟上（万一后面有人再调 HAL_SD_Init） */
    hsd.Init.ClockDiv    = div;
    hsd.Init.ClockBypass = (bypass != 0U) ? SDIO_CLOCK_BYPASS_ENABLE
                                          : SDIO_CLOCK_BYPASS_DISABLE;

    /* 记进 g_tf 供报告用 */
    g_tf.clkDiv       = div;
    g_tf.bypass       = bypass;
    g_tf.sdioClkSrcHz = s_sdioClkSrcHz;
    g_tf.sdioClkHz    = (bypass != 0U) ? s_sdioClkSrcHz
                                       : (s_sdioClkSrcHz / (div + 2U));
}

/** HAL_SD 的 ErrorCode -> TF_Status */
static TF_Status tf_map_error(void)
{
    const uint32_t e = hsd.ErrorCode;

    if ((e & (HAL_SD_ERROR_CMD_RSP_TIMEOUT | HAL_SD_ERROR_TIMEOUT)) != 0U)
    {
        return TF_ERR_CMD_TIMEOUT;
    }
    if ((e & HAL_SD_ERROR_CMD_CRC_FAIL) != 0U)
    {
        return TF_ERR_CMD_CRC;
    }
    if ((e & HAL_SD_ERROR_DATA_TIMEOUT) != 0U)
    {
        return TF_ERR_DATA_TIMEOUT;
    }
    if ((e & HAL_SD_ERROR_DATA_CRC_FAIL) != 0U)
    {
        return TF_ERR_DATA_CRC;
    }
    if ((e & (HAL_SD_ERROR_RX_OVERRUN | HAL_SD_ERROR_TX_UNDERRUN |
              HAL_SD_ERROR_DMA)) != 0U)
    {
        return TF_ERR_DATA_FIFO;
    }
    if ((e & HAL_SD_ERROR_UNSUPPORTED_FEATURE) != 0U)
    {
        return TF_ERR_BUS_WIDTH;
    }
    if ((e & HAL_SD_ERROR_ADDR_OUT_OF_RANGE) != 0U)
    {
        return TF_ERR_ADDR_RANGE;
    }
    if ((e & HAL_SD_ERROR_ILLEGAL_CMD) != 0U)
    {
        return TF_ERR_NOT_SD;
    }
    if ((e & HAL_SD_ERROR_INVALID_VOLTRANGE) != 0U)
    {
        return TF_ERR_VOLTAGE;
    }
    return TF_ERR_CMD_RESP;
}

/** 把 hsd.SdCard 里的信息搬进 g_tf */
static void tf_fill_info(void)
{
    const HAL_SD_CardInfoTypeDef *c = &hsd.SdCard;

    if (c->CardType == CARD_SDHC_SDXC)
    {
        g_tf.cardType = TF_CARD_SDHC;
    }
    else if (c->CardType == CARD_SDSC)
    {
        g_tf.cardType = TF_CARD_SDSC;
    }
    else
    {
        g_tf.cardType = TF_CARD_UNKNOWN;
    }

    g_tf.isV2       = (c->CardVersion >= CARD_V2_X) ? 1U : 0U;
    g_tf.rca        = (uint16_t)c->RelCardAdd;
    g_tf.blockCount = c->LogBlockNbr;               /* 逻辑容量，单位 512B 块 */
    g_tf.capacityKB = (uint32_t)(c->LogBlockNbr / 2U);

    /* 原始 CSD 留一份，容量可疑时可以手工解码核对。
       ★ 注意 CSD[] 是在 SD_HandleTypeDef 里（hsd.CSD），不在 hsd.SdCard 里。 */
    g_tf.csd[0] = hsd.CSD[0];
    g_tf.csd[1] = hsd.CSD[1];
    g_tf.csd[2] = hsd.CSD[2];
    g_tf.csd[3] = hsd.CSD[3];

    g_tf.dmaUsed = (uint8_t)TF_USE_HAL_DMA;
}

/** 等一次异步传输收尾（DMA 版用）：靠 HAL 在中断里把 State 置回 READY */
static TF_Status tf_wait_transfer(uint32_t to_ms)
{
    const uint32_t t0 = HAL_GetTick();

    while (hsd.State == HAL_SD_STATE_BUSY)
    {
        if ((HAL_GetTick() - t0) > to_ms)
        {
            TF_MARK(__LINE__);
            g_tfLastSta = HAL_SD_GetError(&hsd);
            (void)HAL_SD_Abort(&hsd);
            return TF_ERR_DATA_TIMEOUT;
        }
    }

    if (hsd.ErrorCode != HAL_SD_ERROR_NONE)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_Abort(&hsd);
        return tf_map_error();
    }

    return TF_OK;
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */

TF_Status TF_Init(void)
{
    HAL_StatusTypeDef hs;

    if (g_tf.inited != 0U)
    {
        return TF_OK;
    }

    /* ---- 清状态 ---- */
    (void)memset(&g_tf, 0, sizeof(g_tf));
    g_tf.cardType   = TF_CARD_UNKNOWN;
    g_tf.speedLevel = TF_SPEED_AUTO;
    g_tf.speedAuto  = 1U;

    g_tfLastSta  = 0U;
    g_tfLastResp = 0U;
    g_tfErrLine  = 0U;
    g_tfDmaLeft  = 0U;
    g_tfDmaFifo  = 0U;
    g_tfDmaIsr   = 0U;

    s_sdioClkSrcHz = tf_sdioclk_hz();

    /* ★ GPIO / SDIO 时钟 / DMA2 / NVIC 全在 HAL_SD_MspInit() 里（sdio.c），
         而它是由 HAL_SD_Init() 内部回调的 —— 所以这里不用手动配。
       ★ 本驱动【不走】MX_SDIO_SD_Init()：那个函数失败会直接进 Error_Handler()
         停机，而驱动需要能优雅返回错误码给测试台显示。 */
    g_tfStep = TF_STEP_GPIO;

    g_tfStep = TF_STEP_POWER;

    hsd.Instance                 = SDIO;
    hsd.Init.ClockEdge           = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass         = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave      = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide             = SDIO_BUS_WIDE_1B;    /* 先 1 位识别 */
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd.Init.ClockDiv            = TF_CLKDIV_INIT;      /* 400kHz（规范要求） */

    g_tfStep = TF_STEP_CLK_INIT;

    hs = HAL_SD_Init(&hsd);
    if (hs != HAL_OK)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_DeInit(&hsd);
        return tf_map_error();
    }

    /* ---- 切 4 位总线 ---- */
    g_tfStep = TF_STEP_ACMD6;

    hs = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B);
    if (hs != HAL_OK)
    {
        /* 切不过去不算致命：退回 1 位继续用，只是慢一点 */
        TF_MARK(__LINE__);
        g_tfLastSta = HAL_SD_GetError(&hsd);
        g_tf.bus4bit = 0U;
    }
    else
    {
        g_tf.bus4bit = 1U;
    }

    /* ---- 把卡信息搬进来 ---- */
    tf_fill_info();

    g_tf.inited = 1U;
    g_tfStep    = TF_STEP_READY;

    /* ---- 提速：从快到慢试，每档真读一次 0 号扇区来验证 ---- */
    (void)TF_SetSpeed(TF_SPEED_AUTO);

    return TF_OK;
}

TF_Status TF_EnsureInit(void)
{
    if (g_tf.inited != 0U)
    {
        return TF_OK;
    }
    return TF_Init();
}

void TF_DeInit(void)
{
    if (g_tf.inited != 0U)
    {
        (void)HAL_SD_DeInit(&hsd);
    }
    g_tf.inited = 0U;
    g_tfStep    = TF_STEP_NONE;
}

/* ==========================================================================
 * 读写
 * ========================================================================== */

TF_Status TF_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count)
{
    TF_Status st;
    uint32_t  to_ms;

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

    to_ms = count * TF_DATA_TIMEOUT_PER_BLOCK_MS;
    if (to_ms < TF_DATA_TIMEOUT_MIN_MS)
    {
        to_ms = TF_DATA_TIMEOUT_MIN_MS;
    }

#if TF_USE_HAL_DMA
    /* 异步版：靠 SDIO/DMA 中断收尾，返回后要等 State 回到 READY */
    if (HAL_SD_ReadBlocks_DMA(&hsd, buf, block, count) != HAL_OK)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_Abort(&hsd);
        return tf_map_error();
    }
    st = tf_wait_transfer(to_ms);
    if (st != TF_OK)
    {
        return st;
    }
#else
    /* 阻塞轮询版：CPU 自己搬 FIFO，没有中断依赖，最稳但慢一些 */
    if (HAL_SD_ReadBlocks(&hsd, buf, block, count, to_ms) != HAL_OK)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_Abort(&hsd);
        return tf_map_error();
    }
#endif

    /* 收尾：确认卡回到 tran 状态（HAL 的惯例） */
    if (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
    {
        TF_MARK(__LINE__);
        g_tfLastSta = HAL_SD_GetError(&hsd);
        (void)HAL_SD_Abort(&hsd);
        return TF_ERR_DATA_TIMEOUT;
    }

    return TF_OK;
}

TF_Status TF_WriteBlocks(uint32_t block, const uint8_t *buf, uint32_t count)
{
    TF_Status st;
    uint32_t  to_ms;

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

    to_ms = count * TF_DATA_TIMEOUT_PER_BLOCK_MS;
    if (to_ms < TF_DATA_TIMEOUT_MIN_MS)
    {
        to_ms = TF_DATA_TIMEOUT_MIN_MS;
    }

#if TF_USE_HAL_DMA
    if (HAL_SD_WriteBlocks_DMA(&hsd, (uint8_t *)buf, block, count) != HAL_OK)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_Abort(&hsd);
        return tf_map_error();
    }
    st = tf_wait_transfer(to_ms);
    if (st != TF_OK)
    {
        return st;
    }
#else
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buf, block, count, to_ms) != HAL_OK)
    {
        TF_MARK(__LINE__);
        g_tfLastSta  = HAL_SD_GetError(&hsd);
        g_tfLastResp = SDIO->RESP1;
        (void)HAL_SD_Abort(&hsd);
        return tf_map_error();
    }
#endif

    if (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
    {
        TF_MARK(__LINE__);
        g_tfLastSta = HAL_SD_GetError(&hsd);
        (void)HAL_SD_Abort(&hsd);
        return TF_ERR_DATA_TIMEOUT;
    }

    return TF_OK;
}

TF_Status TF_ReadBlockRetry(uint32_t block, uint8_t *buf, uint32_t retry)
{
    TF_Status st = TF_ERR_PARAM;
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
            break;
        }
        HAL_Delay(2U);              /* 让卡喘口气再重试 */
    }

    return st;
}

TF_Status TF_WriteBlockRetry(uint32_t block, const uint8_t *buf, uint32_t retry)
{
    TF_Status st = TF_ERR_PARAM;
    uint32_t  i;

    if (retry == 0U)
    {
        retry = 1U;
    }

    for (i = 0U; i < retry; ++i)
    {
        st = TF_WriteBlocks(block, buf, 1U);
        if (st == TF_OK)
        {
            break;
        }
        HAL_Delay(2U);
    }

    return st;
}

/* ==========================================================================
 * 查询
 * ========================================================================== */

uint32_t TF_BlockCount(void)  { return g_tf.blockCount; }
uint32_t TF_CapacityKB(void)  { return g_tf.capacityKB; }
uint8_t  TF_IsReady(void)     { return g_tf.inited; }
uint8_t  TF_IsBus4Bit(void)   { return g_tf.bus4bit; }
uint32_t TF_SdioClockHz(void) { return g_tf.sdioClkHz; }

const char *TF_StatusStr(TF_Status st)
{
    switch (st)
    {
    case TF_OK:                 return "OK";
    case TF_ERR_PARAM:          return "param";
    case TF_ERR_NOT_INIT:       return "not init";
    case TF_ERR_CMD_TIMEOUT:    return "cmd timeout";
    case TF_ERR_CMD_CRC:        return "cmd crc";
    case TF_ERR_CMD_RESP:       return "cmd resp err";
    case TF_ERR_DATA_TIMEOUT:   return "data timeout";
    case TF_ERR_DATA_CRC:       return "data crc";
    case TF_ERR_DATA_FIFO:      return "fifo/startbit";
    case TF_ERR_V2_FAIL:        return "cmd8 v1 card";
    case TF_ERR_VOLTAGE:        return "acmd41 no ready";
    case TF_ERR_NOT_SD:         return "not sd card";
    case TF_ERR_ADDR_RANGE:     return "addr range";
    case TF_ERR_BUS_WIDTH:      return "bus width";
    case TF_ERR_UNSUPPORTED_HS: return "no high speed";
    default:                    return "unknown";
    }
}

const char *TF_SpeedStr(uint8_t level)
{
    switch (level)
    {
    case TF_SPEED_AUTO:  return "AUTO";
    case TF_SPEED_48M:   return "48MHz";
    case TF_SPEED_24M:   return "24MHz";
    case TF_SPEED_16M:   return "16MHz";
    case TF_SPEED_12M:   return "12MHz";
    case TF_SPEED_8M:    return "8MHz";
    case TF_SPEED_4M:    return "4MHz";
    case TF_SPEED_400K:  return "400kHz";
    default:             return "?";
    }
}

uint8_t TF_GetSpeedLevel(void) { return g_tf.speedLevel; }
uint8_t TF_IsDma(void)         { return (uint8_t)TF_USE_HAL_DMA; }

/**
 * 高速模式：★ 本版【不做】CMD6 切换（HAL_SD 没有对应 API），
 * 所以恒为 0 —— 报告里 HS 显示 "--"，最高速度也就到 24MHz。
 */
uint8_t TF_IsHighSpeed(void) { return g_tf.highSpeedMode; }

/* ==========================================================================
 * 速度
 * ========================================================================== */

/**
 * 直接设一个分频值（不验证）。返回实际生效的 SDIO_CK，单位 Hz。
 * 给外部想手动压速度时用；一般用 TF_SetSpeed() 就够了。
 */
uint32_t TF_SetClockDiv(uint32_t div)
{
    if (g_tf.inited == 0U)
    {
        return 0U;
    }

    tf_apply_clk(div & 0xFFU, 0U);
    return g_tf.sdioClkHz;
}

/**
 * 设速度档。TF_SPEED_AUTO = 从快到慢自动协商，每档都真读一次 0 号扇区验证。
 * ★ 为什么不只看理论值：数据阶段有 CRC16，读成功才是这一档可靠的硬证据。
 * ★ 永远不会失败：最差落到 4MHz。
 * 返回最终生效的档位。
 */
uint8_t TF_SetSpeed(uint8_t level)
{
    uint32_t div;
    uint8_t  bypass;
    uint8_t  i;
    uint8_t  buf[TF_BLOCK_SIZE] __attribute__((aligned(4)));

    if (g_tf.inited == 0U)
    {
        return TF_SPEED_AUTO;
    }

    if (level != TF_SPEED_AUTO)
    {
        /* ★ 48MHz 需要先切到高速模式（CMD6），本版没做，直接按 24MHz 处理 */
        if (level == TF_SPEED_48M)
        {
            level = TF_SPEED_24M;
        }

        div = tf_div_of(level, &bypass);
        tf_apply_clk(div, bypass);
        HAL_Delay(1U);

        g_tf.speedLevel = level;
        g_tf.speedAuto  = 0U;
        return level;
    }

    /* ---- 自动协商 ---- */
    g_tfStep        = TF_STEP_SPEED;
    g_tf.speedTriedMask = 0U;
    g_tf.speedAuto  = 1U;

    for (i = 0U; i < (uint8_t)(sizeof(s_autoOrder) / sizeof(s_autoOrder[0])); ++i)
    {
        level = s_autoOrder[i];
        g_tf.speedTriedMask |= (1UL << level);

        div = tf_div_of(level, &bypass);
        tf_apply_clk(div, bypass);
        HAL_Delay(1U);              /* 给卡几个时钟稳定 */

        if (TF_ReadBlocks(0U, buf, 1U) == TF_OK)
        {
            g_tf.speedLevel = level;
            g_tfStep        = TF_STEP_READY;
            return level;
        }

        /* ★ 这一档读失败了。让卡退出可能残留的传输状态，
           否则下一档会带着"卡还在传"的烂摊子继续，必然一起失败。 */
        (void)HAL_SD_Abort(&hsd);
        HAL_Delay(2U);
    }

    /* 全不行：退回最保守的档 */
    div = tf_div_of(TF_SPEED_4M, &bypass);
    tf_apply_clk(div, bypass);
    HAL_Delay(1U);
    (void)TF_ReadBlocks(0U, buf, 1U);

    g_tf.speedLevel = TF_SPEED_4M;
    g_tfStep        = TF_STEP_READY;
    return TF_SPEED_4M;
}

/* ==========================================================================
 * 自检 / 显示
 * ========================================================================== */

TF_Status TF_SelfTest(TF_SelfTestResult *res)
{
    TF_Status st;
    uint8_t   blk[TF_BLOCK_SIZE] __attribute__((aligned(4)));
    uint32_t  i;
    uint32_t  zeros = 0U;
    uint8_t   all_ff = 1U;
    uint8_t   all_00 = 1U;

    if (res == 0)
    {
        return TF_ERR_PARAM;
    }

    (void)memset(res, 0, sizeof(*res));

    st = TF_EnsureInit();
    if (st != TF_OK)
    {
        return st;
    }

    st = TF_ReadBlockRetry(0U, blk, 3U);
    if (st != TF_OK)
    {
        return st;
    }

    for (i = 0U; i < TF_BLOCK_SIZE; ++i)
    {
        if (blk[i] != 0xFFU) { all_ff = 0U; }
        if (blk[i] != 0x00U) { all_00 = 0U; }
    }

    for (i = 0U; i < (TF_BLOCK_SIZE / 4U); ++i)
    {
        uint32_t w = (uint32_t)blk[i * 4U]
                   | ((uint32_t)blk[i * 4U + 1U] << 8U)
                   | ((uint32_t)blk[i * 4U + 2U] << 16U)
                   | ((uint32_t)blk[i * 4U + 3U] << 24U);
        if (w == 0U) { ++zeros; }
    }

    res->zeroWords   = zeros;
    res->firstByteFF = all_ff;
    res->firstByte00 = all_00;

    /* MBR：签名 0x55AA 在 510/511；分区表第一项在 446，起始 LBA 在 454 */
    res->mbrOk   = ((blk[510] == 0x55U) && (blk[511] == 0xAAU)) ? 1U : 0U;
    res->partType = blk[446U + 4U];
    res->partLba  = ((uint32_t)blk[446U + 8U])
                  | ((uint32_t)blk[446U + 9U]  << 8U)
                  | ((uint32_t)blk[446U + 10U] << 16U)
                  | ((uint32_t)blk[446U + 11U] << 24U);

    return TF_OK;
}

#if TF_USE_OLED
/** 在 OLED 第 y 行打一行自检结果（8x16 字体，最多 16 字符） */
void TF_OledShow(TF_Status st, uint8_t y)
{
    OLED_ShowString(0U, y, (st == TF_OK) ? "TF: OK" : "TF: ",
                    OLED_FONT_08, 1U);

    if (st != TF_OK)
    {
        OLED_ShowString(32U, y, TF_StatusStr(st), OLED_FONT_08, 1U);
    }

    (void)OLED_Refresh();
}
#endif
