#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QImage>

class QLabel;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;

namespace Ui {
class Widget;
}

/**
 * 激光雕刻上位机：图片 -> 灰度 -> 导出 C 二维数组
 *
 * 只做离线转换：不发串口、不生成 G-code。
 * 导出的数组已按「运动顺序」排列（弓字形），固件可直接顺序消费。
 */
class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = 0);
    ~Widget();

private slots:
    void onOpenImage();       /* 载入图片 */
    void refreshGray();       /* 按当前参数重算灰度与预览 */
    void onSaveGrayArray();   /* 导出灰度 C 二维数组 */

    /* 内嵌文件浏览面板 */
    void onFbUp();
    void onFbActivated(QListWidgetItem *item);
    void onFbClicked(QListWidgetItem *item);
    void onFbOpen();
    void onFbCancel();
    void onFbFilterChanged(int idx);

private:
    void buildUi();
    void setStatus(const QString &msg, bool ok);
    void logMsg(const QString &msg, bool ok);
    void updatePreview();
    void loadImage(const QString &path);
    QStringList buildGrayArrayLines() const;

    /* 内嵌文件浏览面板（不弹独立窗口） */
    void enterFileBrowser(bool saveMode, const QStringList &filters,
                          const QString &suggested = QString());
    void refreshFbList();
    QStringList fbCurrentPatterns() const;

    Ui::Widget *ui;

    QImage  m_srcImage;       /* 原图 */
    QImage  m_gray;           /* 灰度数据：黑=255（出光强度），导出用 */
    QImage  m_previewImg;     /* 预览图：翻回正常显示，深色 = 会烧的地方 */
    QString m_srcPath;

    QLabel    *m_preview;
    QLabel    *m_srcLabel;
    QLabel    *m_status;
    QSpinBox  *m_widthSpin;
    QSpinBox  *m_heightSpin;

    QCheckBox *m_bidiBox;     /* 弓字形：奇数行倒序 */
    QSpinBox  *m_grayLoSpin;  /* 灰度映射下限 */
    QSpinBox  *m_grayHiSpin;  /* 灰度映射上限 */

    QPushButton    *m_openBtn;
    QPushButton    *m_grayBtn;
    QPlainTextEdit *m_log;

    /* 文件浏览面板 */
    bool           m_fbSaveMode;
    QString        m_fbDir;
    QStringList    m_pendingGrayLines;
    QStackedWidget *m_stack;
    QWidget        *m_mainPage;
    QLabel         *m_fbPathLabel;
    QPushButton    *m_fbUpBtn;
    QListWidget    *m_fbList;
    QLineEdit      *m_fbNameEdit;
    QComboBox      *m_fbFilterCombo;
    QPushButton    *m_fbOpenBtn;
    QPushButton    *m_fbCancelBtn;
};

#endif // WIDGET_H
