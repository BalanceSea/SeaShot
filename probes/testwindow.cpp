// SeaShot Phase 0 探测用测试窗口。
// 目的：提供一个「已知内容 + 已知几何 + 受自己控制」的窗口，用来判定
// CaptureWindow 返回的图像是否与屏幕上所见一致（含标题栏/边框/圆角/阴影），
// 以及与 CaptureArea 的对应关系。
//
// 用法： testwindow [--w 600] [--h 400] [--x 150] [--y 120] [--seconds 20]
//   - 2x2 四象限纯色（红/绿/蓝/黄）+ 正中 10x10 品红块，便于像素定位
//   - 窗口标题固定为 seashot-probe-window，KWin 侧 resourceClass 同名

#include <QApplication>
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

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("seashot-probe-window"));
    QGuiApplication::setApplicationName(QStringLiteral("seashot-probe-window"));

    int w = 600, h = 400, x = 150, y = 120, seconds = 20;
    const QStringList a = app.arguments();
    for (int i = 1; i + 1 < a.size(); ++i) {
        const QString k = a.at(i);
        const int v = a.at(i + 1).toInt();
        if (k == QLatin1String("--w")) w = v;
        else if (k == QLatin1String("--h")) h = v;
        else if (k == QLatin1String("--x")) x = v;
        else if (k == QLatin1String("--y")) y = v;
        else if (k == QLatin1String("--seconds")) seconds = v;
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
