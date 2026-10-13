#include "widget.h"
#include "ui_widget.h"
#include "serialio.h"

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
#include <QProgressBar>
#include <QDir>
#include <QMessageBox>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSerialPortInfo>
#include <QDateTime>
#include <QSize>
#include <QPixmap>

/* 背景截断阈值的默认值（界面上可调，这个只是初值）。
   见 buildUi() 里 m_grayCutSpin 的注释：小尺寸图建议调小甚至调 0。 */
static const int kGrayCutoffDefault = 20;

/* ★ 构建标记：显示在标题栏 + 启动日志。
   目的：一眼确认"现在跑的到底是哪一版 exe"。
   踩过的坑：跑了旧包，把已经修好的问题当成没修好，白排查一整轮。 */
static const char *kBuildTag = "B20261013-2115";

/* SerialIo 的日志回调 -> 回到 Widget::logMsg（回调在 GUI 线程内被调用，直接操作控件是安全的） */
static void serialLogTrampoline(void *ctx, const QString &line)
{
    Widget *w = static_cast<Widget *>(ctx);
    if (w)
        w->appendSerialLog(line);
}

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget)
{
    ui->setupUi(this);
    buildUi();

    m_ser = new SerialIo(this);
    m_ser->setLogCallback(serialLogTrampoline, this);
    m_ser->setPacketInterval(m_serIntervalSpin->value());
    /* 把界面上配置包的初值同步过去 —— 固件来要配置时 SerialIo 自己打包，
       所以它一开始就得有正确的 X/Y/速度 */
    m_ser->setConfigValues(m_cfgXSpin->value(),
                           m_cfgYSpin->value(),
                           m_cfgSpeedSpin->value(),
                           m_cfgXOrgSpin->value(),
                           m_cfgYOrgSpin->value());

    /* 默认握手模式：固件来要才发，比定时器猜间隔可靠得多 */
    m_ser->setHandshakeTimeout(m_serHsTimeoutSpin->value());
    m_ser->setMode(SerialIo::ModeHandshake);
    m_serModeCombo->setCurrentIndex(0);
    m_serSentPkts  = 0;
    m_serTotalPkts = 0;
    m_serRxBytes   = 0;

    m_monitorActive      = false;
    m_monitorW           = 0;
    m_monitorH           = 0;
    m_monitorBidi        = true;
    m_monitorGot         = 0;
    m_monitorLastPaintMs = 0;
    m_sizeSyncing        = false;

    connect(m_ser, SIGNAL(packetSent(int,int,quint16,int)),
            this, SLOT(onSerialPacketSent(int,int,quint16,int)));
    connect(m_ser, SIGNAL(streamProgress(int,int,int,int)),
            this, SLOT(onSerialProgress(int,int,int,int)));
    connect(m_ser, SIGNAL(streamFinished(bool)),
            this, SLOT(onSerialFinished(bool)));
    connect(m_ser, SIGNAL(errorOccurred(QString)),
            this, SLOT(onSerialError(QString)));
    connect(m_ser, SIGNAL(opened(QString)), this, SLOT(onSerialRefresh()));
    connect(m_ser, SIGNAL(requestReceived(int,int)),
            this, SLOT(onSerialRequestReceived(int,int)));
    connect(m_ser, SIGNAL(handshakeTimedOut(int,int)),
            this, SLOT(onSerialHandshakeTimeout(int,int)));
    connect(m_ser, SIGNAL(rxActivity(qint64)),
            this, SLOT(onSerialRxActivity(qint64)));
    connect(m_ser, SIGNAL(firmwareError(int,QString)),
            this, SLOT(onSerialFirmwareError(int,QString)));
    connect(m_ser, SIGNAL(firmwareReady(int)),
            this, SLOT(onSerialFirmwareReady(int)));
    connect(m_ser, SIGNAL(configRequestReceived(int)),
            this, SLOT(onSerialConfigRequest(int)));
    connect(m_ser, SIGNAL(pixelReported(int,int)),
            this, SLOT(onSerialPixelReported(int,int)));

    onSerialRefresh();

    setWindowTitle(QStringLiteral("激光雕刻上位机 - 图片转灰度数组  [%1]")
                   .arg(QString::fromLatin1(kBuildTag)));

    /* ★ 启动就把构建号写进日志：标题栏 + 日志双保险，
       以后一眼就能确认"现在跑的到底是哪一版"。 */
    logMsg(QStringLiteral("构建版本 %1（监看已含弓字形奇数行校正）")
           .arg(QString::fromLatin1(kBuildTag)), true);

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

    /* ============ 预览区：灰度预览 ⇄ 回传监看（同一块地方切换） ============
       平时显示图纸预览；一开工自动清空并切成监看，实时打出固件回传的点。
       为什么不做成并排：并排会把两块都挤小，而雕刻时用户关心的是"板子画成什么样"。 */
    {
        QHBoxLayout *ph = new QHBoxLayout();
        m_viewTitle = new QLabel(QStringLiteral("灰度图预览"), m_mainPage);
        m_viewTitle->setStyleSheet("font-weight: bold;");
        ph->addWidget(m_viewTitle);

        m_monitorInfo = new QLabel(QStringLiteral("待机"), m_mainPage);
        m_monitorInfo->setStyleSheet("color: gray;");
        ph->addWidget(m_monitorInfo, 1);

        m_viewSwapBtn = new QPushButton(QStringLiteral("切到回传监看"), m_mainPage);
        connect(m_viewSwapBtn, SIGNAL(clicked()), this, SLOT(onViewSwap()));
        ph->addWidget(m_viewSwapBtn);

        main->addLayout(ph);
    }

    m_viewStack = new QStackedWidget(m_mainPage);

    /* 第 0 页：灰度预览 */
    m_preview = new QLabel(m_viewStack);
    m_preview->setMinimumSize(320, 200);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::Box);
    m_preview->setText(QStringLiteral("灰度图预览"));
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_viewStack->addWidget(m_preview);

    /* 第 1 页：回传监看（开工后清空，按固件回传的数据实时打点） */
    m_monitor = new QLabel(m_viewStack);
    m_monitor->setMinimumSize(320, 200);
    m_monitor->setAlignment(Qt::AlignCenter);
    m_monitor->setFrameShape(QFrame::Box);
    /* 白底：和"白纸 + 烧黑"的实物观感一致 */
    m_monitor->setStyleSheet("background:#ffffff; color:#888;");
    m_monitor->setText(QStringLiteral("开工后这里会清空成白纸，并实时打出固件回传的每一个点\n"
                                      "（固件每画完一个像素发一条 0x33 灰度 0xEE，烧过的地方显黑）"));
    m_monitor->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_viewStack->addWidget(m_monitor);

    main->addWidget(m_viewStack, 1);

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

    /* ★ 锁定宽高比：勾上之后改宽度 → 高度按**原图的宽高比**自动算出来（反之亦然）。
       基准取原图而不是"当前的宽:高"，所以中途手调过尺寸也不会把比例带歪，
       换一张图之后比例自动跟着新图走。
       默认不勾：想刻正方形之类的非原图比例时不受干扰。 */
    m_lockRatioBox = new QCheckBox(
        QStringLiteral("锁定宽高比（改一边，另一边按原图比例自动跟）"), m_mainPage);
    m_lockRatioBox->setChecked(false);
    m_lockRatioBox->setToolTip(QStringLiteral(
        "勾选后：改「目标宽」按原图比例自动算「目标高」，改高同理。\n"
        "基准是**原图**的宽高比，不是当前输入框里的比值 ——\n"
        "这样中途手调过尺寸也不会把比例带歪，换图后比例自动跟着新图走。\n"
        "想刻正方形等非原图比例时把它取消勾选即可。"));
    grid->addWidget(m_lockRatioBox, 1, 0, 1, 4);

    /* 极性固定为黑=255：图像灰度按通用定义是 0=黑/255=白（它表示**亮度**），
       而雕刻要的是**出光强度**且"越黑越烧" —— 两者方向相反，代码里固定做 255-v。
       预览已翻回来显示，所以预览里深色的地方才是会烧的地方。 */
    QLabel *polLabel = new QLabel(QStringLiteral("灰度极性：黑=255(越黑越出光)"), m_mainPage);
    polLabel->setStyleSheet("color: gray;");
    grid->addWidget(polLabel, 2, 0, 1, 4);
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

    /* ★ 背景截断阈值（雕刻用）—— 原先是硬编码常量 kGrayCutoff=20，没法调。
       含义：反相后的出光强度 val = 255 - 灰度，
             val < 阈值 的点一律归 0（不出光），用来去掉背景灰底。

       ★ 为什么必须能调（"小图雕出来全白"的头号嫌疑）：
         源图缩放到小尺寸(比如 200x200)时用的是 SmoothTransformation，
         细线条会被**平滑平均**掉，整体灰度变浅；
         此时 255-灰度 很容易落到 20 以下 → 被整片归零 → 激光几乎不出光 → 全白。
         而 474 那种大尺寸细节保留得多，就不会触发。
       调到大值 = 更狠地抠背景；调到 0 = 完全不截断（连白底一起烧）。 */
    m_grayCutSpin = new QSpinBox(m_mainPage);
    m_grayCutSpin->setRange(0, 255);
    m_grayCutSpin->setValue(kGrayCutoffDefault);
    m_grayCutSpin->setToolTip(QStringLiteral(
        "出光强度低于此值的点归零(不出光)。\n"
        "雕刻出来太白/太淡 → 调小这个数(甚至 0)；\n"
        "背景脏、白底也被烧 → 调大。\n"
        "小尺寸图因为缩放平滑会整体变淡，通常需要调小。"));
    grid2->addWidget(new QLabel(QStringLiteral("背景截断阈值"), m_mainPage), 2, 0);
    grid2->addWidget(m_grayCutSpin, 2, 1);
    connect(m_grayCutSpin, SIGNAL(valueChanged(int)), this, SLOT(refreshGray()));

    main->addLayout(grid2);

    /* 导出灰度 C 数组 */
    m_grayBtn = new QPushButton(QStringLiteral("导出灰度C数组(二维)"), m_mainPage);
    m_grayBtn->setMinimumHeight(34);
    m_grayBtn->setStyleSheet("font-weight: bold;");
    main->addWidget(m_grayBtn);

    /* ---------------- 串口下发 ---------------- */
    QLabel *serTitle = new QLabel(QStringLiteral("串口下发"), m_mainPage);
    serTitle->setStyleSheet("font-weight: bold; margin-top: 4px;");
    main->addWidget(serTitle);

    /* 端口选择 */
    QHBoxLayout *serRow1 = new QHBoxLayout();
    m_serPortCombo = new QComboBox(m_mainPage);
    m_serPortCombo->setMinimumWidth(180);
    m_serRefreshBtn = new QPushButton(QStringLiteral("刷新"), m_mainPage);
    m_serOpenBtn    = new QPushButton(QStringLiteral("打开串口"), m_mainPage);
    serRow1->addWidget(new QLabel(QStringLiteral("端口"), m_mainPage));
    serRow1->addWidget(m_serPortCombo, 1);
    serRow1->addWidget(m_serRefreshBtn);
    serRow1->addWidget(m_serOpenBtn);
    main->addLayout(serRow1);

    /* 串口参数：波特率 / 数据位 / 校验位 / 停止位
       ★ 校验位必须和固件一致。不一致时的典型现象是"第 1 个字节对、后面全乱" ——
       因为收端会把停止位当校验位、把下一帧起始位当停止位，帧错误后晚一个 bit 同步。 */
    QHBoxLayout *serCfgRow = new QHBoxLayout();
    m_serBaudCombo = new QComboBox(m_mainPage);
    m_serBaudCombo->addItems(QStringList()
        << "9600" << "19200" << "38400" << "57600"
        << "115200" << "230400" << "460800" << "921600");
    m_serBaudCombo->setCurrentText("115200");

    m_serDataCombo = new QComboBox(m_mainPage);
    m_serDataCombo->addItems(QStringList() << "8" << "7" << "6" << "5");
    m_serDataCombo->setCurrentText("8");

    m_serParityCombo = new QComboBox(m_mainPage);
    m_serParityCombo->addItem(QStringLiteral("无校验"));   /* 0 */
    m_serParityCombo->addItem(QStringLiteral("奇校验"));   /* 1 */
    m_serParityCombo->addItem(QStringLiteral("偶校验"));   /* 2 */
    m_serParityCombo->setCurrentIndex(0);                  /* 默认无校验（与固件实测一致） */
    m_serParityCombo->setToolTip(QStringLiteral(
        "必须与固件一致！实测固件是 8N1（无校验），所以默认选「无校验」。\n\n"
        "不一致的典型症状：第 1 个字节正确、第 2 个起全是乱码。\n"
        "原因：收端把停止位当校验位、把下一帧起始位当停止位，帧错误后\n"
        "晚一个 bit 才重新同步，于是从第 2 字节起整体错位。\n"
        "（0x55/0x44/0xAA 的 1 的个数都是偶数，奇校验位恰好等于停止位 1，\n"
        " 所以第 1 字节既不错位也不报校验错，特别有迷惑性。\n"
        " 实测：8O1 收 8N1 的 55 44 AA → 解成 55 94 FD。）"));

    m_serStopCombo = new QComboBox(m_mainPage);
    m_serStopCombo->addItems(QStringList() << "1" << "1.5" << "2");
    m_serStopCombo->setCurrentText("1");

    serCfgRow->addWidget(new QLabel(QStringLiteral("参数"), m_mainPage));
    serCfgRow->addWidget(m_serBaudCombo, 1);
    serCfgRow->addWidget(m_serDataCombo);
    serCfgRow->addWidget(m_serParityCombo);
    serCfgRow->addWidget(m_serStopCombo);
    main->addLayout(serCfgRow);

    /* 包间隔 + 信息 */
    QHBoxLayout *serRow2 = new QHBoxLayout();
    m_serIntervalSpin = new QSpinBox(m_mainPage);
    m_serIntervalSpin->setRange(0, 2000);
    m_serIntervalSpin->setValue(6);
    m_serIntervalSpin->setSuffix(QStringLiteral(" ms"));
    m_serIntervalSpin->setToolTip(QStringLiteral(
        "包间隔（节流）。69 字节/包，115200 下约 6ms 刚好跑满串口；"
        "固件消化不过来（丢包/花纹错位）就往上加。0 = 不限速，仅本机虚拟串口联调用。"));
    m_serInfo = new QLabel(QStringLiteral("未打开"), m_mainPage);
    m_serInfo->setStyleSheet("color: gray;");
    serRow2->addWidget(new QLabel(QStringLiteral("包间隔"), m_mainPage));
    serRow2->addWidget(m_serIntervalSpin);
    serRow2->addWidget(m_serInfo, 1);
    main->addLayout(serRow2);

    /* 发送模式：握手（默认，固件来要才发）/ 自由（按包间隔自己发） */
    QHBoxLayout *serRow3 = new QHBoxLayout();
    m_serModeCombo = new QComboBox(m_mainPage);
    m_serModeCombo->addItem(QStringLiteral("握手模式（等固件 0x55 0x44 0xAA，来一个发一个）"));
    m_serModeCombo->addItem(QStringLiteral("自由模式（按包间隔自己发）"));
    m_serModeCombo->setToolTip(QStringLiteral(
        "握手模式：固件每发一个 0x55+0x44+0xAA，上位机才下发一个数据包，"
        "节奏完全由固件的物理动作（出光+走位）决定，不会灌爆它的缓冲。\n"
        "自由模式：上位机按包间隔定时往外吐，需要自己猜一个合适的间隔。"));
    serRow3->addWidget(new QLabel(QStringLiteral("模式"), m_mainPage));
    serRow3->addWidget(m_serModeCombo, 1);
    main->addLayout(serRow3);

    /* 握手超时 */
    QHBoxLayout *serRow3b = new QHBoxLayout();
    m_serHsTimeoutSpin = new QSpinBox(m_mainPage);
    m_serHsTimeoutSpin->setRange(0, 60000);
    m_serHsTimeoutSpin->setValue(3000);
    m_serHsTimeoutSpin->setSuffix(QStringLiteral(" ms"));
    m_serHsTimeoutSpin->setToolTip(QStringLiteral(
        "握手模式下等待固件请求的超时。超过这个时间没收到 0x55 0x44 0xAA，"
        "就报错停发，避免固件跑飞时界面无声卡死。0 = 一直等。"));
    serRow3b->addWidget(new QLabel(QStringLiteral("握手超时"), m_mainPage));
    serRow3b->addWidget(m_serHsTimeoutSpin);
    serRow3b->addStretch(1);
    main->addLayout(serRow3b);

    /* 发送 / 中止 / 仅打包 */
    QHBoxLayout *serRow4 = new QHBoxLayout();
    m_serSendBtn = new QPushButton(QStringLiteral("发送整幅图"), m_mainPage);
    m_serSendBtn->setMinimumHeight(34);
    m_serSendBtn->setStyleSheet("font-weight: bold;");
    m_serAbortBtn   = new QPushButton(QStringLiteral("中止"), m_mainPage);
    m_serAbortBtn->setMinimumHeight(34);
    m_serAbortBtn->setEnabled(false);
    m_serPreviewBtn = new QPushButton(QStringLiteral("仅打包"), m_mainPage);
    m_serPreviewBtn->setMinimumHeight(34);
    m_serPreviewBtn->setToolTip(QStringLiteral(
        "不打开串口也能看：把前几包的字节结构、CRC、控制位打到日志，用来核对协议。"));
    serRow4->addWidget(m_serSendBtn, 2);
    serRow4->addWidget(m_serAbortBtn, 1);
    serRow4->addWidget(m_serPreviewBtn, 1);
    main->addLayout(serRow4);

    /* ---- 下发配置（控制位 0x33）：X / Y / 速度 ---- */
    QHBoxLayout *cfgRow = new QHBoxLayout();
    m_cfgXSpin = new QSpinBox(m_mainPage);
    m_cfgXSpin->setRange(0, 65535);
    m_cfgXSpin->setValue(64);
    m_cfgXSpin->setToolTip(QStringLiteral("X轴一行像素点数（= 图像宽）。2 字节，大端。"));
    m_cfgYSpin = new QSpinBox(m_mainPage);
    m_cfgYSpin->setRange(0, 65535);
    m_cfgYSpin->setValue(64);
    m_cfgYSpin->setToolTip(QStringLiteral("Y轴一列像素点数（= 图像高）。2 字节，大端。"));
    m_cfgSpeedSpin = new QSpinBox(m_mainPage);
    m_cfgSpeedSpin->setRange(0, 255);
    m_cfgSpeedSpin->setValue(100);
    m_cfgSpeedSpin->setToolTip(QStringLiteral(
        "加工速度。1 字节，0~255，单位由固件定义。"));

    /* 作业原点：图像起点相对机器原点（回零点）的偏移，单位 = 像素点。 */
    m_cfgXOrgSpin = new QSpinBox(m_mainPage);
    m_cfgXOrgSpin->setRange(0, 65535);
    m_cfgXOrgSpin->setValue(0);
    m_cfgXOrgSpin->setToolTip(QStringLiteral(
        "X 轴作业原点：图像起始点相对机器原点的偏移（像素点）。\n"
        "2 字节，大端，字段标签 0x77。"));
    m_cfgYOrgSpin = new QSpinBox(m_mainPage);
    m_cfgYOrgSpin->setRange(0, 65535);
    m_cfgYOrgSpin->setValue(0);
    m_cfgYOrgSpin->setToolTip(QStringLiteral(
        "Y 轴作业原点：图像起始点相对机器原点的偏移（像素点）。\n"
        "2 字节，大端，字段标签 0x88。"));

    m_serConfigBtn = new QPushButton(QStringLiteral("下发配置"), m_mainPage);
    m_serConfigBtn->setMinimumHeight(30);
    m_serConfigBtn->setToolTip(QStringLiteral(
        "手动发送 0x33 配置包（69 字节）—— 调试用。\n\n"
        "正常流程是固件发 0x55 0x55 0xAA 来要，上位机自动回一个配置包；\n"
        "这个按钮让你不用固件配合也能主动发一个。\n\n"
        "独立命令：不进队列、不受握手门控。\n"
        "图像下发途中会被拒绝，避免插进像素流里。\n"
        "X / Y 会在载入图片后自动填成图像尺寸。"));

    cfgRow->addWidget(new QLabel(QStringLiteral("配置 X"), m_mainPage));
    cfgRow->addWidget(m_cfgXSpin);
    cfgRow->addWidget(new QLabel(QStringLiteral("Y"), m_mainPage));
    cfgRow->addWidget(m_cfgYSpin);
    cfgRow->addWidget(new QLabel(QStringLiteral("速度"), m_mainPage));
    cfgRow->addWidget(m_cfgSpeedSpin);
    cfgRow->addWidget(new QLabel(QStringLiteral("原点X"), m_mainPage));
    cfgRow->addWidget(m_cfgXOrgSpin);
    cfgRow->addWidget(new QLabel(QStringLiteral("Y"), m_mainPage));
    cfgRow->addWidget(m_cfgYOrgSpin);
    cfgRow->addWidget(m_serConfigBtn);
    main->addLayout(cfgRow);

    /* ★ 加工前先走一个图宽 —— 把「原点X」自动设成图像宽度。
       需求来源：图纸的原点是**左上角**，而加工起始点落在图像右上角，
       所以要先把 X 走一个图宽，第 0 行再走回来，图像左上角才和机器原点对上。

       ★ 这条链路本来就通，**不需要改固件、不用重烧**：
           界面「原点X」→ 配置包字节 11/12（标签 0x77）
           → 固件 Bluetooth.c:58  x_work_orgin
           → 固件 main.c:169      goto_point(x_work_orgin, y_work_orgin) 开工前先走过去
       所以这里只做"把原点X 自动填成图宽"这一件事。

       ⚠ 改完必须重新「下发配置」（或复位板子让固件重新来要配置）才生效 ——
         原点是在配置包里下发的，不重发固件还用旧的。 */
    m_orgByWidthBox = new QCheckBox(
        QStringLiteral("加工前先走一个图宽（原点X 自动 = 图像宽度）"), m_mainPage);
    m_orgByWidthBox->setChecked(false);
    m_orgByWidthBox->setToolTip(QStringLiteral(
        "勾选后，「原点X」自动等于当前图像的宽度（点数）。\n\n"
        "固件开工前会先 goto_point(原点X, 原点Y) 走过去，\n"
        "于是机头先走一个图宽，第 0 行再走回来 ——\n"
        "图像左上角正好落在机器原点上。\n\n"
        "⚠ 改完记得重新「下发配置」（或复位板子让固件重新来要），\n"
        "   否则固件还在用上一次的旧原点。"));
    main->addWidget(m_orgByWidthBox);

    m_serBar = new QProgressBar(m_mainPage);
    m_serBar->setRange(0, 100);
    m_serBar->setValue(0);
    m_serBar->setTextVisible(true);
    main->addWidget(m_serBar);

    /* 日志 */
    m_log = new QPlainTextEdit(m_mainPage);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMaximumHeight(140);
    main->addWidget(m_log);

    m_status = new QLabel(QStringLiteral("就绪：打开图片 -> 调好参数 -> 导出灰度C数组 或 串口下发"), m_mainPage);
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
    /* ★ 宽高改成走联动槽（槽内部最后统一调一次 refreshGray）：
       一次改动只需要重算一遍灰度 —— 4096x4096 那种大图算两遍会很卡。
       注意顺序：必须先接联动槽；直接接 refreshGray 会导致算两遍。 */
    connect(m_widthSpin,  SIGNAL(valueChanged(int)), this, SLOT(onSizeWidthChanged(int)));
    connect(m_heightSpin, SIGNAL(valueChanged(int)), this, SLOT(onSizeHeightChanged(int)));
    connect(m_lockRatioBox, SIGNAL(toggled(bool)),   this, SLOT(onLockRatioToggled()));
    connect(m_orgByWidthBox, SIGNAL(toggled(bool)),  this, SLOT(onOrgByWidthToggled()));
    connect(m_grayBtn,    SIGNAL(clicked()), this, SLOT(onSaveGrayArray()));

    connect(m_serRefreshBtn,    SIGNAL(clicked()), this, SLOT(onSerialRefresh()));
    connect(m_serOpenBtn,       SIGNAL(clicked()), this, SLOT(onSerialOpenClose()));
    connect(m_serSendBtn,       SIGNAL(clicked()), this, SLOT(onSerialSend()));
    connect(m_serAbortBtn,      SIGNAL(clicked()), this, SLOT(onSerialAbort()));
    connect(m_serPreviewBtn,    SIGNAL(clicked()), this, SLOT(onSerialPreview()));
    connect(m_serConfigBtn,     SIGNAL(clicked()), this, SLOT(onSerialSendConfig()));
    connect(m_cfgXSpin,         SIGNAL(valueChanged(int)), this, SLOT(onCfgValueChanged()));
    connect(m_cfgYSpin,         SIGNAL(valueChanged(int)), this, SLOT(onCfgValueChanged()));
    connect(m_cfgSpeedSpin,     SIGNAL(valueChanged(int)), this, SLOT(onCfgValueChanged()));
    connect(m_cfgXOrgSpin,      SIGNAL(valueChanged(int)), this, SLOT(onCfgValueChanged()));
    connect(m_cfgYOrgSpin,      SIGNAL(valueChanged(int)), this, SLOT(onCfgValueChanged()));
    connect(m_serIntervalSpin,  SIGNAL(valueChanged(int)),
            this, SLOT(onSerialIntervalChanged(int)));
    connect(m_serModeCombo,     SIGNAL(currentIndexChanged(int)),
            this, SLOT(onSerialModeChanged(int)));
    connect(m_serHsTimeoutSpin, SIGNAL(valueChanged(int)),
            this, SLOT(onSerialHsTimeoutChanged(int)));

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

