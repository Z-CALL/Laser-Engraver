/* ============================================================================
 * tf_fat.h —— 极简 FAT12/FAT16/FAT32 **只读** 查找器（不依赖 FatFs）
 *
 * 用途：TF 卡是给 Windows 格式化过的，你往卡里拷的文件是落在 **FAT 文件系统**
 *       里的。而 tf_card.c 只认"第 N 个扇区"，不知道文件在哪。
 *       这个模块补上这一层：读 MBR -> 分区 -> BPB -> 根目录 -> 簇链，
 *       把"卡上某个文件"变成"一串可以按顺序读出来的字节"。
 *
 * ★ 为什么自己写而不是上 FatFs：
 *   本工程没有 Middlewares 目录，引 FatFs 要加 ff.c/ff.h/ffconf.h/diskio.c
 *   四个文件 + 调 ffconf + 桥接 diskio。而这里只需要"读一个根目录下的文件"
 *   这一件事，自己写 300 行更轻、更透明，也符合本工程一贯的寄存器风格。
 *
 * ★ 能力边界（够用就好，别指望它是完整 FAT 实现）：
 *   - 只读，不写、不创建、不删除
 *   - 只找**根目录**下的文件（不进子目录）
 *   - 只认 **8.3 短名**，而且必须传大写（例如 "IMG.BIN"）。
 *     ★ Windows 拷文件时一定会同时生成 8.3 短名，所以这个限制不影响使用；
 *       但长文件名文件生成什么短名不好猜 —— **建议直接把文件改名成 8.3**。
 *   - 不支持 FAT12 的簇链（能识别出来，但会明确报"不支持"）
 * ========================================================================== */

#ifndef __TF_FAT_H
#define __TF_FAT_H

#include "tf_card.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 卷信息
 * ========================================================================== */
typedef struct
{
    uint8_t  mounted;           /* 1 = 挂载成功 */
    uint8_t  fatType;           /* 12 / 16 / 32 */
    uint16_t bytesPerSector;    /* 通常 512 */
    uint8_t  sectorsPerCluster; /* 通常是 2 的幂 */
    uint8_t  numFats;           /* 通常是 2 */
    uint32_t partStartLba;      /* 分区起始 LBA（整个卡的 MBR 里解析出来） */
    uint32_t totalSectors;      /* 本分区总扇区数 */
    uint32_t fatStartLba;       /* 第一个 FAT 表的起始 LBA */
    uint32_t fatSectors;        /* 每个 FAT 占多少扇区 */
    uint32_t rootDirLba;        /* FAT12/16 根目录起始 LBA（FAT32 时为 0） */
    uint32_t rootDirSectors;    /* FAT12/16 根目录占多少扇区 */
    uint32_t rootCluster;       /* FAT32 根目录起始簇 */
    uint32_t dataStartLba;      /* 数据区起始 LBA（簇 2 从这里开始） */
    uint32_t clusterCount;      /* 总簇数 */
} TF_FAT_Info;

extern TF_FAT_Info g_tfFat;

/* ==========================================================================
 * 文件项
 * ========================================================================== */
typedef struct
{
    char     name[13];          /* 8.3 短名，形如 "IMG.BIN"（已去空格） */
    uint8_t  attr;              /* 属性字节 */
    uint32_t firstCluster;      /* 起始簇号 */
    uint32_t size;              /* 文件字节数 */
} TF_FAT_File;

/* ==========================================================================
 * 顺序读流
 * ========================================================================== */
typedef struct
{
    uint32_t firstCluster;      /* 起始簇 */
    uint32_t size;              /* 文件总字节数 */
    uint32_t cluster;           /* 当前簇号 */
    uint32_t posInFile;         /* 已经读出的字节数 */
    uint32_t posInCluster;      /* 当前簇内已读字节数 */
    uint8_t  opened;            /* 1 = 可用 */
} TF_FAT_Stream;

/* ==========================================================================
 * 接口
 * ========================================================================== */

/**
 * 挂载：读 MBR -> 第一个分区 -> BPB，算出所有布局参数。
 * ★ 依赖 TF_Init() 已经成功（内部会自己调 TF_EnsureInit）。
 */
TF_Status TF_FAT_Mount(void);

/** 卸载（清状态，卡本身不动） */
void TF_FAT_Unmount(void);

/** 在根目录里按 8.3 短名找文件。name83 必须大写，例如 "IMG.BIN"。 */
TF_Status TF_FAT_FindInRoot(const char *name83, TF_FAT_File *out);

/**
 * 诊断用：把根目录里的文件项列出来（跳过 LFN 项、卷标项、已删除项）。
 * 返回实际填了多少项（最多 max 项）。★ 文件在不在卡上，看这个最直接。
 */
uint32_t TF_FAT_ListRoot(TF_FAT_File *out, uint32_t max);

/** 打开一个顺序读流 */
TF_Status TF_FAT_StreamOpen(TF_FAT_Stream *s, const TF_FAT_File *f);

/**
 * 从流里顺序读最多 n 字节到 dst，返回**实际读到的字节数**。
 * ★ 读满 n 说明正常；返回 < n 就是到文件尾了。
 * ★ 内部按"扇区"为单位搬运，跨簇自动接上；单次最多读 4 个扇区。
 */
uint32_t TF_FAT_StreamRead(TF_FAT_Stream *s, uint8_t *dst, uint32_t n);

/** 状态码文字 */
const char *TF_FAT_ErrStr(TF_Status st);

/**
 * 挂载失败的**具体原因**（人话）。
 * ★ TF_FAT_Mount() 返回 TF_ERR_NOT_SD 时，光看错误码不知道是
 *   "卡是 exFAT" / "卡是 NTFS" / "根本没引导扇区" / "是 FAT12" ——
 *   这个函数给出准确说法，排查时直接打出来。
 *   挂载成功时返回 "OK (FAT16)" 之类。
 */
const char *TF_FAT_Reason(void);

/** 某个目录项是不是子目录（attr bit4） */
#define TF_FAT_IS_DIR(attr)   (((attr) & 0x10U) != 0U)

#ifdef __cplusplus
}
#endif

#endif /* __TF_FAT_H */
