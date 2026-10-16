/* ============================================================================
 * tf_image.c —— 从 TF 卡读灰度图像文件，灌进大环形缓冲区
 *
 * 见 tf_image.h 的文件格式说明。这里补充几个实现要点：
 *
 * ★ 环形缓冲用"单调递增的 head/tail 计数器 + 取模"实现：
 *     head/tail 只增不减，读写时再 &(SIZE-1) 取下标。
 *     好处是"可读字节数 = head - tail"直接相减就行，不用管回绕，
 *     也不会有"满/空都是 head==tail"那个经典歧义。
 *     （代价：SIZE 必须是 2 的幂，已用 #error 卡住。）
 *
 * ★ 卡上数据是**分扇区**读进来的（512B 一跳）。读的时候不阻塞：
 *     缓冲填满就返回，剩下的下次再读。所以卡那边的读取抖动
 *     （比如偶发重试）不会直接顶到雕刻节奏上。
 *
 * ★ CRC32 用标准 CRC-32/ISO-HDLC（多项式 0xEDB88320，初值 0xFFFFFFFF，
 *   末尾取反）。Qt 端没有现成的 CRC32（qChecksum 是 CRC-16），
 *   所以两边都是手写同一个算法 —— 改算法必须两边一起改。
 * ========================================================================== */

#include "tf_image.h"
#include "tf_fat.h"
#include <string.h>

#if ((TFIMG_RING_SIZE & (TFIMG_RING_SIZE - 1U)) != 0U)
#error "TFIMG_RING_SIZE must be a power of 2"
#endif

#define TFIMG_RING_MASK   (TFIMG_RING_SIZE - 1U)

/* ==========================================================================
 * 状态
 * ========================================================================== */

static uint8_t  s_ring[TFIMG_RING_SIZE] __attribute__((aligned(4)));
static uint8_t  s_bounce[512]           __attribute__((aligned(4)));

static uint32_t s_head = 0U;            /* 只增，写指针 */
static uint32_t s_tail = 0U;            /* 只增，读指针 */

static TF_FAT_Stream s_stream;

static uint8_t  s_opened    = 0U;
static uint8_t  s_srcEof    = 0U;       /* 卡上的数据已经全部读进缓冲 */
static uint16_t s_w         = 0U;
static uint16_t s_h         = 0U;
static uint32_t s_total     = 0U;       /* = w * h */
static uint32_t s_fileCrc   = 0U;
static uint32_t s_payloadLeft = 0U;     /* 还有多少数据字节没读进来 */
static uint32_t s_loaded    = 0U;
static uint32_t s_consumed  = 0U;
static uint32_t s_crc       = 0xFFFFFFFFU;  /* 累计被取走的字节的 CRC32 */
static TF_Status s_err      = TF_OK;

/* ==========================================================================
 * CRC32（标准 CRC-32/ISO-HDLC）
 * ========================================================================== */

static uint32_t tfimg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    while (n > 0U)
    {
        crc ^= (uint32_t)(*p++);
        for (i = 0U; i < 8U; ++i)
        {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
        --n;
    }
    return crc;
}

/* ==========================================================================
 * 环形缓冲
 * ========================================================================== */

static uint32_t tfimg_avail(void) { return s_head - s_tail; }
static uint32_t tfimg_free(void)  { return TFIMG_RING_SIZE - (s_head - s_tail); }

static void tfimg_push(const uint8_t *src, uint32_t n)
{
    uint32_t at    = s_head & TFIMG_RING_MASK;
    uint32_t first = TFIMG_RING_SIZE - at;

    if (first > n)
    {
        first = n;
    }

    (void)memcpy(&s_ring[at], src, first);

    if (n > first)
    {
        (void)memcpy(&s_ring[0], src + first, n - first);
    }

    s_head += n;
}

/* ==========================================================================
 * 打开 / 关闭
 * ========================================================================== */

