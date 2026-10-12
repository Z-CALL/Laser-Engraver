#include "serialio.h"

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QImage>
#include <QCoreApplication>
#include <QDateTime>
#include <QVector>

/* ============================ 回传帧（33 gg EE） ============================ */

/**
 * 组装一条回传帧（3 字节）。仅用于上位机自检 / 模拟发端。
 * 固件侧请照这个字节序发：0x33 | 灰度 | 0xEE
 */
QByteArray SerialIo::buildReportFrame(quint8 gray)
{
    QByteArray f;
    f.reserve(ReportFrameBytes);
    f.append((char)ReportHead);
    f.append((char)gray);
    f.append((char)ReportTail);
    return f;
}

/* 把字节串转成 "AA BB CC" 形式的 hex，用于日志 */
static QString hexOfBytes(const QByteArray &b)
{
    QString s;
    for (int i = 0; i < b.size(); ++i)
    {
        if (i) s += QChar(' ');
        s += QString("%1").arg((quint8)b.at(i), 2, 16, QChar('0')).toUpper();
    }
    return s;
}

int SerialIo::reportCount() const   { return m_reportCount; }
int SerialIo::reportDropped() const { return m_reportDropped; }

void SerialIo::resetReportStats()
{
    m_reportCount   = 0;
    m_reportDropped = 0;
    m_pumpedReports = 0;   /* 补货基准跟着一起归零，否则换图后会少补 */
    m_echoAccum     = 0;   /* 累加器也要归零 */
}

void SerialIo::setReportWatch(bool on)
{
    m_reportWatch = on;
    if (!on)
        m_reportCount = m_reportDropped = 0;
}

bool SerialIo::reportWatch() const { return m_reportWatch; }

/* ==================== 回显驱动补货 ==================== */

void SerialIo::setEchoDriven(bool on)
{
    if (m_echoDriven == on)
        return;
    m_echoDriven = on;
    emitLog(on
        ? QStringLiteral("[>] 流控方式：回显驱动（固件不再发 55 44 AA，"
                         "上位机每收满 %1 条回显补一包）").arg(m_echoPerPacket)
        : QStringLiteral("[>] 流控方式：请求驱动（等固件发 55 44 AA，来一个发一包）"));
}

bool SerialIo::echoDriven() const { return m_echoDriven; }

void SerialIo::setEchoPerPacket(int n)
{
    if (n <= 0)
        n = 1;
    m_echoPerPacket = n;
}

int SerialIo::echoPerPacket() const { return m_echoPerPacket; }

void SerialIo::setPrefillCount(int n)
{
    /* 下限 1：固件不再开口要数据，预填充 0 会让它一上来就饿死。
       上限 8：固件侧 RX_data 只有 8 包深，再多会把未消费的包覆盖掉。 */
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    m_prefillCount = n;
}

int SerialIo::prefillCount() const { return m_prefillCount; }

/**
 * 启动时主动预填充若干包。
 *
 * ★ 为什么必须有这一步：固件不再发 55 44 AA，它就再也不会"开口要"。
 *   如果上位机也不主动喂，固件一进主循环缓冲区是空的，
 *   只能拿 RX_data 里的陈旧数据去雕 —— 画面从第一行就是错的。
 *
 * ★ 为什么默认 2 包而不是 1 包：
 *     1 包 = 雕第 N 包时手上只有第 N 包，一旦串口有抖动就没余量；
 *     2 包 = 雕第 N 包时第 N+1 包已在手，能容忍一包的传输抖动。
 *   再往上加收益递减，且逼近固件 8 包缓冲上限，所以默认就 2。
 */
void SerialIo::prefillPackets()
{
    int sent = 0;
    for (int i = 0; i < m_prefillCount; ++i)
    {
        if (m_queue.isEmpty())
            break;
        if (!pumpOne())
            break;
        ++sent;
    }

    /* 预填充也算"已补货"，把基准推到当前回显数，避免刚启动就重复补 */
    m_pumpedReports = m_reportCount;

    if (sent > 0)
    {
        emitLog(QStringLiteral("[>] 回显驱动：预填充 %1 包（固件不再发 55 44 AA，"
                               "之后每收满 %2 条回显补 1 包）")
                .arg(sent).arg(m_echoPerPacket));
    }
    else
    {
        emitLog(QStringLiteral("[!] 回显驱动：预填充失败，队列是空的"
                               "（没点「发送整幅图」装载数据？）"));
    }

    /* 起超时哨兵：等固件吐回显。雕刻一包约 64×3ms ≈ 192ms，
       远小于默认 3000ms 的窗口，正常情况下不会误报。 */
    if (m_hsTimeoutMs > 0 && !m_queue.isEmpty())
        m_hsTimer->start(m_hsTimeoutMs);
}

/**
 * 回显计数跨过"一包的点数"就补一包。
 *
 * ★ 用增量基准(m_pumpedReports)而不是取模，是为了抗丢帧：
 *   万一串口丢了几条回显，取模会永久错位；用"当前计数 - 上次补货计数"
 *   则下一包收到时会自动补上，不会累积漂移。
 */
void SerialIo::maybePumpByEcho()
{
    if (!m_echoDriven)
        return;
    if (m_mode != ModeHandshake)
        return;
    if (m_queue.isEmpty())
        return;

    /* 本次新增了多少条回传帧 */
    const int newReports = m_reportCount - m_pumpedReports;
    if (newReports <= 0)
        return;
    m_pumpedReports = m_reportCount;      /* 基准推进 */

    /* ★★ 精确换算（本次修掉的核心 bug）
         固件画完一整行 = 发 m_rowPixels 条回传帧 = 消耗 m_rowPackets 包。
         所以每来一条回传帧就累加 m_rowPackets，攒够 m_rowPixels 补一包。

         对比「固定每 64 条补 1 包」的旧算法：
           474 宽：实际每行需 8 包，旧法只补 7.4  → 每行亏 0.6，亏空  7.4%
           200 宽：实际每行需 4 包，旧法只补 3.1  → 每行亏 0.9，亏空 21.9%
         缓冲区只有 8 包深，200 的图不到 10 行就饿死 → 读到陈旧数据 → 全白。 */
    const int perRowPixels  = (m_rowPixels  > 0) ? m_rowPixels  : m_echoPerPacket;
    const int perRowPackets = (m_rowPackets > 0) ? m_rowPackets : 1;

    m_echoAccum += newReports * perRowPackets;

    while (m_echoAccum >= perRowPixels)
    {
        if (m_queue.isEmpty())
            break;
        if (!pumpOne())
            break;
        m_echoAccum -= perRowPixels;
        ++m_hsSent;

        /* 补货路径 (b)：按实际行数补货。
           ★ 限流：雕刻时这条每秒会走几次，不限流会刷爆日志窗口。 */
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if ((now - m_lastRxLogMs) > 500)
        {
            m_lastRxLogMs = now;
            emitLog(QStringLiteral("[>] 回传帧累计到一行(%1 条) → 补 %2 包"
                                   "（路径 b：回显计数；已收 %3 条，已发 %4 / %5 包）")
                    .arg(perRowPixels).arg(perRowPackets)
                    .arg(m_reportCount).arg(m_sentPkts).arg(m_totalPkts));
        }
    }

    /* 续上哨兵：继续等下一批回显 */
    if (!m_queue.isEmpty() && (m_hsTimeoutMs > 0))
        m_hsTimer->start(m_hsTimeoutMs);
}

/**
 * 回传异常日志限流。
 * ★ 必须限流：固件每 3ms 就发一条，一旦格式不对，
 *   错误日志会以每秒 300 条的速度刷爆界面。
 */
bool SerialIo::mayLogReport()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((now - m_lastReportLogMs) < 1000)
        return false;
    m_lastReportLogMs = now;
    return true;
}

/* ============================ 帧结构常量 ============================ */
static const quint8  kHead      = 0x55;   /* 帧头 */
static const quint8  kTail      = 0xAA;   /* 帧尾 */
static const int     kPixPerPkt = 64;     /* 每包固定 64 个像素点 */
static const int     kPktBytes  = 69;     /* 1 + 1 + 64 + 2 + 1 = 69 */

/* 队列水位：别一次把几十 MB 塞进 QSerialPort 的内部缓冲 */
static const qint64  kHighWater = 128 * 1024;
static const qint64  kLowWater  =  16 * 1024;

/* 默认 6 ms/包 ≈ 69 字节 / 115200bps = 690 bit，8N1 是 10 bit/字节 → 6.0ms 线时。
   所以 6ms 正好把串口跑满而不会在应用层堆积。固件消化不过来就往上调。 */
static const int     kDefaultIntervalMs = 6;

/* ============================ CRC ============================ */

/**
 * CRC16-CCITT-FALSE  (poly 0x1021, init 0xFFFF, 不反转输入输出, 不异或输出)
 *
 * 校验范围 = 控制位 + 64 个像素 = 65 字节（不含帧头/帧尾）。
 * 发送顺序：高字节在前。
 */
