#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>

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
class QProgressBar;
class SerialIo;

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

    /* SerialIo 日志回调的落点（供 C 风格回调函数调用） */
    void appendSerialLog(const QString &line);

private slots:
    void onOpenImage();       /* 载入图片 */
    void refreshGray();       /* 按当前参数重算灰度与预览 */
    void onSaveGrayArray();   /* 导出灰度 C 二维数组 */

    /* 串口面板 */
    void onSerialRefresh();   /* 重新枚举端口 */
    void onSerialOpenClose(); /* 打开/关闭串口 */
    void onSerialSend();      /* 按新协议下发整幅图 */
    void onSerialAbort();     /* 中止发送 */
    void onSerialPreview();   /* 只打包不发，把包结构打到日志 */
    void onSerialIntervalChanged(int ms);
    void onSerialModeChanged(int idx);
    void onSerialHsTimeoutChanged(int ms);
    void onSerialPacketSent(int ctrl, int pixelCount, quint16 crc, int inFlight);
    void onSerialProgress(int sent, int total, int rows, int totalRows);
    void onSerialFinished(bool aborted);
    void onSerialError(const QString &msg);
    void onSerialRequestReceived(int seq, int remaining);
    void onSerialHandshakeTimeout(int waitedMs, int remaining);
    void onSerialRxActivity(qint64 totalBytes);

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

    /* ---- 串口下发 ---- */
    QComboBox    *m_serPortCombo;
    QPushButton  *m_serRefreshBtn;
    QPushButton  *m_serOpenBtn;
    QComboBox    *m_serBaudCombo;      /* 波特率 */
    QComboBox    *m_serDataCombo;      /* 数据位 */
    QComboBox    *m_serParityCombo;    /* 校验位：无/奇/偶 */
    QComboBox    *m_serStopCombo;      /* 停止位 */
    QComboBox    *m_serModeCombo;      /* 握手 / 自由 */
    QSpinBox     *m_serHsTimeoutSpin;  /* 握手超时 ms */
    QSpinBox     *m_serIntervalSpin;   /* 包间隔 ms，限速用 */
    QPushButton  *m_serSendBtn;
    QPushButton  *m_serAbortBtn;
    QPushButton  *m_serPreviewBtn;     /* 仅打包含但不发送 */
    QProgressBar *m_serBar;
    QLabel       *m_serInfo;
    SerialIo     *m_ser;
    QElapsedTimer m_serClock;
    int           m_serSentPkts;       /* 已发包数，握手进度显示用 */
    int           m_serTotalPkts;      /* 本幅图总包数 */
    qint64        m_serRxBytes;        /* 自打开以来收到的上行字节数 */

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
