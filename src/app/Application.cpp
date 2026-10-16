#include "app/Application.h"

#include "app/SelfTest.h"
#include "app/Settings.h"
#include "app/Toast.h"
#include "app/Tray.h"
#include "backend/CaptureBackend.h"
#include "backend/wayland/WaylandBackend.h"
#include "editor/Annotation.h"
#include "editor/AnnotationDocument.h"
#include "editor/AnnotationRenderer.h"
#include "editor/EditorWindow.h"
#include "overlay/Overlay.h"
#include "util/Clipboard.h"
#include "util/Log.h"
#ifdef SEASHOT_WITH_X11
#include "backend/x11/X11Backend.h"
#endif

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <unistd.h>

namespace {
// SIGTERM/SIGINT → 自管道 → 事件循环里优雅退出（会正常卸载 KWin 助手脚本）
int g_signalFds[2] = {-1, -1};
void seashotSignalHandler(int)
{
    if (g_signalFds[1] >= 0) {
        const char token = 1;
        const ssize_t written = ::write(g_signalFds[1], &token, 1);
        Q_UNUSED(written)
    }
}
} // namespace

namespace seashot {

namespace {
BackendKind parseBackendKind(const QString &value)
{
    if (value.compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0)
        return BackendKind::Wayland;
    if (value.compare(QLatin1String("x11"), Qt::CaseInsensitive) == 0)
        return BackendKind::X11;
    return BackendKind::Auto;
}

QString sizeText(const QImage &image)
{
    return QStringLiteral("%1 × %2").arg(image.width()).arg(image.height());
}
} // namespace

Application::Application(const Options &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
{
    m_settings = std::make_unique<Settings>(this);

    m_choice = BackendFactory::resolve(parseBackendKind(m_options.backend),
                                       qEnvironmentVariable("XDG_SESSION_TYPE"),
                                       qEnvironmentVariable("WAYLAND_DISPLAY"),
                                       qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
    m_backendReason = m_choice.reason;

    QString error;
    m_backend.reset(BackendFactory::create(m_choice, &error));
    if (m_backend) {
        if (auto *wayland = dynamic_cast<WaylandBackend *>(m_backend.get()))
            wayland->setKeepShadowMargin(m_settings->keepShadowMargin());
#ifdef SEASHOT_WITH_X11
        if (auto *x11 = dynamic_cast<X11Backend *>(m_backend.get()))
            qApp->installNativeEventFilter(x11);
#endif
        if (!m_backend->initialize(&error)) {
            m_backendError = error;
            SS_WARN() << "[app] 后端初始化失败:" << error;
        } else if (qEnvironmentVariableIsSet("SEASHOT_DISABLE_SELF_EXCLUSION")) {
            // 调试开关：关掉自排除，方便截到自己的覆盖层/浮层（见 README「调试」）
            m_selfExclusion = false;
            SS_WARN() << "[app] SEASHOT_DISABLE_SELF_EXCLUSION 已置位：本次不排除自己的窗口";
        } else {
            QString excludeError;
            m_selfExclusion = m_backend->excludeOwnWindows(true, &excludeError);
            if (!m_selfExclusion)
                SS_INFO() << "[app] 后端不支持自排除，将在截图前隐藏覆盖层：" << excludeError;
        }
    } else {
        m_backendError = error;
    }

    m_overlay = std::make_unique<Overlay>();
    connect(m_overlay.get(), &Overlay::selectionFinished, this, &Application::handleSelection);
    connect(m_overlay.get(), &Overlay::selectionCancelled, this, [this]() {
        m_overlay->stopSelection();
        SS_INFO() << "[app] 已取消截图";
        if (isOneShot())
            QCoreApplication::exit(0);
    });

    m_toast = std::make_unique<Toast>();    m_toast->setEditEnabled(true);
    connect(m_toast.get(), &Toast::saveAsRequested, this, &Application::saveAs);
    connect(m_toast.get(), &Toast::copyRequested, this, &Application::copyLast);
    connect(m_toast.get(), &Toast::editRequested, this,
            [this]() { openEditor(m_lastImage, m_lastSavedPath); });

    // 优雅退出：被 kill/logout 时也把 KWin 助手脚本卸掉（否则会残留成「陈旧脚本」）
    if (::pipe(g_signalFds) == 0) {
        auto *notifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this]() {
            char token = 0;
            const ssize_t read = ::read(g_signalFds[0], &token, 1);
            Q_UNUSED(read)
            SS_INFO() << "[app] 收到退出信号，正在收尾（卸载 KWin 助手脚本）";
            QCoreApplication::quit();
        });
        std::signal(SIGTERM, seashotSignalHandler);
        std::signal(SIGINT, seashotSignalHandler);
    }
}

Application::~Application()
{
    if (m_backend)
        m_backend->shutdown();
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(socketPath());
    }
}

