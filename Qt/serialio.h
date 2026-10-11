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
 *     0x11  数据    —— 本包是**行内**的一段；画完继续等下一包
 *     0x22  换行    —— 本包是**这一行的最后一包**；画完这 64 点后 y++、换到下一行
 *     0x33  下发配置 —— 独立的一次性数据包（格式见下方「配置包格式」）
 *
 * ★ 0x22 的确切语义（固件必须按这个实现，否则整幅图会错行）
 *   标记挂在**行尾包**上，包内的 64 点属于「刚刚结束的这一行」，**不是新的一行**。
 *   所以固件的动作顺序是：**先画完这 64 点，再 y++**。
 *
 *   行宽 128（每行 2 包）时：
 *     0x11(第0行 px0-63) 0x22(第0行 px64-127) 0x11(第1行 px0-63) 0x22(第1行 px64-127)
 *
 *   行宽 64（每行恰好 1 包，即本工程默认配置）退化成最简形式：
 *     0x22(第0行 px0-63) 0x22(第1行 px0-63) 0x22(第2行 px0-63) ...
 *     → 每个 0x22 包正好是一整行，固件「画完 y++」即可，不用关心"新行旧行"。
 *
 * ======================= 配置包格式（控制位 0x33）=======================
 * 骨架与数据包**完全相同**（69 字节，CRC 覆盖同一段 65 字节），只是载荷换了内容：
 *
 *   偏移  长度  内容
 *     0     1   0x55           帧头
 *     1     1   0x33           控制位 = 下发配置
 *     2     1   0x44           X 字段标签
 *     3-4   2   X轴一行像素点   （= 图像宽；大端，高字节在前）
 *     5     1   0x55           Y 字段标签
 *     6-7   2   Y轴一列像素点   （= 图像高；大端，高字节在前）
 *     8     1   0x66           速度字段标签
 *     9     1   加工速度         （1 字节，单位由固件定义）
 *    10-65  56  0xFF 填充        （预留位，用 0xFF 占满）
 *    66-67   2  CRC16           （大端）
 *    68     1   0xAA           帧尾
 *
 *   合计 1+1+1+2+1+2+1+1+56+2+1 = 69 ✓
 *
 * ★ CRC 范围 = 偏移 1..65 共 65 字节（控制位 + 整个载荷），
 *   与数据包**同一套算法、同一段范围** → 固件可直接复用同一个校验函数。
 *
 * ★ 0x44 / 0x55 / 0x66 是三个**字段标签**（X / Y / 速度各带一个），
 *   不是数据。注意 0x55 在这里是标签，与帧头的 0x55 含义不同 ——
 *   解析时必须按**固定偏移**取字段，不要靠扫 0x55 定位。
 *
 * ★ 配置包不走握手、不进队列：它是独立的一次性命令，直接写串口。
 *   图像下发途中会拒发，免得插进像素流里把固件的状态机搞乱。
 *
 * CRC：CRC16-CCITT-FALSE（poly 0x1021, init 0xFFFF, 无反转, 无异或输出），
 *      校验范围 = 控制位 + 64 像素 = 65 字节，发大端（高字节在前）。
 *
 * ---------------------------- 关键设计决定 ----------------------------
 * **0x22 只是标记，换行本身不会多传一个包。**
 * 64 是 8/4/2/1 的公倍数，只要按"行内连续"填充，行边界必然落在包边界上，
 * 所以「一包 = 一行的某一段」恒成立，换行信息由控制位携带就够，无须额外字节。
 * 若真的给换行再发一个空包，固件就要把"收到 0x22 → 推进 y"和
 * "收到 0x22 → 这 64 点属于新行"两条语义叠加，容易重影 —— 所以这里
 * 只保留**一条**语义：0x22 = 本行到头了。
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
        CtrlInitTimeout = 0x10, /* 上行：固件接收超时上报（帧 55 10 AA）
                                   对应固件的 BT_INIT_TIMEOUT */
        CtrlData = 0x11,    /* 下行：数据（行内的一段） */
        CtrlNewline = 0x22, /* 下行：换行（本包是这一行的最后一包，包内 64 点属本行行尾） */
        CtrlConfig = 0x33,  /* 下行：下发配置 */
        CtrlReqData = 0x44, /* 上行：请求下一包像素数据（帧 55 44 AA） */
        CtrlReqCfg  = 0x55, /* 上行：请求下发配置（帧 55 55 AA） */
        CtrlStart   = 0x66, /* 下行：命令固件开始取数据（帧 55 66 AA） */
        CtrlReady   = 0x77  /* 上行：固件就绪通知（帧 55 77 AA） */
    };

    /* ==================== 雕刻回传（固件 → 上位机） ====================
       固件每画完一个像素回传一条，上位机实时画到监看画布上。

         偏移  长度  内容
           0     1   0x33          帧头
           1     1   出光灰度值     （固件实际用的那个值，0~255）
           2     1   0xEE          帧尾

       ★ 为什么必须改解析方式（本文件最关键的一处改动）：
         请求帧是 55 xx AA（以 0x55 开头），回传帧是 33 gg EE（以 0x33 开头）。
         老解析器"在整个缓冲里搜 55 xx AA"，而回传帧的灰度字节是 0~255 任意值，
         连发的回传帧**跨帧拼凑**时很容易凑出 "55 44 AA" 这种假请求帧，
         会让上位机多发数据包 → 握手错乱 → 雕刻中途卡死。
         所以改成"按首字节分派 + 固定长度整体消费"的状态机。

       ★ 帧里不带坐标，只带灰度值 —— 上位机按"第几个到达"依次落点。
         这恰好是最有价值的监控视角：画出来的是**固件实际的输出顺序**，
         固件若重复/跳行/错位，画面上会立刻显形。 */
    enum { ReportFrameBytes = 3, ReportHead = 0x33, ReportTail = 0xEE };

    /* 回传统计：收到多少条、丢弃多少条（帧尾不符） */
    int reportCount() const;
    int reportDropped() const;
    void resetReportStats();

    /* 回传监看开关：关掉后解析器不再尝试解析回传帧 */
    void setReportWatch(bool on);
    bool reportWatch() const;

    /* 组装一条回传帧（用于上位机自检 / 模拟发端） */
    static QByteArray buildReportFrame(quint8 gray);

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

    /* ---- 下发配置（控制位 0x33）----
       两种触发方式：
         (a) 固件发 0x55 0x55 0xAA 来要 —— 握手模式下的正常路径
         (b) 界面上点「下发配置」—— 手动发，调试用
       打包用的是 setConfigValues() 存下来的 X/Y/速度。 */
    bool sendConfigPacket(int xPixels, int yPixels, int speed);

    /* 配置包的三个字段值（界面改动时同步过来；固件来要时用这组值打包） */
    void setConfigValues(int xPixels, int yPixels, int speed,
                         int xOrigin, int yOrigin);
    int  cfgXPixels() const;
    int  cfgYPixels() const;
    int  cfgSpeed() const;
    int  cfgXOrigin() const;      /* 作业原点 X（像素，2 字节大端） */
    int  cfgYOrigin() const;      /* 作业原点 Y（像素，2 字节大端） */

    /* 生成配置包的 64 字节载荷（不含帧头/控制位/CRC/帧尾）。
       发送与「仅打包」预览共用，保证两处字节完全一致。静态，便于预览复用。 */
    static QByteArray buildConfigPayload(int xPixels, int yPixels, int speed,
                                         int xOrigin, int yOrigin);

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

    /* ---- 握手请求帧（固件 → 上位机，均 3 字节，无 CRC）----
       0x55 0x44 0xAA   请求下一包像素数据 → 上位机回一个数据包（0x11 或 0x22）
       0x55 0x55 0xAA   请求下发配置     → 上位机回一个配置包（0x33）

       两者都是 0x55 开头、0xAA 结尾，只差中间那个字节，
       所以解析必须按 **3 字节精确匹配**（见 onReadyRead），
       不能用"见了 0xAA 就收"的宽松扫描 —— 否则下行包的回显会被误判。 */
    static QByteArray reqDataFrame();    /* {0x55, 0x44, 0xAA} */
    static QByteArray reqConfigFrame();  /* {0x55, 0x55, 0xAA} */

    /* ---- 固件错误上报（固件 → 上位机，3 字节）----
       0x55 0x10 0xAA   固件接收超时：它已经发出 0x44 请求，但在自己的超时窗口
                        （固件侧 REPLY_TIMEOUT_MS，默认 1 秒）内没收到完整的 69 字节
                        数据包，固件 OLED 会显示 BT_RX TIMEOUT!!!。
                        上位机必须把"固件为什么收不到数据"讲清楚 ——
                        最常见的原因就是本机发送队列是空的（没点「发送整幅图」）。 */
    static QByteArray initTimeoutFrame(); /* {0x55, 0x10, 0xAA} */

    /* ---- 启动握手（固件先报告就绪，上位机再下令开始）----
       固件进 main() 后先停在 waiting() 里，反复发 0x55 0x77 0xAA 报告"我准备好了"；
       上位机收到就知道板子已就绪，等用户点「发送整幅图」把队列装好，
       再回一个 0x55 0x66 0xAA 让固件进入 BT_Init() 开始要数据。
       这样彻底避开"固件在 main() 开头就要数据、用户根本来不及点"的时序问题。 */
    static QByteArray readyFrame();  /* {0x55, 0x77, 0xAA}  固件 → 上位机 */
    static QByteArray startFrame();  /* {0x55, 0x66, 0xAA}  上位机 → 固件 */

    /* 下发启动命令（帧 55 66 AA）。返回 true = 已写进串口 */
    bool sendStart();

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
    void configRequestReceived(int seq);      /* 收到 0x55 0x55 0xAA */
    void handshakeTimedOut(int waitedMs, int remainingPackets);
    void modeChanged(int mode);
    /* 收到任意上行字节（用于界面显示"我到底听到没有"） */
    void rxActivity(qint64 totalBytes);

    /* 固件主动上报的错误（帧 55 <code> AA）。
       code 目前只有 CtrlInitTimeout(0x10)；msg 是已写好的中文原因说明，界面直接显示即可。
       与 errorOccurred（串口自身出错）区分开：这条是"固件活着、但在抱怨"，
       说明串口链路是通的，问题出在数据供给或时序上。 */
    void firmwareError(int code, const QString &msg);

    /* 收到固件的就绪通知（帧 55 77 AA），count = 累计次数。
       固件会在 waiting() 里反复发，直到上位机回 0x55 0x66 0xAA 为止。 */
    void firmwareReady(int count);

    /* 固件回传了一个像素：seq = 第几个到达（从 0 开始），gray = 出光灰度值。
       ★ 界面按 seq 依次落点，画出的就是固件实际的输出顺序。 */
    void pixelReported(int seq, int gray);

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
    /* 用当前 m_cfg* 值打包一个配置包并直接写串口 */
    bool writeConfigPacket();
    /* 回传异常日志限流（回传帧连续不断，格式错会刷屏） */
    bool mayLogReport();

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
    int          m_reqCount;      /* 收到的取数据请求(0x44)总数 */
    int          m_cfgReqCount;   /* 收到的取配置请求(0x55)总数 */
    int          m_fwErrCount;    /* 收到的固件超时上报(0x10)总数 */
    int          m_readyCount;    /* 收到的固件就绪通知(0x77)总数 */
    int          m_hsSent;        /* 握手模式下已下发的包数 */
    int          m_hsTimeoutMs;
    bool         m_finished;      /* 本幅图已完成标记，防重复 emit */
    qint64       m_lastWriteMs;   /* 上一次真正写串口的时刻，用于最小间隔保险 */
    qint64       m_rxTotal;       /* 自打开以来收到的上行字节总数 */
    bool         m_firstReqSeen;  /* 是否已收到过第一个 0x44 请求（决定超时表何时起） */
    qint64       m_lastRxLogMs;   /* 上行日志限流，防止垃圾流刷爆日志窗口 */

    /* ---- 雕刻回传监看 ----
       ★ 解析从"搜 55 xx AA"改成"按首字节分派的状态机"：
           首字节 0x33 → 按回传帧收 3 字节（33 gg EE）
           首字节 0x55 → 按请求帧收 3 字节（55 xx AA）
         两种帧混在同一字节流里也不会互相误判。 */
    bool         m_reportWatch;
    int          m_reportCount;
    int          m_reportDropped;
    int          m_lastReportLogMs;

    /* 串口参数（默认按需求：115200 / 8 / 奇 / 1） */
    int          m_cfgBaud;
    int          m_cfgDataBits;
    int          m_cfgParity;
    int          m_cfgStopBits;

    /* 配置包(0x33)的三个字段值 */
    int          m_cfgX;
    int          m_cfgY;
    int          m_cfgSpeed;
    int          m_cfgXOrigin;    /* 作业原点 X（像素，2 字节大端） */
    int          m_cfgYOrigin;    /* 作业原点 Y（像素，2 字节大端） */

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
