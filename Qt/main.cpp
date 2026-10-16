#include "widget.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QTransform>
#include <QFrame>
#include <QByteArray>
#include <QRectF>
#include <QDebug>

/* ============================================================================
 * ★ 屏幕方向适配（i.MX6ULL 开发板：LCD 1024x600 横屏）
 *
 * 这个界面是按 600x1024 **竖屏**设计的（见 Widget::designWidth/designHeight）。
 * 开发板 LCD 是 1024x600 横屏，直接显示会把界面的高挤扁；
 * 把整个界面**逆时针转 90°** 之后逻辑尺寸正好变成 1024x600，铺满屏幕。
 *
 * 怎么改方向 / 怎么关掉（优先级从高到低）：
 *   1) 命令行     ./Jiguang --rotate=0        # 0 / 90 / 180 / 270
 *   2) 环境变量   export JIGUANG_ROTATE=270
 *   3) 改下面的 kDefaultRotateCcw
 *
 *   ★ 这里填的角度是「**界面内容逆时针转多少度**」（和肉眼看到的一致）。
 *     90 和 270 都能铺满 1024x600，只是转的方向相反 ——
 *     90  = 内容逆时针（默认，即你要求的）
 *     270 = 内容顺时针
 *     哪个才是"正的"取决于你的屏是往哪边装的，两个都试一下就知道。
 *   ★ 填 0 = 不旋转，退回"按屏幕等比缩小、居中"的老行为。
 *
 * ★★ 注意 Qt 的方向约定（实测确认过，别按直觉写）：
 *     QTransform::rotate(+90) 在屏幕上看是**顺时针**，
 *     QTransform::rotate(-90) 才是**逆时针**。
 *     所以下面传进去的是 -kRotateCcw，不能直接写正数。
 *
 * 实现方式：
 *   把整个界面塞进 QGraphicsScene，用 QGraphicsProxyWidget 托住再旋转。
 *   好处是 QGraphicsProxyWidget 会自动把鼠标/触摸坐标**反向映射**回界面坐标，
 *   不用自己去换算点击位置（否则按钮会点不中）。
 * ==========================================================================*/
static const int kDefaultRotateCcw = 90;   /* 界面内容逆时针转 90° */

static int parseRotateCcw(int argc, char *argv[])
{
    int rot = kDefaultRotateCcw;

    const QByteArray env = qgetenv("JIGUANG_ROTATE");
    if (!env.isEmpty())
        rot = env.toInt();

    for (int i = 1; i < argc; ++i)
    {
        const QByteArray arg(argv[i]);
        if (arg.startsWith("--rotate="))
            rot = arg.mid(9).toInt();
    }

    if (rot != 0 && rot != 90 && rot != 180 && rot != 270)
    {
        qWarning("Jiguang: rotate=%d unsupported (0/90/180/270), fallback to %d",
                 rot, kDefaultRotateCcw);
        rot = kDefaultRotateCcw;
    }
    return rot;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    const int rotCcw = parseRotateCcw(argc, argv);

    const QRect screen = QGuiApplication::primaryScreen()->geometry();

    if (rotCcw == 0)
    {
        /* ---------- 不旋转：老行为，装不下就整体等比缩小并居中 ---------- */
        Widget w;                      /* 固定 600x1024，见 Widget 构造函数 */
        QSize ds(w.designWidth(), w.designHeight());
        if (ds.width() > screen.width() || ds.height() > screen.height())
        {
            const double s = qMin((double)screen.width()  / (double)ds.width(),
                                  (double)screen.height() / (double)ds.height());
            ds = QSize(qRound(ds.width() * s), qRound(ds.height() * s));
        }
        w.setFixedSize(ds);
        w.move(screen.x() + (screen.width()  - ds.width()) / 2,
               screen.y() + qMax(0, (screen.height() - ds.height()) / 2));
        w.show();
        return a.exec();
    }

    /* ---------- 旋转显示 ----------
       ★★ Widget 必须 new 在**堆**上：
          scene.addWidget() 会把控件的所有权交给 QGraphicsProxyWidget，
          场景析构时会 delete 它。如果写成栈对象（Widget w;），
          场景先析构把它删掉、栈对象出作用域又删一次
          → **双重释放，退出时段错误**（实测过，见验证记录）。 */
    Widget *w = new Widget;

    /* ★ 取负号：Qt 的正角度是顺时针，这里要的是逆时针 */
    QTransform t = QTransform().rotate(-rotCcw);

    const QRectF local(0, 0, w->width(), w->height());
    QRectF bb = t.mapRect(local);

    /* ★★ 窗口尺寸 = 「旋转后界面自己的尺寸」，**不是**整块屏幕尺寸。
       600x1024 转 90° 之后正好是 1024x600，铺满开发板的屏；
       要是按屏幕尺寸开窗口，在桌面上（屏比 1024x600 大）就会变成
       "窗口撑满整个屏幕、界面缩在中间一圈空白"。
       只有屏幕真的比界面小才等比缩小（开发板上不会走到这里）。 */
    double s = 1.0;
    if (bb.width() > screen.width() || bb.height() > screen.height())
        s = qMin((double)screen.width()  / bb.width(),
                 (double)screen.height() / bb.height());
    if (s < 1.0)
    {
        t.scale(s, s);
        bb = t.mapRect(local);
    }

    QGraphicsScene scene;
    scene.setSceneRect(0, 0, bb.width(), bb.height());

    QGraphicsProxyWidget *proxy = scene.addWidget(w);
    proxy->setTransform(t);
    /* 把旋转后包围盒的左上角对齐到场景原点 */
    proxy->setPos(-bb.topLeft());

    QGraphicsView view(&scene);
    view.setFrameShape(QFrame::NoFrame);
    view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view.setAlignment(Qt::AlignLeft | Qt::AlignTop);
    view.setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    view.setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    view.setFixedSize(bb.size().toSize());
    /* 屏幕比界面大就居中放（开发板上两者一样大，就是铺满整屏） */
    view.move(screen.x() + (screen.width()  - view.width())  / 2,
              screen.y() + qMax(0, (screen.height() - view.height()) / 2));
    view.show();

    return a.exec();
}
