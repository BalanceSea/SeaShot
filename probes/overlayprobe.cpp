// SeaShot Phase 0 探测：全屏覆盖层（layer-shell）与「自排除」时序验证。
//
// 目的：
//  1) 验证 LayerShellQt 在本机可用：能否拿到 LayerShellQt::Window 角色、能否
//     把全屏覆盖层放到 overlay 层、能否跨输出锚定；
//  2) 验证「截图前先隐藏覆盖层」是否足以自排除：覆盖层可见时截取必然拍到它，
//     隐藏后立即截图是否还残留（竞态窗口有多大）。
//
// 用法：
//   overlayprobe --seconds 6 --probe-area 10,10,160,120 --out-dir probes/out
// 行为：显示纯品红全屏覆盖层 → 打印诊断 → 在 t=1.5s 用外部脚本截取（拍到品红）
//       → 在 t=3.5s 隐藏覆盖层并立即截图（检查残留）→ 退出。

#include <QApplication>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <LayerShellQt/Shell>
#include <LayerShellQt/Window>

#include <csignal>
#include <cstdio>

// SIGUSR1 → 立刻隐藏覆盖层，用来测「隐藏后立即截图」的自排除竞态窗口
static volatile sig_atomic_t g_hideRequested = 0;
static void onHideSignal(int) { g_hideRequested = 1; }

class OverlayWidget : public QWidget
{
public:
    OverlayWidget() : QWidget(nullptr)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setWindowTitle(QStringLiteral("seashot-overlay-probe"));
        QGuiApplication::setDesktopFileName(QStringLiteral("seashot-overlay-probe"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(255, 0, 255)); // 纯品红，便于在截图里统计占比
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QStringList a = app.arguments();
    int seconds = 6;
    for (int i = 1; i + 1 < a.size(); ++i) {
        if (a.at(i) == QLatin1String("--seconds"))
            seconds = a.at(i + 1).toInt();
    }

    LayerShellQt::Shell::useLayerShell();

    OverlayWidget overlay;
    overlay.setGeometry(overlay.screen() ? overlay.screen()->geometry() : QRect(0, 0, 1920, 1080));
    overlay.createWinId(); // 先建立原生窗口，才能拿到 QWindow 设置 role

    bool roleApplied = false;
    if (QWindow *handle = overlay.windowHandle()) {
        if (auto *lsw = LayerShellQt::Window::get(handle)) {
            lsw->setLayer(LayerShellQt::Window::LayerOverlay);
            lsw->setAnchors(LayerShellQt::Window::Anchors(
                LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
            lsw->setExclusiveZone(-1); // 不占空间
            lsw->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
            roleApplied = true;
            std::fprintf(stderr, "SEASHOT_OVERLAY role=applied layer=overlay anchors=all\n");
        } else {
            std::fprintf(stderr, "SEASHOT_OVERLAY role=NULL (LayerShellQt::Window::get 返回空)\n");
        }
    } else {
        std::fprintf(stderr, "SEASHOT_OVERLAY windowHandle=NULL\n");
    }

    overlay.show();
    std::fprintf(stderr, "SEASHOT_OVERLAY shown geometry=%d,%d %dx%d screens=%d dpr=%.3f role=%d\n",
                 overlay.x(), overlay.y(), overlay.width(), overlay.height(),
                 QGuiApplication::screens().size(), overlay.devicePixelRatio(), int(roleApplied));
    std::fflush(stderr);

    QTimer::singleShot(seconds * 1000, &app, [&overlay]() {
        std::fprintf(stderr, "SEASHOT_OVERLAY hidden_at_end\n");
        std::fflush(stderr);
        overlay.hide();
        QTimer::singleShot(300, qApp, &QApplication::quit);
    });

    // SIGUSR1：立即隐藏（用于外部脚本测量「隐藏→截图」的竞态窗口）
    std::signal(SIGUSR1, onHideSignal);
    QTimer hideWatcher;
    hideWatcher.setInterval(5);
    QObject::connect(&hideWatcher, &QTimer::timeout, [&overlay, &hideWatcher]() {
        if (!g_hideRequested)
            return;
        g_hideRequested = 0;
        hideWatcher.stop();
        overlay.hide();
        std::fprintf(stderr, "SEASHOT_OVERLAY hidden_by_signal\n");
        std::fflush(stderr);
    });
    hideWatcher.start();
    return app.exec();
}