/* ====================== 雕刻回传监看 ====================== */

void Widget::showViewPage(int page)
{
    if (!m_viewStack)
        return;
    m_viewStack->setCurrentIndex(page);
    updateViewSwapBtn();

    if (page == 1)
    {
        m_viewTitle->setText(QStringLiteral("雕刻回传监看（固件实画）"));
        m_viewTitle->setStyleSheet("font-weight: bold; color: #060;");
        monitorRepaint();
    }
    else
    {
        m_viewTitle->setText(QStringLiteral("灰度图预览"));
        m_viewTitle->setStyleSheet("font-weight: bold;");
        updatePreview();
    }
}

void Widget::updateViewSwapBtn()
{
    if (!m_viewSwapBtn || !m_viewStack)
        return;
    const bool onMonitor = (m_viewStack->currentIndex() == 1);
    m_viewSwapBtn->setText(onMonitor ? QStringLiteral("切到灰度预览")
                                     : QStringLiteral("切到回传监看"));
}

void Widget::onViewSwap()
{
    if (!m_viewStack)
        return;
    showViewPage(m_viewStack->currentIndex() == 1 ? 0 : 1);
}

/**
 * 开工：**清空画布**并立刻切到监看页（灰度预览让位）。
 * 清空后画布是全黑的，固件回传的点会一个个点亮上去。
 */