QString Application::socketPath()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtime + QStringLiteral("/seashot.sock");
}

bool Application::sendToExistingInstance(const QString &command)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(300))
        return false;
    socket.write(command.toUtf8());
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

bool Application::startServer()
{
    QLocalServer::removeServer(socketPath());
    m_server = new QLocalServer(this);
    if (!m_server->listen(socketPath())) {
        SS_WARN() << "[app] 无法监听" << socketPath() << m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        QLocalSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            const QString command = QString::fromUtf8(socket->readAll()).trimmed();
            SS_INFO() << "[app] 收到实例间命令:" << command;
            if (command == QLatin1String("capture"))
                startCapture();
            else if (command == QLatin1String("quit"))
                QCoreApplication::quit();
            socket->disconnectFromServer();
        });
    });
    return true;
}

bool Application::isOneShot() const
{
    return m_options.selftest || m_options.listWindows || m_options.captureScreen
        || m_options.hasRect || !m_options.windowHandle.isEmpty() || m_options.overlayHoldMs > 0
        || !m_options.editPath.isEmpty() || !m_options.annotateOut.isEmpty();
}

void Application::openEditor(const QImage &image, const QString &sourcePath)
{
    QTextStream out(stdout);
    if (image.isNull()) {
        out << "EDITOR_ERROR 空图像\n";
        out.flush();
        return;
    }
    auto *editor = new EditorWindow(image, sourcePath);
    editor->show();
    editor->raise();
    editor->activateWindow();
    m_editorOpened = true;
    out << "EDITOR_OPEN size=" << image.width() << "x" << image.height()
        << " source=" << (sourcePath.isEmpty() ? QStringLiteral("(none)") : sourcePath) << "\n";
    out.flush();
    connect(editor, &EditorWindow::savedTo, this,
            [](const QString &path) { SS_INFO() << "[editor] 已保存" << path; });
}

int Application::runAnnotateDemo()
{
    QTextStream out(stdout);
    const QImage source(m_options.annotateIn);
    if (source.isNull()) {
        out << "ANNOTATE_ERROR 无法读取 " << m_options.annotateIn << "\n";
        return 2;
    }

    // 五种工具各来一个：既给验收脚本一个可重复的端到端检查，也可当诊断用
    const double w = source.width();
    const double h = source.height();
    AnnotationDocument document;

    Annotation arrow;
    arrow.kind = ToolKind::Arrow;
    arrow.color = QColor(255, 64, 64);
    arrow.width = 5;
    arrow.points = {QPointF(w * 0.15, h * 0.20), QPointF(w * 0.55, h * 0.45)};
    document.add(arrow);

    Annotation rectangle;
    rectangle.kind = ToolKind::Rectangle;
    rectangle.color = QColor(61, 174, 255);
    rectangle.width = 4;
    rectangle.points = {QPointF(w * 0.60, h * 0.15), QPointF(w * 0.90, h * 0.40)};
    document.add(rectangle);

    Annotation ellipse;
    ellipse.kind = ToolKind::Ellipse;
    ellipse.color = QColor(120, 255, 120);
    ellipse.width = 4;
    ellipse.points = {QPointF(w * 0.10, h * 0.55), QPointF(w * 0.40, h * 0.85)};
    document.add(ellipse);

    Annotation pen;
    pen.kind = ToolKind::Pen;
    pen.color = QColor(255, 220, 80);
    pen.width = 3;
    for (int i = 0; i < 20; ++i)
        pen.points.append(QPointF(w * (0.50 + 0.02 * i), h * (0.70 + 0.10 * std::sin(i / 3.0))));
    document.add(pen);

    Annotation text;
    text.kind = ToolKind::Text;
    text.color = QColor(255, 255, 255);
    text.width = 8;
    text.text = QStringLiteral("SeaShot 编辑器");
    text.points = {QPointF(w * 0.15, h * 0.05)};
    document.add(text);

    const QImage rendered = renderAnnotations(source, document.items());
    if (rendered.width() != source.width() || rendered.height() != source.height()) {
        out << "ANNOTATE_ERROR 导出尺寸变了 " << rendered.width() << "x" << rendered.height()
            << " (源 " << source.width() << "x" << source.height() << ")\n";
        return 3;
    }
    QString error;
    if (!Settings::saveImage(m_options.annotateOut, rendered, &error)) {
        out << "ANNOTATE_ERROR " << error << "\n";
        return 4;
    }
    out << "ANNOTATED " << m_options.annotateOut << " size=" << rendered.width() << "x"
        << rendered.height() << " annotations=" << document.count() << "\n";
    out.flush();
    return 0;
}

