#include "widget.h"
#include "ui_widget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QDir>
#include <QMessageBox>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>

static const int kGrayCutoff = 20;

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget)
{
    ui->setupUi(this);
    buildUi();

    setWindowTitle(QStringLiteral("激光雕刻上位机 - 图片转灰度数组"));

    int w = 600;
    int h = 1024;
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    if ((w > avail.width()) || (h > avail.height()))
    {
        const double s = qMin((double)avail.width()  / (double)w,
                              (double)avail.height() / (double)h);
        w = qRound(w * s);
        h = qRound(h * s);
    }
    setFixedSize(w, h);
    move((avail.width() - w) / 2, qMax(0, (avail.height() - h) / 2));
}

Widget::~Widget()
{
    delete ui;
}

void Widget::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);

    /* ================= 主控制页 ================= */
    m_mainPage = new QWidget(this);
    QVBoxLayout *main = new QVBoxLayout(m_mainPage);
    main->setSpacing(6);

    /* 打开图片 */
    QHBoxLayout *row1 = new QHBoxLayout();
    m_openBtn = new QPushButton(QStringLiteral("打开图片"), m_mainPage);
    m_srcLabel = new QLabel(QStringLiteral("未选择图片"), m_mainPage);
    m_srcLabel->setStyleSheet("color: gray;");
    row1->addWidget(m_openBtn);
    row1->addWidget(m_srcLabel, 1);
    main->addLayout(row1);

    /* 灰度预览 */
    m_preview = new QLabel(m_mainPage);
    m_preview->setMinimumSize(320, 200);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::Box);
    m_preview->setText(QStringLiteral("灰度图预览"));
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    main->addWidget(m_preview, 1);

    /* 目标尺寸 + 极性说明 */
    QGridLayout *grid = new QGridLayout();
    grid->addWidget(new QLabel(QStringLiteral("目标宽(点)"), m_mainPage), 0, 0);
    m_widthSpin = new QSpinBox(m_mainPage);
    m_widthSpin->setRange(1, 4096);
    m_widthSpin->setValue(64);
    grid->addWidget(m_widthSpin, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("目标高(点)"), m_mainPage), 0, 2);
    m_heightSpin = new QSpinBox(m_mainPage);
    m_heightSpin->setRange(1, 4096);
    m_heightSpin->setValue(64);
    grid->addWidget(m_heightSpin, 0, 3);

    /* 极性固定为黑=255：图像灰度按通用定义是 0=黑/255=白（它表示**亮度**），
       而雕刻要的是**出光强度**且"越黑越烧" —— 两者方向相反，代码里固定做 255-v。
       预览已翻回来显示，所以预览里深色的地方才是会烧的地方。 */
    QLabel *polLabel = new QLabel(QStringLiteral("灰度极性：黑=255(越黑越出光)"), m_mainPage);
    polLabel->setStyleSheet("color: gray;");
    grid->addWidget(polLabel, 1, 0, 1, 4);
    main->addLayout(grid);

    /* ---- 导出参数 ---- */
    QGridLayout *grid2 = new QGridLayout();

    /* 弓字形：奇数行数据倒序，与"来回扫描"的运动顺序一致。
       固件按「数据顺序 = 运动顺序」切激光强度，所以顺序必须在这里拍平；
       选错的表现是奇数行左右镜像，整幅图成拉花。 */
    m_bidiBox = new QCheckBox(QStringLiteral("弓字形(奇数行倒序，与运动顺序一致)"), m_mainPage);
    m_bidiBox->setChecked(true);
    grid2->addWidget(m_bidiBox, 0, 0, 1, 4);

    /* 灰度映射：把 0~255 线性压到 lo~hi。
       激光留痕有能量门槛，抬下限能把浪费掉的动态范围让给有区分度的部分。
       默认 0~255 = 不映射。 */
    grid2->addWidget(new QLabel(QStringLiteral("灰度映射(下限/上限)"), m_mainPage), 1, 0);
    m_grayLoSpin = new QSpinBox(m_mainPage);
    m_grayLoSpin->setRange(0, 255);
    m_grayLoSpin->setValue(0);
    grid2->addWidget(m_grayLoSpin, 1, 1);
    m_grayHiSpin = new QSpinBox(m_mainPage);
    m_grayHiSpin->setRange(0, 255);
    m_grayHiSpin->setValue(255);
    grid2->addWidget(m_grayHiSpin, 1, 3);
    main->addLayout(grid2);

    /* 导出灰度 C 数组 */
    m_grayBtn = new QPushButton(QStringLiteral("导出灰度C数组(二维)"), m_mainPage);
    m_grayBtn->setMinimumHeight(34);
    m_grayBtn->setStyleSheet("font-weight: bold;");
    main->addWidget(m_grayBtn);

    /* 日志 */
    m_log = new QPlainTextEdit(m_mainPage);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMaximumHeight(140);
    main->addWidget(m_log);

    m_status = new QLabel(QStringLiteral("就绪：打开图片 -> 调好参数 -> 导出灰度C数组"), m_mainPage);
    m_status->setWordWrap(true);
    main->addWidget(m_status);
    main->addStretch(0);

    m_stack->addWidget(m_mainPage);

    /* ================= 文件浏览页 ================= */
    QWidget *fbPage = new QWidget(this);
    QVBoxLayout *fv = new QVBoxLayout(fbPage);
    fv->setSpacing(6);

    QHBoxLayout *fh1 = new QHBoxLayout();
    fh1->addWidget(new QLabel(QStringLiteral("路径"), fbPage));
    m_fbPathLabel = new QLabel(QString(), fbPage);
    m_fbPathLabel->setStyleSheet("color: blue;");
    fh1->addWidget(m_fbPathLabel, 1);
    m_fbUpBtn = new QPushButton(QStringLiteral("上级"), fbPage);
    fh1->addWidget(m_fbUpBtn);
    fv->addLayout(fh1);

    m_fbList = new QListWidget(fbPage);
    m_fbList->setUniformItemSizes(true);
    fv->addWidget(m_fbList, 1);

    QHBoxLayout *fh2 = new QHBoxLayout();
    fh2->addWidget(new QLabel(QStringLiteral("文件名"), fbPage));
    m_fbNameEdit = new QLineEdit(fbPage);
    fh2->addWidget(m_fbNameEdit, 1);
    fv->addLayout(fh2);

    m_fbFilterCombo = new QComboBox(fbPage);
    fv->addWidget(m_fbFilterCombo);

    QHBoxLayout *fh3 = new QHBoxLayout();
    m_fbCancelBtn = new QPushButton(QStringLiteral("取消"), fbPage);
    m_fbCancelBtn->setMinimumHeight(34);
    m_fbOpenBtn = new QPushButton(QStringLiteral("打开"), fbPage);
    m_fbOpenBtn->setMinimumHeight(34);
    m_fbOpenBtn->setStyleSheet("font-weight: bold;");
    fh3->addStretch(1);
    fh3->addWidget(m_fbCancelBtn);
    fh3->addWidget(m_fbOpenBtn);
    fv->addLayout(fh3);

    m_stack->addWidget(fbPage);
    root->addWidget(m_stack);

    /* ---- 信号 ---- */
    connect(m_openBtn,    SIGNAL(clicked()), this, SLOT(onOpenImage()));
    connect(m_widthSpin,  SIGNAL(valueChanged(int)), this, SLOT(refreshGray()));
    connect(m_heightSpin, SIGNAL(valueChanged(int)), this, SLOT(refreshGray()));
    connect(m_grayBtn,    SIGNAL(clicked()), this, SLOT(onSaveGrayArray()));

    connect(m_fbUpBtn,       SIGNAL(clicked()), this, SLOT(onFbUp()));
    connect(m_fbList,        SIGNAL(itemActivated(QListWidgetItem*)),
            this, SLOT(onFbActivated(QListWidgetItem*)));
    connect(m_fbList,        SIGNAL(itemClicked(QListWidgetItem*)),
            this, SLOT(onFbClicked(QListWidgetItem*)));
    connect(m_fbFilterCombo, SIGNAL(activated(int)), this, SLOT(onFbFilterChanged(int)));
    connect(m_fbOpenBtn,     SIGNAL(clicked()), this, SLOT(onFbOpen()));
    connect(m_fbCancelBtn,   SIGNAL(clicked()), this, SLOT(onFbCancel()));

    m_fbSaveMode = false;
    m_pendingGrayLines.clear();
}

