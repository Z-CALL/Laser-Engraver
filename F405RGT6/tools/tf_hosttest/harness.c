/* 主机端验证 tf_fat.c / tf_image.c：把 TF_ReadBlocks 桩到 disk.img 上 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tf_card.h"
#include "tf_fat.h"
#include "tf_image.h"

static FILE *g_img = NULL;

TF_Status TF_EnsureInit(void) { return TF_OK; }

TF_Status TF_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count)
{
    long off = (long)block * 512L;
    if (fseek(g_img, off, SEEK_SET) != 0) { return TF_ERR_DATA_TIMEOUT; }
    if (fread(buf, 1, (size_t)count * 512U, g_img) != (size_t)count * 512U)
    {
        return TF_ERR_DATA_TIMEOUT;
    }
    return TF_OK;
}

const char *TF_StatusStr(TF_Status st) { (void)st; return "?"; }

static int fails = 0;

int main(int argc, char **argv)
{
    TFIMG_TestResult r;
    TF_FAT_File      list[16];
    TF_Status        st;
    uint32_t         n, i;

    /* 预期值可传：harness disk.img [w h payload crc fileSize firstClu] */
    uint32_t eW   = (argc > 2) ? (uint32_t)strtoul(argv[2], 0, 0) : 70U;
    uint32_t eH   = (argc > 3) ? (uint32_t)strtoul(argv[3], 0, 0) : 71U;
    uint32_t ePay = (argc > 4) ? (uint32_t)strtoul(argv[4], 0, 0) : 4970U;
    uint32_t eCrc = (argc > 5) ? (uint32_t)strtoul(argv[5], 0, 16) : 0x98308C3Eu;
    uint32_t eSz  = (argc > 6) ? (uint32_t)strtoul(argv[6], 0, 0) : 4986U;
    uint32_t eClu = (argc > 7) ? (uint32_t)strtoul(argv[7], 0, 0) : 2U;
    uint32_t eFat = (argc > 8) ? (uint32_t)strtoul(argv[8], 0, 0) : 16U;

    if (argc < 2) { printf("usage: harness disk.img [w h payload crc fileSize firstClu]\n"); return 2; }

    g_img = fopen(argv[1], "rb");
    if (!g_img) { printf("cannot open %s\n", argv[1]); return 2; }

    /* ---- [1] 挂载 ---- */
    st = TF_FAT_Mount();
    printf("[1] mount          : %s\n", TF_FAT_ErrStr(st));
    printf("    partStartLba=%u fatType=FAT%u bytesPerSector=%u spc=%u numFats=%u\n",
           g_tfFat.partStartLba, g_tfFat.fatType, g_tfFat.bytesPerSector,
           g_tfFat.sectorsPerCluster, g_tfFat.numFats);
    printf("    fatStartLba=%u fatSectors=%u rootDirLba=%u rootDirSectors=%u\n",
           g_tfFat.fatStartLba, g_tfFat.fatSectors,
           g_tfFat.rootDirLba, g_tfFat.rootDirSectors);
    printf("    dataStartLba=%u clusterCount=%u totalSectors=%u\n",
           g_tfFat.dataStartLba, g_tfFat.clusterCount, g_tfFat.totalSectors);
    if (st != TF_OK) { ++fails; }
    if (g_tfFat.partStartLba != 131) { printf("    !! partStartLba 应为 131\n"); ++fails; }
    if (g_tfFat.fatType != eFat)     { printf("    !! fatType 应为 FAT%u\n", eFat); ++fails; }

    /* ---- [2] 列根目录 ---- */
    n = TF_FAT_ListRoot(list, 16);
    printf("[2] list root      : %u 项（应只有 1 项：卷标和 LFN 项要被跳过）\n", n);
    for (i = 0; i < n; ++i)
    {
        printf("    [%u] %-12s size=%-6u clu=%-4u attr=%02X\n",
               i, list[i].name, list[i].size, list[i].firstCluster, list[i].attr);
    }
    if (n != 1) { printf("    !! 应为 1 项\n"); ++fails; }
    if (n >= 1 && strcmp(list[0].name, "IMG.BIN") != 0) { printf("    !! 名字应为 IMG.BIN\n"); ++fails; }
    if (n >= 1 && list[0].size != eSz) { printf("    !! size 应为 %u\n", eSz); ++fails; }
    if (n >= 1 && list[0].firstCluster != eClu) { printf("    !! 起始簇应为 %u\n", eClu); ++fails; }

    /* ---- [3] 找文件（大小写不敏感）---- */
    {
        TF_FAT_File f;
        st = TF_FAT_FindInRoot("img.bin", &f);
        printf("[3] find img.bin   : %s (size=%u clu=%u)\n", TF_FAT_ErrStr(st), f.size, f.firstCluster);
        if (st != TF_OK || f.size != eSz || f.firstCluster != eClu) { ++fails; }
        st = TF_FAT_FindInRoot("NOPE.BIN", &f);
        printf("    find NOPE.BIN  : %s (应为 file not found)\n", TF_FAT_ErrStr(st));
        if (st == TF_OK) { ++fails; }
    }

    /* ---- [4] 整份自检（含 CRC32）---- */
    st = TFIMG_SelfTest(&r);
    printf("[4] selftest       : %s\n", TFIMG_ErrStr());
    printf("    w=%u h=%u expect=%u loaded=%u consumed=%u\n",
           r.width, r.height, r.expectBytes, r.loadedBytes, r.consumedBytes);
    printf("    fileCrc=%08X calcCrc=%08X crcOk=%u eof=%u\n",
           r.fileCrc32, r.calcCrc32, r.crcOk, r.eofReached);
    if (st != TF_OK) { printf("    !! 自检失败\n"); ++fails; }
    if (r.width != eW || r.height != eH) { printf("    !! 尺寸应为 %ux%u\n", eW, eH); ++fails; }
    if (r.expectBytes != ePay) { printf("    !! payload 应为 %u\n", ePay); ++fails; }
    if (r.fileCrc32 != eCrc) { printf("    !! 文件头 CRC 应为 %08X\n", eCrc); ++fails; }
    if (r.calcCrc32 != r.fileCrc32) { printf("    !! 算出的 CRC 和文件头不一致\n"); ++fails; }
    if (r.consumedBytes != ePay) { printf("    !! 取出的字节数不对\n"); ++fails; }

    /* ---- [5] 模拟 main.c：一包一包取（64 字节）---- */
    {
        uint8_t  pkt[64];
        uint32_t total = 0, got, crc32;
        st = TFIMG_Open("IMG.BIN");
        if (st != TF_OK) { printf("[5] open 失败: %s\n", TFIMG_ErrStr()); ++fails; }
        else
        {
            for (;;)
            {
                (void)TFIMG_Fill();
                got = TFIMG_Get(pkt, 64U);
                if (got == 0U)
                {
                    if (TFIMG_Eof()) { break; }
                    printf("    !! 缓冲空但没到 EOF，死锁\n"); ++fails; break;
                }
                total += got;
                if (total > 20000U) { printf("    !! 取多了\n"); ++fails; break; }
            }
            crc32 = TFIMG_ConsumedCrc32();
            printf("[5] packet loop    : 共取 %u 字节, CRC=%08X (应 4970 / 98308C3E)\n",
                   total, crc32);
            if (total != ePay) { ++fails; }
            if (crc32 != eCrc) { printf("    !! CRC 不对\n"); ++fails; }
        }
    }

    /* ---- [6] 一次取 1 字节（最碎的路径）---- */
    {
        uint8_t  b;
        uint32_t total = 0;
        st = TFIMG_Open("IMG.BIN");
        if (st == TF_OK)
        {
            for (;;)
            {
                if (TFIMG_GetByte(&b) == 0U)
                {
                    if (TFIMG_Eof()) { break; }
                    (void)TFIMG_Fill();
                    if (TFIMG_Avail() == 0U && TFIMG_Eof()) { break; }
                }
                else { ++total; }
                if (total > 20000U) { break; }
            }
            printf("[6] byte loop      : 共取 %u 字节 (应 %u), CRC=%08X\n",
                   total, ePay, TFIMG_ConsumedCrc32());
            if (total != ePay) { ++fails; }
            if (TFIMG_ConsumedCrc32() != eCrc) { ++fails; }
        }
    }

    printf("\nring size = %u 字节\n", TFIMG_RingSize());
    printf("==== %s (%d 项失败) ====\n", (fails == 0) ? "全部通过" : "有失败", fails);
    return (fails == 0) ? 0 : 1;
}