QRect Application::virtualGeometry() const
{
    QRect geometry;
    if (m_backend) {
        for (const OutputInfo &output : m_backend->outputs()) {
            geometry = geometry.isNull() ? output.logicalGeometry
                                         : geometry.united(output.logicalGeometry);
        }
    }
    if (geometry.isNull() && QGuiApplication::primaryScreen())
        geometry = QGuiApplication::primaryScreen()->geometry();
    return geometry;
}

void Application::startResident()
{
    if (m_settings->trayPersistent()) {
        m_tray = std::make_unique<Tray>();
        connect(m_tray.get(), &Tray::captureRequested, this, &Application::startCapture);
        connect(m_tray.get(), &Tray::quitRequested, qApp, &QCoreApplication::quit);
    }

    if (!m_backend)
        return;

    m_backend->setHotkeyCallback([this]() {
        SS_INFO() << "[app] 全局快捷键触发截图";
        startCapture();
    });
    QString hotkeyError;
    const QKeySequence sequence(m_settings->hotkey());
    if (!m_backend->setHotkey(sequence, &hotkeyError))
        SS_WARN() << "[app] 快捷键设置失败:" << hotkeyError;
    else if (m_tray)
        m_tray->setHotkeyLabel(sequence.toString(QKeySequence::NativeText));
}

void Application::startCapture()
{
    if (!m_backend || !m_backendError.isEmpty()) {
        SS_WARN() << "[app] 无法开始截图:" << m_backendError;
        QTextStream(stdout) << "CAPTURE_ERROR " << m_backendError << "\n";
        return;
    }
    m_backend->refresh();
    const QVector<WindowInfo> windows = m_backend->windows();
    m_overlay->setWindows(windows);
    m_overlay->setVirtualGeometry(virtualGeometry());
    m_overlay->startSelection();
    // 覆盖层窗口是刚创建的：再要求一次自排除（KWin 脚本也会给新窗口自动打标记，
    // 这里是双保险，避免「覆盖层出现在自己的截图里」）
    QString excludeError;
    m_selfExclusion = m_backend->excludeOwnWindows(true, &excludeError);
    SS_INFO() << "[app] 选择模式已启动，窗口快照" << windows.size() << "个；"
              << m_overlay->diagnostics();
}

void Application::handleSelection(const SelectionResult &result)
{
    m_pendingSelection = result;
    m_overlay->stopSelection();

    // 支持自排除时可以直接截图（覆盖层不会入镜）；否则先隐藏覆盖层再等一帧。
    // Phase 0 实测：隐藏后 16ms 的截图仍可能是含覆盖层的陈旧帧，所以这里留 250ms。
    const int delayMs = m_selfExclusion ? 0 : 250;
    QTimer::singleShot(delayMs, this, &Application::performCapture);
}

void Application::performCapture()
{
    if (!m_backend)
        return;
    const SelectionResult result = m_pendingSelection;
    if (result.mode == SelectionMode::Window) {
        m_lastAnchorRect = result.window.frameGeometry.toAlignedRect();
        SS_INFO() << "[app] 截取窗口" << result.window.id << result.window.title;
        m_backend->captureWindow(result.window,
                                 [this](const CaptureImage &image) { finishCapture(image); });
    } else {
        m_lastAnchorRect = result.logicalRect.toAlignedRect();
        SS_INFO() << "[app] 截取区域" << result.logicalRect;
        m_backend->captureRect(result.logicalRect,
                              [this](const CaptureImage &image) { finishCapture(image); });
    }
}