void Widget::setStatus(const QString &msg, bool ok)
{
    m_status->setText(msg);
    m_status->setStyleSheet(ok ? "color: green;" : "color: red;");
}

void Widget::logMsg(const QString &msg, bool ok)
{
    m_log->appendPlainText((ok ? QStringLiteral("[>] ") : QStringLiteral("[!] ")) + msg);
}

void Widget::onOpenImage()
{
    enterFileBrowser(false,
        QStringList() << QStringLiteral("图片 (*.jpg *.jpeg *.png *.bmp *.ppm *.xbm)")
                      << QStringLiteral("所有文件 (*)"),
        QString());
}

void Widget::loadImage(const QString &path)
{
    QImage img(path);
    if (img.isNull())
    {
        setStatus(QStringLiteral("图片加载失败：%1").arg(path), false);
        return;
    }
    m_srcPath  = path;
    m_srcImage = img;
    m_srcLabel->setText(QString("%1  (%2x%3)").arg(path).arg(img.width()).arg(img.height()));
    m_srcLabel->setStyleSheet("color: black;");

    /* 目标宽高默认取原图尺寸；超过上限时按比例缩到上限内 */
    int w = img.width();
    int h = img.height();
    const int kMax = m_widthSpin->maximum();
    if ((w > kMax) || (h > kMax))
    {
        const double s = qMin((double)kMax / w, (double)kMax / h);
        w = qRound(w * s);
        h = qRound(h * s);
    }
    m_widthSpin->setValue(w);
    m_heightSpin->setValue(h);

    refreshGray();
}

