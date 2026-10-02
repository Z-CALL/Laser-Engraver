#ifndef SERIALIO_H
#define SERIALIO_H

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

class QSerialPort;
class QTimer;
class QImage;

/**
 * 串口下发模块（默认 115200 / 8 数据位 / 无校验 / 1 停止位）
 *
 * ============================ 数据包格式 ============================
 *     0x55 | 控制位 | 64 字节像素灰度 | CRC16 高字节 | CRC16 低字节 | 0xAA
 *     └帧头┘ └ 1 B ┘ └──── 64 B ────┘ └──────── 2 B ────────┘ └帧尾┘
 *                     合计 69 字节
 *
 * 控制位：
 *     0x11  数据    —— 帧内 64 个像素点，不换行
 *     0x22  换行    —— 帧内 64 个像素点属于**新的一行**（行尾自动补 0x00）
 *     0x33  下发配置 —— 单独一个数据包（本版本未实现）
 *
 * CRC：CRC16-CCITT-FALSE（poly 0x1021, init 0xFFFF, 无反转, 无异或输出），
 *      校验范围 = 控制位 + 64 像素 = 65 字节，发大端（高字节在前）。
 *
 * ---------------------------- 关键设计决定 ----------------------------
 * **0x22 只是标记，换行本身不会多传一个包。**
 * 64 是 8/4/2/1 的公倍数，只要按"行内连续"填充，行边界必然落在包边界上，
 * 所以「一包 = 一行的某一段」恒成立，换行信息由控制位携带就够，无须额外字节。
 * 若真的给换行再发一个空包，固件反而要把"收到 0x22 → 推进 y"和
 * "收到 0x22 → 这 64 点属于新行"两条语义叠加，容易重影。
 *
 * 行宽不是 64 的整数倍时，行尾用 0x00 补齐到整包（0x00 是"完全不出光"，
 * 补在行尾 = 空走一段，不会在纸上留痕）。
 *
 * ============================ 两种发送模式 ============================
 *
 * 【自由模式】free run —— 上位机按 setPacketInterval() 自己定节拍往外吐。
 *   简单，但不看固件脸色：固件消化不过来就在缓冲区里堆包，最后丢包/错位。
 *
 * 【握手模式】handshake —— **固件来要一个，上位机才发一个**。
 *   固件发上行帧     0x55 0x44 0xAA      （3 字节，无 CRC，无像素）
 *   上位机收到就下发 一个完整数据包       （69 字节）
 *   一个 0x44 只换一个数据包，不多发一个字节。
 *
 *   ★ 为什么握手模式对雕刻机更靠谱：
 *     固件每啃完一包（出完 64 点的光 + 走完这 64 点的位移）才来要下一包，
 *     节奏完全由固件的物理动作决定，上位机不可能灌爆它的缓冲。
 *     自由模式下"包间隔该设多少"是个猜谜：设小了丢包、设大了干等；
 *     握手模式下这个参数根本不需要。
 *
 *   ⚠ 注意 0x55 0x44 0xAA 与下行包格式的**区别**：
 *     下行包是 69 字节且带 CRC；上行请求只有 3 字节且无 CRC。
 *     两者都以 0x55 开头、0xAA 结尾，所以解析上行帧时**必须按长度 3 来判**，
 *     不能只靠首尾字节 —— 否则下行包自己的回显/环回会被误认成请求。
 *     见 m_rxBuf 的解析逻辑。
 *
 *   ⚠ 死锁风险：握手下若固件不再发 0x44（跑飞、断线、复位），上位机会一直等。
 *     用 setHandshakeTimeout() 兜底，超时后报错停发，别让界面无声卡住。
 *     ★ 超时表**只在收到第一个请求之后**才开始计（m_firstReqSeen）——
 *       否则"点完发送、人还没去按固件启动"这 3 秒就会误报超时。
 *       等第一个请求之前，等待无限长是正确行为。
 *
 *   ⚠ 队列必须**先装载**：握手模式下的数据来源是 m_queue，而 m_queue 只在
 *     调用 beginImage/feedRow（界面上是「发送整幅图」）时才被填。
 *     光打开串口不装队列，固件来要也没东西可发 —— 这是最常见的"没反应"原因。
 *
 *   ⚠⚠ 校验位必须两端一致！**默认用无校验 8N1**，因为实测固件就是按 8N1 发的。
 *
 *     踩过的坑（留档，因为这类错位极难自行看出）：
 *     最初按需求配了 8O1，而固件实际发 8N1，结果是
 *     收端把"停止位"当成"校验位"、再把下一帧"起始位"当成"停止位" →
 *     帧错误 → 重新找下降沿时晚一个 bit 才同步 → **从第 2 个字节起全部错位**。
 *     实测 0x55 0x44 0xAA 被解成 0x55 0x94 0xFD。
 *     因为 0x55/0x44/0xAA 的 1 的个数都是偶数，奇校验位恰好=停止位=1，
 *     所以**第 1 个字节既不错位也不报校验错**，极具迷惑性。
 *     判据：第 1 字节对、后面全乱 → 几乎一定是校验位/帧格式不匹配。
 *
 *     → 用 setPortParams() 可改；界面上有下拉框。
 *       若哪天要改回奇校验，**MCU 侧必须同步改成奇校验**，否则又是这个现象。
 */