TF_Status TFIMG_Open(const char *name83)
{
    TF_FAT_File f;
    uint8_t     hdr[TFIMG_HEADER_SIZE];
    uint32_t    n;
    TF_Status   st;
    const char *name = (name83 != 0) ? name83 : TFIMG_FILE_NAME;

    /* ---- 复位 ---- */
    s_head = 0U;
    s_tail = 0U;
    s_opened    = 0U;
    s_srcEof    = 0U;
    s_w = 0U;
    s_h = 0U;
    s_total = 0U;
    s_fileCrc = 0U;
    s_payloadLeft = 0U;
    s_loaded = 0U;
    s_consumed = 0U;
    s_crc = 0xFFFFFFFFU;
    s_err = TF_OK;
    (void)memset(&s_stream, 0, sizeof(s_stream));

    /* ---- 挂 FAT ---- */
    st = TF_FAT_Mount();
    if (st != TF_OK)
    {
        s_err = st;
        return st;
    }

    /* ---- 找文件 ---- */
    st = TF_FAT_FindInRoot(name, &f);
    if (st != TF_OK)
    {
        s_err = st;
        return st;
    }

    /* ---- 打开流、读 16 字节头 ---- */
    st = TF_FAT_StreamOpen(&s_stream, &f);
    if (st != TF_OK)
    {
        s_err = st;
        return st;
    }

    n = TF_FAT_StreamRead(&s_stream, hdr, TFIMG_HEADER_SIZE);
    if (n != TFIMG_HEADER_SIZE)
    {
        s_err = TF_ERR_DATA_TIMEOUT;
        return s_err;
    }

    /* ---- 校验魔数 ---- */
    if ((hdr[0] != TFIMG_MAGIC0) || (hdr[1] != TFIMG_MAGIC1) ||
        (hdr[2] != TFIMG_MAGIC2) || (hdr[3] != TFIMG_MAGIC3))
    {
        s_err = TF_ERR_CMD_RESP;        /* 不是我们的文件 */
        return s_err;
    }

    s_w = (uint16_t)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8U));
    s_h = (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8U));

    s_total   = (uint32_t)hdr[8]
              | ((uint32_t)hdr[9]  << 8U)
              | ((uint32_t)hdr[10] << 16U)
              | ((uint32_t)hdr[11] << 24U);

    s_fileCrc = (uint32_t)hdr[12]
              | ((uint32_t)hdr[13] << 8U)
              | ((uint32_t)hdr[14] << 16U)
              | ((uint32_t)hdr[15] << 24U);

    /* ---- 头自洽性检查 ---- */
    if ((s_w == 0U) || (s_h == 0U))
    {
        s_err = TF_ERR_CMD_RESP;
        return s_err;
    }
    if (s_total != ((uint32_t)s_w * (uint32_t)s_h))
    {
        s_err = TF_ERR_CMD_RESP;
        return s_err;
    }
    if (f.size < (TFIMG_HEADER_SIZE + s_total))
    {
        s_err = TF_ERR_DATA_TIMEOUT;    /* 文件被截断了 */
        return s_err;
    }

    s_payloadLeft = s_total;
    s_opened      = 1U;
    s_err         = TF_OK;

    return TF_OK;
}

void TFIMG_Close(void)
{
    s_opened = 0U;
    s_srcEof = 1U;
    s_head   = 0U;
    s_tail   = 0U;
}

/* ==========================================================================
 * 填充 / 取数
 * ========================================================================== */

uint32_t TFIMG_Fill(void)
{
    uint32_t got = 0U;

    if (s_opened == 0U)
    {
        return 0U;
    }

    /* 缓冲至少还能放下一整扇区，且卡上还有数据，才继续读 */
    while ((tfimg_free() >= 512U) && (s_srcEof == 0U) && (s_payloadLeft > 0U))
    {
        uint32_t want = 512U;
        uint32_t n;

        if (want > s_payloadLeft)
        {
            want = s_payloadLeft;       /* 最后不足一扇区的一段 */
        }

        n = TF_FAT_StreamRead(&s_stream, s_bounce, want);
        if (n == 0U)
        {
            s_srcEof = 1U;
            s_err    = TF_ERR_DATA_TIMEOUT;     /* 卡上数据提前没了 */
            break;
        }

        tfimg_push(s_bounce, n);

        s_loaded      += n;
        s_payloadLeft -= n;
        got           += n;

        if (s_payloadLeft == 0U)
        {
            s_srcEof = 1U;
            break;
        }
        if (n < want)
        {
            s_srcEof = 1U;
            s_err    = TF_ERR_DATA_TIMEOUT;
            break;
        }
    }

    return got;
}

