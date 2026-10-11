#include "graydecode.h"

#include <QFile>
#include <QFileInfo>
#include <QRegExp>
#include <QTextCodec>
#include <QTextStream>
#include <QtGlobal>

/* ============================ 编码 ============================ */

/**
 * 读文件，自动处理编码。
 *
 * 为什么不能直接 QTextStream::setCodec("UTF-8")：
 * 导出的文件表头是中文注释，Qt 自己写出来是 UTF-8，
 * 但用户可能用记事本/Keil 另存成 ANSI(GBK) 再拿过来 —— 那样中文会变乱码。
 * 这里先整体按 UTF-8 严格解码，失败再退回本地 8bit(中文 Windows 上是 GBK)。
 */
static bool readTextAuto(const QString &path, QString *out, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        if (err) *err = QString("打不开文件：%1").arg(f.errorString());
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    if (raw.isEmpty())
    {
        if (err) *err = QString("文件是空的");
        return false;
    }

    /* 严格 UTF-8 解码：有非法字节序列就返回空 */
    QTextCodec *u8 = QTextCodec::codecForName("UTF-8");
    QTextCodec::ConverterState st;
    const QString s8 = u8->toUnicode(raw.constData(), raw.size(), &st);
    if (st.invalidChars == 0)
    {
        *out = s8;
        return true;
    }

    /* 退回本地编码（中文 Windows = GBK/CP936） */
    QTextCodec *local = QTextCodec::codecForLocale();
    *out = local->toUnicode(raw);
    if (err) *err = QString("提示：文件不是合法 UTF-8，已按本地编码(%1)解读")
                        .arg(QString::fromLatin1(local->name()));
    return true;
}

/* ============================ 解析 ============================ */

GrayDecodeResult GrayDecode::fromFile(const QString &path)
{
    GrayDecodeResult r;
    QString text, note;
    if (!readTextAuto(path, &text, &note))
    {
        r.errorText = note;
        return r;
    }
    r = fromText(text);
    if (!note.isEmpty())
        r.report.prepend(note);
    return r;
}

