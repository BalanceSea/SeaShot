// SeaShot Phase 0 探测用测试窗口。
// 目的：提供一个「已知内容 + 已知几何 + 受自己控制」的窗口，用来判定
// CaptureWindow 返回的图像是否与屏幕上所见一致（含标题栏/边框/圆角/阴影），
// 以及与 CaptureArea 的对应关系。
//
// 用法： testwindow [--w 600] [--h 400] [--x 150] [--y 120] [--seconds 20]
//   - 2x2 四象限纯色（红/绿/蓝/黄）+ 正中 10x10 品红块，便于像素定位
//   - 窗口标题固定为 seashot-probe-window，KWin 侧 resourceClass 同名

#include <QApplication>
#include <QLinearGradient>
#include <QRandomGenerator>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>

class PatternWindow : public QWidget
{
public:
    PatternWindow() : QWidget(nullptr)
    {
        setWindowTitle(QStringLiteral("seashot-probe-window"));
        setAutoFillBackground(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int w = width();
        const int h = height();
        const int hw = w / 2;
        const int hh = h / 2;
        p.fillRect(0, 0, hw, hh, QColor(255, 0, 0));
        p.fillRect(hw, 0, w - hw, hh, QColor(0, 255, 0));
        p.fillRect(0, hh, hw, h - hh, QColor(0, 0, 255));
        p.fillRect(hw, hh, w - hw, h - hh, QColor(255, 255, 0));
        // 正中品红定位块
        p.fillRect(w / 2 - 5, h / 2 - 5, 10, 10, QColor(255, 0, 255));
        p.setPen(Qt::black);
        p.drawRect(0, 0, w - 1, h - 1);
    }
};

// ── 滚动截屏（Phase D）验收模式 ─────────────────────────────────────────────
// 用法：
//   testwindow --scroll-page --w 600 --h 500 --x 200 --y 150 --seconds 60 \
//              --page-out /tmp/page.png --page-height 3000 --scroll-step 120 \
//              --scroll-interval 300 --scroll-start 4000
// 行为：无边框窗口里显示一张"长页"的视口，按 scroll-step 自动滚动（模拟用户滚动），
//       并把整张长页 dump 到 --page-out 作为 ground truth（验收脚本拿它比对拼接结果）。
class ScrollPageWindow : public QWidget
{
public:
    ScrollPageWindow(int pageHeight, const QString &pageOut)
        : QWidget(nullptr)
        , m_page(pageWidth(), pageHeight, QImage::Format_RGB32)
    {
        setWindowTitle(QStringLiteral("seashot-probe-scrollpage"));
        QGuiApplication::setDesktopFileName(QStringLiteral("seashot-probe-window"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        drawPage();
        if (!pageOut.isEmpty())
            m_page.save(pageOut);
    }

    int pageHeight() const { return m_page.height(); }
    int offset() const { return m_offset; }

    void scrollBy(int step)
    {
        const int maxOffset = qMax(0, m_page.height() - height());
        m_offset = qBound(0, m_offset + step, maxOffset);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        // 视口内容（= 长页的一段）
        painter.drawImage(rect(), m_page.copy(0, m_offset, width(), height()));
        // 右侧滚动条（每帧都在变 —— 拼接时应当被检测出来并裁掉）
        const int barWidth = 14;
        painter.fillRect(width() - barWidth, 0, barWidth, height(), QColor(238, 240, 244));
        const int maxOffset = qMax(1, m_page.height() - height());
        const int thumbHeight = qMax(40, height() * height() / qMax(1, m_page.height()));
        const int travel = qMax(1, height() - thumbHeight);
        const int thumbY = int(double(m_offset) / double(maxOffset) * travel);
        painter.fillRect(width() - barWidth, thumbY, barWidth, thumbHeight, QColor(130, 136, 148));
    }

private:
    static int pageWidth() { return 600; }

    /// 长页内容：编号文字行 + 随机块（行距刻意做成规则的 40px，正是最容易被"假峰"骗的版式）
    void drawPage()
    {
        QPainter painter(&m_page);
        QLinearGradient gradient(0, 0, 0, m_page.height());
        gradient.setColorAt(0.0, QColor(252, 252, 254));
        gradient.setColorAt(1.0, QColor(236, 240, 246));
        painter.fillRect(m_page.rect(), gradient);
        QFont font = painter.font();
        font.setPixelSize(14);
        painter.setFont(font);
        QRandomGenerator rng(4242u);
        for (int line = 0; line * 40 + 30 < m_page.height(); ++line) {
            painter.setPen(QColor(28, 32, 42));
            painter.drawText(16, line * 40 + 30,
                             QStringLiteral("row %1 · %2").arg(line, 4, 10, QLatin1Char('0')).arg(
                                 QString::number(int(rng.bounded(100000)), 16)));
            if (line % 7 == 3) {
                const int x = 16 + int(rng.bounded(400));
                const int y = line * 40 + 12;
                painter.fillRect(x, y, 40 + int(rng.bounded(120)), 20 + int(rng.bounded(14)),
                                 QColor(int(rng.bounded(180)), int(rng.bounded(180)),
                                        int(rng.bounded(180))));
            }
        }
        painter.setPen(QColor(120, 126, 140));
        painter.drawRect(0, 0, m_page.width() - 1, m_page.height() - 1);
    }

    QImage m_page;
    int m_offset = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("seashot-probe-window"));
    QGuiApplication::setApplicationName(QStringLiteral("seashot-probe-window"));

    int w = 600, h = 400, x = 150, y = 120, seconds = 20;
    bool scrollPage = false;
    int pageHeight = 3000, scrollStep = 0, scrollInterval = 300, scrollStart = 4000;
    QString pageOut;
    const QStringList a = app.arguments();
    for (int i = 1; i + 1 < a.size(); ++i) {
        const QString k = a.at(i);
        const int v = a.at(i + 1).toInt();
        if (k == QLatin1String("--w")) w = v;
        else if (k == QLatin1String("--h")) h = v;
        else if (k == QLatin1String("--x")) x = v;
        else if (k == QLatin1String("--y")) y = v;
        else if (k == QLatin1String("--seconds")) seconds = v;
        else if (k == QLatin1String("--page-height")) pageHeight = v;
        else if (k == QLatin1String("--scroll-step")) scrollStep = v;
        else if (k == QLatin1String("--scroll-interval")) scrollInterval = v;
        else if (k == QLatin1String("--scroll-start")) scrollStart = v;
        else if (k == QLatin1String("--page-out")) pageOut = a.at(i + 1);
    }
    for (const QString &arg : a)
        if (arg == QLatin1String("--scroll-page"))
            scrollPage = true;

    if (scrollPage) {
        auto *window = new ScrollPageWindow(pageHeight, pageOut);
        window->resize(w, h);
        window->move(x, y);
        window->show();
        QTextStream(stdout) << "SCROLLPAGE window=" << w << "x" << h << " at " << x << "," << y
                            << " page=600x" << window->pageHeight() << " step=" << scrollStep
                            << " interval=" << scrollInterval << " start=" << scrollStart << "ms"
                            << "\n";
        QTextStream(stdout).flush();
        if (scrollStep > 0) {
            auto *timer = new QTimer(window);
            QObject::connect(timer, &QTimer::timeout, window,
                             [window, scrollStep]() { window->scrollBy(scrollStep); });
            QTimer::singleShot(scrollStart, timer, [timer, scrollInterval]() {
                timer->start(scrollInterval);
            });
        }
        if (seconds > 0)
            QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    PatternWindow win;
    win.resize(w, h);
    win.move(x, y);
    win.show();
    // 请求激活：保证窗口在最上层，CaptureArea 才可能与窗口内容一致
    QTimer::singleShot(400, &win, [&win]() {
        win.raise();
        win.activateWindow();
    });
    QTimer::singleShot(600, &win, [&win]() {
        qInfo("SEASHOT_TESTWINDOW shown geometry=%d,%d %dx%d screen=%s dpr=%.3f",
              win.x(), win.y(), win.width(), win.height(),
              qPrintable(win.screen() ? win.screen()->name() : QStringLiteral("?")),
              win.devicePixelRatio());
    });
    QTimer::singleShot(seconds * 1000, &app, &QApplication::quit);
    return app.exec();
}

