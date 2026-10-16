/* ============================================================================
 * tf_fat.c —— 极简 FAT12/FAT16/FAT32 只读查找器
 *
 * 见 tf_fat.h 的说明。这里只强调几个实现上的注意点：
 *
 * ★ FAT 类型的判定【不能】看 BPB 里 0x36 那个 "FAT16   " 字符串
 *   —— 那是格式化工具随手写的，很多卡上都是错的。
 *   唯一可靠的办法是按簇总数算（微软 FAT 规范里的官方算法）：
 *       簇数 < 4085  -> FAT12
 *       簇数 < 65525 -> FAT16
 *       否则         -> FAT32
 *
 * ★ 簇号 -> LBA：lba = dataStartLba + (cluster - 2) * sectorsPerCluster
 *   簇号从 2 开始（0 和 1 是保留值）。
 *
 * ★ FAT 表项宽度：FAT16 = 2 字节，FAT32 = 4 字节（高 4 位要掩掉）。
 *   链尾标志：FAT16 >= 0xFFF8，FAT32 >= 0x0FFFFFF8。
 *
 * ★ 目录项固定 32 字节：
 *   [0..10] 8.3 名（空格补齐；首字节 0x00 = 目录结束，0xE5 = 已删除）
 *   [11]    属性（0x0F = 长文件名项，0x08 = 卷标，0x10 = 子目录）
 *   [20..21] 起始簇高 16 位（只有 FAT32 用）
 *   [26..27] 起始簇低 16 位
 *   [28..31] 文件字节数
 * ========================================================================== */

#include "tf_fat.h"
#include <string.h>

TF_FAT_Info g_tfFat;

/* 两个扇区缓存：一个给 FAT 表，一个给目录/数据。
   ★ 都用 aligned(4) —— TF_ReadBlocks 走 DMA 时要求内存 4 字节对齐。 */
static uint8_t  s_fatSec[512]  __attribute__((aligned(4)));
static uint32_t s_fatSecLba = 0xFFFFFFFFU;
static uint8_t  s_datSec[512]  __attribute__((aligned(4)));
static uint32_t s_datSecLba = 0xFFFFFFFFU;

#define TF_FAT_BPS          512U        /* 本模块只支持 512 字节扇区 */
#define TF_FAT_EOC16        0xFFF8U     /* FAT16 链尾阈值 */
#define TF_FAT_EOC32        0x0FFFFFF8U /* FAT32 链尾阈值 */

/* 挂载失败的具体原因（给 TF_FAT_Reason() 用）。
   ★ 光靠 TF_Status 说不清"卡是 exFAT"还是"没引导扇区"，
     而这两件事的处理办法完全不同，所以单独留一个原因串。 */
static const char *s_reason = "not mounted yet";

/* ==========================================================================
 * 小工具
 * ========================================================================== */

static uint16_t tf_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t tf_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8U)
         | ((uint32_t)p[2] << 16U)
         | ((uint32_t)p[3] << 24U);
}

/** 读一个扇区到模块自己的缓存（带 1 扇区命中） */
static TF_Status tf_load(uint32_t lba, uint8_t *cache, uint32_t *cacheLba)
{
    if (*cacheLba == lba)
    {
        return TF_OK;                       /* 命中 */
    }
    if (TF_ReadBlocks(lba, cache, 1U) != TF_OK)
    {
        *cacheLba = 0xFFFFFFFFU;
        return TF_ERR_DATA_TIMEOUT;
    }
    *cacheLba = lba;
    return TF_OK;
}

static TF_Status tf_load_fat(uint32_t lba)  { return tf_load(lba, s_fatSec, &s_fatSecLba); }
static TF_Status tf_load_dat(uint32_t lba)  { return tf_load(lba, s_datSec, &s_datSecLba); }

/** 取某个簇的 FAT 表项值（下一个簇号） */
static TF_Status tf_next_cluster(uint32_t cluster, uint32_t *next)
{
    uint32_t offset;
    uint32_t lba;
    uint32_t inSec;

    if (g_tfFat.fatType == 32U)
    {
        offset = cluster * 4U;
    }
    else
    {
        offset = cluster * 2U;
    }

    lba   = g_tfFat.fatStartLba + (offset / TF_FAT_BPS);
    inSec = offset % TF_FAT_BPS;

    if (tf_load_fat(lba) != TF_OK)
    {
        return TF_ERR_DATA_TIMEOUT;
    }

    if (g_tfFat.fatType == 32U)
    {
        *next = tf_le32(&s_fatSec[inSec]) & 0x0FFFFFFFU;
    }
    else
    {
        *next = (uint32_t)tf_le16(&s_fatSec[inSec]);
    }

    return TF_OK;
}

