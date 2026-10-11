#include "graywindow.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

static const int kViewRestored = 0;   /* 还原图（原图观感） */
static const int kViewRaw      = 1;   /* 数组原样 */
static const int kViewSource   = 2;   /* 原图（手动加载，用来对照） */

GrayWindow::GrayWindow(QWidget *parent) :
    QWidget(parent)
{
    buildUi();
}

void GrayWindow::buildUi()
{
    setWindowTitle(QStringLiteral("灰度数组反解校验 —— 看看上位机导出的数组还能不能还原成原图"));
    resize(1180, 800);

    /* ---------------- 顶部工具条 ---------------- */
    m_openBtn = new QPushButton(QStringLiteral("打开导出的数组文件(.h/.c/.txt)"));
    m_openBtn->setMinimumHeight(32);
    connect(m_openBtn, SIGNAL(clicked()), this, SLOT(onOpen()));

    m_viewCombo = new QComboBox;
    m_viewCombo->addItem(QStringLiteral("① 还原图（原图观感：已翻极性+反行+逆映射）"));
    m_viewCombo->addItem(QStringLiteral("② 数组原样（0=黑，核对存储顺序）"));
    m_viewCombo->addItem(QStringLiteral("③ 原图（手动打开，用来对照）"));
    m_viewCombo->setMinimumHeight(32);
    connect(m_viewCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onViewChanged(int)));

    m_srcBtn = new QPushButton(QStringLiteral("打开原图对照"));
    m_srcBtn->setMinimumHeight(32);
    connect(m_srcBtn, SIGNAL(clicked()), this, SLOT(onCompareSource()));

    m_saveBtn = new QPushButton(QStringLiteral("把还原图另存为 PNG"));
    m_saveBtn->setMinimumHeight(32);
    connect(m_saveBtn, SIGNAL(clicked()), this, SLOT(onSaveRestored()));

    QHBoxLayout *top = new QHBoxLayout;
    top->addWidget(m_openBtn);
    top->addWidget(m_viewCombo, 1);
    top->addWidget(m_srcBtn);
    top->addWidget(m_saveBtn);

    /* ---------------- 图像显示 ---------------- */
    m_imageLabel = new QLabel;
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setMinimumSize(300, 300);
    m_imageLabel->setStyleSheet("background:#202020; color:#bbb;");

    m_scroll = new QScrollArea;
    m_scroll->setWidget(m_imageLabel);
    m_scroll->setWidgetResizable(true);

    /* ---------------- 报告 ---------------- */
    m_head = new QLabel(QStringLiteral("还没有载入文件。\n"
        "请点「打开导出的数组文件」，选上位机「导出灰度C数组(二维)」产出的那个 .h/.c/.txt。"));
    m_head->setWordWrap(true);
    m_head->setStyleSheet("font-weight:bold; padding:4px;");

    m_report = new QPlainTextEdit;
    m_report->setReadOnly(true);
    m_report->setStyleSheet("font-family:Consolas,monospace; font-size:12px;");
    m_report->setPlainText(QStringLiteral(
        "【这个工具在验什么】\n"
        "  上位机下发前会做一连串变换：\n"
        "      原图 → 缩放 → 灰度 → 低灰度归零 → 反相(黑=255) → (可)灰度映射\n"
        "           → (可)弓字形奇数行倒序 → 切 64 点/包 → 串口\n"
        "  本工具把导出的数组**反着走一遍**，看能不能还原出原图。\n\n"
        "【怎么判读】\n"
        "  · ① 还原图 与 ③ 原图 一致\n"
        "      → 上位机这条链路没问题。错位/两个图只可能来自：串口下发、固件、机械。\n"
        "  · ① 还原图 与 ③ 原图 不一致（上下颠倒/左右镜像/错行/拉花）\n"
        "      → 上位机侧就有问题，且这里的现象会告诉你坏在哪一步。\n"
        "  · ② 数组原样 用来核对存储顺序：它应该是原图的**负片**；\n"
        "     若奇数行看起来是反的，那是弓字形，正常。\n\n"
        "【注意】\n"
        "  如果导出时用了灰度映射，逆映射是有损的，还原图会比原图\"平\"一些，\n"
        "  这不算错 —— 报告里会单独标出来。\n"));

    QSplitter *sp = new QSplitter(Qt::Horizontal);
    sp->addWidget(m_scroll);
    QWidget *right = new QWidget;
    QVBoxLayout *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(m_head);
    rv->addWidget(m_report, 1);
    sp->addWidget(right);
    sp->setStretchFactor(0, 3);
    sp->setStretchFactor(1, 2);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->addLayout(top);
    root->addWidget(sp, 1);
}

/* ============================ 载入 ============================ */

void GrayWindow::onOpen()
{
    const QString p = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择导出的灰度数组文件"), QString(),
        QStringLiteral("C/文本 (*.h *.c *.txt *.inc);;所有文件 (*)"));
    if (p.isEmpty())
        return;
    loadFile(p);
}

void GrayWindow::onCompareSource()
{
    const QString p = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择原始图片"), QString(),
        QStringLiteral("图片 (*.jpg *.jpeg *.png *.bmp);;所有文件 (*)"));
    if (p.isEmpty())
        return;

    QImage img(p);
    if (img.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("这张图读不出来：%1").arg(p));
        return;
    }
    /* 原图可能带透明通道/彩色；转成灰度显示，尺寸对齐到数组尺寸 */
    m_sourceImage = img.convertToFormat(QImage::Format_Grayscale8);
    if ((m_res.w > 0) && (m_res.h > 0))
        m_sourceImage = m_sourceImage.scaled(m_res.w, m_res.h,
                                             Qt::IgnoreAspectRatio,
                                             Qt::SmoothTransformation);
    m_viewCombo->setCurrentIndex(kViewSource);
    refreshView();
}