class SerialIo : public QObject
{
    Q_OBJECT

public:
    /* 控制位 */
    enum Ctrl {
        CtrlData = 0x11,    /* 数据 */
        CtrlNewline = 0x22, /* 换行（帧内 64 点属于新行） */
        CtrlConfig = 0x33,  /* 下发配置（预留，未实现） */
        CtrlReqNext = 0x44  /* 上行：固件请求下一包（只出现在 0x55 0x44 0xAA 里） */
    };

    /* 校验位（对应界面下拉框，避免在头文件里引入 QSerialPort） */
    enum Parity { ParityNone = 0, ParityOdd = 1, ParityEven = 2 };

    /* 发送模式 */
    enum Mode {
        ModeFree = 0,       /* 自由模式：按包间隔自己吐 */
        ModeHandshake = 1   /* 握手模式：收到 0x55 0x44 0xAA 才发一包 */
    };

    explicit SerialIo(QObject *parent = 0);
    ~SerialIo();

    /* 按 "串口名|描述" 列表枚举可用端口 */
    static QStringList availablePorts();

    bool isOpen() const;

    /* 打开串口；失败原因通过 errorString() 取 */
    bool open(const QString &portName);
    void close();

    /* 串口参数。默认 = 115200 / 8 数据位 / 无校验 / 1 停止位（与固件实测一致）。
       必须在 open() 之前调用才会生效；打开后修改请重新打开。 */
    void setPortParams(int baud, int dataBits, int parity, int stopBits);
    int  cfgBaud() const;
    int  cfgDataBits() const;
    int  cfgParity() const;       /* ParityNone / ParityOdd / ParityEven */
    int  cfgStopBits() const;     /* 0=1位 1=1.5位 2=2位 */
    QString cfgText() const;      /* 如 "115200 8-N-1"，用于显示与日志 */

    QString portName() const;
    QString errorString() const;
    QString stateText() const;

    /* 一包的字节数：1+1+64+2+1 = 69 */
    static int packetBytes();

    /* 发一包；必须 64 字节。返回 false = 参数非法或串口未开 */
    bool sendPacket(quint8 ctrl, const QByteArray &pixels64);

    /* 换行：常驻队列里已经有 64 点待发 → 原地把控制位改成 0x22（零浪费）；
       否则补发一个全 0x00 的 0x22 空包（让固件立即推进 y）。 */
    bool sendNewline();

    /* 下发配置（预留） */
    bool sendConfig(const QByteArray &cfg);

    /* ---- 流式推送 ---- */
    /* 开一幅图：记录尺寸/行号，弓字形倒序在这里生效 */
    void beginImage(int width, int height, bool bidi);
    /* 喂一整行（灰度字节，长度 = width）；内部按 64 切包、行尾补 0x00 + 0x22 */
    void feedRow(const uchar *scanLine, int width);
    /* 结束一幅图：排空队列 */
    void endImage();

    /* 一次性把整幅图推完（小图用；大图请用 beginImage/feedRow） */
    void startStream(const QImage &gray, int rowWidth, bool bidi);

    void stopStream();                  /* 停节拍，保留队列 */
    void abortStream();                 /* 停 + 清队列 */
    bool isStreaming() const;

    /* 队列里还没发出去的字节数（用于流控） */
    int pendingBytes() const;
    /* 队列里还没发出去的**包数**（握手模式下就是"固件还欠我几个 0x44"） */
    int pendingPackets() const;

    /* ---- 发送模式 ---- */
    void setMode(Mode m);
    Mode mode() const;
    bool isHandshake() const;

    /* 握手模式：固件每来一个 0x55 0x44 0xAA 就下发一个数据包 */
    static QByteArray reqNextFrame();   /* 返回 {0x55,0x44,0xAA}，方便打印/自检 */