void Application::finishCapture(const CaptureImage &image)
{
    QTextStream out(stdout);
    if (!image.ok()) {
        SS_WARN() << "[app] 截图失败:" << image.error;
        out << "CAPTURE_ERROR " << image.error << "\n";
        out.flush();
        if (isOneShot())
            QCoreApplication::exit(3);
        return;
    }

    m_lastImage = image.image;

    QString copyError;
    bool copied = false;
    if (!m_options.noCopy && m_settings->copyAfterCapture())
        copied = Clipboard::copyImage(image.image, &copyError);

    QString savedPath = m_options.saveTo;
    QString saveError;
    if (savedPath.isEmpty() && m_settings->autoSaveAfterCapture())
        savedPath = m_settings->defaultSavePath();
    if (!savedPath.isEmpty() && !Settings::saveImage(savedPath, image.image, &saveError)) {
        SS_WARN() << "[app] 保存失败:" << saveError;
        savedPath.clear();
    }
    m_lastSavedPath = savedPath;

    const QString message = QStringLiteral("已截图 %1").arg(sizeText(image.image));
    const bool wantsEditor = m_options.openEditor || m_settings->openEditorAfterCapture();
    // 带 --save 视为脚本化调用：不弹浮层、交付完就退出（验收/自动化靠它保持快）
    const bool scripted = !m_options.saveTo.isEmpty();

    if (wantsEditor) {
        // 「截图后直接打开编辑器」：浮层就不弹了，编辑器里已有复制/保存/另存为
        openEditor(image.image, savedPath);
    } else if (!scripted) {
        m_toast->showResult(image.image, savedPath, message, copyError, m_lastAnchorRect);
    }

    out << "CAPTURE ok size=" << image.image.width() << "x" << image.image.height()
        << " scale=" << image.scale << "\n";
    if (copied)
        out << "COPIED image/png\n";
    if (!savedPath.isEmpty())
        out << "SAVED " << savedPath << "\n";
    if (!copyError.isEmpty())
        out << "COPY_ERROR " << copyError << "\n";
    out.flush();

    if (isOneShot() && m_options.overlayHoldMs == 0 && !wantsEditor) {
        if (scripted) {
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        } else {
            // 一次性 CLI 且没弹编辑器：浮层是用户唯一能操作的界面 →
            // 等它自动消失或用户点「关闭」再退出（否则刚显示的浮层会随进程一起消失，
            // 「编辑 / 另存为」按钮形同虚设）
            m_keepAliveForToast = true;
            connect(m_toast.get(), &Toast::dismissed, qApp, &QCoreApplication::quit,
                    Qt::UniqueConnection);
        }
    }
}

void Application::saveAs()
{
    QTextStream(stdout) << "SAVE_AS_REQUESTED suggested="
                        << QFileInfo(m_settings->defaultSavePath()).absoluteFilePath() << "\n";
    if (m_lastImage.isNull())
        return;
    const QString path = m_settings->defaultSavePath();
    QString error;
    if (Settings::saveImage(path, m_lastImage, &error)) {
        m_lastSavedPath = path;
        m_toast->showResult(m_lastImage, path,
                            QStringLiteral("已保存 %1").arg(sizeText(m_lastImage)), QString(),
                            m_lastAnchorRect);
        QTextStream(stdout) << "SAVED " << path << "\n";
    } else {
        m_toast->showResult(m_lastImage, {}, QStringLiteral("保存失败"), error, m_lastAnchorRect);
    }
}

void Application::copyLast()
{
    if (m_lastImage.isNull())
        return;
    QString error;
    const bool ok = Clipboard::copyImage(m_lastImage, &error);
    m_toast->showResult(m_lastImage, m_lastSavedPath,
                        ok ? QStringLiteral("已复制到剪贴板") : QStringLiteral("复制失败"), error,
                        m_lastAnchorRect);
    QTextStream(stdout) << (ok ? "COPIED image/png\n" : "COPY_ERROR " + error + "\n");
}

int Application::runSelfTest()
{
    return SelfTest::run(m_backend.get(), m_choice, m_settings.get(),
                         m_choice.kind == BackendKind::X11);
}

int Application::runListWindows()
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "ERROR " << m_backendError << "\n";
        return 2;
    }
    out << "BACKEND " << m_backend->name() << " (" << m_backendReason << ")\n";
    out << m_backend->diagnostics() << "\n";
    m_backend->refresh();
    const QVector<WindowInfo> windows = m_backend->windows();
    out << "WINDOWS " << windows.size() << "\n";
    for (const WindowInfo &w : windows) {
        out << QStringLiteral("%1 appId=%2 pid=%3 active=%4 normal=%5 min=%6 visible=%7 stack=%8 "
                             "frame=%9,%10 %11x%12 excluded=%13 title=%14\n")
                   .arg(w.id, w.appId)
                   .arg(w.pid)
                   .arg(w.active ? 1 : 0)
                   .arg(w.normal ? 1 : 0)
                   .arg(w.minimized ? 1 : 0)
                   .arg(w.visible ? 1 : 0)
                   .arg(w.stackIndex)
                   .arg(w.frameGeometry.x())
                   .arg(w.frameGeometry.y())
                   .arg(w.frameGeometry.width())
                   .arg(w.frameGeometry.height())
                   .arg(w.excludedFromCapture ? 1 : 0)
                   .arg(w.title);
    }
    out.flush();
    return 0;
}