/** 这个簇号是不是"链尾/无效" */
static uint8_t tf_is_eoc(uint32_t cluster)
{
    if (g_tfFat.fatType == 32U)
    {
        return (cluster >= TF_FAT_EOC32) ? 1U : 0U;
    }
    return (cluster >= TF_FAT_EOC16) ? 1U : 0U;
}

/** 簇号 -> 起始 LBA */
static uint32_t tf_cluster_lba(uint32_t cluster)
{
    return g_tfFat.dataStartLba
         + ((cluster - 2U) * (uint32_t)g_tfFat.sectorsPerCluster);
}

/** 目录项 -> "IMG.BIN" 形式（去空格；无扩展名就不带点） */
static void tf_make_name(const uint8_t *entry, char *out)
{
    uint8_t n = 0U;
    uint8_t i;

    for (i = 0U; i < 8U; ++i)
    {
        if (entry[i] == ' ')
        {
            break;
        }
        out[n++] = (char)entry[i];
    }

    if (entry[8] != ' ')
    {
        out[n++] = '.';
        for (i = 8U; i < 11U; ++i)
        {
            if (entry[i] == ' ')
            {
                break;
            }
            out[n++] = (char)entry[i];
        }
    }

    out[n] = '\0';
}

/** 大小写不敏感比较（卡上的 8.3 名一律大写，但稳妥起见还是忽略大小写） */
static uint8_t tf_name_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0'))
    {
        char ca = *a;
        char cb = *b;
        if ((ca >= 'a') && (ca <= 'z')) { ca = (char)(ca - 32); }
        if ((cb >= 'a') && (cb <= 'z')) { cb = (char)(cb - 32); }
        if (ca != cb)
        {
            return 0U;
        }
        ++a;
        ++b;
    }
    return ((*a == '\0') && (*b == '\0')) ? 1U : 0U;
}

/* ==========================================================================
 * 挂载
 * ========================================================================== */

/**
 * 看一眼某个引导扇区"像什么文件系统"。
 * ★ 只在挂载失败时用来给提示 —— 判断 FAT 类型仍然以簇总数（官方算法）为准。
 * ★ 返回的字符串必须是纯 ASCII：它要打到 OLED（字库只有 ASCII）和串口。
 * 返回 NULL 表示认不出来。
 */
static const char *tf_sniff_fs(const uint8_t *sec)
{
    if (memcmp(&sec[3], "EXFAT   ", 8U) == 0)
    {
        return "exFAT card - only FAT16/FAT32 supported, reformat as FAT32";
    }
    if (memcmp(&sec[3], "NTFS    ", 8U) == 0)
    {
        return "NTFS card - only FAT16/FAT32 supported";
    }
    if (tf_le16(&sec[11]) != TF_FAT_BPS)
    {
        return "not a FAT boot sector (bytes/sector != 512)";
    }
    return 0;
}

/**
 * 试着把 lba 处的扇区当引导扇区解析，填 g_tfFat 的布局字段。
 * 返回 1 = 看起来是个合法的 FAT 引导扇区。
 */