void Widget::monitorBegin(int w, int h, bool bidi)
{
    if ((w <= 0) || (h <= 0))
        return;

    m_monitorW = w;
    m_monitorH = h;
    /* ★ 锁存本次下发的行方向：整幅雕刻期间不变，
       免得中途改勾选导致监看落点和数据方向对不上。 */
    m_monitorBidi = bidi;
    m_monitorGot = 0;
    m_monitorActive = true;
    m_monitorLastPaintMs = 0;

    /* ★ 白底 = "还没画到"（对应白纸），收到的点按灰度烧黑 —— 和实物观感一致。
       注意极性：固件回传的灰度值是"出光强度"（0 = 不出光 = 白纸，255 = 满功率 = 烧黑），
       所以写画布时要写 (255 - gray)，这样出光强的地方才显示为黑点。
       这和灰度预览的显示约定保持一致（深色 = 会烧的地方）。 */
    m_monitorImg = QImage(w, h, QImage::Format_Grayscale8);
    m_monitorImg.fill(255);

    m_monitor->setText(QString());
    showViewPage(1);

    m_monitorInfo->setText(QStringLiteral("监看中… 0 / %1 点").arg((qint64)w * h));
    m_monitorInfo->setStyleSheet("color: #060;");

    if (m_ser)
        m_ser->resetReportStats();
}