static quint16 crc16CcittFalse(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (int i = 0; i < data.size(); ++i)
    {
        crc ^= (quint16)((quint8)data.at(i)) << 8;
        for (int b = 0; b < 8; ++b)
        {
            if (crc & 0x8000)
                crc = (quint16)((crc << 1) ^ 0x1021);
            else
                crc = (quint16)(crc << 1);
        }
    }
    return crc;
}

/* ============================ 构造 / 析构 ============================ */

/* 握手超时默认值：等 3 秒还没来 0x44 就认为固件跑了/断了 */
static const int     kDefaultHsTimeoutMs = 3000;

/* 发完启动命令(55 66 AA)后，隔多久再预填充。
   ★ 不能为 0：固件处理 55 66 AA 时会调 USART3_RxStart()，
     其内部的 HAL_DMA_Abort 会把正在接收的预填充包冲掉。
     120ms 足够它处理完并重新武装 RX，又不会让用户觉得卡。 */
static const int     kPrefillDelayMs = 120;

SerialIo::SerialIo(QObject *parent) :
    QObject(parent),
    m_port(new QSerialPort(this)),
    m_timer(new QTimer(this)),
    m_hsTimer(new QTimer(this)),
    m_state(QStringLiteral("未打开")),
    m_inFlight(0),
    m_intervalMs(kDefaultIntervalMs),
    m_totalPkts(0),
    m_sentPkts(0),
    m_mode(ModeHandshake),
    m_reqCount(0),
    m_cfgReqCount(0),
    m_fwErrCount(0),
    m_readyCount(0),
    m_hsSent(0),
    m_hsTimeoutMs(kDefaultHsTimeoutMs),
    m_finished(false),
    m_lastWriteMs(0),
    m_rxTotal(0),
    m_firstReqSeen(false),
    m_lastRxLogMs(0),
    m_reportWatch(true),
    m_reportCount(0),
    m_reportDropped(0),
    m_lastReportLogMs(0),
    m_echoDriven(true),           /* 默认走新流控：固件只回显、不再发 55 44 AA */
    m_echoPerPacket(64),          /* 兜底值：拿不到行信息时才用 */
    m_prefillCount(2),            /* 1 包在雕 + 1 包备用 */
    m_pumpedReports(0),
    m_rowPixels(0),               /* beginImage() 里按实际图宽填 */
    m_rowPackets(0),
    m_echoAccum(0),
    m_cfgBaud(115200),
    m_cfgDataBits(8),
    m_cfgParity(ParityNone),     /* 无校验 8N1：与固件实测一致（原按需求配的奇校验是错的） */
    m_cfgStopBits(0),
    m_cfgX(64),
    m_cfgY(64),
    m_cfgSpeed(100),
    m_cfgXOrigin(0),
    m_cfgYOrigin(0),
    m_imgW(0),
    m_imgH(0),
    m_bidi(false),
    m_rowIndex(0),
    m_sentRows(0),
    m_aborted(false),
    m_logFn(0),
    m_logCtx(0)
{
    m_timer->setSingleShot(false);
    m_hsTimer->setSingleShot(true);

    connect(m_timer,   SIGNAL(timeout()), this, SLOT(onTimerTick()));
    connect(m_hsTimer, SIGNAL(timeout()), this, SLOT(onHandshakeTimeout()));
    connect(m_port, SIGNAL(bytesWritten(qint64)), this, SLOT(onBytesWritten(qint64)));
    connect(m_port, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    /* 串口层面的硬错误（校验位错 / 帧错 / 掉线）：
       这是排查"上位机没反应"的第三条线索 ——
       前两条是"收到字节了"(rxActivity) 和"字节能凑成请求帧"(onReadyRead)。
       这里报的是"字节在物理层就被判为坏"，最常见原因就是
       两端校验位/波特率不一致。用 lambda 免去在头文件里引入 QSerialPort。 */
    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError e) { onPortError((int)e); });
}

SerialIo::~SerialIo()
{
    close();
}

/* ============================ 端口枚举 ============================ */

QStringList SerialIo::availablePorts()
{
    QStringList out;
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    for (int i = 0; i < infos.size(); ++i)
    {
        const QSerialPortInfo &info = infos.at(i);
        QString desc = info.description();
        if (desc.isEmpty())
            desc = info.manufacturer();
        if (desc.isEmpty())
            desc = QStringLiteral("(无描述)");
        out << QString("%1|%2").arg(info.portName()).arg(desc);
    }
    return out;
}

/* ============================ 打开 / 关闭 ============================ */

void SerialIo::setPortParams(int baud, int dataBits, int parity, int stopBits)
{
    if (baud > 0)
        m_cfgBaud = baud;
    if ((dataBits >= 5) && (dataBits <= 8))
        m_cfgDataBits = dataBits;
    if ((parity >= ParityNone) && (parity <= ParityEven))
        m_cfgParity = parity;
    if ((stopBits >= 0) && (stopBits <= 2))
        m_cfgStopBits = stopBits;
}

int SerialIo::cfgBaud() const     { return m_cfgBaud; }
int SerialIo::cfgDataBits() const { return m_cfgDataBits; }
int SerialIo::cfgParity() const   { return m_cfgParity; }
int SerialIo::cfgStopBits() const { return m_cfgStopBits; }

QString SerialIo::cfgText() const
{
    const QChar p = (m_cfgParity == ParityOdd)  ? QChar('O')
                  : (m_cfgParity == ParityEven) ? QChar('E')
                                                : QChar('N');
    const QString s = (m_cfgStopBits == 1) ? QStringLiteral("1.5")
                    : (m_cfgStopBits == 2) ? QStringLiteral("2")
                                           : QStringLiteral("1");
    return QString("%1 %2-%3-%4").arg(m_cfgBaud).arg(m_cfgDataBits).arg(p).arg(s);
}

