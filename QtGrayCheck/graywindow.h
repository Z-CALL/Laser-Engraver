#ifndef GRAYWINDOW_H
#define GRAYWINDOW_H

#include <QImage>
#include <QWidget>

#include "graydecode.h"

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QScrollArea;

/**
 * 灰度数组反解 / 回环校验工具的主窗口。
 *
 * 用法：把上位机「导出灰度C数组(二维)」产出的文件拖进来或点「打开文件」，
 *       工具会还原成图像并给出逐项自检报告。
 *
 * 判据：
 *   · 还原出来的图案和原图一致  → 上位机的「图像->灰度->数组」链路没问题，
 *                                 错位只可能出在串口下发 / 固件 / 机械。
 *   · 还原出来就不对            → 上位机侧就有问题（极性、弓字形反转、映射…）。
 */
class GrayWindow : public QWidget
{
    Q_OBJECT

public:
    explicit GrayWindow(QWidget *parent = 0);

    /* 命令行直接给文件时用 */
    void loadFile(const QString &path);

private slots:
    void onOpen();
    void onSaveRestored();
    void onViewChanged(int idx);
    void onCompareSource();

private:
    void buildUi();
    void showResult(const GrayDecodeResult &r);
    void refreshView();

    QPushButton    *m_openBtn;
    QPushButton    *m_srcBtn;
    QPushButton    *m_saveBtn;
    QComboBox      *m_viewCombo;
    QScrollArea    *m_scroll;
    QLabel         *m_imageLabel;
    QPlainTextEdit *m_report;
    QLabel         *m_head;

    GrayDecodeResult m_res;
    QImage           m_sourceImage;   /* 可选：用来并排对照的原图 */
    QString          m_title;
};

#endif // GRAYWINDOW_H
