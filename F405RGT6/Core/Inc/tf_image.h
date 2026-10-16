/* ============================================================================
 * tf_image.h —— 从 TF 卡读"灰度图像文件"，灌进一个大环形缓冲区
 *
 * ============================ 卡上文件格式 ============================
 * 上位机（Qt 主工程）导出的就是下面这个结构，直接拷到卡根目录即可：
 *
 *   偏移  长度  说明
 *   0     4     魔数 "GRY1"  (0x47 0x52 0x59 0x31)
 *   4     2     宽 width      (uint16 小端)
 *   6     2     高 height     (uint16 小端)
 *   8     4     数据字节数    (uint32 小端) = width * height
 *   12    4     数据的 CRC32  (uint32 小端)
 *               标准 CRC-32/ISO-HDLC：多项式 0xEDB88320，初值 0xFFFFFFFF，末尾取反
 *   16    N     数据：逐行，每行 width 字节；y=0 在最上面；行内是"扫描顺序"
 *
 *   ★ "扫描顺序" = 已经按弓字形/灰度映射处理过的那串字节，和原来
 *     串口下发时固件按顺序消费的那串**完全一样**。也就是说：
 *     固件这边拿到这个数组，可以当成"串口数据已经全收好了"。
 *
 *   ★ 文件建议就命名成 **IMG.BIN**（8.3 短名，大写），拷到卡根目录。
 *
 * ============================ 环形缓冲区 ============================
 * 卡上的数据不是一次性全读进 RAM 的（大图放不下），而是：
 *
 *     卡 ──TFIMG_Fill()──> [ 大环形缓冲 ] ──TFIMG_Get()──> 你的雕刻循环
 *
 *   TFIMG_Fill() 尽量把缓冲填满；TFIMG_Get() 从缓冲里取。
 *   两者解耦，所以卡的读取抖动不会直接卡到雕刻节奏上。
 *
 *   典型用法（main.c 里你自己接）：
 *       if (TFIMG_Open("IMG.BIN") == TF_OK) {
 *           uint8_t packet[64];
 *           while (!TFIMG_Eof()) {
 *               TFIMG_Fill();                        // 尽量补满
 *               if (TFIMG_Get(packet, 64) == 64) {
 *                   // packet[] 就是原来 RX_data[?] 那一包，照原样用
 *               }
 *           }
 *       }
 * ========================================================================== */

#ifndef __TF_IMAGE_H
#define __TF_IMAGE_H

#include "tf_card.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 配置
 * ========================================================================== */

/** 环形缓冲区大小（字节）。★ 必须是 2 的幂。
 *  本工程 RAM 目前只用了约 11KB（共 128KB+64KB），所以这里可以给得很宽。
 *  想再大就改这个数 —— 32KB 够放 16 个 2KB 的批次，一般用不着更大。 */
#ifndef TFIMG_RING_SIZE
#define TFIMG_RING_SIZE     (32U * 1024U)
#endif

/** 卡上图像文件的默认名字（8.3 短名，大写） */
#ifndef TFIMG_FILE_NAME
#define TFIMG_FILE_NAME     "IMG.BIN"
#endif

/** 文件头魔数 */
#define TFIMG_MAGIC0        0x47U   /* 'G' */
#define TFIMG_MAGIC1        0x52U   /* 'R' */
#define TFIMG_MAGIC2        0x59U   /* 'Y' */
#define TFIMG_MAGIC3        0x31U   /* '1' */

#define TFIMG_HEADER_SIZE   16U

/* ==========================================================================
 * 自检结果
 * ========================================================================== */
