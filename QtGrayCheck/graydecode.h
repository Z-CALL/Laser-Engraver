#ifndef GRAYDECODE_H
#define GRAYDECODE_H

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

/**
 * 把上位机「导出灰度C数组」产出的文本反解回图像。
 *
 * ======================= 被解析的文件格式 =======================
 * 由 Widget::buildGrayArrayLines() 产出（widget.cpp）：
 *
 *     [块注释: 灰度值二维数组(0~255) —— 由「激光雕刻上位机」导出]
 *     [块注释: 极性    : 黑=255, 白=0 (已反相, 固定)]
 *     [块注释: 排列    : 弓字形(奇数行倒序) —— 与运动顺序一致]
 *     [块注释: 灰度映射: 无(原值 0~255)  或  lo -> hi (下限/上限)]
 *     [块注释: 尺寸/排列/点距/总点数 ...]
 *
 *     #define GRAY_IMG_W  200
 *     #define GRAY_IMG_H  200
 *
 *     const unsigned char gray_image[GRAY_IMG_H][GRAY_IMG_W] = {
 *         {  0,  0, 17, ... },   [注释: y=0 ->]
 *         { 17,  0,  0, ... },   [注释: y=1 <-]
 *         ...
 *     };
 *
 * 关键约定（反解必须完全按这个来，否则"还原"出来就是错的）：
 *   1. **极性与原图相反**：数组里 0 = 白(不出光)，255 = 黑(满功率出光)。
 *      所以还原原图观感要用 (255 - 数组值)。
 *   2. **弓字形**：数组已经是「运动顺序」。奇数行是倒序存的，
 *      还原成图像时必须把奇数行反转回来。
 *      → 每行末尾那个行内注释里的 `y=N ->` / `y=N <-` 就是行方向的权威标记，
 *        有标记就按标记走（比按 "奇数行必然倒序" 猜更可靠）。
 *   3. **灰度映射**：若表头写了 `lo -> hi`，说明导出时把 [0,255] 拉伸到了 [lo,hi]；
 *      要还原原始灰度需要逆映射。逆映射是近似的（拉伸会丢精度），
 *      这一点必须在报告里说清楚，不能假装能精确还原。
 *
 * ======================= 输出 =======================
 *   ok          解析是否成功
 *   errorText   失败原因（给人看的）
 *   w, h        尺寸
 *   values      原始数组值，行主序，长度 w*h（**未做任何变换**）
 *   rowReversed 每行在数组里是否是倒序的（来自注释标记）
 *   restored    还原成"原图观感"的图像（已翻转极性 + 反转奇数行 + 逆映射）
 *   rawImage    数组原样显示（不翻极性、不反行）—— 用来核对存储顺序
 *   report      逐项检查结果，界面直接显示
 */
struct GrayDecodeResult
{
    bool                 ok;
    QString              errorText;
    int                  w;
    int                  h;
    QVector<uchar>       values;      /* 原始数组值，行主序 */
    QVector<bool>        rowReversed; /* 该行在数组里是否为倒序 */
    QImage               restored;    /* 还原图（原图观感） */
    QImage               rawImage;    /* 数组原样 */
    QStringList          report;      /* 检查报告（逐行） */
    QString              headerNote;  /* 表头里抓到的关键信息 */

    /* 统计 */
    int                  minVal;
    int                  maxVal;
    int                  zeroCount;
    int                  satCount;    /* 达到 255 的个数 */
    QString              mapText;     /* 表头里的灰度映射说明 */
    int                  bidiRows;    /* 被标记为倒序的行数 */

    GrayDecodeResult() : ok(false), w(0), h(0), minVal(0), maxVal(0),
                         zeroCount(0), satCount(0), bidiRows(0) {}
};

class GrayDecode
{
public:
    /* 从文件解析（自动处理 UTF-8 / GBK 编码） */
    static GrayDecodeResult fromFile(const QString &path);

    /* 从已经读好的文本解析 */
    static GrayDecodeResult fromText(const QString &text);

    /* 把还原图画成 PNG（黑白为正） */
    static bool savePng(const QImage &img, const QString &path);
};

#endif // GRAYDECODE_H