int Application::runNonInteractiveCapture()
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "CAPTURE_ERROR " << m_backendError << "\n";
        return 2;
    }

    int exitCode = 4;
    bool finished = false;
    QEventLoop loop;
    auto callback = [&](const CaptureImage &image) {
        finished = true;
        if (!image.ok()) {
            out << "CAPTURE_ERROR " << image.error << "\n";
            out.flush();
            exitCode = 3;
        } else {
            finishCapture(image);
            exitCode = 0;
        }
        loop.quit();
    };

    if (!m_options.windowHandle.isEmpty()) {
        WindowInfo info;
        info.id = m_options.windowHandle;
        for (const WindowInfo &w : m_backend->windows()) {
            if (w.id == info.id) {
                info = w;
                break;
            }
        }
        m_lastAnchorRect = info.frameGeometry.toAlignedRect();
        m_backend->captureWindow(info, callback);
    } else if (m_options.hasRect) {
        m_lastAnchorRect = m_options.rect.toAlignedRect();
        m_backend->captureRect(m_options.rect, callback);
    } else {
        m_lastAnchorRect = virtualGeometry();
        m_backend->captureRect(virtualGeometry(), callback);
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!finished) {
        out << "CAPTURE_ERROR 超时（30s）\n";
        out.flush();
        return 4;
    }
    return exitCode;
}

int Application::showOverlayAndHold()
{
    if (!m_backend || !m_backendError.isEmpty())
        return 2;
    startCapture();
    QTextStream(stdout) << "OVERLAY_SHOWN " << m_overlay->diagnostics() << "\n";
    QTextStream(stdout) << "OVERLAY_WINDOWS " << m_backend->windows().size() << "\n";
    QTextStream(stdout) << "OVERLAY_EXCLUDED "
                        << (m_backend->windows().isEmpty() ? -1 : (m_selfExclusion ? 1 : 0)) << "\n";
    QTextStream(stdout).flush();
    QTimer::singleShot(m_options.overlayHoldMs, qApp, &QCoreApplication::quit);
    return QCoreApplication::exec();
}

int Application::run()
{
    QTextStream out(stdout);

    if (m_options.selftest) {
        out << "BACKEND_CHOICE " << m_backendReason << "\n";
        out.flush();
        return runSelfTest();
    }
    if (m_options.listWindows)
        return runListWindows();
    if (!m_options.annotateOut.isEmpty())
        return runAnnotateDemo();
    if (!m_options.editPath.isEmpty()) {
        const QImage image(m_options.editPath);
        if (image.isNull()) {
            out << "EDITOR_ERROR 无法读取 " << m_options.editPath << "\n";
            return 2;
        }
        openEditor(image, m_options.editPath);
        return QCoreApplication::exec();
    }
    if (m_options.captureScreen || m_options.hasRect || !m_options.windowHandle.isEmpty()) {
        const int code = runNonInteractiveCapture();
        // 浮层还在 / 编辑器还开着：别急着退（quitOnLastWindowClosed 会在窗口关掉时收尾）
        if (m_keepAliveForToast || m_editorOpened)
            return QCoreApplication::exec();
        return code;
    }
    if (m_options.overlayHoldMs > 0) {
        if (sendToExistingInstance(QStringLiteral("capture"))) {
            out << "FORWARDED_TO_RUNNING_INSTANCE\n";
            return 0;
        }
        startServer();
        return showOverlayAndHold();
    }
    if (m_options.capture) {
        // 已有常驻实例 → 交给它；否则自己截一次
        if (sendToExistingInstance(QStringLiteral("capture"))) {
            out << "FORWARDED_TO_RUNNING_INSTANCE\n";
            out.flush();
            return 0;
        }
        startServer();
        startCapture();
        return QCoreApplication::exec();
    }

    // 无参数：常驻
    if (sendToExistingInstance(QStringLiteral("capture-already-running-check"))) {
        startResident();
        return QCoreApplication::exec();
    }
    startServer();
    startResident();
    out << "SEASHOT_READY backend=" << (m_backend ? m_backend->name() : QStringLiteral("none"))
        << " hotkey=" << (m_settings->hotkey().isEmpty() ? QStringLiteral("(未绑定)")
                                                         : m_settings->hotkey())
        << "\n";
    out.flush();
    return QCoreApplication::exec();
}

} // namespace seashot
