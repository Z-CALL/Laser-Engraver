#include "serialio.h"

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QImage>
#include <QCoreApplication>
#include <QDateTime>

/* ============================ 帧结构常量 ============================ */
static const quint8  kHead      = 0x55;   /* 帧头 */
static const quint8  kTail      = 0xAA;   /* 帧尾 */
static const int     kPixPerPkt = 64;     /* 每包固定 64 个像素点 */
static const int     kPktBytes  = 69;     /* 1 + 1 + 64 + 2 + 1 = 69 */

/* 队列水位：别一次把几十 MB 塞进 QSerialPort 的内部缓冲 */
static const qint64  kHighWater = 128 * 1024;
static const qint64  kLowWater  =  16 * 1024;

/* 默认 6 ms/包 ≈ 69 字节 / 115200bps(奇校验 11.5 bit/字节) ≈ 6.9ms 线时，
   6ms 基本把串口跑满而不会在应用层堆积。固件消化不过来就往上调。 */
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
    m_hsSent(0),
    m_hsTimeoutMs(kDefaultHsTimeoutMs),
    m_finished(false),
    m_lastWriteMs(0),
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

    /* 115200 / 8 数据位 / 奇校验 / 1 停止位 / 无流控 —— 用户指定，不做自适应 */
    if (!m_port->open(QIODevice::ReadWrite))
    {
        m_state = QStringLiteral("打开失败：%1").arg(m_port->errorString());
        emit errorOccurred(m_state);
        emitLog(QStringLiteral("[!] ") + m_state);
        return false;
    }
    if (!m_port->setBaudRate(QSerialPort::Baud115200))
    {
        m_state = QStringLiteral("设置波特率失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setDataBits(QSerialPort::Data8))
    {
        m_state = QStringLiteral("设置数据位失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setParity(QSerialPort::OddParity))
    {
        m_state = QStringLiteral("设置奇校验失败：%1").arg(m_port->errorString());
        m_port->close();
        emit errorOccurred(m_state);
        return false;
    }
    if (!m_port->setStopBits(QSerialPort::OneStop))
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
    m_hsSent     = 0;
    m_finished   = false;
    m_queue.clear();
    m_rxBuf.clear();
    m_hsTimer->stop();

    m_state = QStringLiteral("已打开 %1 (115200 8-O-1)").arg(portName);
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

bool SerialIo::sendConfig(const QByteArray &cfg)
{
    /* 控制位 0x33：单独一个数据包，本版本按需求暂不实现 */
    Q_UNUSED(cfg);
    emitLog(QStringLiteral("[!] 控制位 0x33（下发配置）暂未实现，已忽略"));
    return false;
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

QByteArray SerialIo::reqNextFrame()
{
    QByteArray f;
    f.append((char)0x55);
    f.append((char)CtrlReqNext);
    f.append((char)0xAA);
    return f;
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
    emitLog(QStringLiteral("[!] 握手超时：%1 ms 没收到 0x55 0x44 0xAA，剩 %2 包未发")
            .arg(m_hsTimeoutMs).arg(left));
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

    m_rxBuf.append(chunk);

    int hit = 0;
    while (true)
    {
        const int i = m_rxBuf.indexOf(reqNextFrame());
        if (i < 0)
            break;
        ++hit;
        /* 从匹配处之后继续找；同一批数据里可以塞多个请求 */
        m_rxBuf.remove(0, i + 3);
    }

    /* 防膨胀：缓冲里剩下的最多只有可能是帧的前缀（<3 字节），
       超过 3 字节说明收到的是垃圾/下行回显，直接丢掉。 */
    if (m_rxBuf.size() > 2)
        m_rxBuf = m_rxBuf.right(2);

    if (hit <= 0)
        return;

    if (m_mode != ModeHandshake)
    {
        emitLog(QStringLiteral("[!] 收到 %1 个 0x55 0x44 0xAA，但当前是自由模式，已忽略").arg(hit));
        return;
    }

    for (int k = 0; k < hit; ++k)
    {
        ++m_reqCount;

        if (m_queue.isEmpty())
        {
            /* 固件要得比图还多：图发完了它还在要。
               不发空包 —— 发了会让它以为还有数据。只提示。 */
            emitLog(QStringLiteral("[!] 收到请求 #%1 但队列已空（整幅图已发完），未发送")
                    .arg(m_reqCount));
            emit requestReceived(m_reqCount, 0);
            continue;
        }

        /* 真正写一包。这就是"收到一个才发一个"的落点。 */
        pumpOne();
        ++m_hsSent;
        emit requestReceived(m_reqCount, m_queue.size());
    }

    /* 刚发完一包，给固件留出消化时间；哨兵等它下次来要 */
    if (!m_queue.isEmpty() || (m_inFlight > 0))
    {
        if (m_hsTimeoutMs > 0)
            m_hsTimer->start(m_hsTimeoutMs);
    }
    else
    {
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

    /* 出水后把哨兵续上，等固件下一次来要 */
    if (hs && !m_aborted && !m_queue.isEmpty() && (m_hsTimeoutMs > 0))
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

    m_reqCount = 0;
    m_hsSent   = 0;
    m_finished = false;
    m_rxBuf.clear();
    m_hsTimer->stop();

    emitLog(QStringLiteral("[>] 开始下发 %1 x %2，共 %3 行 / %4 包，模式=%5")
            .arg(width).arg(height).arg(height).arg(m_totalPkts)
            .arg(isHandshake() ? QStringLiteral("握手(等 0x55 0x44 0xAA)")
                               : QStringLiteral("自由(包间隔 %1ms)").arg(m_intervalMs)));
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
            emitLog(QStringLiteral("[>] 已排入 %1 包 / 预期 %2 包，剩余 %3 包等固件 0x44 来取")
                    .arg(m_sentPkts).arg(m_totalPkts).arg(m_queue.size()));
            if (m_hsTimeoutMs > 0)
                m_hsTimer->start(m_hsTimeoutMs);
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