/**
 * 收到一个像素：**按到达顺序**依次落在画布上。
 *
 * ★ 固件回传帧里不带坐标（只有 33 gg EE 三个字节），所以本来就只能按顺序落。
 *   而这恰好是最好的监控视角 —— 画出来的就是固件**实际的输出顺序**：
 *     · 正常       → 逐行从左到右铺满，最后和图纸一致
 *     · 重复某行   → 画面上出现重复的横带
 *     · 跳过某行   → 出现空洞
 *     · 一行画两遍 → 出现半行错位
 *   不用等刻完拿尺子量，屏幕上直接看。
 */
void Widget::monitorFeed(int gray)
{
    if (!m_monitorActive || m_monitorImg.isNull())
        return;

    const qint64 total = (qint64)m_monitorW * m_monitorH;
    const int seq = m_monitorGot;              /* 第几个到达 */
    ++m_monitorGot;

    if (seq >= total)
        return;                                /* 超出画布：固件多发了 */

    const int y = seq / m_monitorW;
    int x = seq % m_monitorW;

    /* ★ 弓字形校正：奇数行固件是反向走的（右→左），回传顺序也是反的。
       若按到达顺序直接从左往右画，奇数行会整体左右镜像 ——
       对有朝向细节的图案（龙爪、龙须、文字）会产生"编织/拉花"般的碎条纹，
       这就是监看画面看起来被剪碎的原因。这里把奇数行画回图纸方向。

       ★★ 用 m_monitorBidi（开工时锁存），不用 m_bidiBox->isChecked()：
       数据方向是 beginImage() 定下的，中途改勾选会让两者不一致。 */
    if (m_monitorBidi && ((y & 1) != 0))
        x = m_monitorW - 1 - x;

    /* ★ 极性：固件回传的 gray 是"出光强度"（0=不出光=白纸，255=满功率=烧黑）。
       画布是白底（未画过 = 白纸），所以这里要写 (255 - gray)：
         收到 gray=255（满功率）→ 画布写 0   → 显示黑点 ✅
         收到 gray=0  （不出光）→ 画布写 255 → 显示白   ✅
       这样监看画面和实物观感一致：烧过的地方是黑的。 */
    uchar *row = m_monitorImg.scanLine(y);
    row[x] = (uchar)(255 - qBound(0, gray, 255));

    /* 重绘节流：每 40ms 刷一次。
       ★ 必须节流：固件一个点约 3ms，不节流会每秒重绘 300 次，界面卡死。 */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((now - m_monitorLastPaintMs) >= 40)
    {
        m_monitorLastPaintMs = now;
        monitorRepaint();
        /* 状态栏同时显示"画到第几行"，比单纯的点数直观 ——
           卡住时能直接看出停在第几行。 */
        m_monitorInfo->setText(QStringLiteral("监看中… %1 / %2 点 (%3%)   第 %4 / %5 行")
            .arg(m_monitorGot).arg(total)
            .arg(total > 0 ? (int)(100.0 * m_monitorGot / total) : 0)
            .arg(y + 1).arg(m_monitorH));
    }

    /* 全部点到齐 → 自动收尾 */
    if (m_monitorGot >= total)
        monitorEnd(false);
}

void Widget::monitorEnd(bool aborted)
{
    if (!m_monitorActive)
        return;

    m_monitorActive = false;
    monitorRepaint();

    const qint64 total = (qint64)m_monitorW * m_monitorH;
    QString txt = QStringLiteral("%1  %2 / %3 点 (%4%)")
                    .arg(aborted ? QStringLiteral("已中止") : QStringLiteral("监看结束"))
                    .arg(m_monitorGot).arg(total)
                    .arg(total > 0 ? (int)(100.0 * m_monitorGot / total) : 0);

    if (m_monitorGot < total)
        txt += QStringLiteral("   ⚠ 少了 %1 点（固件没画完）").arg(total - m_monitorGot);
    else if (m_monitorGot > total)
        txt += QStringLiteral("   ⚠ 多了 %1 点（固件多画了）").arg(m_monitorGot - total);

    if (m_ser)
        txt += QStringLiteral("   回传帧 %1 条 / 丢弃 %2 条")
               .arg(m_ser->reportCount()).arg(m_ser->reportDropped());

    m_monitorInfo->setText(txt);
    const bool bad = (m_monitorGot != total)
                  || (m_ser && m_ser->reportDropped() > 0);
    m_monitorInfo->setStyleSheet(bad ? "color: #c00;" : "color: #060;");
}

/** 把监看图画到界面上（等比缩放，最近邻保持像素感） */
void Widget::monitorRepaint()
{
    if (m_monitorImg.isNull() || !m_monitor)
        return;

    const QSize sz = m_monitor->size();
    if (sz.width() < 8 || sz.height() < 8)
        return;

    QSize fit = m_monitorImg.size();
    fit.scale(sz, Qt::KeepAspectRatio);
    if (fit.width()  < 1) fit.setWidth(1);
    if (fit.height() < 1) fit.setHeight(1);

    /* 放大用 FastTransformation：监看要看清单个像素，不要平滑 */
    m_monitor->setPixmap(QPixmap::fromImage(
        m_monitorImg.scaled(fit, Qt::IgnoreAspectRatio, Qt::FastTransformation)));
}

void Widget::onSerialPixelReported(int seq, int gray)
{
    Q_UNUSED(seq);          /* 序号仅用于统计，落点用"到达顺序"即可 */
    monitorFeed(gray);
}

/* ====================== 串口下发 ====================== */

void Widget::appendSerialLog(const QString &line)
{
    logMsg(line, true);
}

