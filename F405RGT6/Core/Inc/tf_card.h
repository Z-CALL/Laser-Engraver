/**
  ******************************************************************************
  * @file    tf_card.h
  * @brief   TF(microSD) 卡槽驱动 —— 纯寄存器版 SDIO，4 位总线，全轮询
  *
  * 【为什么不用 HAL 的 SD 驱动】
  *   本工程 stm32f4xx_hal_conf.h 里 HAL_SD_MODULE_ENABLED 是注释掉的，
  *   且 stm32f4xx_hal_sd.c / stm32f4xx_ll_sdmmc.c 都没加进 Keil 工程，
  *   编进去也是空的。所以这里直接操作 SDIO 寄存器，只依赖 CMSIS 和
  *   基础 HAL（HAL_GetTick），不需要改工程里任何其它文件。
  *
  * 【本文件对工程其它部分的侵入 —— 零】
  *   只做三件事，且都不影响现有功能：
  *     1. 打开 SDIO 时钟（RCC->APB2ENR.SDIOEN）—— 该外设原先没人用
  *     2. 打开 GPIOD 时钟（若原先没开）—— 只是为了配 PD2
  *     3. 把 PC8~PC12、PD2 配成 AF12(SDIO)
  *   已核对 configure.h：PC0~PC5、PA8/PA15、PB13~PB15 是现有占用，
  *   PC8~PC12 与 PD2 全部空闲，无冲突。
  *
  * 【引脚映射（对应 TF 卡槽原理图）】
  *   CLK  -> PC12   (SDIO_CK)
  *   CMD  -> PD2    (SDIO_CMD)
  *   DAT0 -> PC8    (SDIO_D0)
  *   DAT1 -> PC9    (SDIO_D1)
  *   DAT2 -> PC10   (SDIO_D2)
  *   CD/DAT3 -> PC11(SDIO_D3, 同时是卡片选择线)
  *   CD   -> 未连接，本驱动不依赖卡检测
  *   注意：4 位模式要求 DAT1/DAT2 也接上 —— 本板已接，故用 4 位。
  *
  * 【时钟】
  *   SDIOCLK = PLL48CLK。本工程 HSE=8M, PLLM=4, PLLN=168, PLLQ=7
  *           => VCO = 8M/4*168 = 336MHz => PLL48CLK = 336/7 = 48MHz
  *   SDIO_CK = SDIOCLK / (CLKDIV + 2)
  *   CLKDIV 在运行时由 RCC->PLLCFGR 反推，不写死，换时钟树也不会错。
  *
  * 【用法（三个调用点，都在你自己的 main.c 里加，本驱动不代劳）】
  *   1) 头文件：  #include "tf_card.h"
  *   2) 初始化：  TF_Status st = TF_Init();          // 放在外设都初始化完之后
  *   3) 读写：    TF_ReadBlocks(0, buf512, 1);       // 读 0 号扇区
  *
  *   ★ 所有 API 内部都会自动补齐初始化（TF_EnsureInit），
  *     所以就算忘了先调 TF_Init() 也不会出错，最多多花一次识别时间。
  *
  *   ★ Keil 里记得把 tf_card.c 加进工程（右键 Source Group -> Add Existing Files）。
  ******************************************************************************
  */

#ifndef __TF_CARD_H
#define __TF_CARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ==========================================================================
 * 引脚映射（只在本驱动内部使用，不碰 configure.h 里的任何宏）
 * ========================================================================== */
#define TF_PORT_CLK     GPIOC
#define TF_PIN_CLK      GPIO_PIN_12     /* SDIO_CK  */

#define TF_PORT_CMD     GPIOD
#define TF_PIN_CMD      GPIO_PIN_2      /* SDIO_CMD */

#define TF_PORT_D0      GPIOC
#define TF_PIN_D0       GPIO_PIN_8      /* SDIO_D0  */

#define TF_PORT_D1      GPIOC
#define TF_PIN_D1       GPIO_PIN_9      /* SDIO_D1  */

#define TF_PORT_D2      GPIOC
#define TF_PIN_D2       GPIO_PIN_10     /* SDIO_D2  */

#define TF_PORT_D3      GPIOC
#define TF_PIN_D3       GPIO_PIN_11     /* SDIO_D3 / CD */

/* ==========================================================================
 * 参数
 * ========================================================================== */

/* 一个扇区固定 512 字节 */
#define TF_BLOCK_SIZE       512U

