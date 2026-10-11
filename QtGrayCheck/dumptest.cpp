#include "graydecode.h"
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <stdio.h>

/*
 * 诊断小程序：把 GrayDecode 的内部结果原样打出来，用来核对"文件里的值"
 * 与"工具算出的还原值"到底哪个环节反了。
 * 用法: dumptest.exe <导出文件.h>
 */
static const char *kPath = 0;

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    if (a.arguments().size() < 2) { fprintf(stderr, "usage: dumptest <file.h>\n"); return 2; }

    const GrayDecodeResult r = GrayDecode::fromFile(a.arguments().at(1));
    if (!r.ok) { fprintf(stderr, "FAIL: %s\n", r.errorText.toLocal8Bit().constData()); return 1; }

    printf("W=%d H=%d min=%d max=%d zero=%d sat=%d bidiRows=%d\n",
           r.w, r.h, r.minVal, r.maxVal, r.zeroCount, r.satCount, r.bidiRows);
    printf("headerNote: %s\n", r.headerNote.toLocal8Bit().constData());

    printf("\n row | 数组值[0..5]                | 还原图[0..5]              | 数组值[last-5..last]        | 还原图[last-5..last]\n");
    for (int y = 0; y < r.h && y < 4; ++y) {
        printf(" %3d |", y);
        for (int x = 0; x < 6; ++x) printf(" %3d", (int)r.values[y * r.w + x]);
        printf(" |");
        for (int x = 0; x < 6; ++x) printf(" %3d", (int)r.restored.constScanLine(y)[x]);
        printf(" |");
        for (int x = r.w - 6; x < r.w; ++x) printf(" %3d", (int)r.values[y * r.w + x]);
        printf(" |");
        for (int x = r.w - 6; x < r.w; ++x) printf(" %3d", (int)r.restored.constScanLine(y)[x]);
        printf("\n");
    }
    printf(" rowReversed[0..3]: %d %d %d %d\n",
           (int)r.rowReversed.value(0), (int)r.rowReversed.value(1),
           (int)r.rowReversed.value(2), (int)r.rowReversed.value(3));
    return 0;
}