void Widget::refreshGray()
{
    if (m_srcImage.isNull())
        return;

    const int w = m_widthSpin->value();
    const int h = m_heightSpin->value();

    QImage scaled = m_srcImage.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage gray   = scaled.convertToFormat(QImage::Format_Grayscale8);

    /* 一次遍历同时产出两张方向相反的图：
       · data(导出用)：反相 + 低灰度截断 → 黑=255，即出光强度
       · view(预览用)：把 data 再翻回来 → 深色 = 会烧的地方
       ⚠ 直接把 data 拿去显示会变成负片（该烧的地方反而是白的）。 */
    QImage data(gray.size(), QImage::Format_Grayscale8);
    QImage view(gray.size(), QImage::Format_Grayscale8);

    for (int y = 0; y < gray.height(); ++y)
    {
        const uchar *src = gray.constScanLine(y);
        uchar       *dat = data.scanLine(y);
        uchar       *pvw = view.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
        {
            int val = 255 - (int)src[x];
            if (val < kGrayCutoff) { val = 0; }
            dat[x] = (uchar)val;
            pvw[x] = (uchar)(255 - val);
        }
    }

    m_gray       = data;
    m_previewImg = view;

    updatePreview();
    setStatus(QStringLiteral("灰度图已就绪：%1 x %2 点").arg(w).arg(h), true);
}

void Widget::updatePreview()
{
    if (m_previewImg.isNull())
        return;
    m_preview->setPixmap(QPixmap::fromImage(
                 m_previewImg.scaled(m_preview->size(), Qt::KeepAspectRatio,
                                     Qt::FastTransformation)));
}

/* ================= 灰度数组导出 ================= */

/**
 * 把当前灰度图拼成 C 二维数组的文本行（每行一条，逐行写盘即为完整文件）。
 *
 * 生成的样子：
 *     #define GRAY_IMG_W  64
 *     #define GRAY_IMG_H  64
 *     const unsigned char gray_image[GRAY_IMG_H][GRAY_IMG_W] = {
 *         {  0,  0, 17, ... },   // y=0
 *     };
 *
 * ★ 数组 = 固件按顺序消费的那串字节：勾了弓字形则奇数行倒序、
 *   灰度映射也一并压进去 —— 固件不认"图像第几列"，只认"第几个点"。
 * ⚠ 勾了弓字形时，奇数行的 x 增大 = 图像从右往左（文件头注释会写明）。
 */