uint32_t TFIMG_Get(uint8_t *dst, uint32_t n)
{
    uint32_t avail;
    uint32_t take;
    uint32_t at;
    uint32_t first;

    if ((dst == 0) || (n == 0U))
    {
        return 0U;
    }

    avail = tfimg_avail();
    take  = (n < avail) ? n : avail;
    if (take == 0U)
    {
        return 0U;
    }

    at    = s_tail & TFIMG_RING_MASK;
    first = TFIMG_RING_SIZE - at;
    if (first > take)
    {
        first = take;
    }

    (void)memcpy(dst, &s_ring[at], first);
    if (take > first)
    {
        (void)memcpy(dst + first, &s_ring[0], take - first);
    }

    s_tail     += take;
    s_consumed += take;
    s_crc       = tfimg_crc32_update(s_crc, dst, take);

    return take;
}

uint8_t TFIMG_GetByte(uint8_t *out)
{
    return (TFIMG_Get(out, 1U) == 1U) ? 1U : 0U;
}

uint8_t TFIMG_Eof(void)
{
    return ((s_srcEof != 0U) && (tfimg_avail() == 0U)) ? 1U : 0U;
}

/* ==========================================================================
 * 查询
 * ========================================================================== */

uint8_t  TFIMG_Opened(void)      { return s_opened; }
uint16_t TFIMG_Width(void)       { return s_w; }
uint16_t TFIMG_Height(void)      { return s_h; }
uint32_t TFIMG_TotalBytes(void)  { return s_total; }
uint32_t TFIMG_FileCrc32(void)   { return s_fileCrc; }
uint32_t TFIMG_Avail(void)       { return tfimg_avail(); }
uint32_t TFIMG_Loaded(void)      { return s_loaded; }
uint32_t TFIMG_Consumed(void)    { return s_consumed; }
uint32_t TFIMG_RingSize(void)    { return TFIMG_RING_SIZE; }
TF_Status TFIMG_LastError(void)  { return s_err; }

/** 累计被取走的字节的 CRC32（已取走 0 字节时返回 0） */
uint32_t TFIMG_ConsumedCrc32(void)
{
    if (s_consumed == 0U)
    {
        return 0U;
    }
    return s_crc ^ 0xFFFFFFFFU;
}

const char *TFIMG_ErrStr(void)
{
    if (s_err == TF_OK)
    {
        return "OK";
    }
    if (s_err == TF_ERR_CMD_RESP)
    {
        return "bad magic/header";
    }
    return TF_FAT_ErrStr(s_err);
}

/* ==========================================================================
 * 自检：整份读一遍并校验 CRC32
 * ========================================================================== */

TF_Status TFIMG_SelfTest(TFIMG_TestResult *res)
{
    TFIMG_TestResult local;
    uint8_t          buf[256];
    TF_Status        st;

    (void)memset(&local, 0, sizeof(local));

    st = TFIMG_Open(0);
    if (st != TF_OK)
    {
        local.err = st;
        if (res != 0) { *res = local; }
        return st;
    }

    local.opened       = 1U;
    local.headerOk     = 1U;
    local.mountOk      = g_tfFat.mounted;
    local.fatType      = g_tfFat.fatType;
    local.width        = s_w;
    local.height       = s_h;
    local.expectBytes  = s_total;
    local.fileCrc32    = s_fileCrc;

    /* 把整份数据读进来再取走，边取边算 CRC */
    while (TFIMG_Eof() == 0U)
    {
        uint32_t n;

        (void)TFIMG_Fill();

        n = TFIMG_Get(buf, (uint32_t)sizeof(buf));
        if (n == 0U)
        {
            /* 缓冲空且没到文件尾 = 卡读不动了，防死循环 */
            if (TFIMG_Avail() == 0U)
            {
                break;
            }
        }
    }

    local.loadedBytes   = TFIMG_Loaded();
    local.consumedBytes = TFIMG_Consumed();
    local.calcCrc32     = TFIMG_ConsumedCrc32();
    local.eofReached    = TFIMG_Eof();
    local.crcOk         = ((local.calcCrc32 == local.fileCrc32) &&
                           (local.consumedBytes == local.expectBytes)) ? 1U : 0U;
    local.err           = TFIMG_LastError();

    if (res != 0)
    {
        *res = local;
    }

    return (local.crcOk != 0U) ? TF_OK : TF_ERR_DATA_CRC;
}