typedef struct
{
    uint8_t  opened;            /* 1 = 文件打开成功 */
    uint8_t  headerOk;          /* 1 = 魔数/尺寸/长度自洽 */
    uint8_t  crcOk;             /* 1 = 读出来的数据 CRC32 和文件头里记的一致 */
    uint8_t  eofReached;        /* 1 = 把整个文件读完了 */
    uint16_t width;
    uint16_t height;
    uint32_t expectBytes;       /* 文件头里说的数据字节数 */
    uint32_t loadedBytes;       /* 实际从卡上读进缓冲的字节数 */
    uint32_t consumedBytes;     /* 实际从缓冲取出的字节数 */
    uint32_t fileCrc32;         /* 文件头里记的 CRC32 */
    uint32_t calcCrc32;         /* 实际算出来的 CRC32 */
    uint32_t mountOk;           /* 1 = FAT 挂载成功 */
    uint8_t  fatType;           /* 12 / 16 / 32 */
    uint32_t fileSize;          /* 卡上这个文件的字节数（含 16 字节头） */
    uint32_t firstCluster;      /* 文件起始簇（诊断用） */
    TF_Status err;              /* 出错时的状态码 */
} TFIMG_TestResult;

/* ==========================================================================
 * 接口
 * ========================================================================== */

/**
 * 打开卡上的图像文件：挂 FAT -> 找文件 -> 读 16 字节头 -> 校验魔数。
 * @param name83 8.3 短名（大写），传 NULL 用 TFIMG_FILE_NAME
 */
TF_Status TFIMG_Open(const char *name83);

/** 关闭（清状态，卡本身不动） */
void TFIMG_Close(void);

uint8_t  TFIMG_Opened(void);
uint16_t TFIMG_Width(void);
uint16_t TFIMG_Height(void);
uint32_t TFIMG_TotalBytes(void);    /* = width * height */
uint32_t TFIMG_FileCrc32(void);     /* 文件头里记的 CRC32 */

/**
 * 尽量把环形缓冲填满（从卡上按扇区读）。
 * ★ 非阻塞：读满就返回，不会等到文件读完。
 * ★ 返回本次**新读进来**的字节数；返回 0 说明缓冲满了或者已经读到文件尾。
 *   所以调用方不需要区分这两种情况 —— 看 TFIMG_Avail() / TFIMG_Eof() 就行。
 */
uint32_t TFIMG_Fill(void);

/** 缓冲里现在还有多少字节可取 */
uint32_t TFIMG_Avail(void);

/** 1 = 卡上的数据已经全部取完了（都读进缓冲且都取走了） */
uint8_t  TFIMG_Eof(void);

/**
 * 从缓冲里取最多 n 字节到 dst，返回**实际取到的字节数**。
 * ★ 返回 < n 就说明缓冲空了 —— 先调 TFIMG_Fill() 再来取。
 * ★ 不会阻塞、不会自己去读卡，节奏完全由调用方掌握。
 */
uint32_t TFIMG_Get(uint8_t *dst, uint32_t n);

/** 取一个字节：1 = 取到，0 = 缓冲空 */
uint8_t  TFIMG_GetByte(uint8_t *out);

/** 诊断：累计从卡读入 / 累计被取走 / 环形缓冲总大小 */
uint32_t TFIMG_Loaded(void);
uint32_t TFIMG_Consumed(void);
uint32_t TFIMG_RingSize(void);

/**
 * 累计**已经被取走**的那些字节的 CRC32。
 * ★ 用途：雕刻完（数据全部取走）之后拿它和 TFIMG_FileCrc32() 比，
 *   相等就说明"卡上那份数据一路到激光都没走样"。一个数就能验证整条链路。
 * 取走 0 字节时返回 0。
 */
uint32_t TFIMG_ConsumedCrc32(void);

/** 最近一次错误 */
TF_Status   TFIMG_LastError(void);
const char *TFIMG_ErrStr(void);

/**
 * 自检：打开 -> 把整个文件读进缓冲并取出 -> 算 CRC32 -> 和文件头比对。
 * ★ 全程不动你的 main.c，可以单独调它验证"卡上的文件和导出的是否一致"。
 * @param res 结果（可以传 NULL）
 */
TF_Status TFIMG_SelfTest(TFIMG_TestResult *res);

#ifdef __cplusplus
}
#endif

#endif /* __TF_IMAGE_H */