static uint8_t tf_parse_bpb(uint32_t lba)
{
    uint32_t rootDirSectors;
    uint32_t fatSz;
    uint32_t totalSectors;
    uint32_t dataSectors;
    uint32_t clusters;

    if (tf_load_dat(lba) != TF_OK)
    {
        return 0U;
    }

    /* 只支持 512 字节扇区（SD 卡本来就是 512，正常不会触发） */
    if (tf_le16(&s_datSec[11]) != TF_FAT_BPS)
    {
        return 0U;
    }

    g_tfFat.bytesPerSector    = TF_FAT_BPS;
    g_tfFat.sectorsPerCluster = s_datSec[13];
    g_tfFat.numFats           = s_datSec[16];
    g_tfFat.rootDirSectors    = 0U;
    g_tfFat.rootCluster       = 0U;

    /* 基本合法性检查 */
    if ((g_tfFat.sectorsPerCluster == 0U) ||
        ((g_tfFat.sectorsPerCluster & (g_tfFat.sectorsPerCluster - 1U)) != 0U) ||
        (g_tfFat.numFats == 0U) || (g_tfFat.numFats > 4U))
    {
        return 0U;
    }

    /* FAT 表长度：先看 16 位字段，为 0 就用 32 位字段（FAT32） */
    fatSz = (uint32_t)tf_le16(&s_datSec[22]);
    if (fatSz == 0U)
    {
        fatSz = tf_le32(&s_datSec[36]);
    }
    if (fatSz == 0U)
    {
        return 0U;
    }
    g_tfFat.fatSectors = fatSz;

    /* 总扇区数：先看 16 位字段，为 0 就用 32 位字段 */
    totalSectors = (uint32_t)tf_le16(&s_datSec[19]);
    if (totalSectors == 0U)
    {
        totalSectors = tf_le32(&s_datSec[32]);
    }
    if (totalSectors == 0U)
    {
        return 0U;
    }
    g_tfFat.totalSectors = totalSectors;

    /* 布局 */
    g_tfFat.fatStartLba = lba + (uint32_t)tf_le16(&s_datSec[14]);   /* + 保留扇区 */

    /* FAT12/16 有固定大小的根目录区；FAT32 的根目录是一个簇链 */
    rootDirSectors = (((uint32_t)tf_le16(&s_datSec[17]) * 32U) + (TF_FAT_BPS - 1U)) / TF_FAT_BPS;

    if (rootDirSectors == 0U)
    {
        /* FAT32：根目录起始簇在 0x2C */
        g_tfFat.rootDirSectors = 0U;
        g_tfFat.rootDirLba     = 0U;
        g_tfFat.rootCluster    = tf_le32(&s_datSec[44]);
        g_tfFat.dataStartLba   = g_tfFat.fatStartLba + (uint32_t)g_tfFat.numFats * fatSz;
    }
    else
    {
        g_tfFat.rootDirSectors = rootDirSectors;
        g_tfFat.rootDirLba     = g_tfFat.fatStartLba + (uint32_t)g_tfFat.numFats * fatSz;
        g_tfFat.dataStartLba   = g_tfFat.rootDirLba + rootDirSectors;
    }

    /* ★ FAT 类型：按簇总数算（官方算法），别看 0x36 那个字符串。
       ★★ 注意基准：totalSectors 是【分区内】的扇区数，而 dataStartLba 是
          【绝对】LBA（含分区偏移 partStartLba）。相减前必须先把基准对齐 ——
          否则分区偏移越大簇数算得越少，大卡会被误判成 FAT16
          （现象：簇链按 2 字节项解析，读出全垃圾）。
          （这个 bug 是主机端 FAT32 镜像测出来的，别删这段注释。） */
    if (g_tfFat.dataStartLba <= g_tfFat.partStartLba)
    {
        return 0U;
    }
    if ((g_tfFat.dataStartLba - g_tfFat.partStartLba) >= totalSectors)
    {
        return 0U;
    }
    dataSectors = totalSectors - (g_tfFat.dataStartLba - g_tfFat.partStartLba);
    clusters    = dataSectors / g_tfFat.sectorsPerCluster;
    g_tfFat.clusterCount = clusters;

    if (clusters < 4085U)
    {
        g_tfFat.fatType = 12U;
    }
    else if (clusters < 65525U)
    {
        g_tfFat.fatType = 16U;
    }
    else
    {
        g_tfFat.fatType = 32U;
    }

    return 1U;
}