bool SerialIo::open(const QString &portName)
{
    close();

    if (portName.isEmpty())
    {
        m_state = QStringLiteral("端口名为空");
        emit errorOccurred(m_state);
        return false;
    }

    m_port->setPortName(portName);

    /* 参数来自 setPortParams()，默认 115200 / 8 / 奇 / 1。
       ⚠ 校验位必须与固件一致，否则从第 2 个字节起就会错位（见头文件说明）。 */
    QSerialPort::DataBits db = QSerialPort::Data8;
    switch (m_cfgDataBits)
    {
    case 5: db = QSerialPort::Data5; break;
    case 6: db = QSerialPort::Data6; break;
    case 7: db = QSerialPort::Data7; break;
    default: db = QSerialPort::Data8; break;
    }
    QSerialPort::Parity par = QSerialPort::NoParity;
    if (m_cfgParity == ParityOdd)       par = QSerialPort::OddParity;
    else if (m_cfgParity == ParityEven) par = QSerialPort::EvenParity;
    QSerialPort::StopBits sb = QSerialPort::OneStop;
    if (m_cfgStopBits == 1)      sb = QSerialPort::OneAndHalfStop;
    else if (m_cfgStopBits == 2) sb = QSerialPort::TwoStop;

    if (!m_port->open(QIODevice::ReadWrite))
    {
        m_state = QStringLiteral("打开失败：%1").arg(m_port->errorString());
        emit errorOccurred(m_state);
        emitLog(QStringLiteral("[!] ") + m_state);
        return false;
    }
    if (!m_port->setBaudRate(m_cfgBaud))
    {
        m_state = QStringLiteral("设置波特率失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setDataBits(db))
    {
        m_state = QStringLiteral("设置数据位失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setParity(par))
    {
        m_state = QStringLiteral("设置校验位失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setStopBits(sb))
    {
        m_state = QStringLiteral("设置停止位失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setFlowControl(QSerialPort::NoFlowControl))
    {
        m_state = QStringLiteral("设置流控失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    m_port->clear();

    m_inFlight   = 0;
    m_sentPkts   = 0;
    m_sentRows   = 0;
    m_reqCount   = 0;
    m_cfgReqCount = 0;
    m_hsSent     = 0;
    m_finished   = false;
    m_rxTotal    = 0;
    m_firstReqSeen = false;
    m_queue.clear();
    m_rxBuf.clear();
    m_hsTimer->stop();

    m_state = QStringLiteral("已打开 %1 (%2)").arg(portName).arg(cfgText());
    emitLog(QStringLiteral("[>] ") + m_state);
    emit opened(portName);
    return true;
}

void SerialIo::close()
{
    m_timer->stop();
    m_hsTimer->stop();
    if (m_port->isOpen())
    {
        m_port->close();
        emitLog(QStringLiteral("[>] 串口已关闭"));
    }
    m_port->setPortName(QString());
    m_queue.clear();
    m_rxBuf.clear();
    m_inFlight = 0;
    m_reqCount = 0;
    m_cfgReqCount = 0;
    m_hsSent   = 0;
    m_finished = false;
    m_state    = QStringLiteral("未打开");
    emit closed();
}

bool SerialIo::isOpen() const
{
    return m_port->isOpen();
}

QString SerialIo::portName() const
{
    return m_port->portName();
}

QString SerialIo::errorString() const
{
    return m_port->errorString();
}

QString SerialIo::stateText() const
{
    return m_state;
}

int SerialIo::packetBytes()
{
    return kPktBytes;
}

/* ============================ 打包 ============================ */

/**
 * 拼一个完整数据包。pixels64 长度必须是 64。
 *
 *     偏移  0    : 0x55
 *     偏移  1    : 控制位
 *     偏移  2..65: 64 个像素灰度
 *     偏移 66..67: CRC16 大端
 *     偏移 68    : 0xAA
 */
QByteArray SerialIo::buildPacket(quint8 ctrl, const QByteArray &pixels64) const
{
    QByteArray body;
    body.reserve(1 + kPixPerPkt);
    body.append((char)ctrl);
    body.append(pixels64);                       /* 已是 64 字节 */

    const quint16 crc = crc16CcittFalse(body);

    QByteArray pkt;
    pkt.reserve(kPktBytes);
    pkt.append((char)kHead);
    pkt.append(body);
    pkt.append((char)((crc >> 8) & 0xFF));       /* CRC 高字节 */
    pkt.append((char)(crc & 0xFF));              /* CRC 低字节 */
    pkt.append((char)kTail);
    return pkt;
}

/* ============================ 单包发送 ============================ */

bool SerialIo::sendPacket(quint8 ctrl, const QByteArray &pixels64)
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法发送"));
        return false;
    }
    if (pixels64.size() != kPixPerPkt)
    {
        emit errorOccurred(QStringLiteral("像素字段必须为 %1 字节，当前 %2")
                           .arg(kPixPerPkt).arg(pixels64.size()));
        return false;
    }
    m_queue.enqueue(buildPacket(ctrl, pixels64));
    kickTimerIfNeeded();
    return true;
}

/**
 * 换行。
 *
 * 若队列尾部的包本身就是 0x11 的数据包（= 还没有固件尚未见到的行边界），
 * 直接把它原地升级成 0x22 —— 这一包的 64 点立刻变成"新一行"的点，零浪费。
 * 否则补发一个全 0x00 的 0x22 空包，仅用于把行边界钉死。
 *
 * ★ 判定条件必须是"队尾最后一个是 0x11"：因为它之后没有任何包，
 *   把换行信息挂在它身上才不会有别的像素插到"新行"里去。
 */
bool SerialIo::sendNewline()
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法换行"));
        return false;
    }

    if (!m_queue.isEmpty())
    {
        QByteArray &last = m_queue.last();
        if (last.size() >= 2 && (quint8)last.at(1) == (quint8)CtrlData)
        {
            QByteArray pix = last.mid(2, kPixPerPkt);
            last = buildPacket((quint8)CtrlNewline, pix);
            emitLog(QStringLiteral("[>] 换行：末包控制位 0x11 -> 0x22（复用 64 点，未补空包）"));
            kickTimerIfNeeded();
            return true;
        }
    }

    QByteArray zeros(kPixPerPkt, (char)0x00);
    m_queue.enqueue(buildPacket((quint8)CtrlNewline, zeros));
    emitLog(QStringLiteral("[>] 换行：补发 0x22 空包（64 x 0x00）"));
    kickTimerIfNeeded();
    return true;
}

/* ============================ 配置包（0x33）============================ */

/* 配置包载荷里的三个字段标签与填充字节 */
static const quint8 kCfgTagX    = 0x44;   /* X 轴字段标签 */
static const quint8 kCfgTagY    = 0x55;   /* Y 轴字段标签 */
static const quint8 kCfgTagSpd  = 0x66;   /* 加工速度字段标签 */
static const quint8 kCfgTagXOrg = 0x77;   /* X 轴作业原点字段标签 */
static const quint8 kCfgTagYOrg = 0x88;   /* Y 轴作业原点字段标签 */
static const quint8 kCfgFill    = 0xFF;   /* 预留位填充 */

/**
 * 生成配置包的 64 字节载荷。
 *
 * 布局（包内偏移 = 整帧偏移 - 2）：
 *   0      1   0x44
 *   1-2    2   X（大端）            X 轴一行像素点
 *   3      1   0x55
 *   4-5    2   Y（大端）            Y 轴一列像素点
 *   6      1   0x66
 *   7      1   速度
 *   8      1   0x77
 *   9-10   2   X 作业原点（大端）
 *   11     1   0x88
 *   12-13  2   Y 作业原点（大端）
 *   14-63 50   0xFF
 *
 * ★ 大端（高字节在前）是为了和 CRC 的字节序保持一致 —— 全包都是大端，
 *   固件读 X 就是 (buf[3] << 8) | buf[4]，不用为不同字段记两套顺序。
 *   若固件按小端读，把各字段的两个 append 顺序对调即可。
 */
QByteArray SerialIo::buildConfigPayload(int xPixels, int yPixels, int speed,
                                        int xOrigin, int yOrigin)
{
    if (xPixels < 0)     { xPixels = 0; }
    if (xPixels > 0xFFFF){ xPixels = 0xFFFF; }
    if (yPixels < 0)     { yPixels = 0; }
    if (yPixels > 0xFFFF){ yPixels = 0xFFFF; }
    if (speed   < 0)     { speed = 0; }
    if (speed   > 0xFF)  { speed = 0xFF; }
    if (xOrigin < 0)     { xOrigin = 0; }
    if (xOrigin > 0xFFFF){ xOrigin = 0xFFFF; }
    if (yOrigin < 0)     { yOrigin = 0; }
    if (yOrigin > 0xFFFF){ yOrigin = 0xFFFF; }

    QByteArray p;
    p.reserve(kPixPerPkt);

    p.append((char)kCfgTagX);
    p.append((char)((xPixels >> 8) & 0xFF));      /* X 高字节 */
    p.append((char)(xPixels & 0xFF));             /* X 低字节 */

    p.append((char)kCfgTagY);
    p.append((char)((yPixels >> 8) & 0xFF));      /* Y 高字节 */
    p.append((char)(yPixels & 0xFF));             /* Y 低字节 */

    p.append((char)kCfgTagSpd);
    p.append((char)(speed & 0xFF));

    p.append((char)kCfgTagXOrg);
    p.append((char)((xOrigin >> 8) & 0xFF));      /* X 作业原点 高字节 */
    p.append((char)(xOrigin & 0xFF));             /* X 作业原点 低字节 */

    p.append((char)kCfgTagYOrg);
    p.append((char)((yOrigin >> 8) & 0xFF));      /* Y 作业原点 高字节 */
    p.append((char)(yOrigin & 0xFF));             /* Y 作业原点 低字节 */

    while (p.size() < kPixPerPkt)                 /* 补齐到 64 字节 */
        p.append((char)kCfgFill);

    return p;
}

void SerialIo::setConfigValues(int xPixels, int yPixels, int speed,
                               int xOrigin, int yOrigin)
{
    m_cfgX       = xPixels;
    m_cfgY       = yPixels;
    m_cfgSpeed   = speed;
    m_cfgXOrigin = xOrigin;
    m_cfgYOrigin = yOrigin;
}

int SerialIo::cfgXPixels() const { return m_cfgX; }
int SerialIo::cfgYPixels() const { return m_cfgY; }
int SerialIo::cfgSpeed()   const { return m_cfgSpeed; }
int SerialIo::cfgXOrigin() const { return m_cfgXOrigin; }
int SerialIo::cfgYOrigin() const { return m_cfgYOrigin; }

/**
 * 用当前 m_cfg* 值打包一个配置包并**直接写串口**。
 *
 * ★ 不走 m_queue：配置包是单个独立包，塞进队列反而要额外处理"它在像素流里的位置"。
 *   直接写会排在**已经缓冲出去的字节之后**，语义上就是"紧接着发一个配置包"。
 */
bool SerialIo::writeConfigPacket()
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法下发配置"));
        return false;
    }

    const QByteArray payload = buildConfigPayload(m_cfgX, m_cfgY, m_cfgSpeed,
                                                  m_cfgXOrigin, m_cfgYOrigin);
    const QByteArray pkt     = buildPacket((quint8)CtrlConfig, payload);

    const qint64 n = m_port->write(pkt);
    if (n <= 0)
    {
        emit errorOccurred(QStringLiteral("配置包写入失败：%1").arg(m_port->errorString()));
        return false;
    }
    m_inFlight += n;

    const quint16 crc = (quint16)(((quint8)pkt.at(66) << 8) | (quint8)pkt.at(67));

    /* 把整包 hex 打出来：配置错了整幅图都会错位，值得让它可核对 */
    QString hex;
    for (int i = 0; i < pkt.size(); ++i)
    {
        if (i && (i % 16 == 0))
            hex += QStringLiteral(" ");
        hex += QStringLiteral("%1 ").arg((quint8)pkt.at(i), 2, 16, QChar('0')).toUpper();
    }

    emitLog(QStringLiteral("[>] 配置包已下发：X=%1 Y=%2 速度=%3 原点=(%4,%5)，CRC=0x%6")
            .arg(m_cfgX).arg(m_cfgY).arg(m_cfgSpeed)
            .arg(m_cfgXOrigin).arg(m_cfgYOrigin)
            .arg(crc, 4, 16, QChar('0')).toUpper());
    emit packetSent((int)CtrlConfig, kPixPerPkt, crc, pendingBytes());
    emitLog(QStringLiteral("[>] 69 字节：%1").arg(hex.trimmed()));
    return true;
}

/**
 * 下发配置包（界面按钮走这里）。
 *
 * ★ 图像正在下发时**拒发**：这是保护手动误点。
 *   而固件主动发 0x55 0x55 0xAA 来要时不拦 —— 那是固件明确要求的，
 *   它自己知道当前处于什么状态（见 onReadyRead）。
 */
bool SerialIo::sendConfigPacket(int xPixels, int yPixels, int speed)
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法下发配置"));
        return false;
    }
    if (isStreaming())
    {
        emitLog(QStringLiteral("[!] 正在下发图像，配置包已拒绝 —— "
                               "请等图像发完或先点「中止」再下发配置"));
        return false;
    }

    /* 作业原点沿用界面上已经同步过来的值（onCfgValueChanged 随时在同步） */
    setConfigValues(xPixels, yPixels, speed, m_cfgXOrigin, m_cfgYOrigin);
    return writeConfigPacket();
}

/* ============================ 流式推送 ============================ */

int SerialIo::pendingBytes() const
{
    return (int)((qint64)m_queue.size() * kPktBytes + m_inFlight);
}

int SerialIo::pendingPackets() const
{
    return m_queue.size();
}

/* ============================ 发送模式 / 握手 ============================ */

void SerialIo::setMode(Mode m)
{
    if (m_mode == m)
        return;
    m_mode = m;

    if (m_mode == ModeHandshake)
    {
        /* 从自由模式切过来：停掉自走的节拍，改由 0x44 驱动。
           已经写在路上的字节不管它（就那么几十字节，吃完自然停）。 */
        m_timer->stop();
        emitLog(QStringLiteral("[>] 发送模式：握手（等固件 0x55 0x44 0xAA，来一个发一包）"));
        if (!m_queue.isEmpty())
            emitLog(QStringLiteral("[>] 队列里有 %1 包待发，等固件来要").arg(m_queue.size()));
    }
    else
    {
        m_hsTimer->stop();
        emitLog(QStringLiteral("[>] 发送模式：自由（按包间隔 %1 ms 自己发）").arg(m_intervalMs));
        kickTimerIfNeeded();
    }

    emit modeChanged((int)m_mode);
}

SerialIo::Mode SerialIo::mode() const
{
    return m_mode;
}

bool SerialIo::isHandshake() const
{
    return (m_mode == ModeHandshake);
}

QByteArray SerialIo::reqDataFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlReqData);
    f.append((char)0xAA);
    return f;
}