/* 识别阶段 SDIO_CK 必须 <= 400kHz（SD 规范要求），先用最小速度把卡认出来 */
#define TF_CLKDIV_INIT      118U        /* 48MHz / (118+2) = 400 kHz */

/* 正式读写的目标档位（TF_SPEED_AUTO 时从快到慢自动协商） */
#define TF_CLKDIV_RUN       0U          /* 兜底默认值，实际由 TF_SetSpeed 决定 */

/* ==========================================================================
 * ★ 速度档位（关于"能不能跑到 48MHz"）
 *
 *   SDIO_CK = SDIOCLK / (CLKDIV + 2)，本工程 SDIOCLK = PLL48CLK = 48MHz
 *     CLKDIV=0  -> 24MHz   ← 用分频器能达到的最高值
 *     CLKDIV=1  -> 16MHz
 *     CLKDIV=2  -> 12MHz
 *     CLKDIV=4  ->  8MHz
 *   想更高只有一条路：开 BYPASS（CLKCR.BYPASS=1），让 SDIOCLK 直接送给卡
 *     → SDIO_CK = 48MHz
 *
 *   ★★ 但 48MHz 有个硬前提：卡必须先在【高速模式】下。
 *      SD 规范里 Default Speed 模式的上限就是 25MHz，
 *      High Speed 模式才允许到 50MHz，而切模式要靠 CMD6(SWITCH_FUNC)。
 *      本驱动会自动发 CMD6，不会硬闯。
 *
 *   ★★ 另外一个现实问题：48MHz 对走线很挑。杜邦线/长排线跑 48MHz
 *      基本必丢 CRC。所以默认用 AUTO 自动协商：从 48MHz 往下试，
 *      哪一档能连续读出正确数据就用哪一档。
 * ========================================================================== */
#define TF_SPEED_AUTO       0U      /* 自动协商（默认，推荐） */
#define TF_SPEED_48M        1U      /* BYPASS，需高速模式 */
#define TF_SPEED_24M        2U      /* CLKDIV=0 */
#define TF_SPEED_16M        3U      /* CLKDIV=1 */
#define TF_SPEED_12M        4U      /* CLKDIV=2 */
#define TF_SPEED_8M         5U      /* CLKDIV=4 */
#define TF_SPEED_4M         6U      /* CLKDIV=10 */
#define TF_SPEED_400K       7U      /* CLKDIV=118，识别档 */
#define TF_SPEED_LEVELS     8U

/* ==========================================================================
 * DMA
 *   STM32F405 上 SDIO 只能挂 DMA2：
 *       SDIO_RX -> DMA2_Stream3, Channel 4（卡 → 内存）
 *       SDIO_TX -> DMA2_Stream6, Channel 4（内存 → 卡）
 *   本工程 DMA1 的 Stream1/3 被 USART3 占了，DMA2 完全空闲。
 *
 *   ★ 这里用纯寄存器配 DMA，且【不注册中断】——
 *     DMA 中断服务函数要写进 stm32f4xx_it.c，本驱动的约定是不动它。
 *     轮询 SDIO 自己的 DATAEND/DCRCFAIL 标志一样准确。
 *
 *   改成 0 就退回原来的纯 CPU 轮询搬数（慢一些，但少一层依赖）。 */
#ifndef TF_USE_DMA
#define TF_USE_DMA          1
#endif

/* DMA 优先级。SDIO 数据流对时序敏感，给次高（避免抢占已有外设的中断优先级） */
#define TF_DMA_PRIORITY     2U      /* 0=低 1=中 2=高 3=最高 */

/* 命令响应超时（毫秒） */
#define TF_CMD_TIMEOUT_MS   500U

/* 数据块超时（毫秒），单块 512B @16MHz 约 0.3ms，多块按块数放大 */
#define TF_DATA_TIMEOUT_PER_BLOCK_MS   50U
#define TF_DATA_TIMEOUT_MIN_MS         500U

/* ACMD41 轮询上限。
 * 卡上电到 ready 通常几十次就够。★ 每轮里有 HAL_Delay(1)，所以这个数
 * 基本就是"最多等多少毫秒"：原来是 0xFFFFF(≈17 分钟！)，卡要是不 ready
 * 会像死机一样卡在识别里（配合 TF_INIT_RETRY=3 更久）。
 * SD 规范要求卡在上电 1 秒内初始化完成，给 4000(≈4 秒) 已经非常宽裕。 */