void GrayWindow::loadFile(const QString &path)
{
    const GrayDecodeResult r = GrayDecode::fromFile(path);

    if (!r.ok)
    {
        m_report->setPlainText(QStringLiteral("解析失败：\n\n%1").arg(r.errorText));
        m_head->setText(QStringLiteral("解析失败 —— 见右侧说明"));
        m_head->setStyleSheet("font-weight:bold; padding:4px; color:#c00;");
        m_imageLabel->setPixmap(QPixmap());
        m_imageLabel->setText(QStringLiteral("解析失败"));
        return;
    }

    m_title = QFileInfo(path).fileName();
    showResult(r);
}

void GrayWindow::showResult(const GrayDecodeResult &r)
{
    m_res = r;

    /* ---- 报告 ---- */
    QStringList lines;
    lines << QStringLiteral("文件：%1").arg(m_title);
    lines << QString();
    lines << QStringLiteral("===== 结构自检 =====");
    lines << r.report;
    lines << QString();

    if (!r.headerNote.isEmpty())
    {
        lines << QStringLiteral("===== 文件表头声明 =====");
        lines << r.headerNote.split('\n', QString::SkipEmptyParts);
        lines << QString();
    }

    lines << QStringLiteral("===== 结论怎么看 =====");
    lines << QStringLiteral("把「① 还原图」和「③ 原图」并排比：");
    lines << QStringLiteral("  · 一样      -> 上位机链路正常，问题在下发/固件/机械");
    lines << QStringLiteral("  · 上下颠倒  -> 行序反了（数组 y=0 对应的不是图像最上面一行）");
    lines << QStringLiteral("  · 左右镜像  -> 弓字形反转用错了（奇数行该反没反，或反之）");
    lines << QStringLiteral("  · 错行/斜纹 -> 行与包切分错位（宽度不是 64 的整数倍时容易出现）");
    lines << QStringLiteral("  · 整体反相  -> 极性搞反了（黑=255 这条约定被破坏）");

    m_report->setPlainText(lines.join('\n'));

    /* ---- 顶部摘要 ---- */
    QString head = QStringLiteral("%1   %2 x %3 = %4 点   值域 %5..%6   ")
                       .arg(m_title).arg(r.w).arg(r.h).arg(r.w * r.h)
                       .arg(r.minVal).arg(r.maxVal);
    head += (r.bidiRows > 0)
                ? QStringLiteral("弓字形：%1 行倒序").arg(r.bidiRows)
                : QStringLiteral("弓字形：无（自然行序）");
    if (!r.mapText.isEmpty())
        head += QStringLiteral("   映射：%1").arg(r.mapText);
    m_head->setText(head);
    m_head->setStyleSheet("font-weight:bold; padding:4px; color:#060;");

    m_viewCombo->setCurrentIndex(kViewRestored);
    refreshView();
}

/* ============================ 显示 ============================ */

void GrayWindow::onViewChanged(int idx)
{
    Q_UNUSED(idx);
    refreshView();
}

void GrayWindow::refreshView()
{
    if (!m_res.ok)
        return;

    const int idx = m_viewCombo->currentIndex();

    QImage show;
    if (idx == kViewRaw)
        show = m_res.rawImage;
    else if (idx == kViewSource)
        show = m_sourceImage;
    else
        show = m_res.restored;

    if (show.isNull())
    {
        m_imageLabel->setPixmap(QPixmap());
        m_imageLabel->setText((idx == kViewSource)
            ? QStringLiteral("还没打开原图。\n点上面的「打开原图对照」选一张。")
            : QStringLiteral("（空）"));
        return;
    }

    m_imageLabel->setText(QString());
    /* 放大到看得清，但别超过窗口太多 */
    const int maxW = qMax(300, m_scroll->viewport()->width() - 24);
    const int maxH = qMax(300, m_scroll->viewport()->height() - 24);
    QImage big = show;
    if ((show.width() < maxW / 2) && (show.width() > 0))
    {
        const int scale = qMax(1, qMin(maxW / show.width(), maxH / show.height()));
        if (scale > 1)
            big = show.scaled(show.width() * scale, show.height() * scale,
                              Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    m_imageLabel->setPixmap(QPixmap::fromImage(big));
    m_imageLabel->resize(big.size());
}

void GrayWindow::onSaveRestored()
{
    if (!m_res.ok)
    {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("先打开一个导出的数组文件"));
        return;
    }

    QString suggest = QStringLiteral("restored.png");
    if (!m_title.isEmpty())
        suggest = QFileInfo(m_title).completeBaseName() + QStringLiteral("_restored.png");

    const QString p = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存还原图"), suggest,
        QStringLiteral("PNG (*.png)"));
    if (p.isEmpty())
        return;

    if (!GrayDecode::savePng(m_res.restored, p))
        QMessageBox::warning(this, QStringLiteral("失败"),
                             QStringLiteral("写不出去：%1").arg(p));
    else
        m_head->setText(QStringLiteral("还原图已保存：%1").arg(p));
}