TF_Status TF_FAT_Mount(void)
{
    TF_Status st;
    uint32_t  i;
    uint8_t   entries[4][16];       /* ★ 先把分区表拷出来 ——
                                       tf_parse_bpb() 会覆盖 s_datSec，
                                       直接边循环边读会把第 2~4 项读成垃圾 */
    uint8_t   haveEntries = 0U;

    (void)memset(&g_tfFat, 0, sizeof(g_tfFat));
    s_fatSecLba = 0xFFFFFFFFU;
    s_datSecLba = 0xFFFFFFFFU;
    s_reason    = "not mounted yet";

    st = TF_EnsureInit();
    if (st != TF_OK)
    {
        s_reason = "SD card init failed (see TF_Init)";
        return st;
    }

    /* ---- 先按 MBR 解析：找第一个非空分区 ---- */
    if (tf_load_dat(0U) != TF_OK)
    {
        s_reason = "read LBA0 failed";
        return TF_ERR_DATA_TIMEOUT;
    }

    if ((s_datSec[510] == 0x55U) && (s_datSec[511] == 0xAAU))
    {
        for (i = 0U; i < 4U; ++i)
        {
            (void)memcpy(entries[i], &s_datSec[446U + (i * 16U)], 16U);
        }
        haveEntries = 1U;
    }

    if (haveEntries != 0U)
    {
        for (i = 0U; i < 4U; ++i)
        {
            const uint8_t *e = entries[i];
            uint32_t startLba;
            uint32_t sectors;

            if ((e[4] == 0x00U) || (e[4] == 0xEEU) || (e[4] == 0xEFU))
            {
                continue;               /* 空项 / GPT 保护项 */
            }

            startLba = tf_le32(&e[8]);
            sectors  = tf_le32(&e[12]);
            if ((startLba == 0U) || (sectors == 0U))
            {
                continue;
            }

            g_tfFat.partStartLba = startLba;

            if (tf_parse_bpb(startLba) != 0U)
            {
                if (g_tfFat.fatType == 12U)
                {
                    s_reason = "FAT12 card - not supported, reformat as FAT16/FAT32";
                    return TF_ERR_NOT_SD;
                }
                g_tfFat.mounted = 1U;
                s_reason = (g_tfFat.fatType == 32U) ? "OK (FAT32)" : "OK (FAT16)";
                return TF_OK;
            }

            /* 解析失败：嗅一下这个分区的引导扇区像什么（exFAT / NTFS / 不是引导扇区） */
            {
                const char *why = tf_sniff_fs(s_datSec);
                if (why != 0)
                {
                    s_reason = why;
                }
            }
        }
    }

    /* ---- 没有可用 MBR：按"软盘格式"（引导扇区直接在 LBA 0）再试一次 ---- */
    if (tf_parse_bpb(0U) != 0U)
    {
        if (g_tfFat.fatType == 12U)
        {
            s_reason = "FAT12 card - not supported, reformat as FAT16/FAT32";
            return TF_ERR_NOT_SD;
        }
        g_tfFat.partStartLba = 0U;
        g_tfFat.mounted      = 1U;
        s_reason = (g_tfFat.fatType == 32U) ? "OK (FAT32, no MBR)" : "OK (FAT16, no MBR)";
        return TF_OK;
    }

    /* 全试完了。先给个默认话术，下面 sniff 能认出更具体的原因就覆盖它 */
    s_reason = "no FAT boot sector (no usable MBR partition, LBA0 not a boot sector)";
    {
        const char *why = tf_sniff_fs(s_datSec);
        if (why != 0)
        {
            s_reason = why;
        }
    }

    return TF_ERR_NOT_SD;
}

const char *TF_FAT_Reason(void)
{
    return s_reason;
}

void TF_FAT_Unmount(void)
{
    g_tfFat.mounted = 0U;
    s_fatSecLba = 0xFFFFFFFFU;
    s_datSecLba = 0xFFFFFFFFU;
}

/* ==========================================================================
 * 根目录扫描
 * ========================================================================== */

/**
 * 遍历根目录项，交给回调处理。
 * ★ FAT12/16 的根目录是"固定扇区区"，没有簇链，直接按 rootDirSectors 扫；
 *   FAT32 的根目录是普通簇链，从 rootCluster 开始跟。
 */