    /* 握手超时（毫秒）：等待 0x44 超过这个时间没等到就报错停发。
       0 = 不超时（一直等）。默认 3000。 */
    void setHandshakeTimeout(int ms);
    int  handshakeTimeout() const;

    /* 已收到的请求帧计数 / 已因握手发出的包数 */
    int requestCount() const;
    int handshakeSentCount() const;

    /* 发送节流：包间隔毫秒；0 = 不限速（仅用于本机虚拟串口联调）。
       ⚠ 握手模式下这个值只作为"两个响应之间最小间隔"的保险丝，
       真正的节拍由固件决定。 */
    void setPacketInterval(int ms);
    int  packetInterval() const;
    /* 依据"目标总时长(秒)"反推包间隔，用于把整幅图的耗时压在预期内 */
    void setIntervalByDuration(double seconds, int totalPackets);

    /* 日志回调：把每包摘要吐给界面 */
    void setLogCallback(void (*fn)(void *ctx, const QString &line), void *ctx);

signals:
    void opened(const QString &portName);
    void closed();
    void errorOccurred(const QString &msg);
    void packetSent(int ctrl, int pixelCount, quint16 crc, int bytesInFlight);
    void streamProgress(int sentPackets, int totalPackets, int sentRows, int totalRows);
    void packetIntervalAutoChanged(int ms);   /* setIntervalByDuration 反推结果 */
    void streamFinished(bool aborted);

    /* 握手：收到固件请求 / 等待请求超时 */
    void requestReceived(int seq, int remainingPackets);
    void handshakeTimedOut(int waitedMs, int remainingPackets);
    void modeChanged(int mode);
    /* 收到任意上行字节（用于界面显示"我到底听到没有"） */
    void rxActivity(qint64 totalBytes);

private slots:
    void onTimerTick();
    void onBytesWritten(qint64 n);
    void onReadyRead();
    void onHandshakeTimeout();

private:
    QByteArray buildPacket(quint8 ctrl, const QByteArray &pixels64) const;
    void emitLog(const QString &line);
    /* 串口层面的错误（校验位错/帧错/掉线）——诊断"字节到了但解不出来"的关键 */
    void onPortError(int err);
    /* 队列降到低水位以下才返回，避免一次性把整幅图灌进驱动 */
    void waitQueueDrain();
    /* 真的有包要发、且没有被握手卡住时，才启动定时器 */
    void kickTimerIfNeeded();
    /* 从队列摘一包真正写出去；返回是否写了 */
    bool pumpOne();
    /* 握手模式收尾（幂等） */
    void finishHandshake(bool aborted);

    QSerialPort *m_port;
    QTimer      *m_timer;
    QTimer      *m_hsTimer;
    QString      m_state;

    QQueue<QByteArray> m_queue;   /* 已封装好的整包 FIFO */
    qint64       m_inFlight;      /* 已交给驱动、尚未写完的字节数 */
    int          m_intervalMs;
    int          m_totalPkts;     /* 整幅图预期包数，仅用于进度显示 */
    int          m_sentPkts;

    /* 发送模式 */
    Mode         m_mode;
    QByteArray   m_rxBuf;         /* 上行字节累积缓冲（3 字节滑动窗） */
    int          m_reqCount;      /* 收到的 0x44 请求总数 */
    int          m_hsSent;        /* 握手模式下已下发的包数 */
    int          m_hsTimeoutMs;
    bool         m_finished;      /* 本幅图已完成标记，防重复 emit */
    qint64       m_lastWriteMs;   /* 上一次真正写串口的时刻，用于最小间隔保险 */
    qint64       m_rxTotal;       /* 自打开以来收到的上行字节总数 */
    bool         m_firstReqSeen;  /* 是否已收到过第一个 0x44 请求（决定超时表何时起） */
    qint64       m_lastRxLogMs;   /* 上行日志限流，防止垃圾流刷爆日志窗口 */

    /* 串口参数（默认按需求：115200 / 8 / 奇 / 1） */
    int          m_cfgBaud;
    int          m_cfgDataBits;
    int          m_cfgParity;
    int          m_cfgStopBits;

    /* 图像级状态（beginImage 时重置） */
    int          m_imgW;
    int          m_imgH;
    bool         m_bidi;
    int          m_rowIndex;      /* 当前正在喂的行号，0 起 */
    int          m_sentRows;
    bool         m_aborted;

    void (*m_logFn)(void *ctx, const QString &line);
    void        *m_logCtx;
};

#endif // SERIALIO_H