QByteArray SerialIo::reqConfigFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlReqCfg);
    f.append((char)0xAA);
    return f;
}

QByteArray SerialIo::initTimeoutFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlInitTimeout);
    f.append((char)0xAA);
    return f;
}

QByteArray SerialIo::readyFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlReady);
    f.append((char)0xAA);
    return f;
}

QByteArray SerialIo::startFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlStart);
    f.append((char)0xAA);
    return f;
}

/**
 * 下发启动命令（帧 55 66 AA）：告诉固件"队列已就绪，可以开始取数据了"。
 *
 * ★ 它是新启动握手的后半段（前半段见 onReadyRead 里的 55 77 AA 分支）：
 *     固件  ：main() → waiting()，反复发 55 77 AA 报告就绪
 *     上位机：收到后提示用户；用户点「发送整幅图」装载队列；本函数发 55 66 AA
 *     固件  ：收到 55 66 AA → 立刻 BT_Init() 取 8 包 → 进主循环
 *   这样"上位机什么时候有货"由上位机自己掌握，固件不会再因为等不到数据而超时。
 *
 * ⚠ 必须在队列**装载完成之后**调用 —— 固件一收到这个命令就会马上开始要数据。
 *
 * 返回 true = 命令已写进串口。
 */
bool SerialIo::sendStart()
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法下发启动命令"));
        return false;
    }

    const QByteArray pkt = startFrame();
    const qint64 n = m_port->write(pkt);
    if (n <= 0)
    {
        emit errorOccurred(QStringLiteral("启动命令写入失败：%1").arg(m_port->errorString()));
        return false;
    }

    emitLog(QStringLiteral("已下发启动命令 55 66 AA —— 固件将开始取数据（队列 %1 包）")
            .arg(m_queue.size()));

    /* ★ 回显驱动模式下必须主动预填充：
         固件已经不再发 55 44 AA 了，它永远不会开口要第一包。
         这里不喂，它一进主循环就只能拿 RX_data 里的陈旧数据去雕。

       ★★ 为什么要延迟一拍（约 120ms）而不是立刻发：
         固件在 waiting() 里收到 55 66 AA 之后会调 USART3_RxStart()，
         而那个函数开头就是 HAL_DMA_Abort(&hdma_usart3_rx) ——
         它会把**此刻正在接收的**预填充包直接冲掉。
         （BT_Init() 里那 8 次循环更是每次都会 abort 一次。）
         等固件把 55 66 AA 处理完、RX 重新武装好，再喂就稳了。 */
    if (m_echoDriven)
    {
        emitLog(QStringLiteral("[>] 已下发启动命令，%1 ms 后预填充 %2 包"
                               "（等固件处理完 55 66 AA 再喂，避免被其 RxStart 冲掉）")
                .arg(kPrefillDelayMs).arg(m_prefillCount));
        QTimer::singleShot(kPrefillDelayMs, this, SLOT(onPrefillTimeout()));
    }

    return true;
}

void SerialIo::onPrefillTimeout()
{
    if (!m_echoDriven)
        return;
    if (m_queue.isEmpty())
    {
        emitLog(QStringLiteral("[!] 预填充时队列是空的，跳过"
                               "（可能已中止，或还没点「发送整幅图」）"));
        return;
    }
    prefillPackets();
}

void SerialIo::setHandshakeTimeout(int ms)
{
    m_hsTimeoutMs = qMax(0, ms);
}

int SerialIo::handshakeTimeout() const
{
    return m_hsTimeoutMs;
}

int SerialIo::requestCount() const
{
    return m_reqCount;
}

int SerialIo::handshakeSentCount() const
{
    return m_hsSent;
}

/**
 * 启动"等固件来要"的超时哨兵。
 *
 * 只在**真的有包要发、而且此刻没包在路上**时才起表 —— 否则会误报：
 * 固件每收到一包都要花时间出光/走位，这段时间上位机手上没活了，
 * 不该算"等超时"。所以表是在"上一包写完 + 队列还有货"的那一刻才起。
 */
void SerialIo::onHandshakeTimeout()
{
    if (m_mode != ModeHandshake)
        return;
    if (m_queue.isEmpty() && (m_inFlight <= 0))
        return;                       /* 发完了，不是超时 */

    const int left = m_queue.size();

    /* ★ 两种流控方式，超时含义完全不同，提示必须分开：
         回显驱动 → 等的是固件吐回显(33 gg EE)，说明它没在雕（卡死/限位/没启动）
         请求驱动 → 等的是固件发 55 44 AA，说明它没开口要数据 */
    if (m_echoDriven)
    {
        /* ★ 把"已收到几条回传帧"打出来 —— 这是最关键的判别信息：
             累计 0 条   → 固件一条都没发出来，根本没在画点（卡在 BT_Init / 没进主循环 / TIM2 没跑）
             0 < 累计 < 64 → 固件在画，但还没画够一包，只是慢，不是死
             累计 ≥ 64  → 不该超时，属于误报 */
        const int got = m_reportCount;
        const int need = m_echoPerPacket;
        const int inThisPkt = got - m_pumpedReports;

        emitLog(QStringLiteral("[!] 回显超时：%1 ms 内没有补货动作，剩 %2 包未发\n"
                               "    已收到回传帧：%3 条（本包内已收 %4 / %5）\n"
                               "    %6")
                .arg(m_hsTimeoutMs).arg(left)
                .arg(got).arg(inThisPkt).arg(need)
                .arg(got == 0
                     ? QStringLiteral(
                         "★ 累计 0 条 = 固件一条都没发出来，压根没在画点。\n"
                         "      查：① 固件是否还在 BT_Init() 里等数据（Bluetooth.c 的 8 次 Send_Next 删了没）\n"
                         "          ② 是否真的进了主循环 —— TIM2 是在 waiting() 之后才启动的\n"
                         "          ③ TIM2 中断里 4 个限位开关任一有效会直接 return，步进全停\n"
                         "          ④ 回传帧发送那几行（33 gg EE）是否真的开着")
                     : (inThisPkt < need
                        ? QStringLiteral("★ 固件在画，但本包还没满 —— 只是慢，不是死。可调大握手超时。")
                        : QStringLiteral("★ 计数已够却没补货，属异常，检查队列是否为空。"))));
    }
    else
    {
        emitLog(QStringLiteral("[!] 握手超时：%1 ms 没收到 0x55 0x44 0xAA（取数据），剩 %2 包未发")
                .arg(m_hsTimeoutMs).arg(left));
    }

    m_timer->stop();
    emit handshakeTimedOut(m_hsTimeoutMs, left);
}