static TF_Status tf_walk_root(uint8_t (*cb)(const uint8_t *entry, void *arg), void *arg)
{
    uint32_t sec;

    if (g_tfFat.mounted == 0U)
    {
        return TF_ERR_NOT_INIT;
    }
    if (g_tfFat.fatType == 12U)
    {
        return TF_ERR_NOT_SD;       /* 明确不支持 FAT12 的簇链 */
    }

    /* ★ 这里用 fatType 判断（而不是 rootDirSectors 是否为 0）——
       必须和 tf_next_cluster() 用同一个判据，否则"目录按 FAT32 走、
       FAT 表按 FAT16 读"这种半对半错最难查。 */
    if (g_tfFat.fatType != 32U)
    {
        /* ---- FAT12/16：根目录是固定扇区区，没有簇链 ---- */
        for (sec = 0U; sec < g_tfFat.rootDirSectors; ++sec)
        {
            uint32_t k;
            if (tf_load_dat(g_tfFat.rootDirLba + sec) != TF_OK)
            {
                return TF_ERR_DATA_TIMEOUT;
            }
            for (k = 0U; k < (TF_FAT_BPS / 32U); ++k)
            {
                uint8_t r = cb(&s_datSec[k * 32U], arg);
                if (r == 1U) { return TF_OK; }      /* 找到了 */
                if (r == 2U) { return TF_OK; }      /* 目录结束 */
            }
        }
    }
    else
    {
        /* ---- FAT32：跟簇链 ---- */
        uint32_t cluster = g_tfFat.rootCluster;
        uint32_t guard   = 0U;

        while ((cluster >= 2U) && (tf_is_eoc(cluster) == 0U))
        {
            uint32_t s;
            for (s = 0U; s < (uint32_t)g_tfFat.sectorsPerCluster; ++s)
            {
                uint32_t k;
                if (tf_load_dat(tf_cluster_lba(cluster) + s) != TF_OK)
                {
                    return TF_ERR_DATA_TIMEOUT;
                }
                for (k = 0U; k < (TF_FAT_BPS / 32U); ++k)
                {
                    uint8_t r = cb(&s_datSec[k * 32U], arg);
                    if (r == 1U) { return TF_OK; }
                    if (r == 2U) { return TF_OK; }
                }
            }

            if (tf_next_cluster(cluster, &cluster) != TF_OK)
            {
                return TF_ERR_DATA_TIMEOUT;
            }
            if ((++guard) > 0x10000U)
            {
                return TF_ERR_DATA_CRC;     /* 链成环了，防死循环 */
            }
        }
    }

    return TF_OK;
}

/* ---- 回调：按名字找 ---- */
typedef struct
{
    const char *want;
    TF_FAT_File *out;
    uint8_t      found;
} TF_FAT_FindCtx;

static uint8_t tf_cb_find(const uint8_t *e, void *arg)
{
    TF_FAT_FindCtx *c = (TF_FAT_FindCtx *)arg;
    char name[13];

    if (e[0] == 0x00U) { return 2U; }               /* 目录结束 */
    if (e[0] == 0xE5U) { return 0U; }               /* 已删除 */
    if (e[11] == 0x0FU) { return 0U; }              /* 长文件名项 */
    if ((e[11] & 0x08U) != 0U) { return 0U; }       /* 卷标 */
    if ((e[11] & 0x10U) != 0U) { return 0U; }       /* 子目录 */

    tf_make_name(e, name);
    if (tf_name_eq(name, c->want) == 0U)
    {
        return 0U;
    }

    (void)memcpy(c->out->name, name, 13U);
    c->out->attr  = e[11];
    c->out->size  = tf_le32(&e[28]);
    c->out->firstCluster = (uint32_t)tf_le16(&e[26])
                         | ((g_tfFat.fatType == 32U)
                                ? ((uint32_t)tf_le16(&e[20]) << 16U) : 0U);
    c->found = 1U;
    return 1U;
}

TF_Status TF_FAT_FindInRoot(const char *name83, TF_FAT_File *out)
{
    TF_FAT_FindCtx c;
    TF_Status      st;

    if ((name83 == 0) || (out == 0))
    {
        return TF_ERR_PARAM;
    }

    (void)memset(out, 0, sizeof(*out));
    c.want  = name83;
    c.out   = out;
    c.found = 0U;

    st = tf_walk_root(tf_cb_find, &c);
    if (st != TF_OK)
    {
        return st;
    }

    return (c.found != 0U) ? TF_OK : TF_ERR_ADDR_RANGE;    /* 找不到 */
}

/* ---- 回调：列举 ---- */
typedef struct
{
    TF_FAT_File *out;
    uint32_t     max;
    uint32_t     n;
} TF_FAT_ListCtx;