void Widget::onSerialRefresh()
{
    const QString keep = m_serPortCombo->currentText().section('|', 0, 0);
    m_serPortCombo->clear();

    const QStringList ports = SerialIo::availablePorts();
    for (const QString &p : ports)
        m_serPortCombo->addItem(p);

    if (ports.isEmpty())
    {
        m_serPortCombo->addItem(QStringLiteral("(未发现串口)"));
        m_serInfo->setText(QStringLiteral("未发现串口设备"));
        m_serInfo->setStyleSheet("color: red;");
    }
    else
    {
        const int idx = m_serPortCombo->findText(keep, Qt::MatchStartsWith);
        if (idx >= 0)
            m_serPortCombo->setCurrentIndex(idx);
        if (m_ser->isOpen())
        {
            m_serInfo->setText(m_ser->stateText());
            m_serInfo->setStyleSheet("color: green;");
        }
        else
        {
            m_serInfo->setText(QStringLiteral("发现 %1 个端口").arg(ports.size()));
            m_serInfo->setStyleSheet("color: gray;");
        }
    }
}

void Widget::onSerialOpenClose()
{
    if (m_ser->isOpen())
    {
        m_ser->abortStream();
        m_ser->close();
        m_serOpenBtn->setText(QStringLiteral("打开串口"));
        m_serSendBtn->setEnabled(true);
        m_serInfo->setText(QStringLiteral("已关闭"));
        m_serInfo->setStyleSheet("color: gray;");
        return;
    }

    const QString item = m_serPortCombo->currentText();
    const QString port = item.section('|', 0, 0).trimmed();
    if (port.isEmpty() || (m_serPortCombo->currentIndex() < 0))
    {
        setStatus(QStringLiteral("请先选择一个串口"), false);
        return;
    }

    /* ★ 打开前先把界面上的串口参数写进去。
       校验位错了会表现成"第 1 字节对、后面全乱"，先在这里落定。 */
    const int baud     = m_serBaudCombo->currentText().toInt();
    const int dataBits = m_serDataCombo->currentText().toInt();
    const int parity   = m_serParityCombo->currentIndex();   /* 0无 1奇 2偶 */
    const int stopBits = m_serStopCombo->currentIndex();     /* 0=1 1=1.5 2=2 */
    m_ser->setPortParams(baud, dataBits, parity, stopBits);

    logMsg(QStringLiteral("串口参数：%1").arg(m_ser->cfgText())
           + (parity == 1 ? QStringLiteral("  （奇校验：固件侧也必须配奇校验）")
                          : (parity == 0 ? QStringLiteral("  （无校验：固件侧也要 No Parity）")
                                         : QStringLiteral("  （偶校验）"))), true);

    if (!m_ser->open(port))
    {
        setStatus(m_ser->stateText(), false);
        m_serInfo->setText(m_ser->stateText());
        m_serInfo->setStyleSheet("color: red;");
        return;
    }
    m_serOpenBtn->setText(QStringLiteral("关闭串口"));
    m_serInfo->setText(m_ser->stateText() + QStringLiteral("  收 0 字节"));
    m_serInfo->setStyleSheet("color: green;");
    setStatus(QStringLiteral("串口已打开：%1").arg(port), true);
    m_serRxBytes = 0;

    if (m_ser->isHandshake())
    {
        logMsg(QStringLiteral("握手模式就绪。上位机「不会主动发任何数据」，"
                              "完全由固件发请求来驱动："), true);
        logMsg(QStringLiteral("  · 0x55 0x44 0xAA → 回一个像素数据包（0x11 / 0x22）"), true);
        logMsg(QStringLiteral("  · 0x55 0x55 0xAA → 回一个配置包（0x33）"), true);
        logMsg(QStringLiteral("★ 取像素前先点「发送整幅图」把数据装进队列；"
                              "配置包不用装载，固件来要就直接回。"), false);
        logMsg(QStringLiteral("（收到的上行字节会打在日志里；若一直没提示，说明字节根本没到）"), true);
    }
}

void Widget::onSerialIntervalChanged(int ms)
{
    if (m_ser)
        m_ser->setPacketInterval(ms);
}

void Widget::onSerialModeChanged(int idx)
{
    if (!m_ser)
        return;
    const bool hs = (idx == 0);
    m_ser->setMode(hs ? SerialIo::ModeHandshake : SerialIo::ModeFree);

    /* 握手模式下"包间隔"不再是节拍，只是保险丝 → 灰掉，避免误导 */
    m_serIntervalSpin->setEnabled(!hs);
    m_serHsTimeoutSpin->setEnabled(hs);

    if (hs)
        logMsg(QStringLiteral("已切到握手模式：固件每发一个 0x55 0x44 0xAA，上位机下发一个包"), true);
    else
        logMsg(QStringLiteral("已切到自由模式：按包间隔定时下发，需自行确认固件能跟上"), false);
}

void Widget::onSerialHsTimeoutChanged(int ms)
{
    if (m_ser)
        m_ser->setHandshakeTimeout(ms);
}

void Widget::onSerialRequestReceived(int seq, int remaining)
{
    m_serBar->setFormat(QStringLiteral("握手第 %1 次请求 / 剩 %2 包").arg(seq).arg(remaining));
    if (remaining > 0)
        setStatus(QStringLiteral("握手中… 固件已请求 %1 次，已下发，剩 %2 包")
                  .arg(seq).arg(remaining), true);
    else
        setStatus(QStringLiteral("固件第 %1 次请求，但队列已空 —— 未下发。"
                                 "若是初次使用，请先点「发送整幅图」装载数据")
                  .arg(seq), false);
}

void Widget::onSerialHandshakeTimeout(int waitedMs, int remaining)
{
    m_serSendBtn->setEnabled(true);
    m_serAbortBtn->setEnabled(false);
    m_serOpenBtn->setEnabled(true);
    m_serBar->setFormat(QStringLiteral("握手超时"));
    setStatus(QStringLiteral("握手超时：固件要过包之后就没了动静，%1 ms 没再收到请求，剩 %2 包未发")
              .arg(waitedMs).arg(remaining), false);
    logMsg(QStringLiteral("[!] 该超时的含义是「固件要过一个包之后就不再来了」。"
                          "若从头到尾一次请求都没收到，那不是超时，"
                          "而是字节根本没到 —— 查接线 / 端口 / 波特率 / 校验位。"), false);
}

/** 每收到上行字节就刷新一次"收 N 字节"，让用户区分"没收到"和"收到了但不匹配"。 */
void Widget::onSerialRxActivity(qint64 totalBytes)
{
    m_serRxBytes = totalBytes;
    if (m_ser->isOpen())
        m_serInfo->setText(QStringLiteral("%1  收 %2 字节")
                           .arg(m_ser->portName()).arg(totalBytes));
}

/**
 * 固件主动上报的错误：帧 0x55 0x10 0xAA（对应固件的 BT_INIT_TIMEOUT）。
 *
 * ★ 这条信息比"上位机没反应"有价值得多 —— 它证明串口链路是通的、板子在正常跑，
 *   只是固件拿不到它要的数据。所以重点不是"报个错"，而是给出**下一步该做什么**。
 *
 * 详细原因已由 SerialIo 经日志回调打出来（见 onReadyRead 里的错误分支），
 * 这里只做状态栏的醒目提示，避免同一段文字打两遍。
 */
void Widget::onSerialFirmwareError(int code, const QString &msg)
{
    Q_UNUSED(code);
    Q_UNUSED(msg);

    setStatus(QStringLiteral("★ 固件报错：蓝牙接收超时 —— 原因与操作顺序见日志区"), false);

    if (m_serInfo)
        m_serInfo->setStyleSheet("color: orange;");
}

/**
 * 收到固件的就绪通知（帧 55 77 AA）—— 板子已停在 waiting()，等上位机下令。
 *
 * 这是新启动握手的"上半段"：固件先把"我准备好了"说出来，
 * 上位机再去装载队列、发 55 66 AA 让它开始。
 * 所以这里只做提示，不自动发任何东西 —— 什么时候启动由用户决定。
 */
void Widget::onSerialFirmwareReady(int count)
{
    setStatus(QStringLiteral("★ 固件已就绪（第 %1 次通知）—— 请点「发送整幅图」，"
                             "装载完会自动下发 55 66 AA 启动")
              .arg(count), true);

    if (m_serInfo)
        m_serInfo->setStyleSheet("color: green;");
}

/**
 * 只打包含、不落端口：用来在没接硬件时核对协议。
 *
 * 把第 0 行（自然序）与第 1 行（弓字形时倒序）的头几包打印出来，
 * 顺便把"行尾补 0x00 到 64 点 + 控制位置 0x22"这件事在日志里显式呈现。
 */