GrayDecodeResult GrayDecode::fromText(const QString &text)
{
    GrayDecodeResult r;

    /* ---- 1. 抓尺寸宏 ----
       允许 `#define GRAY_IMG_W  200` / `GRAY_IMG_W 200` 两种写法，
       也允许中间有 tab 或多空格。 */
    QRegExp reW("GRAY_IMG_W\\s+(\\d+)");
    QRegExp reH("GRAY_IMG_H\\s+(\\d+)");
    if (reW.indexIn(text) < 0 || reH.indexIn(text) < 0)
    {
        r.errorText = QStringLiteral(
            "找不到 GRAY_IMG_W / GRAY_IMG_H 这两个尺寸宏。\n"
            "请确认这是上位机「导出灰度C数组(二维)」产出的文件。");
        return r;
    }
    r.w = reW.cap(1).toInt();
    r.h = reH.cap(1).toInt();

    /* ---- 2. 抓表头里的关键信息（极性/排列/灰度映射），用于自检 ---- */
    {
        QRegExp rePol("极性\\s*:\\s*([^\\r\\n\\*]+)");
        if (rePol.indexIn(text) >= 0)
            r.headerNote += QString("极性: %1\n").arg(rePol.cap(1).trimmed());

        QRegExp reArr("排列\\s*:\\s*([^\\r\\n\\*]+)");
        if (reArr.indexIn(text) >= 0)
            r.headerNote += QString("排列: %1\n").arg(reArr.cap(1).trimmed());

        QRegExp reMap("灰度映射\\s*:\\s*([^\\r\\n\\*]+)");
        if (reMap.indexIn(text) >= 0)
        {
            r.mapText = reMap.cap(1).trimmed();
            r.headerNote += QString("灰度映射: %1\n").arg(r.mapText);
        }
    }

    if (r.w <= 0 || r.h <= 0 || r.w > 20000 || r.h > 20000)
    {
        r.errorText = QString("尺寸不合理：%1 x %2").arg(r.w).arg(r.h);
        return r;
    }

    /* ---- 3. 切出数组体：从 '=' 之后到最后一个 '}' ----
       ★ 不能从第一个 '{' 开始切：注释里可能带花括号，而且宏定义里没有 '{'，
         但为了稳妥统一从 '=' 后面找第一个 '{'。 */
    const int eq = text.indexOf(QLatin1Char('='), reH.indexIn(text));
    const int b0 = (eq >= 0) ? text.indexOf(QLatin1Char('{'), eq)
                             : text.indexOf(QLatin1Char('{'));
    const int b1 = text.lastIndexOf(QLatin1Char('}'));
    if (b0 < 0 || b1 <= b0)
    {
        r.errorText = QStringLiteral("找不到 `= { ... };` 形式的数组体");
        return r;
    }

    /* ---- 4. 按行切分：每个 '{' 到 '}' 之间是一行 ----
       这样天然免疫换行/缩进的排版差异。 */
    const QString body = text.mid(b0 + 1, b1 - b0 - 1);

    QVector< QVector<int> > rows;
    QVector<bool>           revs;

    int i = 0;
    const int n = body.size();
    while (i < n)
    {
        /* 找下一个 '{' */
        while (i < n && body.at(i) != QLatin1Char('{')) ++i;
        if (i >= n) break;
        const int rowStart = ++i;                 /* 跳过 '{' */

        int close = body.indexOf(QLatin1Char('}'), rowStart);
        if (close < 0) break;

        const QString rowText = body.mid(rowStart, close - rowStart);

        /* 行尾注释里的方向标记：-> 正序, <- 倒序 */
        bool rev = false;
        bool hasMark = false;
        {
            const int cmt = body.indexOf(QLatin1String("/*"), close);
            const int cmtEnd = (cmt >= 0) ? body.indexOf(QLatin1String("*/"), cmt + 2) : -1;
            if (cmt >= 0 && cmtEnd > cmt)
            {
                const QString c = body.mid(cmt, cmtEnd - cmt);
                if (c.contains(QLatin1String("<-"))) { rev = true;  hasMark = true; }
                else if (c.contains(QLatin1String("->"))) { rev = false; hasMark = true; }
            }
        }

        /* 抽数字：扫一遍字符流，遇到数字就累积（比 split 更快也更宽容） */
        QVector<int> vals;
        vals.reserve(r.w);
        int cur = -1;
        for (int k = 0; k < rowText.size(); ++k)
        {
            const ushort c = rowText.at(k).unicode();
            if (c >= '0' && c <= '9')
            {
                if (cur < 0) cur = 0;
                cur = cur * 10 + (c - '0');
            }
            else
            {
                if (cur >= 0) { vals.append(cur); cur = -1; }
            }
        }
        if (cur >= 0) vals.append(cur);

        rows.append(vals);
        revs.append(rev);
        Q_UNUSED(hasMark);

        i = close + 1;
    }

    /* ---- 5. 结构自检 ---- */
    r.report << QStringLiteral("尺寸: %1 x %2（表头宏）").arg(r.w).arg(r.h);
    r.report << QStringLiteral("数组里读到 %1 行").arg(rows.size());

    if (rows.size() != r.h)
        r.report << QStringLiteral("[!] 行数不符：表头说 %1 行，实际读到 %2 行")
                        .arg(r.h).arg(rows.size());

    int badRows = 0, shortRows = 0;
    for (int y = 0; y < rows.size(); ++y)
    {
        if (rows.at(y).size() != r.w)
        {
            ++badRows;
            if (rows.at(y).size() < r.w) ++shortRows;
        }
    }
    if (badRows)
        r.report << QStringLiteral("[!] 有 %1 行的数值个数不等于 %2（其中 %3 行偏少）")
                        .arg(badRows).arg(r.w).arg(shortRows);
    else
        r.report << QStringLiteral("每行数值个数都等于宽度 %1 ✅").arg(r.w);

    /* ---- 6. 拼成 values（缺的补 0，多的截掉，保证后续不越界）---- */
    r.values.resize(r.w * r.h);
    r.values.fill(0);
    r.rowReversed.resize(r.h);
    r.rowReversed.fill(false);

    int outOfRange = 0;
    r.minVal = 255;
    r.maxVal = 0;

    for (int y = 0; y < r.h; ++y)
    {
        if (y < revs.size())
            r.rowReversed[y] = revs.at(y);

        if (y < rows.size())
        {
            const QVector<int> &v = rows.at(y);
            for (int x = 0; x < r.w; ++x)
            {
                int val = (x < v.size()) ? v.at(x) : 0;
                if (val < 0)   { val = 0;   ++outOfRange; }
                if (val > 255) { val = 255; ++outOfRange; }
                r.values[y * r.w + x] = (uchar)val;

                if (val < r.minVal) r.minVal = val;
                if (val > r.maxVal) r.maxVal = val;
                if (val == 0)   ++r.zeroCount;
                if (val >= 255) ++r.satCount;
            }
        }
    }

    r.bidiRows = 0;
    for (int y = 0; y < r.h; ++y)
        if (r.rowReversed.at(y)) ++r.bidiRows;

    r.report << QStringLiteral("每行末尾的 `->` / `<-` 标记：倒序行 %1 行 / 共 %2 行")
                    .arg(r.bidiRows).arg(r.h);
    r.report << QStringLiteral("数值范围: %1 .. %2；等于0的 %3 个，等于255的 %4 个")
                    .arg(r.minVal).arg(r.maxVal).arg(r.zeroCount).arg(r.satCount);

    if (outOfRange)
        r.report << QStringLiteral("[!] 有 %1 个值超出 0..255，已夹紧").arg(outOfRange);

    /* 弓字形自检：约定是"奇数行倒序"。
       若文件里的标记与这个约定不符，说明导出时的设置和注释对不上，必须报出来。 */
    if (r.bidiRows > 0)
    {
        int mismatch = 0;
        for (int y = 0; y < r.h; ++y)
        {
            const bool expectRev = ((y & 1) != 0);
            if (r.rowReversed.at(y) != expectRev) ++mismatch;
        }
        if (mismatch)
            r.report << QStringLiteral("[!] 有 %1 行的方向标记不符合「奇数行倒序」的约定 —— "
                                       "导出时的弓字形设置与文件注释不一致").arg(mismatch);
        else
            r.report << QStringLiteral("方向标记符合「奇数行倒序」约定 ✅");
    }
    else
    {
        r.report << QStringLiteral("没有任何行被标记为倒序（本次导出是自然行序）");
    }

    /* ---- 7. 逆映射（若表头写了 lo -> hi）---- */
    int lo = 0, hi = 255;
    bool mapped = false;
    if (!r.mapText.isEmpty() && !r.mapText.contains(QStringLiteral("无")))
    {
        QRegExp re("(\\d+)\\s*->\\s*(\\d+)");
        if (re.indexIn(r.mapText) >= 0)
        {
            lo = re.cap(1).toInt();
            hi = re.cap(2).toInt();
            mapped = (lo != 0) || (hi != 255);
        }
    }
    if (mapped)
    {
        r.report << QStringLiteral("检出灰度映射 %1 -> %2，还原时做逆映射（有精度损失）")
                        .arg(lo).arg(hi);
        if (hi <= lo)
        {
            r.report << QStringLiteral("[!] 映射区间非法(%1 -> %2)，已按原值显示")
                            .arg(lo).arg(hi);
            mapped = false;
        }
    }

    /* 逆映射表：把数组值 v(0..255) 映射回原始灰度。
       导出是  v' = lo + (v*(hi-lo)+127)/255   → 反解取最近似的 v。 */
    uchar inv[256];
    for (int v = 0; v < 256; ++v)
    {
        if (!mapped) { inv[v] = (uchar)v; continue; }
        if (v == 0)  { inv[v] = 0; continue; }   /* 0 不参与映射（原始就是 0） */
        const double orig = (double)(v - lo) * 255.0 / (double)(hi - lo);
        int o = (int)(orig + 0.5);
        if (o < 0)   o = 0;
        if (o > 255) o = 255;
        inv[v] = (uchar)o;
    }

    /* ---- 8. 生成两张图 ----
       rawImage : 数组原样（0=黑，直接当灰度画）—— 核对存储顺序用
       restored : 还原原图观感 = 反转奇数行 + 翻极性(255-v) + 逆映射
                  ★ 注意：数组值 0 表示"不出光"，在原图里对应白色，
                    所以显示成原图观感要用 255-v。 */
    r.rawImage  = QImage(r.w, r.h, QImage::Format_Grayscale8);
    r.restored  = QImage(r.w, r.h, QImage::Format_Grayscale8);

    for (int y = 0; y < r.h; ++y)
    {
        uchar *raw = r.rawImage.scanLine(y);
        uchar *res = r.restored.scanLine(y);
        const bool rev = r.rowReversed.at(y);

        for (int x = 0; x < r.w; ++x)
        {
            /* 数组里第 x 个值，对应图像的哪一列？倒序行要翻回来 */
            const int sx = rev ? (r.w - 1 - x) : x;
            const uchar v = r.values[y * r.w + x];

            raw[x] = v;                                  /* 原样 */
            res[sx] = (uchar)(255 - (int)inv[v]);        /* 还原观感 */
        }
    }

    r.ok = true;
    return r;
}

bool GrayDecode::savePng(const QImage &img, const QString &path)
{
    if (img.isNull())
        return false;
    return img.save(path, "PNG");
}