/**
 * 解析上行的握手请求。
 *
 * 固件来的是一个 3 字节定长帧：0x55 0x44 0xAA。
 *
 * ★ 为什么按长度 3 严格匹配、而不做"看到 0xAA 就收"的宽松扫描：
 *   下行包也是 0x55 开头、0xAA 结尾，只是长度 69。
 *   如果上位机在自环/回显场景下收到自己刚发的东西（或者线路串扰），
 *   宽松扫描会把下行包里的字节误判成请求，于是多发一包 → 越滚越乱。
 *   按 (0x55, 0x44, 0xAA) 三字节滑动窗匹配，天然免疫。
 *
 * 收到一个就下发一包，一个请求只换一个数据包，绝不多发。
 */
void SerialIo::onReadyRead()
{
    const QByteArray chunk = m_port->readAll();
    if (chunk.isEmpty())
        return;

    m_rxTotal += chunk.size();
    emit rxActivity(m_rxTotal);

    /* 记下进入本次解析前的回传计数：
       后面用来区分"收到的是垃圾"和"收到的只是回传帧" ——
       回传帧每 3ms 一条，若把它当垃圾报警，日志会被刷爆。 */
    const int repCountBefore = m_reportCount;

    /* ---- 1. 把本次收到的字节并进缓冲 ----
       这是"上位机到底听没听到"的唯一直接证据。
       没有它，用户只能猜：是字节根本没到？还是到了但不匹配？
       两者原因完全不同（前者查接线/端口/波特率，后者查帧格式/校验位）。 */
    m_rxBuf.append(chunk);

    /* ---- 2. 按**首字节分派**的状态机解析：一个流里混着两种帧 ----
       ★ 为什么不能再"搜 55 xx AA"（这是本次改动的核心）：
         固件每画完一个像素就回传一条 3 字节帧 33 gg EE。
         灰度值 gg 是 0~255 任意值，连发的回传帧**跨帧拼凑**时很容易凑出
         "55 44 AA" 这种假请求帧；老解析器会在整个缓冲里搜这个序列，
         一旦命中就当成"取数据请求" → 上位机多发一包 → 握手错乱 → 雕刻中途卡死。
         （实测：256 个灰度值连发时，老解析器会解出大量假请求帧。）

         改成"按首字节分派 + 固定长度整体消费"后，两种帧各吃各的，互不干扰：
             首字节 0x33 → 吃掉 3 字节，按回传帧校验（末字节必须 0xEE）
             首字节 0x55 → 吃掉 3 字节，按请求帧校验（末字节必须 0xAA）

       ★ 顺序必须保留：固件若连发「取数据 + 取配置」，
         按到达顺序处理才不会让它收到错的回应。 */
    QVector<quint8> tokens;   /* ReqData / ReqCfg / InitTimeout / Ready */

    while (!m_rxBuf.isEmpty())
    {
        const quint8 b0 = (quint8)m_rxBuf.at(0);

        /* ---------- 回传帧：0x33 gg 0xEE 共 3 字节 ---------- */
        if (m_reportWatch && b0 == (quint8)ReportHead)
        {
            if (m_rxBuf.size() < ReportFrameBytes)
                break;                              /* 还没收全，等下一批 */

            const quint8 gg = (quint8)m_rxBuf.at(1);
            const quint8 tt = (quint8)m_rxBuf.at(2);

            if (tt == (quint8)ReportTail)
            {
                const int seq = m_reportCount;      /* 第几个到达的点 */
                ++m_reportCount;
                emit pixelReported(seq, (int)gg);
                m_rxBuf.remove(0, ReportFrameBytes);
                continue;
            }

            /* 帧尾不对：只丢这 1 个字节，让后面的 0x33 有机会重新对齐 */
            ++m_reportDropped;
            if (mayLogReport())
            {
                emitLog(QStringLiteral("[!] 回传帧帧尾不对（已丢弃 %1 条）：%2")
                        .arg(m_reportDropped)
                        .arg(hexOfBytes(m_rxBuf.left(ReportFrameBytes))));
            }
            m_rxBuf.remove(0, 1);
            continue;
        }

        /* ---------- 请求帧：0x55 xx 0xAA 共 3 字节 ---------- */
        if (b0 == (quint8)kHead)
        {
            if (m_rxBuf.size() < 3)
                break;                              /* 还不够 3 字节 */

            const quint8 b1 = (quint8)m_rxBuf.at(1);
            const quint8 b2 = (quint8)m_rxBuf.at(2);

            if (b2 == (quint8)kTail)
            {
                quint8 which = 0;
                bool   known = true;
                if      (b1 == (quint8)CtrlReqData)     which = (quint8)CtrlReqData;
                else if (b1 == (quint8)CtrlReqCfg)      which = (quint8)CtrlReqCfg;
                else if (b1 == (quint8)CtrlInitTimeout) which = (quint8)CtrlInitTimeout;
                else if (b1 == (quint8)CtrlReady)       which = (quint8)CtrlReady;
                else                                    known = false;

                if (known)
                {
                    tokens.append(which);
                    m_rxBuf.remove(0, 3);
                }
                else
                {
                    /* 55 xx AA 但 xx 不认识：丢掉这个 0x55，继续往后对齐 */
                    m_rxBuf.remove(0, 1);
                }
                continue;
            }

            /* 既不是完整请求帧：丢 1 字节重新找帧头 */
            m_rxBuf.remove(0, 1);
            continue;
        }

        /* ---------- 其它字节：丢 1 字节 ---------- */
        m_rxBuf.remove(0, 1);
    }

    /* ---- 2.5 回显驱动补货 ----
       ★ 位置很关键：必须放在下面那句「全是回传帧就 return」之前。
         新流控下固件不再发 55 44 AA，于是**绝大多数 onReadyRead 里 hit 都是 0**
         （收到的全是 33 gg EE）。若把补货排在 return 之后，就永远轮不到补货，
         链路会静悄悄饿死 —— 而且日志干干净净，极难排查。 */
    maybePumpByEcho();

    const int hit = tokens.size();

    /* 防膨胀：状态机已经把所有能认的帧都消费掉了，剩下的只可能是
       "还没收全的帧前缀"（0x33 开头不足 3 字节，或 0x55 开头不足 3 字节）。
       ★ 不能再粗暴地 right(2) —— 那会把只收了一半的回传帧截断，
         而回传帧是连续不断的，会被反复截断成垃圾。
       只在"缓冲异常膨胀"时才清理，且保留最后一个已知帧头之后的部分。 */
    if (m_rxBuf.size() > 64)
    {
        int keepFrom = -1;
        for (int i = m_rxBuf.size() - 1; i >= 0; --i)
        {
            const quint8 c = (quint8)m_rxBuf.at(i);
            if (c == (quint8)kHead
                || (m_reportWatch && c == (quint8)ReportHead))
            {
                keepFrom = i;
                break;
            }
        }
        if (keepFrom > 0)
            m_rxBuf.remove(0, keepFrom);
        else if (keepFrom < 0)
            m_rxBuf.clear();
    }

    /* ---- 3. 收到的字节上日志（限流，防止垃圾流刷爆窗口） ---- */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool   mayLog = (now - m_lastRxLogMs) > 500;

    if (hit <= 0)
    {
        /* ★ 必须区分两种情况，否则回传数据会被误报成"链路坏了"：
             (a) 本次收到的确实是垃圾 → 值得报警（排查校验位/波特率不匹配）
             (b) 本次只收到了回传帧、没有请求帧 → 完全正常，别刷日志
           判据：回传计数在这次调用里有没有增长。 */
        if (m_reportCount != repCountBefore)
            return;                     /* 全是回传帧，正常，不打扰用户 */

        /* 收到东西了、但既不是请求帧也不是回传帧 —— 这条日志是排查
           "校验位/波特率不匹配"的第一手证据（那种情况下字节会到，但内容被解错）。 */
        if (mayLog)
        {
            m_lastRxLogMs = now;
            QString hex;
            const int n = qMin(chunk.size(), 24);
            for (int i = 0; i < n; ++i)
                hex += QString("%1 ").arg((quint8)chunk.at(i), 2, 16, QChar('0')).toUpper();
            if (chunk.size() > n)
                hex += QStringLiteral("...");
            emitLog(QStringLiteral("[!] 收到 %1 字节但不是任何已知帧"
                                   "（期望 0x55 0x44 0xAA / 0x55 0x55 0xAA / 0x33 gg 0xEE）：%2")
                    .arg(chunk.size()).arg(hex.trimmed()));
        }
        return;
    }

    {
        int nData = 0, nCfg = 0, nTmo = 0, nRdy = 0;
        for (int i = 0; i < tokens.size(); ++i)
        {
            const quint8 t = tokens.at(i);
            if (t == (quint8)CtrlReqCfg)
                ++nCfg;
            else if (t == (quint8)CtrlInitTimeout)
                ++nTmo;
            else if (t == (quint8)CtrlReady)
                ++nRdy;
            else
                ++nData;
        }

        m_lastRxLogMs = now;
        QString hex;
        const int n = qMin(chunk.size(), 12);
        for (int i = 0; i < n; ++i)
            hex += QString("%1 ").arg((quint8)chunk.at(i), 2, 16, QChar('0')).toUpper();
        if (chunk.size() > n)
            hex += QStringLiteral("...");
        emitLog(QStringLiteral("[>] 收到 %1 个帧（取数据 %2 / 取配置 %3 / 固件超时 %4 / 就绪 %5）"
                               "本次 %6 字节：%7")
                .arg(hit).arg(nData).arg(nCfg).arg(nTmo).arg(nRdy)
                .arg(chunk.size()).arg(hex.trimmed()));
    }

    /* ---- 4. 固件主动上报的错误：与发送模式无关，必须先处理 ----
       ★ 放在 m_mode 检查之前 —— 无论上位机处于握手还是自由模式，
         固件"我收不到数据"这件事都必须显示出来。否则用户只看到板子 OLED 报错、
         上位机一声不吭，完全无从下手。 */
    for (int k = 0; k < hit; ++k)
    {
        if (tokens.at(k) != (quint8)CtrlInitTimeout)
            continue;

        ++m_fwErrCount;

        QString why;
        if (m_totalPkts == 0)
        {
            why = QStringLiteral(
                "上位机还没装载任何像素数据（没点「发送整幅图」）。\n"
                "    正确顺序：① 打开串口  →  ② 点「发送整幅图」装载队列  →  ③ 再复位板子。\n"
                "    固件在 main() 开头就开始要数据，晚一步点就必然超时。");
        }
        else if (m_queue.isEmpty() && (m_sentPkts > 0))
        {
            why = QStringLiteral("整幅图已经发完了（共下发 %1 包），固件还在继续要数据。")
                  .arg(m_sentPkts);
        }
        else
        {
            why = QStringLiteral(
                "队列里还有 %1 包没发出去，但固件没等到。\n"
                "    检查：串口写入是否失败、线路是否受干扰、固件的接收超时窗口是否太短。")
                  .arg(m_queue.size());
        }

        /* 注意：emitLog 的正文**不要自带 [>] / [!] 前缀** ——
           界面侧的 logMsg() 会统一加，自带了就会打成 "[>] [>] xxx"。 */
        const QString msg = QStringLiteral(
            "固件报错：蓝牙接收超时（板子 OLED 显示 BT_RX TIMEOUT）\n"
            "    含义：固件发出了取数据请求（0x55 0x44 0xAA），但在自己的超时窗口内\n"
            "          没收到完整的 69 字节数据包。\n"
            "    原因：%1").arg(why);

        emitLog(QStringLiteral("第 %1 次收到固件超时上报（0x55 0x10 0xAA）")
                .arg(m_fwErrCount));
        emitLog(msg);
        emit firmwareError((int)CtrlInitTimeout, msg);
    }

    /* ---- 5. 固件就绪通知（帧 55 77 AA）：与模式无关，先处理 ----
       固件在 waiting() 里发这一帧报告"我准备好了"，然后就在等上位机的 55 66 AA。
       上位机不需要回任何东西，只需要提示用户"可以点下发了"。 */
    for (int k = 0; k < hit; ++k)
    {
        if (tokens.at(k) != (quint8)CtrlReady)
            continue;

        ++m_readyCount;
        emitLog(QStringLiteral("固件就绪 #%1（55 77 AA）—— 板子已停在 waiting()，"
                               "现在可以点「发送整幅图」；装载完会自动下发 55 66 AA 启动")
                .arg(m_readyCount));
        emit firmwareReady(m_readyCount);
    }

    if (m_mode != ModeHandshake)
    {
        emitLog(QStringLiteral("[!] 当前是自由模式，请求已忽略（切到握手模式才会响应）"));
        return;
    }

    for (int k = 0; k < hit; ++k)
    {
        /* ---- 取配置：固件发 0x55 0x55 0xAA ---- */
        if (tokens.at(k) == (quint8)CtrlReqCfg)
        {
            ++m_cfgReqCount;
            /* ★ 不设 m_firstReqSeen：那个标志专指"固件要过像素数据"，
               用来决定握手超时表何时起，不能被配置请求误触发。
               （否则固件只要过配置、一直不要图，就会误报"握手超时"。） */
            emitLog(QStringLiteral("[>] 固件请求配置 #%1 → 下发 0x33 配置包"
                                   "（X=%2 Y=%3 速度=%4 原点=(%5,%6)）")
                    .arg(m_cfgReqCount).arg(m_cfgX).arg(m_cfgY).arg(m_cfgSpeed)
                    .arg(m_cfgXOrigin).arg(m_cfgYOrigin));
            emit configRequestReceived(m_cfgReqCount);
            /* 固件主动要的，此时不拦（哪怕是下发途中）—— 它自己知道在干什么 */
            writeConfigPacket();
            continue;
        }

        /* ---- 取数据：固件发 0x55 0x44 0xAA ---- */
        ++m_reqCount;
        m_firstReqSeen = true;

        /* ★★ 两条补货路径**并存**（不是互斥）—— 这是本方案的核心：
             (a) 固件主动发 55 44 AA（Send_Next）→ 上位机回一包
             (b) 固件每画满一包的点数（默认 64 条回传帧）→ 上位机自动补一包
           启动/追赶阶段靠 (a) 快速把缓冲区填起来；
           稳定雕刻阶段靠 (b) 按实际消耗速度背压补货。
           两条路都通向同一个 pumpOne()，谁先到谁先发。 */

        if (m_queue.isEmpty())
        {
            /* 固件来要，但上位机手里没货。两种可能：
               (a) 整幅图已发完，固件多要了；
               (b) 用户根本没点「发送整幅图」，队列从来没装过。
               都不发空包（发了会让它以为还有数据），但要说清是哪一种 ——
               这是"没反应"最常见的根因。 */
            emitLog(QStringLiteral("[!] 收到请求 #%1，但发送队列是空的，未下发任何数据！"
                                   "%2")
                    .arg(m_reqCount)
                    .arg(m_totalPkts == 0
                         ? QStringLiteral("（原因：还没点「发送整幅图」装载数据 → 请先点它）")
                         : QStringLiteral("（原因：整幅图已发完，固件多要了 %1 次）")
                               .arg(m_reqCount - m_sentPkts)));
            emit requestReceived(m_reqCount, 0);
            continue;
        }

        /* 真正写一包 —— 补货路径 (a)：固件发 55 44 AA 要，就给一个。 */
        pumpOne();
        ++m_hsSent;
        if (mayLog)
        {
            m_lastRxLogMs = now;
            emitLog(QStringLiteral("[>] 收到 0x55 0x44 0xAA #%1 → 补 1 包"
                                   "（路径 a：Next 请求；已收回传帧 %2 条）")
                    .arg(m_reqCount).arg(m_reportCount));
        }
        emit requestReceived(m_reqCount, m_queue.size());
    }

    /* 刚发完一包，给固件留出消化时间；哨兵等它下次来要。
       ★ 只在**队列里还有像素包**时才起表：
         队列空了就没什么可等的了（最后一个包在路上由 onBytesWritten 收尾）。
         这样"固件只来要配置、一直不要图"的场景不会被误判成握手超时。 */
    if (!m_queue.isEmpty())
    {
        if (m_hsTimeoutMs > 0)
            m_hsTimer->start(m_hsTimeoutMs);
    }
    else
    {
        m_hsTimer->stop();
        if (m_inFlight <= 0)
            finishHandshake(false);
    }
}