QStringList Widget::buildGrayArrayLines() const
{
    const int  w      = m_gray.width();
    const int  h      = m_gray.height();
    const bool bidi   = m_bidiBox->isChecked();
    const int  lo     = m_grayLoSpin->value();
    const int  hi     = m_grayHiSpin->value();
    const bool mapped = (lo != 0) || (hi != 255);

    QStringList out;

    /* 文件头注释：数组离开界面就只剩数字，把"怎么来的"钉在文件里 */
    out << QStringLiteral("/*");
    out << QStringLiteral(" * 灰度值二维数组(0~255) —— 由「激光雕刻上位机」导出");
    out << QStringLiteral(" *");
    out << QStringLiteral(" * 源图    : %1").arg(m_srcPath.isEmpty()
                                                  ? QStringLiteral("(未记录)")
                                                  : m_srcPath);
    out << QStringLiteral(" * 尺寸    : %1 x %2  (宽 %1 点/行, 高 %2 行)").arg(w).arg(h);
    out << QStringLiteral(" * 取值    : 0~255");
    out << QStringLiteral(" * 极性    : 黑=255, 白=0 (已反相, 固定)");
    out << QStringLiteral(" * 低灰度  : 灰度值 < %1 的已归零(去背景灰底)").arg(kGrayCutoff);
    out << QStringLiteral(" * 排列    : %1").arg(bidi
                                    ? QStringLiteral("弓字形(奇数行倒序) —— 与运动顺序一致")
                                    : QStringLiteral("自然行序(每行都从左到右)"));
    out << QStringLiteral(" * 灰度映射: %1").arg(mapped
                                    ? QStringLiteral("%1 -> %2 (下限/上限)").arg(lo).arg(hi)
                                    : QStringLiteral("无(原值 0~255)"));
    out << QStringLiteral(" * 行序    : gray_image[y][x]");
    out << QStringLiteral(" *           y = 第 y 条扫描线 = 图像第 y 行, y=0 在最上面");
    out << QStringLiteral(" *           x = 该行**扫描顺序**第 x 个点%1")
           .arg(bidi ? QStringLiteral("(奇数行 x 增大 = 图像从右往左)")
                     : QStringLiteral("(x 增大 = 图像从左往右)"));
    out << QStringLiteral(" * 点距    : 由单片机侧决定 —— 本文件只给灰度值, 不含物理尺寸");
    out << QStringLiteral(" * 总点数  : %1 = %2 x %3").arg((qint64)w * h).arg(h).arg(w);
    out << QStringLiteral(" */");
    out << QString();

    out << QStringLiteral("#define GRAY_IMG_W  %1").arg(w);
    out << QStringLiteral("#define GRAY_IMG_H  %1").arg(h);
    out << QString();
    out << QStringLiteral("const unsigned char gray_image[GRAY_IMG_H][GRAY_IMG_W] = {");

    const int perLine = 16;               /* 每行排 16 个值，纯排版 */

    for (int y = 0; y < h; ++y)
    {
        const uchar *row = m_gray.constScanLine(y);
        const bool   rev = bidi && ((y & 1) != 0);
        QString s = QStringLiteral("    {");

        for (int x = 0; x < w; ++x)
        {
            const int sx = rev ? (w - 1 - x) : x;   /* 按扫描顺序取源列 */
            int v = (int)row[sx];
            /* v==0 是「完全不出光」，不参与映射 —— 否则会被下限 lo 抬起来 */
            if (mapped && (v != 0))
            {
                v = lo + (v * (hi - lo) + 127) / 255;
                if (v < 0)   { v = 0; }
                if (v > 255) { v = 255; }
            }

            s += QString("%1").arg(v, 3, 10, QChar(' '));   /* 右对齐，便于肉眼核对 */

            if (x == w - 1)
                break;                    /* 本行最后一个值：不加逗号 */
            s += QChar(',');
            if ((x + 1) % perLine == 0)
                s += QStringLiteral("\n     ");
            else
                s += QChar(' ');
        }

        s += (y == h - 1) ? QStringLiteral(" }") : QStringLiteral(" },");
        s += QStringLiteral("   /* y=%1 %2 */")
                 .arg(y).arg(rev ? QStringLiteral("<-") : QStringLiteral("->"));
        out << s;
    }

    out << QStringLiteral("};");
    return out;
}