static uint8_t tf_cb_list(const uint8_t *e, void *arg)
{
    TF_FAT_ListCtx *c = (TF_FAT_ListCtx *)arg;

    if (e[0] == 0x00U) { return 2U; }
    if (e[0] == 0xE5U) { return 0U; }
    if (e[11] == 0x0FU) { return 0U; }
    if ((e[11] & 0x08U) != 0U) { return 0U; }

    if (c->n >= c->max)
    {
        return 1U;                  /* 装满了，收工 */
    }

    tf_make_name(e, c->out[c->n].name);
    c->out[c->n].attr = e[11];
    c->out[c->n].size = tf_le32(&e[28]);
    c->out[c->n].firstCluster = (uint32_t)tf_le16(&e[26])
                              | ((g_tfFat.fatType == 32U)
                                     ? ((uint32_t)tf_le16(&e[20]) << 16U) : 0U);
    ++c->n;

    return 0U;
}

uint32_t TF_FAT_ListRoot(TF_FAT_File *out, uint32_t max)
{
    TF_FAT_ListCtx c;

    if ((out == 0) || (max == 0U))
    {
        return 0U;
    }

    c.out = out;
    c.max = max;
    c.n   = 0U;

    (void)tf_walk_root(tf_cb_list, &c);

    return c.n;
}

/* ==========================================================================
 * 顺序读
 * ========================================================================== */

TF_Status TF_FAT_StreamOpen(TF_FAT_Stream *s, const TF_FAT_File *f)
{
    if ((s == 0) || (f == 0))
    {
        return TF_ERR_PARAM;
    }
    if (g_tfFat.mounted == 0U)
    {
        return TF_ERR_NOT_INIT;
    }
    if ((f->firstCluster < 2U) || (f->size == 0U))
    {
        return TF_ERR_PARAM;        /* 空文件 / 没有数据簇 */
    }

    (void)memset(s, 0, sizeof(*s));
    s->firstCluster = f->firstCluster;
    s->size         = f->size;
    s->cluster      = f->firstCluster;
    s->opened       = 1U;

    return TF_OK;
}

uint32_t TF_FAT_StreamRead(TF_FAT_Stream *s, uint8_t *dst, uint32_t n)
{
    uint32_t done = 0U;
    uint32_t clusterBytes;

    if ((s == 0) || (dst == 0) || (s->opened == 0U) || (n == 0U))
    {
        return 0U;
    }

    clusterBytes = (uint32_t)g_tfFat.sectorsPerCluster * TF_FAT_BPS;

    while ((done < n) && (s->posInFile < s->size))
    {
        uint32_t want;
        uint32_t inClusterLeft;
        uint32_t fileLeft;
        uint32_t sectorIdx;
        uint32_t inSector;
        uint32_t chunk;

        /* 本簇读完了就跳到下一簇 */
        if (s->posInCluster >= clusterBytes)
        {
            uint32_t next;
            if (tf_next_cluster(s->cluster, &next) != TF_OK)
            {
                break;
            }
            if (tf_is_eoc(next) != 0U)
            {
                break;                  /* 链断了，但文件大小还没到 —— 数据不可信 */
            }
            s->cluster      = next;
            s->posInCluster = 0U;
        }

        inClusterLeft = clusterBytes - s->posInCluster;
        fileLeft      = s->size - s->posInFile;
        want          = n - done;
        if (want > inClusterLeft) { want = inClusterLeft; }
        if (want > fileLeft)      { want = fileLeft; }
        if (want == 0U)           { break; }

        sectorIdx = s->posInCluster / TF_FAT_BPS;
        inSector  = s->posInCluster % TF_FAT_BPS;
        chunk     = TF_FAT_BPS - inSector;
        if (chunk > want) { chunk = want; }

        if (tf_load_dat(tf_cluster_lba(s->cluster) + sectorIdx) != TF_OK)
        {
            break;
        }

        (void)memcpy(&dst[done], &s_datSec[inSector], chunk);

        done            += chunk;
        s->posInFile    += chunk;
        s->posInCluster += chunk;
    }

    return done;
}

/* ==========================================================================
 * 状态码文字
 * ========================================================================== */

const char *TF_FAT_ErrStr(TF_Status st)
{
    switch (st)
    {
    case TF_OK:               return "OK";
    case TF_ERR_PARAM:        return "param";
    case TF_ERR_NOT_INIT:     return "not mounted";
    case TF_ERR_DATA_TIMEOUT: return "card read fail";
    case TF_ERR_NOT_SD:       return "no FAT / unsupported";
    case TF_ERR_ADDR_RANGE:   return "file not found";
    default:                  return TF_StatusStr(st);
    }
}