/**
 * 握手模式的收尾。
 *
 * 真正的"发完了"发生在**最后一包的最后一个字节写完之后** ——
 * 靠 onBytesWritten 把 m_inFlight 归零来触发，而不是 endImage()，
 * 因为 endImage() 被调用时队列里往往还剩一大截（固件在慢慢要）。
 */
void SerialIo::finishHandshake(bool aborted)
{
    if (m_finished)
        return;
    m_finished = true;

    m_hsTimer->stop();
    emitLog(QStringLiteral("[>] 握手下发完成：固件共请求 %1 次，下发 %2 包 / 预期 %3 包%4")
            .arg(m_reqCount).arg(m_sentPkts).arg(m_totalPkts)
            .arg(aborted ? QStringLiteral("（已中止）") : QString()));
    emit streamFinished(aborted);
}

void SerialIo::setPacketInterval(int ms)
{
    m_intervalMs = qMax(0, ms);
    if (m_timer->isActive() && (m_intervalMs > 0))
        m_timer->setInterval(m_intervalMs);
}

int SerialIo::packetInterval() const
{
    return m_intervalMs;
}

/**
 * 按"目标总时长"反推包间隔。
 *
 * 用途：一张 256x256 的图是 1024 包，6ms/包 要 6.1 秒 —— 这个节拍本身由
 * 固件决定（它按"出光/移动"时间消费像素），上位机不能自己拍脑袋，
 * 所以给个换算入口，让用户用"整幅图想跑多久"来定，比暴露"包间隔 ms"直观。
 */