void Widget::onSaveGrayArray()
{
    if (m_gray.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先打开一张图片"));
        return;
    }

    const int    w     = m_gray.width();
    const int    h     = m_gray.height();
    const qint64 total = (qint64)w * h;

    /* 大图拦一下：文本约 5.4 字节/值，4096x4096 会到 90MB 级纯文本 */
    if (total > 1024 * 1024)
    {
        const qint64 estMb = (qint64)((double)total * 5.42 / (1024.0 * 1024.0) + 0.5);
        const QString msg = QStringLiteral(
            "当前尺寸 %1 x %2 = %3 个灰度值，\n"
            "导出成 C 二维数组大约要 %4 MB 文本（每个值约 5.4 字节）。\n\n"
            "确定继续吗？")
                .arg(w).arg(h).arg(total).arg(estMb);
        if (QMessageBox::question(this, QStringLiteral("导出确认"), msg,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    logMsg(QStringLiteral("导出参数: %1; 灰度映射 %2; 低灰度<%3 归零")
           .arg(m_bidiBox->isChecked() ? QStringLiteral("弓字形(奇数行倒序)")
                                       : QStringLiteral("自然行序"))
           .arg((m_grayLoSpin->value() != 0 || m_grayHiSpin->value() != 255)
                    ? QStringLiteral("%1~%2").arg(m_grayLoSpin->value())
                                            .arg(m_grayHiSpin->value())
                    : QStringLiteral("无(原值)"))
           .arg(kGrayCutoff), true);

    m_pendingGrayLines = buildGrayArrayLines();

    enterFileBrowser(true,
        QStringList() << QStringLiteral("C 源文件 (*.h *.c *.txt)")
                      << QStringLiteral("所有文件 (*)"),
        QStringLiteral("gray_image.h"));
}

/* ================= 内嵌文件浏览面板 ================= */

void Widget::enterFileBrowser(bool saveMode, const QStringList &filters,
                              const QString &suggested)
{
    m_fbSaveMode = saveMode;
    m_fbDir = QDir::currentPath();
    m_fbOpenBtn->setText(saveMode ? QStringLiteral("保存") : QStringLiteral("打开"));
    m_fbNameEdit->setText(suggested);
    m_fbFilterCombo->clear();
    m_fbFilterCombo->addItems(filters);
    refreshFbList();
    m_stack->setCurrentIndex(1);
    m_fbList->setFocus();
}

void Widget::refreshFbList()
{
    m_fbPathLabel->setText(m_fbDir);
    m_fbList->clear();
    QDir d(m_fbDir);

    /* 子目录在前，标 [目录] 前缀 */
    const QStringList dirs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &n : dirs)
        m_fbList->addItem(QStringLiteral("[目录] ") + n);

    const QStringList files = d.entryList(fbCurrentPatterns(), QDir::Files, QDir::Name);
    for (const QString &n : files)
        m_fbList->addItem(n);
}

QStringList Widget::fbCurrentPatterns() const
{
    /* 从 "图片 (*.jpg *.png)" 里取出 *.xxx 部分 */
    const QString f = m_fbFilterCombo->currentText();
    const int l = f.indexOf('(');
    const int r = f.lastIndexOf(')');
    if ((l < 0) || (r <= l))
        return QStringList() << "*";
    const QString inner = f.mid(l + 1, r - l - 1);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QStringList pats = inner.split(QChar(' '), Qt::SkipEmptyParts);
#else
    QStringList pats = inner.split(QChar(' '), QString::SkipEmptyParts);
#endif
    return pats.isEmpty() ? (QStringList() << "*") : pats;
}

void Widget::onFbFilterChanged(int idx)
{
    Q_UNUSED(idx);
    refreshFbList();
}

void Widget::onFbUp()
{
    QDir d(m_fbDir);
    if (d.cdUp())
    {
        m_fbDir = d.absolutePath();
        refreshFbList();
    }
}

void Widget::onFbActivated(QListWidgetItem *item)
{
    if (!item) return;
    const QString t = item->text();
    if (t.startsWith(QStringLiteral("[目录] ")))
    {
        QDir d(m_fbDir);
        if (d.cd(t.mid(5)))
        {
            m_fbDir = d.absolutePath();
            refreshFbList();
        }
    }
    else
    {
        m_fbNameEdit->setText(t);
        if (!m_fbSaveMode)
            onFbOpen();                   /* 打开模式：双击文件即直接打开 */
    }
}

void Widget::onFbClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString t = item->text();
    if (!t.startsWith(QStringLiteral("[目录] ")))
        m_fbNameEdit->setText(t);
}

void Widget::onFbOpen()
{
    const QString name = m_fbNameEdit->text().trimmed();
    if (name.isEmpty()) return;
    const QString full = QDir(m_fbDir).absoluteFilePath(name);

    m_stack->setCurrentIndex(0);          /* 先切回主页面板 */

    if (!m_fbSaveMode)
    {
        loadImage(full);
        return;
    }

    /* 灰度数组写盘：UTF-8 字节直写 + 统一 CRLF + **不带 BOM**。
       ⚠ 不走 QTextStream —— 它写 UTF-8 会带 BOM，armcc/Keil 对 BOM 敏感。 */
    QFile f(full);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        setStatus(QStringLiteral("写入失败：%1").arg(full), false);
        return;
    }
    QByteArray buf;
    for (const QString &l : m_pendingGrayLines)
    {
        buf += l.toUtf8();
        buf += "\r\n";
    }
    f.write(buf);
    f.close();

    setStatus(QStringLiteral("灰度数组已保存：%1 x %2 -> %3")
              .arg(m_gray.height()).arg(m_gray.width()).arg(full), true);
    logMsg(QStringLiteral("已导出 %1 x %2 (%3 个值) -> %4")
           .arg(m_gray.height()).arg(m_gray.width())
           .arg((qint64)m_gray.width() * m_gray.height()).arg(full), true);

    m_pendingGrayLines.clear();
}

void Widget::onFbCancel()
{
    m_stack->setCurrentIndex(0);
    m_pendingGrayLines.clear();
}