void Widget::onSerialPreview()
{
    logMsg(QStringLiteral("—— 打包预览（不发串口）——"), true);
    logMsg(QStringLiteral("帧结构: 0x55 + 控制位 + 64 字节像素 + CRC16H + CRC16L + 0xAA = 69 字节"), true);

    /* 配置包不依赖图片，先单独dump 出来 —— 没接硬件也能逐字节核对 */
    {
        const int cx = m_cfgXSpin->value();
        const int cy = m_cfgYSpin->value();
        const int cs = m_cfgSpeedSpin->value();
        const int cox = m_cfgXOrgSpin->value();
        const int coy = m_cfgYOrgSpin->value();
        const QByteArray cp = SerialIo::buildConfigPayload(cx, cy, cs, cox, coy);

        QString chex;
        for (int i = 0; i < cp.size(); ++i)
        {
            if (i && (i % 16 == 0)) chex += QStringLiteral(" ");
            chex += QStringLiteral("%1 ").arg((quint8)cp.at(i), 2, 16, QChar('0')).toUpper();
        }
        logMsg(QStringLiteral("[配置包 0x33] X=%1(2B大端) Y=%2(2B大端) 速度=%3(1B) "
                              "原点X=%4(2B大端) 原点Y=%5(2B大端)，后 50 字节 0xFF 填充")
               .arg(cx).arg(cy).arg(cs).arg(cox).arg(coy), true);
        logMsg(QStringLiteral("  载荷 64B: %1").arg(chex.trimmed()), true);
        logMsg(QStringLiteral("  完整包 = 0x55 0x33 | 上述 64B | CRC16H CRC16L | 0xAA"
                              "（CRC 覆盖控制位+载荷 = 65 字节，与数据包同算法）"), true);
    }

    if (m_gray.isNull())
    {
        setStatus(QStringLiteral("配置包预览已输出；未载入图片，跳过数据包预览"), true);
        logMsg(QStringLiteral("（未载入图片，跳过数据包预览）"), true);
        return;
    }

    const int w = m_gray.width();
    const int h = m_gray.height();
    const int pktsPerRow = (w + 63) / 64;
    const bool bidi = m_bidiBox->isChecked();

    logMsg(QStringLiteral("CRC    : CRC16-CCITT-FALSE(init 0xFFFF, poly 0x1021) 覆盖 控制位+64像素(65字节)"), true);
    logMsg(QStringLiteral("尺寸   : %1 x %2，行宽 %1 点 → 每行 %3 包，共 %4 包")
           .arg(w).arg(h).arg(pktsPerRow).arg((qint64)pktsPerRow * h), true);
    logMsg(QStringLiteral("排列   : %1")
           .arg(bidi ? QStringLiteral("弓字形(奇数行倒序)")
                     : QStringLiteral("自然行序")), true);

    logMsg(QStringLiteral("上行   : 固件 → 上位机，两种 3 字节请求帧（无 CRC）："), true);
    logMsg(QStringLiteral("           0x55 0x44 0xAA → 回一个像素数据包（0x11 或 0x22）"), true);
    logMsg(QStringLiteral("           0x55 0x55 0xAA → 回一个配置包（0x33）"), true);
    logMsg(QStringLiteral("         一个请求只换一个包，绝不多发"), true);
    logMsg(QStringLiteral("         当前模式: %1")
           .arg(m_ser->isHandshake() ? QStringLiteral("握手（收到才发）")
                                     : QStringLiteral("自由（按间隔自己发）")), true);
    logMsg(QStringLiteral(""), true);
    for (int y = 0; y <= 1; ++y)
    {
        if (y >= h)
            break;
        const uchar *row = m_gray.constScanLine(y);
        const bool rev = bidi && ((y & 1) != 0);
        for (int p = 0; p < pktsPerRow; ++p)
        {
            const bool last = (p == pktsPerRow - 1);
            const int col  = p * 64;
            const int take = last ? (w - col) : 64;

            QByteArray pix;
            for (int k = 0; k < take; ++k)
            {
                const int c  = col + k;
                const int sx = rev ? (w - 1 - c) : c;
                pix.append((char)row[sx]);
            }
            const int pad = 64 - pix.size();
            while (pix.size() < 64)
                pix.append((char)0x00);

            /* 与 SerialIo 内部完全一致的打包路径（借道一次临时打开是多余的，
               这里直接调静态算 CRC，保证预览与实发同源） */
            logMsg(QStringLiteral("y=%1 p=%2 ctrl=0x%3 有效%4点 补0x00 %5点 首4字节 %6")
                   .arg(y).arg(p)
                   .arg(last ? "22" : "11")
                   .arg(take).arg(pad)
                   .arg(QString::fromLatin1(pix.left(4).toHex(), 8).toUpper()), true);
            if (p >= 2)
            {
                logMsg(QStringLiteral("  ... 本行其余 %1 包略").arg(pktsPerRow - p - 1), true);
                break;
            }
        }
    }
    logMsg(QStringLiteral("—— 预览结束，实际发送请点「发送整幅图」——"), true);
    setStatus(QStringLiteral("打包预览已输出到日志"), true);
}

/**
 * 配置的 X/Y/速度 改动 → 同步给 SerialIo。
 *
 * 固件发 0x55 0x55 0xAA 来要配置时，SerialIo 是**自己**打包回应的，
 * 所以它必须随时持有最新的 X/Y/速度 —— 不能只在点按钮时才传。
 */
void Widget::onCfgValueChanged()
{
    if (m_ser)
        m_ser->setConfigValues(m_cfgXSpin->value(),
                               m_cfgYSpin->value(),
                               m_cfgSpeedSpin->value(),
                               m_cfgXOrgSpin->value(),
                               m_cfgYOrgSpin->value());
}

/** 固件发 0x55 0x55 0xAA 来要配置：SerialIo 已经回好了，这里只更新界面状态。 */
void Widget::onSerialConfigRequest(int seq)
{
    setStatus(QStringLiteral("固件请求配置第 %1 次 → 已回 0x33 配置包"
                             "（X=%2 Y=%3 速度=%4 原点=(%5,%6)）")
              .arg(seq).arg(m_cfgXSpin->value())
              .arg(m_cfgYSpin->value()).arg(m_cfgSpeedSpin->value())
              .arg(m_cfgXOrgSpin->value()).arg(m_cfgYOrgSpin->value()), true);
}

/**
 * 下发配置包（控制位 0x33）—— 手动按钮。
 *
 * 正常流程是**固件发 0x55 0x55 0xAA 来要**；这个按钮是给调试用的，
 * 不用固件配合也能主动发一个。
 */
void Widget::onSerialSendConfig()
{
    if (!m_ser->isOpen())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先打开串口"));
        return;
    }
    if (m_ser->isStreaming())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("正在下发图像，请先等它发完或点「中止」，"
                                            "再下发配置。\n\n"
                                            "配置包插进像素流中间会让固件的状态机错乱。"));
        return;
    }

    const int x = m_cfgXSpin->value();
    const int y = m_cfgYSpin->value();
    const int s = m_cfgSpeedSpin->value();

    if (m_ser->sendConfigPacket(x, y, s))
    {
        setStatus(QStringLiteral("配置已下发：X=%1 Y=%2 速度=%3 原点=(%4,%5)"
                                 "（69 字节，CRC 覆盖 65 字节）")
                  .arg(x).arg(y).arg(s)
                  .arg(m_cfgXOrgSpin->value()).arg(m_cfgYOrgSpin->value()), true);
    }
    else
    {
        setStatus(QStringLiteral("配置下发失败，详见日志"), false);
    }
}

