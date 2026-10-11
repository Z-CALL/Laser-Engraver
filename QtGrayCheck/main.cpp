#include "graywindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QStringList>

/**
 * GrayCheck —— 灰度数组反解 / 回环校验
 *
 * 命令行：
 *   GrayCheck.exe                     打开窗口，手动选文件
 *   GrayCheck.exe <数组文件>           直接载入该文件
 *   GrayCheck.exe <数组文件> <出.png>  无界面模式：解析并把还原图写出（方便脚本化校验）
 *
 * 退出码：
 *   0 = 解析成功（GUI 模式下也返回 0）
 *   1 = 解析失败
 *   2 = 参数错误
 */
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    const QStringList args = a.arguments();

    /* ---- 无界面模式：只要解析结果，不要窗口 ---- */
    if (args.size() >= 3)
    {
        const GrayDecodeResult r = GrayDecode::fromFile(args.at(1));
        if (!r.ok)
        {
            fprintf(stderr, "PARSE FAILED: %s\n",
                    r.errorText.toLocal8Bit().constData());
            return 1;
        }
        if (!GrayDecode::savePng(r.restored, args.at(2)))
        {
            fprintf(stderr, "SAVE FAILED: %s\n", qPrintable(args.at(2)));
            return 1;
        }
        /* 把自检报告也打出来，方便脚本判断 */
        printf("OK %dx%d values=%d min=%d max=%d zero=%d sat=%d bidiRows=%d\n",
               r.w, r.h, r.w * r.h, r.minVal, r.maxVal,
               r.zeroCount, r.satCount, r.bidiRows);
        for (int i = 0; i < r.report.size(); ++i)
            printf("  %s\n", r.report.at(i).toLocal8Bit().constData());
        return 0;
    }

    GrayWindow w;
    w.show();

    if (args.size() >= 2)
    {
        const QString p = args.at(1);
        if (QFileInfo(p).exists())
            w.loadFile(p);
        else
            fprintf(stderr, "no such file: %s\n", qPrintable(p));
    }
    return a.exec();
}