void SerialIo::setIntervalByDuration(double seconds, int totalPackets)
{
    if ((seconds <= 0.0) || (totalPackets <= 0))
        return;
    const double ms = seconds * 1000.0 / (double)totalPackets;
    const int v = (int)(ms + 0.5);
    setPacketInterval(v);
    emitLog(QStringLiteral("[>] 按 %1 秒 / %2 包反推：包间隔 %3 ms")
            .arg(seconds, 0, 'f', 2).arg(totalPackets).arg(v));
    emit packetIntervalAutoChanged(v);
}

void SerialIo::setLogCallback(void (*fn)(void *ctx, const QString &line), void *ctx)
{
    m_logFn  = fn;
    m_logCtx = ctx;
}

void SerialIo::emitLog(const QString &line)
{
    if (m_logFn)
        m_logFn(m_logCtx, line);
}

/**
 * 串口物理层错误。
 *
 * ★ 这个处理器的价值：用户报"固件发了 0x55 0x44 0xAA，上位机没回"时，
 *   三种可能原因的区分就靠这几条日志：
 *     · 一条日志都没有，收字节数也是 0  → 字节根本没到（接线/端口选错/固件没发）
 *     · "收到 N 字节但不是请求帧"        → 字节到了，但内容不对（校验位/波特率错）
 *     · "校验位错误" (本函数)           → 直接坐实两端校验设置不一致
 *   没有这些线索，就只能瞎猜。
 */
void SerialIo::onPortError(int err)
{
    const QSerialPort::SerialPortError e = (QSerialPort::SerialPortError)err;
    if (e == QSerialPort::NoError)
        return;

    /* 校验/帧错误会持续刷，限流 */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool mayLog = (now - m_lastRxLogMs) > 500;

    switch (e)
    {
    case QSerialPort::ParityError:
        if (mayLog)
        {
            m_lastRxLogMs = now;
            emitLog(QStringLiteral("[!] 校验位错误！上位机当前配置是 %1，"
                                   "固件侧的校验位必须与之一致。"
                                   "不一致会导致从第 2 个字节起整体错位。")
                    .arg(cfgText()));
        }
        break;

    case QSerialPort::FramingError:
        if (mayLog)
        {
            m_lastRxLogMs = now;
            emitLog(QStringLiteral("[!] 帧错误：停止位或波特率与固件不一致"
                                   "（上位机 115200 / 8 / 奇 / 1）。"));
        }
        break;

    case QSerialPort::ResourceError:
        emit errorOccurred(QStringLiteral("串口异常：被拔出或不可用（%1）")
                           .arg(m_port->errorString()));
        m_state = QStringLiteral("串口异常掉线");
        m_timer->stop();
        m_hsTimer->stop();
        break;

    case QSerialPort::DeviceNotFoundError:
    case QSerialPort::PermissionError:
    case QSerialPort::OpenError:
        emit errorOccurred(QStringLiteral("串口错误：%1").arg(m_port->errorString()));
        break;

    default:
        /* 其余（TimeoutError 等）在双向通信里很常见，不值得刷屏 */
        break;
    }
}

/**
 * 队列降到低水位以下才返回。
 *
 * 为什么需要：整幅图若一次性灌进 QSerialPort，驱动内部缓冲会涨到几百 MB；
 * 而且中间一旦出错/中止，已经排好的包还在往外吐，现场就废了。
 * 用「高水位压住生产者」的经典做法把内存钉在 128KB 以内。
 *
 * ★ 握手模式的坑：这里等的是"队列被消耗"，而队列只由 0x44 驱动消耗。
 *   所以必须把 readyRead 事件喂进来（processEvents），否则固件的请求
 *   永远进不了 onReadyRead，队列水位下不去 → 死等。
 *   同时不能让哨兵在这期间误判超时，故等之前先把 hsTimer 停掉。
 */
void SerialIo::waitQueueDrain()
{
    const bool hs = isHandshake();
    if (hs)
        m_hsTimer->stop();

    while (m_port->isOpen() && !m_aborted && (pendingBytes() > kLowWater))
    {
        m_port->waitForBytesWritten(10);
        QCoreApplication::processEvents();
    }

    /* 出水后把哨兵续上，等固件下一次来要。
       ★ 必须加 m_firstReqSeen 这个门槛：本函数在 feedRow 里、**第一个请求到来之前**
       就会被调用（大图灌队列时）。若此时就起表，"用户还在按固件启动键"这几秒
       会被误判成握手超时。没收到过请求，等待就该是无限的。 */
    if (hs && m_firstReqSeen && !m_aborted && !m_queue.isEmpty() && (m_hsTimeoutMs > 0))
        m_hsTimer->start(m_hsTimeoutMs);
}

/* ============================ 流式推送 ============================ */

/**
 * 开一幅图。
 *
 * 行宽可任意 —— 帧里的 64 点是**帧内固定**的，跟一行多少点无关：
 *     · 行内连续的点按 64 切；最后一个不满 64 的包用 0x00 补齐并置 0x22
 *     · 因为 64 是 8/4/2/1 的公倍数，行边界必然正好落在包边界上，
 *       所以「一包恰好属于一行（或其行尾）」这个性质恒成立，
 *       固件只看控制位就能数行，不需要额外的换行包。
 * 行宽 ≤ 64 时，每行正好一包，行为与"换行单独一包"完全一致。
 */
void SerialIo::beginImage(int width, int height, bool bidi)
{
    m_imgW     = width;
    m_imgH     = height;
    m_bidi     = bidi;
    m_rowIndex = 0;
    m_sentRows = 0;
    m_sentPkts = 0;
    m_aborted  = false;
    m_totalPkts = 0;

    if ((width <= 0) || (height <= 0))
        return;

    const int pktsPerRow = (width + kPixPerPkt - 1) / kPixPerPkt;
    m_totalPkts = pktsPerRow * height;

    /* ★ 记下"一行有多少像素 / 占几包"，回显补货要按这个精确换算。
       固件画完一行 == 发 width 条回传帧 == 消耗 pktsPerRow 包。 */
    m_rowPixels  = width;
    m_rowPackets = pktsPerRow;
    m_echoAccum  = 0;

    /* 注意：m_cfgReqCount 不在这里重置 —— 配置交换是按"会话"计的，
       不属于某一幅图，重置了反而看不出"这台设备一共要过几次配置"。 */
    m_reqCount = 0;
    m_hsSent   = 0;
    m_finished = false;
    m_firstReqSeen = false;
    m_rxBuf.clear();
    m_hsTimer->stop();

    /* 换一幅图就把回传/补货基准一起归零：
       回传计数是按"第几个到达的点"累加的，跨图不清会让新图的补货错位。 */
    resetReportStats();

    emitLog(QStringLiteral("[>] 开始下发 %1 x %2，共 %3 行 / %4 包，模式=%5")
            .arg(width).arg(height).arg(height).arg(m_totalPkts)
            .arg(m_echoDriven
                 ? QStringLiteral("回显驱动(每行 %1 条回显 → 补 %2 包)")
                       .arg(m_rowPixels).arg(m_rowPackets)
                 : QStringLiteral("握手(等 0x55 0x44 0xAA)")));

    /* ★ 把换算比讲清楚 —— 这是判断"为什么图窄了会全白"的关键数字 */
    if (m_echoDriven && (m_rowPixels > 0))
    {
        const double naive = (double)m_rowPixels / (double)m_echoPerPacket;   /* 旧算法每行补多少 */
        const double lack  = (double)m_rowPackets - naive;                    /* 每行亏几包 */
        emitLog(QStringLiteral("[>] 补货换算：固件每行发 %1 条回传帧、消耗 %2 包"
                               "（若按固定 %3 条补 1 包，每行只能补 %4 包，"
                               "每行亏 %5 包 → 亏空 %6%）")
                .arg(m_rowPixels).arg(m_rowPackets).arg(m_echoPerPacket)
                .arg(naive, 0, 'f', 2)
                .arg(lack, 0, 'f', 2)
                .arg((lack / (double)m_rowPackets) * 100.0, 0, 'f', 1));
    }
}

/**
 * 喂一整行灰度（scanLine 长度至少 width 字节）。
 *
 * 弓字形在这里生效：奇数行倒序 —— 固件按"数据顺序 = 运动顺序"切激光强度，
 * 来回扫描时奇数行物理上是从右往左走的，不在这里倒序整幅图就会拉花。
 */