void Widget::onSerialSend()
{
    if (m_gray.isNull())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先打开一张图片"));
        return;
    }
    if (!m_ser->isOpen())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先打开串口"));
        return;
    }
    if (m_ser->isStreaming())
    {
        setStatus(QStringLiteral("正在发送中，请先中止或等待完成"), false);
        return;
    }
    if (m_ser->pendingBytes() > 0)
    {
        if (QMessageBox::question(this, QStringLiteral("确认"),
                QStringLiteral("队列里还有 %1 字节未发完，确定重新开始下发整幅图吗？\n"
                               "（会先清空队列）").arg(m_ser->pendingBytes()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        m_ser->abortStream();
    }

    /* ★★ 开工前体检：固件到底在不在待命点？ ★★
       这是本项目最贵的一次踩坑，必须拦住：

       固件只在 waiting() 里认 55 66 AA。它要是不在 waiting()
       （刚刻完停在 while(1)、或还在跑上一幅被中止的像素数），
       我们的启动命令就只是串口上一串没人理的字节 ——
       它继续按自己的 y.all_point 空跑，读的是全零的 RX_data：
         · 机器一直在动            （步进照走）
         · 但不雕刻                （set_delay_time(0) → 不出光）
         · 回显全是白色            （回传的灰度就是 0）
       而且要把 图宽×图高 个像素跑完才停 —— 474x476 约 15 分钟，
       全程看起来"在工作"，实际上什么都没刻。

       判据只有一个：waiting() 里的固件会主动发就绪帧 55 77 AA。
       只要本次会话收到过 ≥1 条，就说明它进过 waiting()。 */
    if (m_ser->isHandshake() && m_ser->readyCount() == 0)
    {
        logMsg(QStringLiteral("[!] 开工体检未通过：本次会话没收到过固件就绪帧（55 77 AA）"), false);
        if (QMessageBox::question(this, QStringLiteral("固件可能不在待命状态"),
                QStringLiteral(
                    "从打开串口到现在，**没有收到固件的就绪帧（55 77 AA）**。\n\n"
                    "这说明板子很可能没停在 waiting()，现在发会有两种结果：\n\n"
                    "  · 板子刚上电/刚复位过 → 正常，继续发即可\n"
                    "  · 板子还在跑上一幅、或停在 while(1)\n"
                    "    → 现象是「机器一直在动、但不雕刻、回显全白」，\n"
                    "      而且要空跑完 图宽×图高 个像素才停（474×476 约 15 分钟）\n\n"
                    "建议：先按一下板子的复位键，等日志出现「固件就绪」再发。\n\n"
                    "确定要硬发吗？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        {
            setStatus(QStringLiteral("已取消下发 —— 请先复位板子，等日志出现「固件就绪（55 77 AA）」"), false);
            return;
        }
        logMsg(QStringLiteral("[!] 用户选择硬发 —— 若机器一直在动却不雕刻，就是固件不在待命点"), false);
    }

    const int w = m_gray.width();
    const int h = m_gray.height();
    const int pktsPerRow = (w + 63) / 64;
    const qint64 totalPkts = (qint64)pktsPerRow * h;
    const bool hs = m_ser->isHandshake();
    const double seconds = hs ? 0.0
                              : (double)totalPkts * m_serIntervalSpin->value() / 1000.0;

    if (hs)
        logMsg(QStringLiteral("发送参数: %1 x %2, 弓字形=%3, 握手模式(等固件 0x55 0x44 0xAA), 共 %4 包")
               .arg(w).arg(h)
               .arg(m_bidiBox->isChecked() ? QStringLiteral("是") : QStringLiteral("否"))
               .arg(totalPkts), true);
    else
        logMsg(QStringLiteral("发送参数: %1 x %2, 弓字形=%3, 自由模式 包间隔 %4ms, 预计 %5 秒")
               .arg(w).arg(h)
               .arg(m_bidiBox->isChecked() ? QStringLiteral("是") : QStringLiteral("否"))
               .arg(m_serIntervalSpin->value())
               .arg(seconds, 0, 'f', 1), true);

    /* ★ 每次开工都把当前生效的作业原点报一遍 ——
       原点是在**配置包**里下发的，勾了「先走一个图宽」却忘了重发配置，
       固件就还在用旧原点；而症状只是"整体偏移一个图宽"，
       图本身完全正常，肉眼很容易漏过去。 */
    logMsg(QStringLiteral("作业原点: X=%1  Y=%2%3")
           .arg(m_cfgXOrgSpin->value())
           .arg(m_cfgYOrgSpin->value())
           .arg((m_orgByWidthBox && m_orgByWidthBox->isChecked())
                ? QStringLiteral("   ← 已开「先走一个图宽」；刚改过的话必须重新下发配置才生效")
                : QString()),
           true);

    m_serSentPkts  = 0;
    m_serTotalPkts = (int)qMin<qint64>(totalPkts, 2000000000LL);
    m_serBar->setRange(0, m_serTotalPkts);
    m_serBar->setValue(0);
    m_serBar->setFormat(QStringLiteral("0/%1 包").arg(m_serTotalPkts));
    m_serSendBtn->setEnabled(false);
    m_serAbortBtn->setEnabled(true);
    m_serOpenBtn->setEnabled(false);
    m_serClock.start();

    /* ★ 进入雕刻：清空监看画布并切到监看页，开始接收固件回传的每一个像素。
       行方向在这里一并锁存（与下面 beginImage 用的是同一个值）。 */
    monitorBegin(w, h, m_bidiBox->isChecked());

    /* 用 beginImage + 逐行 feedRow：每行之间回一次事件循环，进度条才动得起来。
       倒序（弓字形）在 SerialIo 内部按行号判定，这里只负责给原始行。 */
    m_ser->beginImage(w, h, m_bidiBox->isChecked());
    for (int y = 0; y < h; ++y)
    {
        if (!m_ser->isOpen())
            break;
        m_ser->feedRow(m_gray.constScanLine(y), w);

        /* ★ 握手模式下 feedRow 内部会因 waitQueueDrain() 阻塞等固件来要，
           所以这里必须把事件循环喂出去，否则固件的 0x44 进不来 → 死等。
           自由模式只是为了让进度条动，频率可以低一些。 */
        const int stride = hs ? 0x07 : 0x0F;
        if ((y & stride) == stride)
        {
            setStatus(QStringLiteral("下发中… 第 %1 / %2 行").arg(y + 1).arg(h), true);
            QGuiApplication::processEvents();
        }
    }
    m_ser->endImage();

    /* 握手模式：endImage 在"队列还有货"时会提前返回（等固件来取），
       不会走 onSerialFinished。所以这里要自己把状态摆成"已装载、待命"。 */
    if (hs && m_ser->isOpen())
    {
        /* ★ 队列装载完毕，现在才通知固件"可以开始了"（帧 55 66 AA）。
           顺序不能反 —— 固件一收到这个命令就会立刻进 BT_Init() 开始要数据，
           必须先等队列装好，否则它第一轮就扑空、白等一个超时。
           固件那边此时正停在 waiting() 里反复发 55 77 AA 等这一帧。 */
        if (m_ser->sendStart())
        {
            setStatus(QStringLiteral("已下发启动命令（55 66 AA），队列 %1 包待取 —— "
                                     "等固件发 0x55 0x44 0xAA，来一个发一个")
                      .arg(m_ser->pendingPackets()), true);
        }
        else
        {
            setStatus(QStringLiteral("数据已装载 %1 包，但启动命令下发失败 —— 见日志")
                      .arg(m_serBar->maximum()), false);
        }
    }
}

void Widget::onSerialAbort()
{
    if (m_ser)
        m_ser->abortStream();
    setStatus(QStringLiteral("已中止发送"), false);
}

void Widget::onSerialPacketSent(int ctrl, int pixelCount, quint16 crc, int inFlight)
{
    /* 逐包刷日志会拖慢发送，只在异常时提示；正常进度用进度条表达 */
    Q_UNUSED(ctrl);
    Q_UNUSED(pixelCount);
    Q_UNUSED(crc);
    Q_UNUSED(inFlight);
}

void Widget::onSerialProgress(int sent, int total, int rows, int totalRows)
{
    /* 用自己记的总数更稳：握手模式下 total 可能在某次 beginImage 之前是 0 */
    const int tot = (total > 0) ? total : m_serTotalPkts;
    m_serSentPkts = sent;
    if (tot > 0)
    {
        m_serBar->setRange(0, tot);
        m_serBar->setValue(sent);
    }
    /* 握手模式不改 format —— 那里由 onSerialRequestReceived 显示请求次数，
       更能反映"固件要了几次"。 */
    if (!m_ser->isHandshake())
        m_serBar->setFormat(QStringLiteral("%1/%2 包  %3/%4 行")
                            .arg(sent).arg(tot).arg(rows).arg(qMax(0, totalRows)));
    Q_UNUSED(rows);
}

void Widget::onSerialFinished(bool aborted)
{
    m_serSendBtn->setEnabled(true);
    m_serAbortBtn->setEnabled(false);
    m_serOpenBtn->setEnabled(true);

    const double sec = (double)m_serClock.elapsed() / 1000.0;
    if (aborted)
    {
        m_serBar->setFormat(QStringLiteral("已中止"));
        setStatus(QStringLiteral("下发已中止"), false);
        /* 中止：固件不会再画了，收尾监看 */
        monitorEnd(true);
        return;
    }
    m_serBar->setValue(m_serBar->maximum());
    if (m_ser->isHandshake())
        m_serBar->setFormat(QStringLiteral("完成 %1 包 / 固件请求 %2 次")
                            .arg(m_serSentPkts).arg(m_ser->requestCount()));
    else
        m_serBar->setFormat(QStringLiteral("完成"));
    setStatus(QStringLiteral("下发完成，用时 %1 秒").arg(sec, 0, 'f', 1), true);
    logMsg(QStringLiteral("下发完成，用时 %1 秒").arg(sec, 0, 'f', 1), true);
}

void Widget::onSerialError(const QString &msg)
{
    logMsg(msg, false);
    setStatus(msg, false);
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
    /* ★ 载图时 w/h 是一起算好的，先把联动关掉再写：
       否则写宽度会触发"按比例算高度"，把这里刚算好的 h 覆盖掉
       （同一张图算出来基本一致，但四舍五入可能差 1 点，而且白算一遍灰度）。 */
    m_sizeSyncing = true;
    m_widthSpin->setValue(w);
    m_heightSpin->setValue(h);
    m_sizeSyncing = false;

    refreshGray();
}

/* ================= 目标尺寸联动（锁定宽高比） ================= */

/**
 * 按**原图**宽高比算另一边的点数。
 *
 * ★ 基准取原图，不取两个输入框当前的比值：
 *   输入框可能已经被手调成非原图比例（比如想刻正方形），
 *   拿它当基准会把比例一路带偏；用原图则永远对齐真实形状，
 *   换一张图之后也会自动跟着新图的比例走。
 * ★ 没载图时返回 0 = "无从换算"，调用方跳过联动。
 */
static int ratioCounterpart(const QImage &src, int known, bool knownIsWidth)
{
    if (src.isNull() || src.width() <= 0 || src.height() <= 0)
        return 0;

    const double v = knownIsWidth
                     ? (double)known * src.height() / src.width()
                     : (double)known * src.width()  / src.height();
    return qRound(v);
}

/**
 * 改「目标宽」→ 勾了锁定就按原图比例把「目标高」改掉。
 *
 * ★ 联动用 setValue 回写对边，那会再次触发对边的槽；
 *   靠 m_sizeSyncing 让对边那个槽**直接返回**，
 *   这样一次改动只走一条路径、只算一遍灰度（大图算两遍很卡）。
 */
void Widget::onSizeWidthChanged(int w)
{
    if (m_sizeSyncing)
        return;

    if (m_lockRatioBox && m_lockRatioBox->isChecked())
    {
        const int h = ratioCounterpart(m_srcImage, w, true);
        if (h > 0)
        {
            const int hv = qBound(1, h, m_heightSpin->maximum());
            if (hv != m_heightSpin->value())
            {
                m_sizeSyncing = true;
                m_heightSpin->setValue(hv);
                m_sizeSyncing = false;
            }
            /* 只有极端长宽比（原图比例 > 4096:1）才会被上限截断。
               这里用日志而不是状态栏 —— 状态栏马上会被 refreshGray 覆盖掉。 */
            if (hv != h)
                logMsg(QStringLiteral("[!] 高度按比例应为 %1 点，超出上限已截为 %2 点，比例已变形")
                       .arg(h).arg(hv), false);
        }
    }

    refreshGray();
}

/** 改「目标高」→ 对称处理（见 onSizeWidthChanged 的说明） */
void Widget::onSizeHeightChanged(int h)
{
    if (m_sizeSyncing)
        return;

    if (m_lockRatioBox && m_lockRatioBox->isChecked())
    {
        const int w = ratioCounterpart(m_srcImage, h, false);
        if (w > 0)
        {
            const int wv = qBound(1, w, m_widthSpin->maximum());
            if (wv != m_widthSpin->value())
            {
                m_sizeSyncing = true;
                m_widthSpin->setValue(wv);
                m_sizeSyncing = false;
            }
            if (wv != w)
                logMsg(QStringLiteral("[!] 宽度按比例应为 %1 点，超出上限已截为 %2 点，比例已变形")
                       .arg(w).arg(wv), false);
        }
    }

    refreshGray();
}

/**
 * 刚把「锁定宽高比」勾上时，立刻按当前宽度对齐一次高度。
 * 否则用户要再拨一下宽度才看得到效果，会以为没生效。
 * 取消勾选不做任何事 —— 保持当前尺寸，接下来手调即可。
 */
void Widget::onLockRatioToggled()
{
    if (m_lockRatioBox && m_lockRatioBox->isChecked())
        onSizeWidthChanged(m_widthSpin->value());
}

/**
 * 「加工前先走一个图宽」的勾选处理。
 *
 * 勾上 → 立刻把「原点X」设成当前图像宽度，并且**锁住输入框**：
 *   托管期间它的值必须恒等于图宽，手改一下就会和图像对不上，
 *   而且这种错很难看出来（只是整体偏移一个图宽，图本身还是好的）。
 * 取消 → 立刻解锁，值保留（用户想调成别的偏移随时改）。
 */
void Widget::onOrgByWidthToggled()
{
    if (!m_orgByWidthBox || !m_cfgXOrgSpin)
        return;

    const bool on = m_orgByWidthBox->isChecked();

    m_cfgXOrgSpin->setReadOnly(on);
    /* 托管时给个底色提示，不然用户会以为输入框坏了 */
    m_cfgXOrgSpin->setStyleSheet(on ? "background:#eef; color:#036;" : QString());

    if (on)
    {
        const int w = m_widthSpin->value();
        m_cfgXOrgSpin->setValue(w);
        logMsg(QStringLiteral("已开启「加工前先走一个图宽」：原点X 托管为图像宽度 %1 点。"
                              "⚠ 记得重新「下发配置」或复位板子，否则固件还用旧原点。")
               .arg(w), true);
    }
    else
    {
        logMsg(QStringLiteral("已取消「加工前先走一个图宽」：原点X 交回手动输入（当前 %1 点）。")
               .arg(m_cfgXOrgSpin->value()), true);
    }
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

    /* ★ 截断阈值取界面上的值（原来写死为常量 20，小图雕白时没法救） */
    const int cut = m_grayCutSpin ? m_grayCutSpin->value() : kGrayCutoffDefault;

    /* 顺便统计一下被砍掉多少 —— 这是"为什么雕出来是白的"最直接的证据 */
    int nZero = 0;
    const int total = gray.width() * gray.height();

    for (int y = 0; y < gray.height(); ++y)
    {
        const uchar *src = gray.constScanLine(y);
        uchar       *dat = data.scanLine(y);
        uchar       *pvw = view.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
        {
            int val = 255 - (int)src[x];
            if (val < cut) { val = 0; }
            if (val == 0) { ++nZero; }
            dat[x] = (uchar)val;
            pvw[x] = (uchar)(255 - val);
        }
    }

    m_gray       = data;
    m_previewImg = view;

    /* 配置包的 X/Y 跟着图像尺寸走：X = 一行点数（宽），Y = 一列点数（高）。
       载入新图后自动同步一次，省得用户手动填、填错导致固件按错的尺寸摆点。 */
    m_cfgXSpin->setValue(w);
    m_cfgYSpin->setValue(h);

    /* ★ 勾了「加工前先走一个图宽」→ 原点X 跟着图像宽度走。
       放这里而不是放 onSizeWidthChanged()：载图、改尺寸、改比例锁定，
       任何一条会让 w 变化的路径最后都会走到 refreshGray()，
       只在这里同步一次就不会漏。 */
    if (m_orgByWidthBox && m_orgByWidthBox->isChecked())
        m_cfgXOrgSpin->setValue(w);

    /* ★ 换了新图纸就切回预览页：否则看到的还是上一幅的监看画面，
       会误以为"新图已经画上去了"或"图纸没换"。 */
    if (!m_monitorActive)
        showViewPage(0);

    updatePreview();

    /* ★ 把"有多少点被归零"直接报出来 —— 雕刻出来全白时，一眼就能看出
         是数据本身被砍光了（阈值问题），还是数据没送到固件（链路问题）。 */
    if (total > 0)
    {
        const double pct = 100.0 * (double)nZero / (double)total;
        QString warn;
        if (pct > 95.0)
        {
            warn = QStringLiteral(
                " ⚠ 几乎全被截断(%1%)，雕刻出来会是空白！请把「背景截断阈值」调小(可试 0)");
        }
        else if (pct > 80.0)
        {
            warn = QStringLiteral(" （偏淡：%1% 的点不出光，可考虑调小阈值）");
        }
        setStatus(QStringLiteral("灰度图已就绪：%1 x %2 点，不出光的点占 %3%%4")
                  .arg(w).arg(h).arg(pct, 0, 'f', 1).arg(warn.arg(pct, 0, 'f', 1)),
                  pct <= 95.0);
    }
    else
    {
        setStatus(QStringLiteral("灰度图已就绪：%1 x %2 点").arg(w).arg(h), true);
    }
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
    out << QStringLiteral(" * 低灰度  : 出光强度 < %1 的已归零(去背景灰底)")
               .arg(m_grayCutSpin ? m_grayCutSpin->value() : kGrayCutoffDefault);
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
           .arg(m_grayCutSpin ? m_grayCutSpin->value() : kGrayCutoffDefault), true);

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