#define TF_ACMD41_TRIES     4000U

/* ==========================================================================
 * 返回值
 * ========================================================================== */
typedef enum
{
    TF_OK = 0,              /* 成功 */
    TF_ERR_PARAM,           /* 参数不对（空指针 / 越界 / count=0） */
    TF_ERR_NOT_INIT,        /* 未初始化 */
    TF_ERR_CMD_TIMEOUT,     /* 命令超时：卡没响应（多半没插卡 / 没供电 / 接线错） */
    TF_ERR_CMD_CRC,         /* 命令响应 CRC 错（多半速度太高或线太长） */
    TF_ERR_CMD_RESP,        /* 响应内容不对 */
    TF_ERR_DATA_TIMEOUT,    /* 数据超时（多块读时超时要放大） */
    TF_ERR_DATA_CRC,        /* 数据 CRC 错 */
    TF_ERR_DATA_FIFO,       /* FIFO 上溢/下溢、起始位错 */
    TF_ERR_V2_FAIL,         /* CMD8 失败：V1.x 卡（继续按 V1 走，不算致命） */
    TF_ERR_VOLTAGE,         /* ACMD41 一直不 ready：供电不足或不是 SD 卡 */
    TF_ERR_NOT_SD,          /* 不是 SD 卡（MMC / 无卡） */
    TF_ERR_ADDR_RANGE,      /* 访问块号超出容量 */
    TF_ERR_BUS_WIDTH,       /* 切 4 位失败 */
    TF_ERR_UNSUPPORTED_HS   /* 这张卡不支持高速模式（跑不了 48MHz，不是错误） */
} TF_Status;

/* 卡类型 */
#define TF_CARD_UNKNOWN     0U
#define TF_CARD_SDSC        1U      /* 标准容量 SD：CMD17/24 的参数是【字节】地址 */
#define TF_CARD_SDHC        2U      /* 高容量 SDHC/SDXC：参数是【块】地址 */

/* ==========================================================================
 * 状态信息
 * ========================================================================== */
typedef struct
{
    uint8_t  inited;        /* 1 = 已初始化完成 */
    uint8_t  cardType;      /* TF_CARD_xxx */
    uint8_t  bus4bit;       /* 1 = 已切到 4 位总线 */
    uint8_t  isV2;          /* 1 = SD 2.0+ 卡 */
    uint16_t rca;           /* 卡的相对地址 */
    uint16_t reserved;
    uint32_t blockCount;    /* 总扇区数（512B/扇区） */
    uint32_t capacityKB;    /* 容量，单位 KB */
    uint32_t clkDiv;        /* 当前 CLKDIV */
    uint32_t sdioClkHz;     /* 推算出的 SDIO_CK，单位 Hz */
    uint32_t sdioClkSrcHz;  /* 推算出的 SDIOCLK(PLL48CLK)，单位 Hz */

    /* ---- 速度协商结果 ---- */
    uint8_t  speedLevel;    /* 最终生效的档位，TF_SPEED_xxx */
    uint8_t  speedAuto;     /* 1 = 是自动协商出来的 */
    uint8_t  highSpeedMode; /* 1 = CMD6 已把卡切到高速模式 */
    uint8_t  bypass;        /* 1 = 当前走 BYPASS（SDIO_CK = SDIOCLK） */
    uint8_t  dmaUsed;       /* 1 = 数据搬运走 DMA */
    uint8_t  negedge;       /* 1 = 时钟用下降沿相位（BYPASS 时为 1） */
    uint8_t  reserved2[2];
    uint32_t speedTriedMask;/* 诊断：试过哪几档（bit0=48M ... 见 TF_SPEED_xxx） */

    /* 诊断：CMD9 读回的原始 CSD（128 位）。
       csd[0] = CSD bits 127:96，csd[3] = bits 31:0。
       ★ 容量算得不对时，把这 4 个字打出来手工解码最快 ——
         CSD v1.0 和 v2.0 的 C_SIZE 位置不一样，光看结果猜不出来。 */
    uint32_t csd[4];
} TF_Info;

extern TF_Info g_tf;

/* 诊断用全局量（出错时看这两个就知道走到哪一步了）：
     g_tfStep    : 初始化进度，见 TF_STEP_xxx 注释
     g_tfLastSta : 出错瞬间 SDIO->STA 的原始值（看哪个标志置位就知原因）
     g_tfErrLine : 出错所在的源文件行号 */