void SerialIo::feedRow(const uchar *scanLine, int width)
{
    if (!m_port->isOpen() || !scanLine || (width <= 0) || m_aborted)
        return;
    if (m_rowIndex >= m_imgH)
        return;

    const bool rev = m_bidi && ((m_rowIndex & 1) != 0);
    const int  pktsPerRow = (width + kPixPerPkt - 1) / kPixPerPkt;

    int col = 0;
    for (int p = 0; p < pktsPerRow; ++p)
    {
        QByteArray chunk;
        chunk.reserve(kPixPerPkt);

        const bool lastPkt = (p == pktsPerRow - 1);
        const int  take    = lastPkt ? (width - col) : kPixPerPkt;

        for (int k = 0; k < take; ++k)
        {
            const int c = col + k;
            const int sx = rev ? (width - 1 - c) : c;   /* 按扫描顺序取源列 */
            chunk.append((char)scanLine[sx]);
        }
        /* 行尾不足 64：用 0x00 补齐。
           0x00 = 完全不出光，补在行尾等于空走 64-remain 个点，
           不会在纸上留痕，也不会把"不出光"和"出光0"混淆。 */
        while (chunk.size() < kPixPerPkt)
            chunk.append((char)0x00);

        /* 一行里的最后那个包 = 行边界 → 0x22；行内的包 → 0x11。
           行宽恰好是 64 的整数倍时，每行最后一包也是满的，
           此时 0x22 是**唯一**的行边界信号 —— 固件必须靠它推进 y。 */
        m_queue.enqueue(buildPacket(lastPkt ? (quint8)CtrlNewline
                                           : (quint8)CtrlData, chunk));

        col += take;

        kickTimerIfNeeded();
        if ((p & 0x0F) == 0x0F)
            waitQueueDrain();
    }

    ++m_rowIndex;
    if ((m_rowIndex & 0x0F) == 0)
        m_port->waitForBytesWritten(10);
}

/** 结束一幅图：排空队列，触发完成信号。 */
void SerialIo::endImage()
{
    if (!m_port->isOpen())
        return;

    const bool aborted = m_aborted;

    if (isHandshake() && !aborted)
    {
        /* 握手模式：活干完没有不由上位机说了算 —— 队列只在固件来要时才减。
           这里不能"等队列空"（固件跑飞就死等），只把手上这一包冲完就交差，
           剩下的交给 0x44 继续推。真正的结束判据是队列空 + 无在写。 */
        m_port->waitForBytesWritten(200);
        QCoreApplication::processEvents();

        if (!m_queue.isEmpty())
        {
            emitLog(QStringLiteral("[>] 数据已装载 %1 包，现在等固件发 0x55 0x44 0xAA 来取")
                    .arg(m_queue.size()));
            emitLog(QStringLiteral("[!] 注意：此时上位机不会主动发任何东西，"
                                   "固件不发请求就一直等（无超时）"));
            /* ★ 这里**故意不启动** hsTimer：
               用户很可能先点了发送、再去按固件那边启动，中间几秒很正常。
               等第一个请求真的来了（说明固件活着且在说话），再开始掐表。 */
            return;                       /* 不 emit streamFinished，等取完 */
        }
    }
    else
    {
        while (m_port->isOpen() && !m_aborted
               && ((!m_queue.isEmpty()) || (m_inFlight > 0)))
        {
            m_port->waitForBytesWritten(100);
            QCoreApplication::processEvents();
        }
    }

    /* 队列已空 + 无在写 → 真的发完了 */
    m_hsTimer->stop();
    emitLog(QStringLiteral("[>] 下发结束：已排入 %1 包 / 预期 %2 包%3")
            .arg(m_sentPkts).arg(m_totalPkts)
            .arg(aborted ? QStringLiteral("（已中止）") : QString()));
    emit streamFinished(aborted);
}

/** 一次性推完（小图方便用；大图请 beginImage + feedRow 逐行推，UI 才有机会刷新）。 */
void SerialIo::startStream(const QImage &gray, int rowWidth, bool bidi)
{
    if (!m_port->isOpen())
    {
        emit errorOccurred(QStringLiteral("串口未打开，无法下发图像"));
        return;
    }
    if (gray.isNull() || (rowWidth <= 0))
    {
        emit errorOccurred(QStringLiteral("灰度图或行宽无效"));
        return;
    }

    beginImage(gray.width(), gray.height(), bidi);
    for (int y = 0; y < gray.height(); ++y)
    {
        if (m_aborted || !m_port->isOpen())
            break;
        feedRow(gray.constScanLine(y), gray.width());
    }
    endImage();
}

void SerialIo::stopStream()
{
    if (m_timer->isActive())
        m_timer->stop();
    m_hsTimer->stop();
    emitLog(QStringLiteral("[>] 已停止发送（队列剩余 %1 包）").arg(m_queue.size()));
}

void SerialIo::abortStream()
{
    m_aborted = true;
    if (m_timer->isActive())
        m_timer->stop();
    m_hsTimer->stop();
    m_queue.clear();
    m_inFlight  = 0;
    m_rxBuf.clear();
    m_rowIndex  = 0;
    m_sentRows  = 0;
    m_sentPkts  = 0;
    m_totalPkts = 0;
    m_reqCount  = 0;
    m_hsSent    = 0;
    m_finished  = true;              /* 阻止 onBytesWritten 再触发完成 */
    emitLog(QStringLiteral("[>] 已中止并清空队列"));
    emit streamFinished(true);
}

bool SerialIo::isStreaming() const
{
    if (isHandshake())
    {
        /* 握手模式：队列里还有货 或 有包在路上，就算"还在发" */
        return (!m_queue.isEmpty() || (m_inFlight > 0)) && !m_finished;
    }
    return (m_timer->isActive() && !m_queue.isEmpty());
}

/* ============================ 节拍 ============================ */

/** 只负责"有活干才让定时器跑"这一个判断，避免握手模式下空转刷包。 */
void SerialIo::kickTimerIfNeeded()
{
    if (!m_port->isOpen())
        return;
    if (m_queue.isEmpty())
        return;
    /* 握手模式：节拍由 0x44 驱动，这里绝不自己启动定时器 */
    if (isHandshake())
        return;
    if (!m_timer->isActive())
        m_timer->start(qMax(1, m_intervalMs));
}

/**
 * 从队列摘一个包真正写出去。返回是否写了。
 *
 * ⚠ 顺手修掉一个隐患：QSerialPort::write() 可能只写进去一部分，
 * 老代码把"期望写的长度"整个记进 m_inFlight，会导致这笔账永远对不平
 * （bytesWritten 只报实际写出的量）。这里按实际返回值记账。
 */
bool SerialIo::pumpOne()
{
    if (!m_port->isOpen() || m_queue.isEmpty())
        return false;

    const QByteArray pkt = m_queue.dequeue();
    const qint64 n = m_port->write(pkt);
    if (n <= 0)
    {
        emit errorOccurred(QStringLiteral("写入失败：%1").arg(m_port->errorString()));
        return false;
    }
    if (n < pkt.size())
        emitLog(QStringLiteral("[!] 单次写入不完整：%1/%2 字节").arg(n).arg(pkt.size()));
    m_inFlight += n;

    const quint8  ctrl = (pkt.size() >= 2)  ? (quint8)pkt.at(1)  : 0;
    const quint16 crc  = (pkt.size() >= 68) ? (quint16)(((quint8)pkt.at(66) << 8)
                                                       | (quint8)pkt.at(67)) : 0;
    ++m_sentPkts;
    if (ctrl == (quint8)CtrlNewline)
        ++m_sentRows;

    m_lastWriteMs = QDateTime::currentMSecsSinceEpoch();

    emit packetSent((int)ctrl, kPixPerPkt, crc, pendingBytes());
    emit streamProgress(m_sentPkts, m_totalPkts, m_sentRows, m_imgH);
    return true;
}

void SerialIo::onTimerTick()
{
    if (!m_port->isOpen())
    {
        m_timer->stop();
        return;
    }

    /* 握手模式：定时器完全不该跑，跑到这里说明状态错乱，直接停 */
    if (isHandshake())
    {
        m_timer->stop();
        return;
    }

    /* 正在写的还没吐完就先等等，避免 QSerialPort 内部缓冲膨胀 */
    if (m_inFlight > (qint64)kPktBytes)
        return;

    if (m_queue.isEmpty())
    {
        m_timer->stop();
        emitLog(QStringLiteral("[>] 队列已发空，共 %1 包").arg(m_sentPkts));
        emit streamProgress(m_sentPkts, m_totalPkts, m_sentRows, m_imgH);
        emit streamFinished(false);
        return;
    }

    pumpOne();
}

void SerialIo::onBytesWritten(qint64 n)
{
    m_inFlight -= n;
    if (m_inFlight < 0)
        m_inFlight = 0;

    /* 握手模式：最后一包吐完 = 整幅图真的发完了 */
    if (isHandshake() && !m_finished && m_queue.isEmpty() && (m_inFlight <= 0)
        && (m_sentPkts > 0) && (m_totalPkts > 0) && (m_sentPkts >= m_totalPkts))
    {
        finishHandshake(false);
    }
}