#define TF_STEP_NONE        0U
#define TF_STEP_GPIO        1U
#define TF_STEP_POWER       2U
#define TF_STEP_CLK_INIT    3U
#define TF_STEP_CMD0        4U
#define TF_STEP_CMD8        5U
#define TF_STEP_ACMD41      6U
#define TF_STEP_CMD2        7U
#define TF_STEP_CMD3        8U
#define TF_STEP_CMD9        9U
#define TF_STEP_CMD7        10U
#define TF_STEP_CMD16       11U
#define TF_STEP_ACMD6       12U
#define TF_STEP_CLK_RUN     13U
#define TF_STEP_READY       14U
#define TF_STEP_READ        15U
#define TF_STEP_WRITE       16U
#define TF_STEP_SPEED       17U     /* 正在做速度协商 */

extern volatile uint32_t g_tfStep;
extern volatile uint32_t g_tfLastSta;
extern volatile uint32_t g_tfErrLine;
extern volatile uint32_t g_tfLastResp;  /* 出错时那条命令的 RESP1 原值 */

/* ==========================================================================
 * API
 * ========================================================================== */

/** 初始化（识别 + 切 4 位）。幂等：已初始化过会直接返回 TF_OK。
 *  插拔卡后想重新识别，先 TF_DeInit() 再调本函数。 */
TF_Status TF_Init(void);

/** 确保已初始化（没初始化就做一次）。所有读写 API 内部都会调它。 */
TF_Status TF_EnsureInit(void);

/** 反初始化：断电、关时钟。之后可重新 TF_Init()。 */
void TF_DeInit(void);

/** 读若干块（每块 512B）。
 *  block : 起始块号（SDHC 直接是块号；SDSC 内部会换算成字节地址）
 *  buf   : 目标缓冲，长度 >= count*512
 *  count : 块数，>=1
 *  ★ count 很大时（比如一次读 64KB）超时会自动按块数放大。 */
TF_Status TF_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count);

/** 写若干块。参数含义同上。 */
TF_Status TF_WriteBlocks(uint32_t block, const uint8_t *buf, uint32_t count);

/** 读一块并重试 retry 次（抗偶发 CRC 错，读关键数据时用） */
TF_Status TF_ReadBlockRetry(uint32_t block, uint8_t *buf, uint32_t retry);

/* ---- 查询 ---- */
uint32_t TF_BlockCount(void);       /* 总扇区数 */
uint32_t TF_CapacityKB(void);       /* 容量 KB */
uint8_t  TF_IsReady(void);          /* 1 = 可用 */
uint8_t  TF_IsBus4Bit(void);        /* 1 = 4 位总线 */
uint32_t TF_SdioClockHz(void);      /* 当前 SDIO_CK */
const char *TF_StatusStr(TF_Status st);   /* 返回值的可读文字 */

/* ---- 速度控制 ----
 * TF_SetSpeed(level)：
 *   传 TF_SPEED_AUTO   -> 从 48MHz 往下自动协商，取能稳定工作的最高档
 *   传具体档位         -> 直接切到那一档（内部会先确保卡进了高速模式）
 *   返回值：最终生效的档位（TF_SPEED_xxx）
 *   ★ 协商时每换一档都真读一次 0 号扇区来验证，CRC 不过就降档，
 *     所以不会"设了个跑不动的速度然后就一直坏"。
 * 注意：48MHz 需要卡支持并成功切高速模式；切不了就自动落到 24MHz。 */
uint8_t TF_SetSpeed(uint8_t level);

/** 当前档位 / 当前频率 / 是否走了 DMA / 是否高速模式 */
uint8_t  TF_GetSpeedLevel(void);
uint8_t  TF_IsHighSpeed(void);
uint8_t  TF_IsDma(void);
const char *TF_SpeedStr(uint8_t level);   /* "48MHz" / "24MHz" ... */

/** 老接口保留（按分频直接设，0=24MHz 1=16MHz ...），返回实际分频 */
uint32_t TF_SetClockDiv(uint32_t div);

/* ---- 自检（最有用的一个函数）----
 * 做三件事：
 *   1. TF_Init()
 *   2. 读 0 号扇区
 *   3. 检查 MBR 签名 0x55AA（在 buf[510],buf[511]），并扫出第一个分区类型
 * 返回值就是失败原因。成功时 g_tf 里有容量、卡类型等信息。
 * 建议第一次接卡时在 main 里调它一次，最省事。 */
typedef struct
{
    uint8_t  mbrOk;         /* 1 = 签名 0x55AA 正确 */
    uint8_t  partType;      /* 第一个分区的类型字节，0x00 = 无分区 */
    uint8_t  firstByteFF;   /* 1 = 全片读出来都是 0xFF（很可能是接触不良） */
    uint8_t  firstByte00;   /* 1 = 全片读出来都是 0x00（很可能是没供电/没插卡） */
    uint32_t partLba;       /* 第一个分区的起始 LBA */
    uint32_t zeroWords;     /* 512B 里全 0 的字数（辅助判断） */
} TF_SelfTestResult;

TF_Status TF_SelfTest(TF_SelfTestResult *res);

/* ---- OLED 简报（需要工程里的 oled.h 可用；不需要就把下面宏改成 0）----
 * 把一行结果打到 OLED 上，8 字/行太窄，所以只放最关键的信息。 */
#ifndef TF_USE_OLED
#define TF_USE_OLED     1
#endif

#if TF_USE_OLED
/** 在 OLED 第 y 行显示一行自检结果，最长 16 字符 */
void TF_OledShow(TF_Status st, uint8_t y);
#endif

/* ==========================================================================
 * 测试 / 调试（报告直接显示在 OLED 上）
 *
 * ★ 输出到 OLED：用 6x8 小字体，一行 21 字符、一屏 8 行。
 *   报告先攒进日志缓冲，再分页轮播。
 * ★ 同时会往 USART1(PA9) 镜像一份 —— 引脚没引出来也没关系，
 *   不想要就把 TF_DBG_UART_EN 设成 0，就完全不碰 USART1。
 * ★ 整段可以用宏 TF_INCLUDE_TEST 定义成 0 关掉，不影响驱动本体。
 * ========================================================================== */
#ifndef TF_INCLUDE_TEST
#define TF_INCLUDE_TEST     1
#endif

#if TF_INCLUDE_TEST
/**
 * 完整测试。报告攒进日志，结束后自动在 OLED 上显示结论页。
 *
 * 依次做：
 *   [1] 初始化并记录卡信息（类型/容量/位宽/频率/高速模式/DMA/耗时）
 *   [2] 读 0 号扇区，校验 MBR 签名并解析分区表
 *   [3] 读第一个分区的引导扇区，识别文件系统（FAT32/exFAT/NTFS）
 *   [4] 同一扇区重复读 4 次，比对内容是否一致
 *   [5] 连读 256 个扇区(128KB) ×3，用 DWT 计时算吞吐
 *   [6] 写回测试（仅 doWriteTest=1）
 *
 * ★ 卡里**不需要放任何文件**，也不需要特定格式 —— 驱动是扇区级的，
 *   第 [2][3] 步读的是卡上本来就有的 MBR 和分区引导扇区，
 *   用来证明"读回来的数据真实有效"。空卡也能跑，只是没有 MBR 而已。
 *
 * @param doWriteTest 0 = 只读测试（**绝不写卡，绝对安全**）
 *                    1 = 额外做写回测试：只写【卡的最后一个扇区】，
 *                        先备份原内容、验证完再写回，不会破坏数据。
 * @return 0 = 全部通过；非 0 = 失败的检查项个数
 */
uint8_t TF_Test(uint8_t doWriteTest);

/**
 * 把测试报告分页轮播到 OLED（**阻塞函数**）。
 * @param pageMs 每页停留毫秒（0 -> 1000）
 * @param loops  轮播几圈（0 -> 1）
 *
 * ⚠ 会一直占到播完才返回。**别放在等上位机配置握手的那段流程里** ——
 *   固件 BT_Send_Ctrl() 的超时只有 10 秒，播太久会握手失败。
 *   建议放在握手成功之后再调。
 */
void TF_ShowLog(uint16_t pageMs, uint8_t loops);

/** 只显示结论页（最后一页），不等待。TF_Test 结束时会自动调一次 */
void TF_ShowLastPage(void);

/** 报告行数 / 取第 n 行文本（想自己显示或搬运时用） */
uint16_t    TF_LogLines(void);
const char *TF_LogLine(uint16_t n);
#endif


#ifdef __cplusplus
}
#endif

#endif /* __TF_CARD_H */
