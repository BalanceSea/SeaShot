#include "app/Application.h"

#include "app/SelfTest.h"
#include "app/PinWindow.h"
#include "app/Settings.h"
#include "app/SaveExportDialog.h"
#include "app/SettingsDialog.h"
#include "app/Toast.h"
#include "app/Tray.h"
#include "backend/CaptureBackend.h"
#include "backend/wayland/WaylandBackend.h"
#include "editor/Annotation.h"
#include "editor/AnnotationDocument.h"
#include "editor/AnnotationRenderer.h"
#include "editor/EditorWindow.h"
#include "overlay/Overlay.h"
#include "record/GifEncoder.h"
#include "record/GifRecorder.h"
#include "record/PipeWireFrameSource.h"
#include "record/PortalScreencast.h"
#include "hotkey/GlobalShortcutsPortal.h"
#include "ocr/OcrEngine.h"
#include "ocr/OcrWindow.h"
#include "record/RecordingIndicator.h"
#include "scroll/ScrollCapturer.h"
#include "util/Clipboard.h"
#include "util/Log.h"
#ifdef SEASHOT_WITH_X11
#include "backend/x11/X11Backend.h"
#endif

#include <algorithm>
#include <limits>

#include <QApplication>
#include <QCoreApplication>
#include <QWheelEvent>
#include <QDateTime>
#include <QDir>
#include <cmath>
#include <cstdio>
#include <sys/resource.h>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QScopedValueRollback>
#include <QProcess>
#include <QProcessEnvironment>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QtTest/QTest>
#include <QTimer>

#include <csignal>
#include <unistd.h>

namespace {
// SIGTERM/SIGINT → 自管道 → 事件循环里优雅退出（会正常卸载 KWin 助手脚本）
int g_signalFds[2] = {-1, -1};
// 诊断模式会在 QCoreApplication::exec() 之外再起嵌套 QEventLoop；
// QCoreApplication::quit() 只退主循环，所以这里登记一个"当前嵌套循环"一并退出。
QEventLoop *g_nestedLoop = nullptr;
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
    : Application(options, std::unique_ptr<CaptureBackend>(), parent)
{
}

Application::Application(const Options &options, std::unique_ptr<CaptureBackend> backend, QObject *parent)
    : QObject(parent)
    , m_options(options)
    , m_backend(std::move(backend))
{
    m_settings = std::make_unique<Settings>(this);

    // 拖动延迟基准测试需要截到自己的浮层：在创建后端之前把自排除关掉
    if (m_options.toastDragBenchMs > 0)
        qputenv("SEASHOT_DISABLE_SELF_EXCLUSION", "1");

    m_choice = BackendFactory::resolve(parseBackendKind(m_options.backend),
                                       qEnvironmentVariable("XDG_SESSION_TYPE"),
                                       qEnvironmentVariable("WAYLAND_DISPLAY"),
                                       qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
    m_backendReason = m_choice.reason;

    QString error;
    if (!m_backend)
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

    // 上一次崩溃/被 kill 可能留下几百 MB~几 GB 的中间文件（占磁盘），启动时清掉。
    // 只清理进程已不存在的那些，不碰正在录制的另一个实例。
    {
        const int cleaned = GifEncoder::cleanStaleIntermediates();
        if (cleaned > 0)
            SS_INFO() << "[app] 启动清理：已删除" << cleaned << "个残留中间文件";
    }

    m_overlay = std::make_unique<Overlay>();
    connect(m_overlay.get(), &Overlay::selectionFinished, this, &Application::handleSelection);
    connect(m_overlay.get(), &Overlay::selectionCancelled, this, [this]() {
        m_recordIntent = false;
        m_overlay->stopSelection();
        SS_INFO() << "[app] 已取消截图";
        if (isOneShot())
            QCoreApplication::exit(0);
    });
    // Phase A1：就地标注阶段的确认 / 取消。
    // 确认时**不重新选区域**，直接沿用标注阶段里固定的那个选区继续走现有截图链路。
    connect(m_overlay.get(), &Overlay::annotationConfirmed, this,
            [this](const SelectionResult &result, Overlay::ConfirmAction action) {
                SS_INFO() << "[app] 就地标注确认，进入截图" << result.logicalRect << "动作="
                          << (action == Overlay::ConfirmAction::Copy ? "复制"
                              : action == Overlay::ConfirmAction::Save ? "保存"
                                                                       : "按设置");
                m_pendingSelection = result;
                m_pendingAction = action; // Phase B：Ctrl+C / Ctrl+S / Enter 分别对应不同交付
                if (action == Overlay::ConfirmAction::Ocr) {
                    // 文字识别：拿"屏幕上看到的那张选区图"（含标注）去识别，然后收掉覆盖层
                    const QImage selection =
                        m_overlay->renderSelection(m_overlay->frozenSelection());
                    m_overlay->stopSelection();
                    m_pendingAction = Overlay::ConfirmAction::Plain;
                    startOcr(selection, true);
                    return;
                }
                if (action == Overlay::ConfirmAction::Scroll) {
                    // 长截图不走"先截一张再交付"那条路：选区一定就直接开始拼
                    m_overlay->stopSelection();
                    m_pendingAction = Overlay::ConfirmAction::Plain;
                    startScrollCapture(result);
                    return;
                }
                finishSelectionPhase();
            });
    connect(m_overlay.get(), &Overlay::annotationCancelled, this, [this]() {
        m_overlay->stopSelection();
        SS_INFO() << "[app] 已取消截图（标注阶段）";
        if (isOneShot())
            QCoreApplication::exit(0);
    });

    // 截图后的结果浮层（Toast）已退役：截图完只自动复制/保存，不弹任何窗；
    // 截图与录制的保存/导出统一走 SaveExportDialog（见 openSaveExport）。

    // 录制（Phase 4）
    // 滚动截屏（Phase D）：复用录制指示器（区域外 + layer-shell + 自排除）
    m_scrollCapturer = std::make_unique<ScrollCapturer>(m_backend.get());
    connect(m_scrollCapturer.get(), &ScrollCapturer::finished, this,
            &Application::onScrollFinished);
    connect(m_scrollCapturer.get(), &ScrollCapturer::failed, this, &Application::onScrollFailed);

    m_recorder = std::make_unique<GifRecorder>();
    m_recordStopTimer.setSingleShot(true);
    connect(&m_recordStopTimer, &QTimer::timeout, this, &Application::stopRecording);
    m_recordingIndicator = std::make_unique<RecordingIndicator>();
    connect(m_recorder.get(), &GifRecorder::stoppedForSpace, this, [](qint64 available) {
        SS_WARN() << "[app] 录制因临时空间不足提前停止，剩余" << available / 1048576 << "MB";
    });
    connect(m_recorder.get(), &GifRecorder::progress, this, [this]() {
        if (!m_recordingIndicator || !m_recorder)
            return;
        const GifRecorder::Stats stats = m_recorder->stats();
        m_recordingIndicator->updateStats(stats.captured, stats.dropped, stats.elapsedMs);
    });
    connect(m_recorder.get(), &GifRecorder::finished, this,
            [this](const QString &path, const GifRecorder::Stats &stats, const QString &error) {
                onRecordingFinished(path, stats.captured, stats.dropped, stats.failed,
                                    stats.elapsedMs, error);
            });
    connect(m_recordingIndicator.get(), &RecordingIndicator::stopRequested, this, [this]() {
        // 同一块指示器两种用途：录制中 → 停止录制；滚动截屏中 → 结束拼接（交付已拼的部分）
        if (m_scrollCapturer && m_scrollCapturer->isRunning())
            m_scrollCapturer->stop();
        else
            stopRecording();
    });

    // 配置变化即时生效（配置界面每次改动都会发这个信号）
    connect(m_settings.get(), &Settings::changed, this, &Application::applySettings);

    // 优雅退出：被 kill/logout 时也把 KWin 助手脚本卸掉（否则会残留成「陈旧脚本」）
    if (::pipe(g_signalFds) == 0) {
        auto *notifier = new QSocketNotifier(g_signalFds[0], QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this]() {
            char token = 0;
            const ssize_t read = ::read(g_signalFds[0], &token, 1);
            Q_UNUSED(read)
            SS_INFO() << "[app] 收到退出信号，正在收尾（卸载 KWin 助手脚本）";
            if (g_nestedLoop)
                g_nestedLoop->quit();
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
    if (socket.write(command.toUtf8() + '\n') < 0)
        return false;
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(500))
            return false;
    }
    socket.disconnectFromServer();
    return true;
}

bool Application::startServer()
{
    auto lock = std::make_unique<QLockFile>(socketPath() + QStringLiteral(".lock"));
    if (!lock->tryLock(0)) {
        SS_WARN() << "[app] 已有实例持有服务锁";
        return false;
    }
    QLocalServer::removeServer(socketPath());
    m_server = new QLocalServer(this);
    if (!m_server->listen(socketPath())) {
        SS_WARN() << "[app] 无法监听" << socketPath() << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_instanceLock = std::move(lock);
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket *socket = m_server->nextPendingConnection();
            const auto buffer = std::make_shared<QByteArray>();
            const auto receive = [this, socket, buffer](bool closed) {
                buffer->append(socket->readAll());
                if (buffer->size() > 1024 * 1024) {
                    socket->abort();
                    return;
                }
                const qsizetype end = buffer->indexOf('\n');
                if (end < 0 && !closed)
                    return;
                const QByteArray command = end >= 0 ? buffer->left(end) : *buffer;
                buffer->clear();
                if (!command.isEmpty())
                    handleInstanceCommand(QString::fromUtf8(command).trimmed());
                if (socket->state() != QLocalSocket::UnconnectedState)
                    socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, [receive]() { receive(false); });
            connect(socket, &QLocalSocket::disconnected, this, [receive]() { receive(true); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            if (socket->bytesAvailable() > 0)
                receive(false);
        }
    });
    return true;
}

void Application::handleInstanceCommand(const QString &command)
{
    SS_INFO() << "[app] 收到实例间命令:" << command;
    if (command.startsWith(QLatin1Char('{'))) {
        const QJsonObject message = QJsonDocument::fromJson(command.toUtf8()).object();
        if (message.value(QStringLiteral("command")).toString() != QLatin1String("record"))
            return;
        RecordingRequest request;
        request.saveTo = message.value(QStringLiteral("saveTo")).toString();
        request.seconds = message.value(QStringLiteral("seconds")).toInt();
        if (request.seconds < 0 || request.seconds > std::numeric_limits<int>::max() / 1000)
            return;
        const QJsonArray rect = message.value(QStringLiteral("rect")).toArray();
        request.hasRect = !rect.isEmpty();
        if (request.hasRect) {
            if (rect.size() != 4)
                return;
            request.rect = QRectF(rect.at(0).toDouble(), rect.at(1).toDouble(),
                                   rect.at(2).toDouble(), rect.at(3).toDouble());
            if (!request.rect.isValid())
                return;
        }
        startRecordingRequest(request);
    } else if (command == QLatin1String("capture")) {
        startCapture();
    } else if (command == QLatin1String("record")) {
        startRecordFlow();
    } else if (command.startsWith(QLatin1String("record-rect "))) {
        const QStringList parts = command.mid(12).split(QLatin1Char(','));
        if (parts.size() == 4) {
            RecordingRequest request;
            request.hasRect = true;
            request.rect = QRectF(parts.at(0).toDouble(), parts.at(1).toDouble(),
                                   parts.at(2).toDouble(), parts.at(3).toDouble());
            startRecordingRequest(request);
        }
    } else if (command == QLatin1String("stop-record")) {
        stopRecording();
    } else if (command == QLatin1String("quit")) {
        QCoreApplication::quit();
    }
}

bool Application::isOneShot() const
{
    return m_options.selftest || m_options.listWindows || m_options.captureScreen
        || m_options.hasRect || !m_options.windowHandle.isEmpty() || m_options.overlayHoldMs > 0
        || !m_options.editPath.isEmpty() || !m_options.annotateOut.isEmpty()
        || m_options.hotkeyStatus || m_options.printConfig || m_options.record
        || m_options.scrollCapture || !m_options.ocrPath.isEmpty()
        || m_options.hotkeyPortalStatus || m_options.hotkeyPortalRequest;
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
    if (!qEnvironmentVariable("SEASHOT_EDITOR_GRAB").isEmpty()) {
        editor->show();
        QTimer::singleShot(400, this, [editor]() {
            const QString path = qEnvironmentVariable("SEASHOT_EDITOR_GRAB");
            editor->grab().save(path);
            QTextStream(stdout) << "UI_GRAB " << path << " size=" << editor->width() << "x"
                                << editor->height() << "\n";
            QTextStream(stdout).flush();
            QCoreApplication::quit();
        });
        return;
    }
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

void Application::startRecordFlow()
{
    if (m_recorder->isRecording()) {
        stopRecording();
        return;
    }
    startRecordingRequest(RecordingRequest());
}

bool Application::startRecordingRequest(const RecordingRequest &request)
{
    if (!m_backend || !m_backendError.isEmpty()) {
        SS_WARN() << "[app] 无法开始录制:" << m_backendError;
        QTextStream(stdout) << "RECORD_ERROR " << m_backendError << "\n";
        return false;
    }
    if (m_recordStarting || m_recorder->isRecording() || m_recorder->isFinishing() || m_recordIntent) {
        QTextStream(stdout) << "RECORD_ERROR 已有录制正在进行或收尾\n";
        return false;
    }
    m_recordingRequest = request;
    if (request.hasRect) {
        SelectionResult selection;
        selection.mode = SelectionMode::Rect;
        selection.logicalRect = request.rect;
        return beginRecording(selection);
    }
    m_recordIntent = true;
    m_overlay->setIntent(Overlay::Intent::Record);
    // 录制不需要就地标注阶段（录制选完就直接开录），显式关掉，
    // 免得沿用上一次截图留下的开关。
    m_overlay->setAnnotationEnabled(false);
    m_backend->refresh();
    m_overlay->setWindows(m_backend->windows());
    m_overlay->setVirtualGeometry(virtualGeometry());
    m_overlay->startSelection();
    QString excludeError;
    m_selfExclusion = m_backend->excludeOwnWindows(true, &excludeError);
    SS_INFO() << "[app] 录制选择模式已启动";
    return true;
}

QString Application::defaultGifPath() const
{
    return m_settings->defaultSavePath(QStringLiteral("gif"));
}

bool Application::beginRecording(const SelectionResult &selection)
{
    if (m_recordStarting || m_recorder->isRecording() || m_recorder->isFinishing())
        return false;
    const QScopedValueRollback<bool> starting(m_recordStarting, true);
    m_recordIntent = false;
    if (!m_backend)
        return false;

    const QRectF wanted = selection.mode == SelectionMode::Window ? selection.window.frameGeometry
                                                                  : selection.logicalRect;
    const QRectF region = wanted.intersected(QRectF(virtualGeometry()));
    if (region.width() < 2 || region.height() < 2) {
        QTextStream(stdout) << "RECORD_ERROR 录制区域无效\n";
        return false;
    }

    const QString path = m_recordingRequest.saveTo.isEmpty() ? defaultGifPath()
                                                            : m_recordingRequest.saveTo;
    m_recorder->setOutputPath(path);
    // 交互式录制：先只收尾无损中间文件，格式由用户在浮层工具栏上选（GIF / MP4）。
    // 脚本式（--save 给了路径）保持一次到底：按扩展名直接产出，便于验收与自动化。
    m_recorder->setDeferExport(m_recordingRequest.saveTo.isEmpty());
    // 中间格式由配置决定：无损（转 GIF/MP4 只损失一次，临时文件 ≈554MB/分钟）
    // 还是直接录 MP4（临时文件小，转 GIF 多一次有损）
    m_recorder->setLossless(m_settings->recordLossless());

    QString error;
    // 优先 PipeWire（真正的视频流）；不可用时回退到 ScreenShot2 轮询。
    // 回退是常态而不是异常：没有 token、没编译 libpipewire、或显式要求轮询时都会走到。
    if (startPipeWireRecording(region, m_settings->gifFps(), &error)) {
        // 已由 PipeWire 路径启动
    } else {
        SS_INFO() << "[app] PipeWire 帧源不可用，回退轮询：" << error;
        QTextStream(stdout) << "RECORD_SOURCE polling (" << error << ")\n";
        QTextStream(stdout).flush();
        if (!m_recorder->start(m_backend.get(), region, m_settings->gifFps(), &error)) {
            SS_WARN() << "[app] 录制启动失败:" << error;
            QTextStream(stdout) << "RECORD_ERROR " << error << "\n";
            return false;
        }
    }
    m_recordingIndicator->showFor(region, m_recorder->fps(), path);
    if (m_tray)
        m_tray->setRecording(true);
    m_keepAliveForRecording = true;

    m_recordStopTimer.stop();
    if (m_recordingRequest.seconds > 0) {
        SS_INFO() << "[app] 将在" << m_recordingRequest.seconds << "秒后自动停止录制";
        m_recordStopTimer.start(m_recordingRequest.seconds * 1000);
    }
    return true;
}

bool Application::startPipeWireRecording(const QRectF &region, int fps, QString *error)
{
    if (!m_backend) {
        *error = QStringLiteral("没有可用的截图后端");
        return false;
    }
    if (!PipeWireFrameSource::isAvailable()) {
        *error = QStringLiteral("编译时没有 libpipewire-0.3");
        return false;
    }
    // 强制走轮询（A/B 对比与排障用）
    if (qEnvironmentVariableIsSet("SEASHOT_RECORD_POLLING")) {
        *error = QStringLiteral("SEASHOT_RECORD_POLLING 已置位");
        return false;
    }

    // 需要 restore token：有它才能静默建会话（否则会弹授权对话框，不适合录制场景）
    QString token;
    {
        QFile file(m_options.portalTokenFile);
        if (file.open(QIODevice::ReadOnly))
            token = QString::fromUtf8(file.readAll()).trimmed();
    }
    if (token.isEmpty()) {
        *error = QStringLiteral("没有 restore token（先跑一次 --probe-portal-screencast 授权）");
        return false;
    }

    // 选区落在哪个输出上（PipeWire 给的是整屏，得知道怎么裁）
    const QVector<OutputInfo> outputs = m_backend->outputs();
    if (outputs.isEmpty()) {
        *error = QStringLiteral("拿不到输出信息");
        return false;
    }
    OutputInfo monitor = outputs.first();
    qint64 best = -1;
    for (const OutputInfo &info : outputs) {
        const QRect overlap = QRectF(info.logicalGeometry).intersected(region).toRect();
        const qint64 area = qint64(overlap.width()) * overlap.height();
        if (area > best) {
            best = area;
            monitor = info;
        }
    }
    if (best <= 0) {
        *error = QStringLiteral("选区不在任何输出内");
        return false;
    }
    const double scale = monitor.scale > 0 ? monitor.scale : 1.0;
    const QSize frameSize(qMax(2, qRound(region.width() * scale)),
                          qMax(2, qRound(region.height() * scale)));

    m_recordingPortal = std::make_unique<PortalScreencast>();
    PortalScreencast::Result result;
    {
        QEventLoop loop;
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        m_recordingPortal->start(token, /*persistMode=*/2,
                                 [&](const PortalScreencast::Result &r) {
                                     result = r;
                                     loop.quit();
                                 });
        loop.exec();
    }
    if (!result.ok || result.streams.isEmpty()) {
        *error = result.error.isEmpty() ? QStringLiteral("portal 没有返回流") : result.error;
        stopPipeWireRecording();
        return false;
    }
    m_recordingPipeWireFd = m_recordingPortal->openPipeWireRemote(error);
    if (m_recordingPipeWireFd < 0) {
        stopPipeWireRecording();
        return false;
    }

    m_pipeWireSource = std::make_shared<PipeWireFrameSource>();
    m_pipeWireSource->setTargetFps(fps);
    if (!m_pipeWireSource->start(m_recordingPipeWireFd, result.streams.first().nodeId,
                                 QRectF(monitor.logicalGeometry), region, error)) {
        stopPipeWireRecording();
        return false;
    }

    const auto source = m_pipeWireSource;
    if (!m_recorder->startFromProvider([source] { return source->latestFrame(); }, frameSize,
                                       region, fps, error)) {
        stopPipeWireRecording();
        return false;
    }
    QTextStream(stdout) << "RECORD_SOURCE pipewire node=" << result.streams.first().nodeId
                        << " format=\"" << m_pipeWireSource->negotiatedFormat() << "\" frameSize="
                        << frameSize.width() << "x" << frameSize.height() << "\n";
    QTextStream(stdout).flush();
    SS_INFO() << "[app] 录制帧源 = PipeWire" << m_pipeWireSource->negotiatedFormat();
    return true;
}

void Application::stopPipeWireRecording()
{
    if (m_pipeWireSource) {
        m_pipeWireSource->stop();
        m_pipeWireSource.reset();
    }
    if (m_recordingPortal) {
        m_recordingPortal->closeSession();
        m_recordingPortal.reset();
    }
    if (m_recordingPipeWireFd >= 0) {
        ::close(m_recordingPipeWireFd);
        m_recordingPipeWireFd = -1;
    }
}

void Application::stopRecording()
{
    m_recordStopTimer.stop();
    if (!m_recorder || !m_recorder->isRecording()) {
        SS_INFO() << "[app] 当前没有在录制";
        return;
    }
    QTextStream(stdout) << "RECORD_STOP requested\n";
    QTextStream(stdout).flush();
    m_recorder->stop();
    stopPipeWireRecording(); // 必须在 recorder->stop() 之后：拉动帧的计时器已停
    m_recordingIndicator->hideIndicator();
    if (m_tray)
        m_tray->setRecording(false);
}

void Application::exportRecording(RecordingFormat format)
{
    QTextStream out(stdout);
    if (!m_recorder) {
        out << "RECORD_EXPORT_ERROR 没有录制结果\n";
        out.flush();
        return;
    }
    const QString suffix = format == RecordingFormat::Mp4 ? QStringLiteral("mp4")
                                                          : QStringLiteral("gif");
    const QString path = m_settings->defaultSavePath(suffix);
    QString error;
    // 导出是同步的（ffmpeg 调色板量化可能几秒）：先把界面刷一下，别让人以为卡死
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    const bool ok = m_recorder->exportTo(path, &error);
    out << (ok ? "RECORD_EXPORT ok format=" : "RECORD_EXPORT_ERROR format=") << suffix
        << (ok ? QStringLiteral(" path=%1").arg(path) : QStringLiteral(" error=%1").arg(error))
        << "\n";
    out.flush();
    if (m_saveExport) {
        if (ok)
            m_saveExport->setRecording(0, 0, QString(), false); // 中间文件已清，没有待导出的了
        else
            m_saveExport->setRecording(0, 0, QStringLiteral("导出失败：%1").arg(error), false);
    }
}

void Application::copyRecordingGif()
{
    QTextStream out(stdout);
    if (!m_recorder) {
        out << "RECORD_COPY_ERROR 没有录制结果\n";
        out.flush();
        return;
    }
    // 先导出成文件（剪贴板要投递完整字节），再整体放进剪贴板 —— 动图不能只投一帧。
    const QString path = m_settings->defaultSavePath(QStringLiteral("gif"));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QString error;
    if (!m_recorder->exportTo(path, &error)) {
        out << "RECORD_COPY_ERROR " << error << "\n";
        out.flush();
        return;
    }
    const bool copied = Clipboard::copyFile(path, QStringLiteral("image/gif"), &error);
    out << (copied ? "RECORD_COPY ok mime=image/gif path=" : "RECORD_COPY_ERROR ")
        << (copied ? path : error) << "\n";
    out.flush();
    if (m_saveExport && copied)
        m_saveExport->setRecording(0, 0, QString(), false);
}

void Application::exportRecordingGif()
{
    exportRecording(RecordingFormat::Gif);
}

void Application::exportRecordingMp4()
{
    exportRecording(RecordingFormat::Mp4);
}

void Application::onRecordingFinished(const QString &gifPath, int captured, int dropped,
                                      int failed, qint64 elapsedMs, const QString &error)
{
    m_recordStopTimer.stop();
    stopPipeWireRecording();
    QTextStream out(stdout);
    if (m_recordingIndicator)
        m_recordingIndicator->hideIndicator();
    if (m_tray)
        m_tray->setRecording(false);

    if (!error.isEmpty()) {
        out << "RECORD_ERROR " << error << "\n";
        out.flush();
        if (m_recordingRequest.quitWhenFinished)
            QCoreApplication::exit(3);
        return;
    }

    const double seconds = elapsedMs > 0 ? double(elapsedMs) / 1000.0 : 0.0;
    const double effectiveFps = seconds > 0 ? double(captured) / seconds : 0.0;
    out << "RECORD ok path=" << gifPath << " frames=" << captured << " dropped=" << dropped
        << " failed=" << failed << " elapsedMs=" << elapsedMs
        << " fps=" << QString::number(effectiveFps, 'f', 2) << "\n";
    out.flush();

    // 录制结束：**自动打开**「保存/导出」窗口的录制页 —— 格式由用户在那里选。
    // （原来是结果浮层上的两个按钮；用户要求把"保存录制"和"另存为"统一进一个界面。）
    // 脚本式（--save）已经"一次到底"导出了，不弹窗、直接退出。
    const bool scripted = !m_recordingRequest.saveTo.isEmpty();
    if (!scripted)
        openSaveExport(true);
    if (scripted && m_recordingRequest.quitWhenFinished)
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void Application::startResident()
{
    // 托盘 / 浮层超时 / 阴影边距 / 快捷键 均由 applySettings() 按当前配置建立，
    // 之后任何配置改动（配置界面里点一下）也会走同一条路径即时生效。
    if (m_backend) {
        m_backend->setHotkeyCallback([this]() {
            // 录制中：同一个快捷键 = 停止录制；否则开始截图
            if (m_recorder && m_recorder->isRecording()) {
                SS_INFO() << "[app] 全局快捷键：停止录制";
                stopRecording();
                return;
            }
            SS_INFO() << "[app] 全局快捷键触发截图";
            startCapture();
        });
    }
    // 顺序很重要：先决定"要不要首次运行问一次"，再由 setupShortcutPortal 发**唯一一次**
    // ListShortcuts —— 那一次的结果同时用于"是否已授权"和"要不要发起注册"。
    // （之前是两处各发一次 list，结果先到的那个空应答被当成"没有绑定"→ 多发起了一次 bind，
    //   而 bind 会带着 preferred_trigger 写回绑定，有覆盖用户已有按键的风险 —— ISSUE-014 第 5 条。）
    maybeAskForHotkeyOnFirstRun();
    setupShortcutPortal();
    applySettings();
}

void Application::setupShortcutPortal()
{
    // Phase F：常驻启动时**静默**看一眼"用户之前授权过没有"：
    //   · 授权过 → 走 portal 这条路（监听 Activated → 与快捷键回调同一条路径）；
    //   · 没授权过 → 什么都不做（**不主动弹窗**，那会每次启动打扰用户）。
    //     由「设置 → 全局快捷键」的按钮 / 托盘菜单 「注册全局快捷键（由我申请）…」 / CLI 发起，
    //     用户自己决定授权还是不授权。
    uint version = 0;
    QString error;
    if (!GlobalShortcutsPortal::available(&version, &error)) {
        m_portalStateKnown = true; // 没有门户：直接走 KGlobalAccel
        return;
    }
    ensureShortcutPortal()->listShortcuts();
}

void Application::maybeAskForHotkeyOnFirstRun()
{
    // 只在**常驻启动**这条正常路径上做（--capture/--record 等一次性调用不会走到这里）。
    // 验收/自动化用 SEASHOT_NO_FIRST_RUN_ASK=1 关掉（否则跑测试会被系统授权对话框挡住）。
    if (qEnvironmentVariableIsSet("SEASHOT_NO_FIRST_RUN_ASK"))
        return;
    if (!m_settings || m_settings->portalHotkeyAsked())
        return; // 问过了（允许或拒绝都算）—— 不再打扰
    if (!m_settings->hotkey().isEmpty())
        return; // 已经用 KGlobalAccel 那条路设过键，不必再问
    uint version = 0;
    QString error;
    if (!GlobalShortcutsPortal::available(&version, &error)) {
        QTextStream(stdout) << "HOTKEY_FIRST_RUN skip（没有 GlobalShortcuts 门户：" << error << "）\n";
        QTextStream(stdout).flush();
        return;
    }
    // 只置标志：真正"要不要发起"由紧随其后的那次 ListShortcuts 结果决定（见 listed 处理），
    // 这样绝不会在"其实已经授权"的情况下盲目 bind（那会覆盖用户已有的键）。
    m_firstRunAskPending = true;
}

GlobalShortcutsPortal *Application::ensureShortcutPortal()
{
    if (m_shortcutPortal)
        return m_shortcutPortal.get();
    m_shortcutPortal = std::make_unique<GlobalShortcutsPortal>();
    // ⚠️ 信号**只在这里接一次**，而且不要给 lambda 传 Qt::UniqueConnection：
    //    那个标志只对成员函数指针有效，用 lambda 时 Qt 干脆**不建立连接**
    //    （实测：BindShortcuts 明明成功了，回调一行都没打印）。
    connect(m_shortcutPortal.get(), &GlobalShortcutsPortal::activated, this,
            [this](const QString &shortcutId) {
                QTextStream stream(stdout);
                stream << "HOTKEY_PORTAL_ACTIVATED id=" << shortcutId << "\n";
                stream.flush();
                // ⚠️ 不按固定 id 过滤：这个 id 是"当年发起注册时用的名字"。
                //    同一台机器上可能因为历史/调试而不同（实测：用户授权的是 first-run-demo，
                //    而默认 id 是 screenshot）—— 会话里只会来我们自己的 id，全收。
                if (m_recorder && m_recorder->isRecording())
                    stopRecording();
                else
                    startCapture();
            });
    connect(m_shortcutPortal.get(), &GlobalShortcutsPortal::shortcutsChanged, this,
            [this](const GlobalShortcutsPortal::Result &result) {
                QTextStream stream(stdout);
                QString trigger;
                for (const auto &item : result.shortcuts) {
                    if (!item.trigger.isEmpty())
                        trigger = item.trigger;
                }
                stream << "HOTKEY_PORTAL_CHANGED " << result.describe() << "\n";
                stream.flush();
                if (!trigger.isEmpty()) {
                    applyPortalShortcut(trigger);
                    emit portalFlowFinished(true, trigger);
                }
            });
    connect(m_shortcutPortal.get(), &GlobalShortcutsPortal::bound, this,
            [this](const GlobalShortcutsPortal::Result &result) {
                QTextStream stream(stdout);
                if (!result.ok) {
                    stream << "HOTKEY_PORTAL_DENIED " << result.describe() << "\n";
                    stream.flush();
                    emit portalFlowFinished(false, QString());
                    return;
                }
                QString trigger;
                for (const auto &item : result.shortcuts) {
                    if (!item.trigger.isEmpty())
                        trigger = item.trigger;
                }
                stream << "HOTKEY_PORTAL_BOUND " << result.describe() << "\n";
                stream.flush();
                if (!trigger.isEmpty()) {
                    applyPortalShortcut(trigger);
                    emit portalFlowFinished(true, trigger);
                    return;
                }
                // 应答里没带按键（KDE 的 BindShortcuts 应答常常只有 id）：
                // 再静默 ListShortcuts 一次，看用户在系统对话框里到底选了什么键。
                stream << "HOTKEY_PORTAL_RELIST（应答里没有按键，再查一次当前绑定）\n";
                stream.flush();
                m_portalRelistPending = true;
                m_shortcutPortal->listShortcuts();
            });
    connect(m_shortcutPortal.get(), &GlobalShortcutsPortal::listed, this,
            [this](const GlobalShortcutsPortal::Result &result) {
                if (!result.ok) {
                    SS_INFO() << "[hotkey-portal] 查询绑定失败（当作未授权）：" << result.error;
                    if (m_portalRelistPending) {
                        m_portalRelistPending = false;
                        QTextStream out(stdout);
                        out << "HOTKEY_PORTAL_ERROR " << result.error << "\n";
                        out.flush();
                        emit portalFlowFinished(false, QString());
                    }
                    return;
                }
                QString trigger;
                QString triggerId;
                for (const auto &shortcut : result.shortcuts) {
                    if (shortcut.trigger.isEmpty())
                        continue;
                    // 优先默认 id，其次任意一条（同上一段的理由：id 名可能不是默认那个）
                    if (trigger.isEmpty()
                        || shortcut.id == GlobalShortcutsPortal::shortcutId()) {
                        trigger = shortcut.trigger;
                        triggerId = shortcut.id;
                    }
                }
                Q_UNUSED(triggerId)
                QTextStream out(stdout);
                if (m_portalRelistPending) {
                    m_portalRelistPending = false;
                    if (trigger.isEmpty()) {
                        out << "HOTKEY_PORTAL_NO_TRIGGER（已授权，但系统没给按键 —— 可在设置里点"
                               "「更改快捷键…」，或带 --hotkey-portal-configure 重跑）\n";
                        out.flush();
                        if (m_options.hotkeyPortalConfigure)
                            configureHotkeyPortal();
                        emit portalFlowFinished(true, QString());
                    } else {
                        out << "HOTKEY_PORTAL_TRIGGER " << trigger << "\n";
                        out.flush();
                        applyPortalShortcut(trigger);
                        emit portalFlowFinished(true, trigger);
                    }
                    return;
                }
                if (m_firstRunAskPending) {
                    m_firstRunAskPending = false;
                    if (!trigger.isEmpty()) {
                        out << "HOTKEY_FIRST_RUN already-authorized trigger=" << trigger
                            << "（不再打扰）\n";
                        out.flush();
                        return;
                    }
                    // 真的没绑定过 → 问一次：这次会弹系统授权对话框，由用户决定
                    m_settings->setPortalHotkeyAsked(true); // 只问一次（无论结果）
                    out << "HOTKEY_FIRST_RUN asking（首次运行：向系统申请注册，会弹授权对话框）\n";
                    out.flush();
                    requestHotkeyPortal();
                    return;
                }
                // 常驻启动时的静默自检：只报状态，不弹任何窗
                if (!m_portalStateKnown) {
                    m_portalStateKnown = true;
                    // 门户没给绑定 → 走 KGlobalAccel（按配置里的键）；给了 → 门户说了算
                    if (trigger.isEmpty())
                        applyHotkey(m_settings ? m_settings->hotkey() : QString());
                }
                if (!trigger.isEmpty() && !m_portalHotkeyActive) {
                    m_portalHotkeyActive = true;
                    // 关键：把这条快捷键在**本会话**里 bind 一次，否则门户不会派发 Activated
                    // （只建会话+查绑定是收不到按键的）。已经授权过 → 这一步是静默的；
                    // preferred_trigger 直接用它**现在生效的键**，不会改动用户的选择。
                    if (!m_portalBoundInSession) {
                        m_portalBoundInSession = true;
                        GlobalShortcutsPortal::Shortcut existing;
                        existing.id = triggerId.isEmpty() ? GlobalShortcutsPortal::shortcutId()
                                                          : triggerId;
                        existing.description = QStringLiteral("截图（SeaShot 截图）");
                        existing.preferredTrigger = GlobalShortcutsPortal::toXdgTrigger(trigger);
                        out << "HOTKEY_PORTAL_BIND_SESSION id=" << existing.id
                            << " trigger=" << existing.preferredTrigger
                            << "（让门户在本会话里派发按键）\n";
                        out.flush();
                        ensureShortcutPortal()->requestBind({existing}, 30000);
                    }
                    // 解绑 KGlobalAccel 那条：同一个键只保留一条注册，系统设置里也只该有一条
                    if (m_backend) {
                        QString unbindError;
                        m_backend->setHotkey(QKeySequence(), &unbindError);
                    }
                    if (m_tray)
                        m_tray->setHotkeyLabel(trigger + QStringLiteral("（门户）"));
                    out << "HOTKEY_PORTAL_WINS 停止注册 KGlobalAccel（避免两条快捷键）\n";
                    out.flush();
                }
                out << (trigger.isEmpty()
                            ? QStringLiteral("HOTKEY_PORTAL_STATE not-authorized")
                            : QStringLiteral("HOTKEY_PORTAL_STATE authorized id=%1 trigger=%2")
                                  .arg(triggerId, trigger))
                    << "\n";
                out.flush();
            });
    return m_shortcutPortal.get();
}

void Application::requestHotkeyPortal()
{
    QTextStream out(stdout);
    uint version = 0;
    QString error;
    if (!GlobalShortcutsPortal::available(&version, &error)) {
        out << "HOTKEY_PORTAL_ERROR " << error << "\n";
        out.flush();
        emit portalFlowFinished(false, QString());
        return;
    }
    GlobalShortcutsPortal *portal = ensureShortcutPortal();
    GlobalShortcutsPortal::Shortcut shortcut;
    shortcut.id = m_options.hotkeyPortalId.isEmpty() ? GlobalShortcutsPortal::shortcutId()
                                                     : m_options.hotkeyPortalId;
    shortcut.description = QStringLiteral("截图（SeaShot 截图）");
    // 把期望的按键一起发过去：系统授权对话框会**预填**这个键，用户直接允许即可；
    // 也可以当场改成别的键。传空的话对话框里就是空的，用户得自己敲一个（体验差）。
    const QString preferred = (m_settings && !m_settings->hotkey().isEmpty())
        ? m_settings->hotkey()
        : QStringLiteral("Ctrl+Alt+S");
    shortcut.preferredTrigger = GlobalShortcutsPortal::toXdgTrigger(preferred);
    out << "HOTKEY_PORTAL_REQUESTING id=" << shortcut.id << "\n";
    out.flush();
    portal->requestBind({shortcut}, qMax(1000, m_options.hotkeyPortalMs));
}

void Application::configureHotkeyPortal()
{
    ensureShortcutPortal()->configure();
}

void Application::applySettings()
{

    if (auto *wayland = dynamic_cast<WaylandBackend *>(m_backend.get()))
        wayland->setKeepShadowMargin(m_settings->keepShadowMargin());

    // 托盘：跟随「常驻托盘」开关创建/销毁
    if (m_settings->trayPersistent() && !m_tray) {
        m_tray = std::make_unique<Tray>();
        connect(m_tray.get(), &Tray::captureRequested, this, &Application::startCapture);
        connect(m_tray.get(), &Tray::recordToggleRequested, this, [this]() {
            if (m_recorder && m_recorder->isRecording())
                stopRecording();
            else
                startRecordFlow();
        });
        connect(m_tray.get(), &Tray::settingsRequested, this, &Application::openSettings);
        connect(m_tray.get(), &Tray::shortcutSettingsRequested, this, [this]() {
            const QString program = QStandardPaths::findExecutable(QStringLiteral("systemsettings"));
            if (program.isEmpty()) {
                SS_WARN() << "[app] 找不到 systemsettings";
                return;
            }
            QProcess::startDetached(program, {QStringLiteral("kcm_keys")});
        });
        connect(m_tray.get(), &Tray::portalHotkeyRequested, this,
                &Application::requestHotkeyPortal);
        connect(m_tray.get(), &Tray::saveExportRequested, this,
                [this]() { openSaveExport(false); });
        connect(m_tray.get(), &Tray::quitRequested, qApp, &QCoreApplication::quit);
        SS_INFO() << "[app] 托盘已创建";
    } else if (!m_settings->trayPersistent() && m_tray) {
        m_tray.reset();
        SS_INFO() << "[app] 托盘已移除（配置里关掉了常驻托盘）";
    }

    applyHotkey(m_settings->hotkey());
}

void Application::applyHotkey(const QString &sequence)
{
    if (!m_backend)
        return;
    if (!m_portalStateKnown && m_shortcutPortal) {
        // 门户状态还没回来：先别注册，等 listed 结果出来再决定（见 listed 处理）
        SS_INFO() << "[app] 等门户状态确定后再决定是否注册 KGlobalAccel";
        return;
    }
    if (m_portalHotkeyActive) {
        // 门户那条路已经拿到绑定：这里再注册一次就会出现"两条 SeaShot 快捷键"，
        // 而且可能抢同一个键 —— 交给门户处理（想改用 KGlobalAccel 就 --set-hotkey）。
        SS_INFO() << "[app] 已由门户提供全局快捷键，跳过 KGlobalAccel 注册" << sequence;
        return;
    }
    if (m_hotkeyApplied && sequence == m_appliedHotkey)
        return; // 配置界面里敲字会频繁触发，避免反复改注册
    QString error;
    if (!m_backend->setHotkey(QKeySequence(sequence), &error)) {
        SS_WARN() << "[app] 快捷键设置失败:" << error;
        return;
    }
    m_appliedHotkey = sequence;
    m_hotkeyApplied = true;
    if (m_tray)
        m_tray->setHotkeyLabel(sequence);
    SS_INFO() << "[app] 快捷键已更新:" << (sequence.isEmpty() ? QStringLiteral("(未绑定)") : sequence)
              << "实际生效:" << m_backend->hotkeyStatus();
}

void Application::openSettings()
{
    if (m_settingsDialog) {
        m_settingsDialog->show();
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto *dialog = new SettingsDialog(m_settings.get(), m_backend.get(), nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    // 诊断/取证：把界面直接渲染成 PNG（不受其它窗口遮挡影响）
    if (!qEnvironmentVariable("SEASHOT_UI_GRAB").isEmpty()) {
        dialog->show();
        QTimer::singleShot(200, this, [this, dialog]() {
            const QString path = qEnvironmentVariable("SEASHOT_UI_GRAB");
            dialog->grab().save(path);
            QTextStream(stdout) << "UI_GRAB " << path << " size=" << dialog->size().width() << "x"
                                << dialog->size().height() << "\n";
            QTextStream(stdout).flush();
            QCoreApplication::quit();
        });
        return;
    }
    m_settingsDialog = dialog;
    connect(dialog, &QObject::destroyed, this, [this]() { m_settingsDialog = nullptr; });
    connect(dialog, &SettingsDialog::hotkeyPortalRequested, this,
            &Application::requestHotkeyPortal);
    connect(dialog, &SettingsDialog::hotkeyPortalConfigureRequested, this,
            &Application::configureHotkeyPortal);
    connect(dialog, &SettingsDialog::hotkeyChanged, this, &Application::applyHotkey);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    QTextStream(stdout) << "SETTINGS_OPENED file=" << m_settings->configFilePath() << "\n";
    QTextStream(stdout).flush();
}

int Application::runPrintConfig()
{
    // 只打印**当前真实生效值**，不在这里注册（打印配置不该改全局状态）
    QTextStream out(stdout);
    out << "CONFIG file=" << m_settings->configFilePath() << "\n";
    out << "CONFIG copyAfterCapture=" << (m_settings->copyAfterCapture() ? 1 : 0) << "\n";
    out << "CONFIG openEditorAfterCapture=" << (m_settings->openEditorAfterCapture() ? 1 : 0) << "\n";
    out << "CONFIG autoSaveAfterCapture=" << (m_settings->autoSaveAfterCapture() ? 1 : 0) << "\n";
    out << "CONFIG keepShadowMargin=" << (m_settings->keepShadowMargin() ? 1 : 0) << "\n";
    out << "CONFIG saveDir=" << m_settings->saveDir() << "\n";
    out << "CONFIG filenameTemplate=" << m_settings->filenameTemplate() << "\n";
    out << "CONFIG hotkey=" << (m_settings->hotkey().isEmpty() ? QStringLiteral("(none)")
                                                               : m_settings->hotkey())
        << "\n";
    out << "CONFIG hotkeyActual=" << (m_backend ? m_backend->hotkeyStatus() : QStringLiteral("-")) << "\n";
    out << "CONFIG gifFps=" << m_settings->gifFps() << "\n";
    out << "CONFIG toastAutoHideMs=" << m_settings->toastAutoHideMs() << "\n";
    out << "CONFIG trayPersistent=" << (m_settings->trayPersistent() ? 1 : 0) << "\n";
    out << "CONFIG backend=" << (m_backend ? m_backend->name() : QStringLiteral("none")) << "\n";
    out.flush();
    return 0;
}

void Application::startCapture()
{
    if (!m_backend || !m_backendError.isEmpty()) {
        SS_WARN() << "[app] 无法开始截图:" << m_backendError;
        QTextStream(stdout) << "CAPTURE_ERROR " << m_backendError << "\n";
        return;
    }
    m_recordIntent = false;
    m_pendingAction = Overlay::ConfirmAction::Plain;
    m_overlay->setIntent(Overlay::Intent::Screenshot);
    // Phase A1：交互式截图才开就地标注（脚本式调用压根不走 startCapture）。
    // SEASHOT_INPLACE_ANNOTATE=0 可退回"选完直接截图"的旧行为（A/B 对比/应急）。
    const bool inplaceAnnotate =
        qEnvironmentVariable("SEASHOT_INPLACE_ANNOTATE") != QLatin1String("0");
    m_overlay->setAnnotationEnabled(inplaceAnnotate);
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
    if (m_recordIntent) {
        // 录制：直接用选中的区域/窗口外框开录
        m_overlay->stopSelection();
        beginRecording(result);
        return;
    }
    m_pendingSelection = result;

    // Phase A1：交互式截图**不退出覆盖层**，就地冻结屏幕并进入标注阶段
    // （对齐 QQ/PixPin：选完直接在屏幕上标注，不打开独立窗口）。
    if (m_overlay->annotationEnabled()) {
        enterAnnotationPhase();
        return;
    }
    finishSelectionPhase();
}

void Application::enterAnnotationPhase()
{
    // 冻结屏幕必须**不含覆盖层自己**：支持自排除时可以直接抓（覆盖层不入镜），
    // 否则先把覆盖层藏起来再抓。
    // Phase 0 实测：隐藏后 16ms 抓到的仍可能是含覆盖层的陈旧帧，所以留 250ms。
    const bool selfExcluded = m_selfExclusion;
    if (!selfExcluded)
        m_overlay->hide();

    const int delayMs = selfExcluded ? 0 : 250;
    QTimer::singleShot(delayMs, this, [this]() {
        if (!m_backend) {
            finishSelectionPhase();
            return;
        }
        const QRect geometry = virtualGeometry();
        SS_INFO() << "[app] 抓取冻结屏幕" << geometry << "自排除=" << m_selfExclusion;
        m_backend->captureRect(QRectF(geometry), [this](const CaptureImage &image) {
            if (!image.ok()) {
                // 拿不到冻结图就不硬撑：退回原来的"选完直接截图"行为，截图本身仍然可用
                SS_WARN() << "[app] 冻结屏幕失败，退回直接截图：" << image.error;
                finishSelectionPhase();
                return;
            }
            m_overlay->beginAnnotating(m_pendingSelection, image.image, image.scale);
        });
    });
}

void Application::finishSelectionPhase()
{
    if (m_overlay->isAnnotating()) {
        // 交付用户刚才看到的冻结画面，后台窗口变化不会影响结果。
        const CaptureImage image = m_overlay->annotatedCapture();
        m_lastAnchorRect = m_pendingSelection.logicalRect.toAlignedRect();
        const int count = m_overlay->document().count();
        m_overlay->stopSelection();
        if (count > 0) {
            QTextStream(stdout) << "ANNOTATED count=" << count << " size=" << image.image.width()
                                << "x" << image.image.height() << "\n";
        }
        finishCapture(image);
        return;
    }
    // 未进入标注阶段时，隐藏覆盖层后按选区抓取实时画面。
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

    const QImage &result = m_lastImage;

    // Phase B：Ctrl+C / Ctrl+S 是"明确要求"，覆盖设置里的开关；Enter/✓ 走设置
    const Overlay::ConfirmAction action = m_pendingAction;
    m_pendingAction = Overlay::ConfirmAction::Plain;

    QString copyError;
    bool copied = false;
    if (action == Overlay::ConfirmAction::Copy || (!m_options.noCopy && m_settings->copyAfterCapture()))
        copied = Clipboard::copyImage(result, &copyError);

    QString savedPath = m_options.saveTo;
    QString saveError;
    if (savedPath.isEmpty()
        && (action == Overlay::ConfirmAction::Save || m_settings->autoSaveAfterCapture()))
        savedPath = m_settings->defaultSavePath();
    if (!savedPath.isEmpty() && !Settings::saveImage(savedPath, result, &saveError)) {
        SS_WARN() << "[app] 保存失败:" << saveError;
        savedPath.clear();
    }
    m_lastSavedPath = savedPath;

    const bool wantsEditor = m_options.openEditor || m_settings->openEditorAfterCapture();
    // 带 --save 视为脚本化调用：不弹浮层、交付完就退出（验收/自动化靠它保持快）
    const bool scripted = !m_options.saveTo.isEmpty();

    if (action == Overlay::ConfirmAction::Pin) {
        // 贴图：把结果钉到屏幕上（不复制/不保存/不弹浮层，和 QQ 的"钉图"一致）
        pinImage(result, m_lastAnchorRect.topLeft() + QPoint(12 * m_pins.size(), 12 * m_pins.size()));
    } else if (wantsEditor) {
        // 「截图后直接打开编辑器」：浮层就不弹了，编辑器里已有复制/保存/另存为
        openEditor(result, savedPath);
    } else if (!scripted && action != Overlay::ConfirmAction::Copy) {
        // 截图后的结果浮层已按用户要求**去掉**：结果通过剪贴板/自动保存交付，不弹任何窗。
        // （要看图/另存/编辑：托盘菜单「保存 / 导出上一次结果…」）
        QTextStream(stdout) << "UI_SILENT 截图完成（不弹窗）："
                            << (copied ? QStringLiteral("已复制到剪贴板")
                                       : QStringLiteral("未复制"))
                            << (savedPath.isEmpty() ? QString()
                                                    : QStringLiteral("，已保存 ") + savedPath)
                            << "\n";
        QTextStream(stdout).flush();
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
            // 只需记个标记：真正的等待在 run() 里用**局部 QEventLoop** 做。
            // 不要在这里 connect 到 QCoreApplication::quit：那会把退出标记全局置位，
            // 而浮层点「编辑」时也会发 dismissed → 会顺手把刚打开的编辑器一起带走。
            m_keepAliveForToast = true;
        }
    }
}

void Application::openSaveExport(bool recordingPage)
{
    if (!m_saveExport) {
        m_saveExport = std::make_unique<SaveExportDialog>();
        connect(m_saveExport.get(), &SaveExportDialog::copyScreenshotRequested, this,
                &Application::copyLast);
        connect(m_saveExport.get(), &SaveExportDialog::saveScreenshotRequested, this,
                &Application::saveToDefault);
        connect(m_saveExport.get(), &SaveExportDialog::saveScreenshotAsRequested, this,
                &Application::saveAs);
        connect(m_saveExport.get(), &SaveExportDialog::editScreenshotRequested, this,
                [this]() { openEditor(m_lastImage, m_lastSavedPath); });
        connect(m_saveExport.get(), &SaveExportDialog::exportGifRequested, this,
                &Application::exportRecordingGif);
        connect(m_saveExport.get(), &SaveExportDialog::exportMp4Requested, this,
                &Application::exportRecordingMp4);
        connect(m_saveExport.get(), &SaveExportDialog::copyGifRequested, this,
                &Application::copyRecordingGif);
    }
    m_saveExport->setScreenshot(m_lastImage, m_lastSavedPath);
    const bool hasRecording = m_recorder && m_recorder->hasPendingExport();
    if (m_recorder) {
        const GifRecorder::Stats stats = m_recorder->stats();
        m_saveExport->setRecording(stats.captured, stats.elapsedMs, QString(), hasRecording);
    } else {
        m_saveExport->setRecording(0, 0, QString(), false);
    }
    m_saveExport->showPage(recordingPage ? SaveExportDialog::Page::Recording
                                        : SaveExportDialog::Page::Screenshot);
    m_saveExport->show();
    m_saveExport->raise();
    m_saveExport->activateWindow();
}

void Application::saveAs()
{
    QTextStream out(stdout);
    if (m_lastImage.isNull())
        return;

    // 你的要求：另存为要弹对话框（不再默默落到默认目录）
    const QString suggested = m_lastSavedPath.isEmpty() ? m_settings->defaultSavePath()
                                                        : m_lastSavedPath;
    const QString path = QFileDialog::getSaveFileName(m_saveExport.get(), QStringLiteral("另存为"),
                                                      suggested,
                                                      QStringLiteral("PNG 图片 (*.png)"));
    out << "SAVE_AS_DIALOG suggested=" << suggested << " chosen="
        << (path.isEmpty() ? QStringLiteral("(cancelled)") : path) << "\n";
    out.flush();
    if (path.isEmpty())
        return;

    QString error;
    if (Settings::saveImage(path, m_lastImage, &error)) {
        m_lastSavedPath = path;
        if (m_saveExport)
            m_saveExport->setScreenshot(m_lastImage, path); // 统一窗口里刷新"已保存"
        out << "SAVED " << path << "\n";
    } else {
        out << "SAVE_ERROR " << error << "\n";
    }
    out.flush();
}

void Application::saveToDefault()
{
    QTextStream out(stdout);
    if (m_lastImage.isNull())
        return;

    // 与「另存为」的区别：直接落到设置里的保存目录 + 文件名模板，不弹对话框
    const QString path = m_settings->defaultSavePath();
    QString error;
    if (Settings::saveImage(path, m_lastImage, &error)) {
        m_lastSavedPath = path;
        if (m_saveExport)
            m_saveExport->setScreenshot(m_lastImage, path);
        out << "SAVED " << path << "\n";
    } else {
        out << "SAVE_ERROR " << error << "\n";
    }
    out.flush();
}

void Application::copyLast()
{
    if (m_lastImage.isNull())
        return;
    QString error;
    const bool ok = Clipboard::copyImage(m_lastImage, &error);
    if (ok && m_saveExport)
        m_saveExport->setScreenshot(m_lastImage, m_lastSavedPath);
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

void Application::pinImage(const QImage &image, const QPoint &anchorGlobal)
{
    if (image.isNull())
        return;
    auto *pin = new PinWindow(image, anchorGlobal);
    m_pins.append(pin);

    connect(pin, &PinWindow::copyRequested, this, [](const QImage &img) {
        QTextStream out(stdout);
        QString error;
        const bool ok = Clipboard::copyImage(img, &error);
        out << (ok ? "PIN_COPY ok mime=image/png\n"
                   : QStringLiteral("PIN_COPY_ERROR %1\n").arg(error));
        out.flush();
    });
    connect(pin, &PinWindow::saveRequested, this, [this](const QImage &img) {
        QTextStream out(stdout);
        const QString path = m_settings->defaultSavePath();
        QString error;
        const bool ok = Settings::saveImage(path, img, &error);
        out << (ok ? QStringLiteral("PIN_SAVE ok path=%1\n").arg(path)
                   : QStringLiteral("PIN_SAVE_ERROR %1\n").arg(error));
        out.flush();
    });
    connect(pin, &PinWindow::closed, this, [this, pin]() {
        m_pins.removeAll(pin);
        pin->deleteLater();
        QTextStream out(stdout);
        out << "PIN_CLOSED 剩余=" << m_pins.size() << "\n";
        out.flush();
        if (m_quitWhenPinsClosed && m_pins.isEmpty())
            QCoreApplication::quit();
    });

    pin->showPin();
    QTextStream out(stdout);
    out << "PIN_CREATED " << pin->diagnostics() << " 共" << m_pins.size() << "张\n";
    out.flush();
}

void Application::startScrollCapture(const SelectionResult &selection)
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "SCROLL_ERROR 没有可用的截图后端：" << m_backendError << "\n";
        out.flush();
        return;
    }
    ScrollCapturer::Config config;
    config.region = selection.mode == SelectionMode::Window ? selection.window.frameGeometry
                                                            : selection.logicalRect;
    if (m_options.scrollMs > 0)
        config.maxDurationMs = m_options.scrollMs;
    // 指示器放在选区外面（自排除保证它不会进帧）
    if (m_recordingIndicator)
        m_scrollCapturer->setIndicator(m_recordingIndicator.get());
    // 指示器上的「完成」/Esc = 结束并交付已拼的部分
    m_scrollCapturer->start(config);
}

void Application::onScrollFinished(const QImage &image, const QString &report)
{
    QTextStream out(stdout);
    if (image.isNull() || image.height() <= 0) {
        out << "SCROLL_ERROR 没拼出内容；" << report << "\n";
        out.flush();
        return;
    }
    m_lastImage = image;
    m_lastAnchorRect = virtualGeometry();

    QString savedPath = m_options.saveTo;
    QString saveError;
    if (savedPath.isEmpty() && m_settings->autoSaveAfterCapture())
        savedPath = m_settings->defaultSavePath();
    if (!savedPath.isEmpty() && !Settings::saveImage(savedPath, image, &saveError)) {
        SS_WARN() << "[app] 长截图保存失败:" << saveError;
        savedPath.clear();
    }
    m_lastSavedPath = savedPath;

    const bool scripted = !m_options.saveTo.isEmpty();
    if (!scripted) {
        // 截图类结果**不弹窗**（按用户要求）：这里只复制/保存；要看图从托盘里的统一窗口打开
        QString copyError;
        const bool copied = m_options.noCopy ? false : Clipboard::copyImage(image, &copyError);
        QTextStream(stdout) << "UI_SILENT 长截图完成（不弹窗）："
                            << (copied ? QStringLiteral("已复制到剪贴板")
                                       : QStringLiteral("未复制（--no-copy）"))
                            << "\n";
        QTextStream(stdout).flush();
    }

    out << "SCROLL ok size=" << image.width() << "x" << image.height() << "\n";
    out << "SCROLL_REPORT " << report << "\n";
    if (!savedPath.isEmpty())
        out << "SAVED " << savedPath << "\n";
    out.flush();

    if (isOneShot() && m_options.overlayHoldMs == 0) {
        if (scripted)
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        else
            m_keepAliveForToast = true; // 让用户能在浮层上复制/保存/贴图
    }
}

void Application::onScrollFailed(const QString &error)
{
    QTextStream out(stdout);
    out << "SCROLL_ERROR " << error << "\n";
    out.flush();
    if (isOneShot())
        QCoreApplication::exit(4);
}

int Application::runScrollCaptureCli()
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "SCROLL_ERROR 没有可用的截图后端：" << m_backendError << "\n";
        return 2;
    }
    SelectionResult selection;
    if (m_options.hasRect) {
        selection.mode = SelectionMode::Rect;
        selection.logicalRect = m_options.rect;
    } else {
        // 没给矩形就退回交互式选择（与 --record 一致）
        startCapture();
        return QCoreApplication::exec();
    }
    startScrollCapture(selection);
    return QCoreApplication::exec();
}

void Application::startOcr(const QImage &image, bool interactive)
{
    QTextStream out(stdout);
    if (image.isNull()) {
        out << "OCR_ERROR 空图像\n";
        out.flush();
        return;
    }
    QStringList languages;
    if (!m_options.ocrLangs.isEmpty())
        languages = m_options.ocrLangs.split(QLatin1Char('+'), Qt::SkipEmptyParts);

    OcrEngine::recognizeAsync(image, languages, 3, this, [this, interactive](const OcrResult &result) {
        QTextStream stream(stdout);
        if (!result.ok) {
            stream << "OCR_ERROR " << result.error << "\n";
            stream.flush();
            if (interactive) {
                auto *window = new OcrWindow();
                m_ocrWindow = window;
                window->setResult(result);
                window->setAttribute(Qt::WA_DeleteOnClose, false);
                connect(window, &OcrWindow::closed, this, [this]() {
                    if (m_ocrWindow)
                        m_ocrWindow->deleteLater();
                    m_ocrWindow = nullptr;
                    if (isOneShot())
                        QCoreApplication::quit();
                });
                window->show();
            } else if (isOneShot()) {
                QCoreApplication::exit(3);
            }
            return;
        }
        stream << "OCR ok lang=" << result.languages << " psm=" << result.psm
               << " lines=" << result.lines.size() << " elapsedMs=" << result.elapsedMs << "\n";
        stream << "OCR_TEXT_BEGIN\n" << result.text << "OCR_TEXT_END\n";
        stream.flush();

        if (!interactive) {
            if (isOneShot())
                QCoreApplication::exit(0);
            return;
        }
        auto *window = new OcrWindow();
        m_ocrWindow = window;
        window->setResult(result);
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(window, &OcrWindow::closed, this, [this]() {
            if (m_ocrWindow)
                m_ocrWindow->deleteLater();
            m_ocrWindow = nullptr;
            if (isOneShot())
                QCoreApplication::quit();
        });
        window->show();
        window->raise();
        window->activateWindow();

        // 验收用：OCR 动作模式下自动把两个复制入口都走一遍（复制全部 / 复制当前行），
        // 然后退出 —— 这样"识别 + 面板 + 复制到剪贴板"可以在脚本里自动判。
        if (qEnvironmentVariable("SEASHOT_INPLACE_DEMO_ACTION") == QLatin1String("ocr")) {
            QTimer::singleShot(700, window, [window]() {
                window->copyAll();
                window->copySelection(); // 没选中时会复制光标所在行（面板默认就在第 1 行）
                QTimer::singleShot(300, qApp, &QCoreApplication::quit);
            });
        }
    });
}

int Application::runHotkeyPortalStatus()
{
    QTextStream out(stdout);
    uint version = 0;
    QString error;
    const bool ok = GlobalShortcutsPortal::available(&version, &error);
    out << "HOTKEY_PORTAL available=" << (ok ? 1 : 0) << " version=" << version
        << " error=" << (error.isEmpty() ? QStringLiteral("(none)") : error) << "\n";
    out.flush();
    if (!ok)
        return 1;

    // 静默列一次当前绑定（没有授权过就是空列表 —— 这一步不会弹窗）
    GlobalShortcutsPortal portal;
    QEventLoop loop;
    int count = -1;
    QStringList triggers;
    connect(&portal, &GlobalShortcutsPortal::listed, this,
            [&](const GlobalShortcutsPortal::Result &result) {
                count = result.shortcuts.size();
                for (const auto &shortcut : result.shortcuts)
                    triggers << QStringLiteral("%1=%2").arg(
                        shortcut.id, shortcut.trigger.isEmpty() ? QStringLiteral("(未绑定)")
                                                                : shortcut.trigger);
                if (!result.ok)
                    triggers << QStringLiteral("error=%1").arg(result.error);
                loop.quit();
            });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    portal.listShortcuts();
    loop.exec();
    portal.closeSession();
    out << "HOTKEY_PORTAL_LIST count=" << count
        << " shortcuts=" << (triggers.isEmpty() ? QStringLiteral("(空)") : triggers.join(QLatin1Char(',')))
        << "\n";
    out.flush();
    return count >= 0 ? 0 : 1;
}

int Application::runHotkeyPortalRequest()
{
    QTextStream out(stdout);
    uint version = 0;
    QString error;
    if (!GlobalShortcutsPortal::available(&version, &error)) {
        out << "HOTKEY_PORTAL_ERROR " << error << "\n";
        out.flush();
        return 2;
    }
    int code = 1;
    QString finalTrigger;
    QEventLoop loop;
    connect(this, &Application::portalFlowFinished, &loop,
            [&](bool ok, const QString &trigger) {
                code = ok ? 0 : 3;
                finalTrigger = trigger;
                out << (ok ? "HOTKEY_PORTAL_FLOW ok=1 trigger="
                           : "HOTKEY_PORTAL_FLOW ok=0 trigger=")
                    << (trigger.isEmpty() ? QStringLiteral("(未设置)") : trigger) << "\n";
                out.flush();
                loop.quit();
            });
    // 兜底：等用户应答的上限（授权对话框/配置对话框都可能开很久）
    QTimer::singleShot(qMax(1000, m_options.hotkeyPortalMs), &loop, &QEventLoop::quit);
    requestHotkeyPortal();
    loop.exec();

    // 授权成功且要求停留时：留在这里等按键 —— 好把"授权 → 按一下 → 真的截图"验完
    if (code == 0 && m_options.hotkeyPortalListenMs != 0 && m_shortcutPortal) {
        const int listenMs = m_options.hotkeyPortalListenMs < 0 ? 3600000
                                                               : m_options.hotkeyPortalListenMs;
        out << "HOTKEY_PORTAL_LISTENING ms=" << listenMs << "（按一下那个键就会截图）\n";
        out.flush();
        QEventLoop listenLoop;
        connect(m_shortcutPortal.get(), &GlobalShortcutsPortal::activated, &listenLoop,
                [&out, &listenLoop](const QString &shortcutId) {
                    QTextStream stream(stdout);
                    stream << "HOTKEY_PORTAL_ACTIVATED id=" << shortcutId << "\n";
                    stream.flush();
                    QTimer::singleShot(800, &listenLoop, &QEventLoop::quit);
                });
        QTimer::singleShot(listenMs, &listenLoop, &QEventLoop::quit);
        listenLoop.exec();
    }
    if (m_shortcutPortal)
        m_shortcutPortal->closeSession();
    return code;
}

void Application::applyPortalShortcut(const QString &trigger)
{
    // 授权成功后：把结果记进配置（供界面/CLI 显示），并接上"按下就截图"。
    // 注意 portal 的**授权**由桌面按 app id 记住，我们这里只存"实际生效的按键"用于展示。
    if (!m_settings)
        return;
    if (!trigger.isEmpty())
        m_settings->setHotkey(trigger);
    QTextStream out(stdout);
    out << "HOTKEY_PORTAL_APPLIED trigger="
        << (trigger.isEmpty() ? QStringLiteral("(未返回按键，需在系统对话框里确认)") : trigger)
        << "\n";
    out.flush();
}

int Application::runOcrCli(const QString &path)
{
    const QImage image(path);
    if (image.isNull()) {
        QTextStream(stderr) << "OCR_ERROR 无法读取 " << path << "\n";
        return 2;
    }
    QStringList languages;
    if (!m_options.ocrLangs.isEmpty())
        languages = m_options.ocrLangs.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    const OcrResult result = OcrEngine::recognize(image, languages);
    QTextStream out(stdout);
    if (!result.ok) {
        out << "OCR_ERROR " << result.error << "\n";
        return 3;
    }
    out << "OCR ok lang=" << result.languages << " psm=" << result.psm
        << " lines=" << result.lines.size() << " elapsedMs=" << result.elapsedMs << "\n";
    out << "OCR_TEXT_BEGIN\n" << result.text << "OCR_TEXT_END\n";
    out.flush();
    return 0;
}

int Application::runPinFile(const QString &path)
{
    QTextStream out(stdout);
    const QImage image(path);
    if (image.isNull()) {
        out << "PIN_ERROR 无法读取 " << path << "\n";
        return 2;
    }
    // 默认贴在主屏左上方偏下一点的位置（与 QQ 贴图类似）
    const QRect virtualRect = virtualGeometry();
    m_quitWhenPinsClosed = true; // 关掉最后一张贴图就退出（--pin 是独立进程）
    pinImage(image, virtualRect.topLeft() + QPoint(120, 120));
    return QCoreApplication::exec();
}

int Application::runPinDemo(const QString &outPrefix)
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "PIN_DEMO_ERROR 需要可用的截图后端（用来验证贴图在屏幕上的存在）: "
            << m_backendError << "\n";
        return 2;
    }

    // 用**纯色块**当贴图：验收脚本可以直接在屏幕截图上按颜色找它，
    // 从而证明"贴图真的在合成结果里 / 真的盖在全屏窗口之上"。
    QImage pin(240, 160, QImage::Format_RGB32);
    pin.fill(QColor(0xff, 0x2d, 0x55));
    {
        QPainter painter(&pin);
        painter.setPen(Qt::white);
        painter.drawRect(pin.rect().adjusted(0, 0, -1, -1));
        painter.drawText(pin.rect(), Qt::AlignCenter, QStringLiteral("PIN"));
    }

    const QRect virtualRect = virtualGeometry();
    const QPoint start = virtualRect.topLeft() + QPoint(300, 260);
    pinImage(pin, start);
    if (m_pins.isEmpty()) {
        out << "PIN_DEMO_ERROR 贴图窗口没建起来\n";
        return 2;
    }
    PinWindow *first = m_pins.first();
    const QRect rect0 = first->pinRect();
    auto pinRectsForCheck = std::make_shared<QVector<QRect>>();

    QTimer::singleShot(600, this, [this, first, outPrefix, start, rect0, virtualRect, pinRectsForCheck]() {
        QTextStream out(stdout);
        auto send = [](QWidget *widget, QEvent::Type type, const QPointF &pos,
                       Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, pos, widget->mapToGlobal(pos.toPoint()), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(widget, &event);
        };

        // 1) 拖动：按下并移动 (70,50)
        const QPointF from(rect0.width() / 2.0, rect0.height() / 2.0);
        send(first, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        send(first, QEvent::MouseMove, from + QPointF(70, 50), Qt::NoButton, Qt::LeftButton);
        send(first, QEvent::MouseButtonRelease, from + QPointF(70, 50), Qt::LeftButton,
             Qt::NoButton);
        const QRect rect1 = first->pinRect();
        const bool dragOk = (rect1.topLeft() - rect0.topLeft()) == QPoint(70, 50);

        // 2) 滚轮缩放（以图心为锚点）
        const double zoomBefore = first->zoom();
        QWheelEvent wheel(QPointF(rect1.width() / 2.0, rect1.height() / 2.0),
                          first->mapToGlobal(rect1.center()), QPoint(0, 0), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(first, &wheel);
        const QRect rect2 = first->pinRect();
        const bool zoomOk = first->zoom() > zoomBefore && rect2.width() > rect1.width();

        // 3) 右键菜单 → 复制（走的是和用户一样的鼠标路径）
        out << "PIN_DEMO drag=" << (dragOk ? 1 : 0) << " zoom=" << (zoomOk ? 1 : 0)
            << " zoomValue=" << first->zoom() << " rect=" << rect2.x() << "," << rect2.y()
            << " " << rect2.width() << "x" << rect2.height() << "\n";
        const QPointF menuPoint(rect2.width() * 0.4, rect2.height() * 0.4);
        send(first, QEvent::MouseButtonPress, menuPoint, Qt::RightButton, Qt::RightButton);
        send(first, QEvent::MouseButtonRelease, menuPoint, Qt::RightButton, Qt::NoButton);
        const bool menuOpened = first->menuOpen();
        // 菜单项位置：从菜单矩形里取（PinWindow 自己算的）——用 menuItemAt 反查
        PinWindow::MenuItem copyItem = PinWindow::MenuItem::None;
        QPoint copyPoint;
        for (int y = 0; y < first->height(); ++y) {
            for (int x = 0; x < first->width(); x += 4) {
                if (first->menuItemAt(QPoint(x, y)) == PinWindow::MenuItem::Copy) {
                    copyItem = PinWindow::MenuItem::Copy;
                    copyPoint = QPoint(x, y); // 这个点已被 menuItemAt 验证落在「复制」上
                }
            }
            if (copyItem != PinWindow::MenuItem::None)
                break;
        }
        bool copyOk = false;
        out << "PIN_DEMO menuProbe point=" << copyPoint.x() << "," << copyPoint.y()
            << " itemAtPoint=" << static_cast<int>(first->menuItemAt(copyPoint))
            << " menuOpenBefore=" << (first->menuOpen() ? 1 : 0) << "\n";
        out.flush();
        if (copyItem != PinWindow::MenuItem::None) {
            send(first, QEvent::MouseButtonPress, copyPoint, Qt::LeftButton, Qt::LeftButton);
            send(first, QEvent::MouseButtonRelease, copyPoint, Qt::LeftButton, Qt::NoButton);
            copyOk = !first->menuOpen();
        }

        // 「保存到默认目录」也走一遍（默认目录 = ~/Pictures/Screenshots，验收脚本会删掉产物）
        bool saveOk = false;
        {
            send(first, QEvent::MouseButtonPress, menuPoint, Qt::RightButton, Qt::RightButton);
            send(first, QEvent::MouseButtonRelease, menuPoint, Qt::RightButton, Qt::NoButton);
            PinWindow::MenuItem saveItem = PinWindow::MenuItem::None;
            QPoint savePoint;
            for (int y = 0; y < first->height() && saveItem == PinWindow::MenuItem::None; ++y) {
                for (int x = 0; x < first->width(); x += 4) {
                    if (first->menuItemAt(QPoint(x, y)) == PinWindow::MenuItem::Save) {
                        saveItem = PinWindow::MenuItem::Save;
                        savePoint = QPoint(x, y);
                        break;
                    }
                }
            }
            if (saveItem != PinWindow::MenuItem::None) {
                send(first, QEvent::MouseButtonPress, savePoint, Qt::LeftButton, Qt::LeftButton);
                send(first, QEvent::MouseButtonRelease, savePoint, Qt::LeftButton, Qt::NoButton);
                saveOk = !first->menuOpen();
            }
        }


        // 4) 第二张并存
        QImage second(160, 120, QImage::Format_RGB32);
        second.fill(QColor(0x2d, 0xff, 0x88));
        pinImage(second, start + QPoint(320, 40));
        const bool twoPins = m_pins.size() == 2;

        out << "PIN_DEMO menu=" << (menuOpened ? 1 : 0) << " copy=" << (copyOk ? 1 : 0)
            << " menuCopyPoint=" << (copyItem != PinWindow::MenuItem::None ? 1 : 0)
            << " saveItem=" << (saveOk ? 1 : 0) << " twoPins=" << (twoPins ? 1 : 0) << "\n";
        const bool ok = dragOk && zoomOk && menuOpened && copyOk && saveOk && twoPins;

        // 6) 把两张贴图**保持一会儿**，让验收脚本用另一个进程去截屏核对位置与像素。
        //    ⚠️ 不能在本进程里截屏：同一进程的 ScreenShot2 拍不到自己的 layer-shell 表面
        //    （ISSUE-007 实测过，Toast 拖动基准也是因此才用子进程当观测者）。
        QStringList rectText;
        for (PinWindow *pin : std::as_const(m_pins)) {
            const QRect rect = pin->pinRect();
            rectText << QStringLiteral("%1,%2 %3x%4")
                            .arg(rect.x())
                            .arg(rect.y())
                            .arg(rect.width())
                            .arg(rect.height());
        }
        int holdMs = qEnvironmentVariableIntValue("SEASHOT_PIN_DEMO_HOLD");
        if (holdMs <= 0)
            holdMs = 2500;
        out << "PIN_DEMO holding=" << holdMs << " pinRects=" << rectText.join(QLatin1Char(';'))
            << "\n";
        out.flush();

        QTimer::singleShot(holdMs, this, [this, first, ok]() {
            QTextStream out(stdout);
            // 7) 关掉第一张：第二张还在；再全部关掉 → 无残留
            first->closePin();
            const bool oneLeft = m_pins.size() == 1;
            while (!m_pins.isEmpty())
                m_pins.first()->closePin();
            QCoreApplication::processEvents();
            const bool noResidue = m_pins.isEmpty();
            out << "PIN_DEMO close oneLeft=" << (oneLeft ? 1 : 0) << " noResidue="
                << (noResidue ? 1 : 0) << "\n";
            const bool allOk = ok && oneLeft && noResidue;
            out << (allOk ? "PIN_DEMO PASS" : "PIN_DEMO FAIL") << "\n";
            out.flush();
            QCoreApplication::exit(allOk ? 0 : 1);
        });
    });

    return QCoreApplication::exec();
}

int Application::runInplaceDemo(const QString &outPrefix)
{
    QTextStream out(stdout);
    if (!m_backend || !m_backendError.isEmpty()) {
        out << "INPLACE_DEMO_ERROR 就地标注演示需要可用的截图后端：" << m_backendError << "\n";
        out.flush();
        return 2;
    }

    // 走的就是交互式截图入口（annotationEnabled 已在 startCapture() 里打开）
    startCapture();

    // 合成一次真实的拖拽：直接投递 QMouseEvent，走用户的 mousePress/Move/Release 同一条路径
    const QPointF from(220, 180);
    const QPointF to(760, 560);
    QTimer::singleShot(500, this, [this, from, to]() {
        auto post = [this](QEvent::Type type, const QPointF &pos, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
            QMouseEvent event(type, pos, m_overlay->mapToGlobal(pos.toPoint()), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(m_overlay.get(), &event);
        };
        post(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        constexpr int kSteps = 8;
        for (int i = 1; i <= kSteps; ++i) {
            const double t = double(i) / kSteps;
            post(QEvent::MouseMove, from + (to - from) * t, Qt::NoButton, Qt::LeftButton);
        }
        post(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QTextStream(stdout) << "INPLACE_DEMO drag=" << from.x() << "," << from.y() << " -> "
                            << to.x() << "," << to.y() << "\n";
        QTextStream(stdout).flush();
    });

    // 拖拽 → selectionFinished → 异步抓冻结屏幕 → beginAnnotating：给这条链 2s。
    // ⚠️ 抓屏是异步的：如果 2s 后还没进 Annotating（机器忙/有别的进程在抢屏幕），
    //    就再等一会儿重试，而不是直接往下跑出一堆看不懂的失败。
    auto verification = std::make_shared<std::function<void(int)>>();
    *verification = [this, outPrefix, verification](int attempt) {
        if (!m_overlay->isAnnotating() && attempt < 20) {
            QTimer::singleShot(250, this, [verification, attempt]() { (*verification)(attempt + 1); });
            return;
        }
        [this, outPrefix]() {
        QTextStream out(stdout);
        const bool annotating = m_overlay->isAnnotating();
        const QImage frozen = m_overlay->frozenFrame();
        const QPixmap grabbed = m_overlay->grab();
        const QImage shot = grabbed.toImage();
        const double dpr = grabbed.devicePixelRatio();
        const QRectF localSelection = m_overlay->localSelectionRect();

        out << "INPLACE_DEMO " << m_overlay->annotationDiagnostics() << "\n";
        out << "INPLACE_DEMO grab=" << shot.width() << "x" << shot.height() << " dpr=" << dpr
            << "\n";
        // 诊断：拖拽本该选中 220,180 540x380。若不一样（例如外面的东西点了一下，被当成
        // "选中那个窗口"），这里直接写出来，省得对着一堆失败排查。
        {
            const QRectF got = m_overlay->localSelectionRect();
            const QRectF want(220, 180, 540, 380);
            if ((got.topLeft() - want.topLeft()).manhattanLength() > 1
                || qAbs(got.width() - want.width()) > 1 || qAbs(got.height() - want.height()) > 1) {
                out << "INPLACE_DEMO selectionMismatch got=" << qRound(got.x()) << ","
                    << qRound(got.y()) << " " << qRound(got.width()) << "x" << qRound(got.height())
                    << " want=220,180 540x380\n";
            }
        }

        if (!outPrefix.isEmpty()) {
            shot.save(outPrefix + QStringLiteral("-overlay.png"));
            frozen.save(outPrefix + QStringLiteral("-frozen.png"));
            out << "INPLACE_DEMO saved=" << outPrefix << "-overlay.png," << outPrefix
                << "-frozen.png\n";
        }

        // 判据 1：**没有新窗口** —— 可见的顶层部件应当只有覆盖层自己
        QStringList tops;
        const QList<QWidget *> allTops = QApplication::topLevelWidgets();
        for (QWidget *w : allTops) {
            if (w->isVisible())
                tops << w->windowTitle();
        }
        const bool onlyOverlay =
            tops.size() == 1 && tops.first() == QLatin1String("seashot-overlay");
        out << "INPLACE_DEMO visibleTops="
            << (tops.isEmpty() ? QStringLiteral("(none)") : tops.join(QLatin1Char(','))) << "\n";

        // 判据 2：选区内**逐像素 1:1**（内缩 4px 避开边框与手柄）
        int mismatched = -1;
        int maxDiff = -1;
        const QRect area = localSelection.toAlignedRect()
                               .adjusted(4, 4, -4, -4)
                               .intersected(frozen.rect())
                               .intersected(shot.rect());
        if (!frozen.isNull() && area.isValid() && qFuzzyCompare(dpr, 1.0)) {
            mismatched = 0;
            maxDiff = 0;
            for (int y = area.top(); y <= area.bottom(); ++y) {
                const QRgb *a = reinterpret_cast<const QRgb *>(shot.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(frozen.constScanLine(y));
                for (int x = area.left(); x <= area.right(); ++x) {
                    const int d =
                        qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                             qMax(qAbs(qBlue(a[x]) - qBlue(b[x])), qAbs(qAlpha(a[x]) - qAlpha(b[x]))));
                    if (d > 0)
                        ++mismatched;
                    maxDiff = qMax(maxDiff, d);
                }
            }
        }

        // 判据 3：8 个手柄真的画出来了（手柄中心 = 强调色 0x3daeff）
        int handleHits = 0;
        if (!frozen.isNull()) {
            const QRectF r = localSelection;
            const QVector<QPointF> handles{
                r.topLeft(),
                QPointF(r.center().x(), r.top()),
                r.topRight(),
                QPointF(r.left(), r.center().y()),
                QPointF(r.right(), r.center().y()),
                r.bottomLeft(),
                QPointF(r.center().x(), r.bottom()),
                r.bottomRight()};
            for (const QPointF &p : handles) {
                const QPoint c(qRound(p.x()), qRound(p.y()));
                if (!shot.rect().contains(c))
                    continue;
                const QColor color = shot.pixelColor(c);
                if (qAbs(color.red() - 0x3d) <= 16 && qAbs(color.green() - 0xae) <= 16
                    && qAbs(color.blue() - 0xff) <= 16)
                    ++handleHits;
            }
        }

        // 判据 4：选区**外**被压暗（kDimAlpha=120 → 原亮度 ×0.529）
        // 工具条盖在选区下方，落在它上面的探针不能用（那里量到的是工具条）
        int dimChecked = 0;
        int dimBad = 0;
        int dimSkipped = 0;
        if (!frozen.isNull() && qFuzzyCompare(dpr, 1.0)) {
            const QRect sel = localSelection.toAlignedRect();
            const QRectF barRect = m_overlay->toolbarRect();
            const QVector<QPoint> probes{
                QPoint(sel.center().x(), sel.top() - 30),
                QPoint(sel.center().x(), sel.bottom() + 30),
                QPoint(sel.left() - 30, sel.center().y()),
                QPoint(sel.right() + 30, sel.center().y())};
            constexpr double kKeep = 1.0 - 120.0 / 255.0;
            for (const QPoint &p : probes) {
                if (!shot.rect().contains(p) || !frozen.rect().contains(p)) {
                    ++dimSkipped;
                    continue;
                }
                if (barRect.contains(QPointF(p))) {
                    ++dimSkipped; // 被工具条盖住的那一条
                    continue;
                }
                ++dimChecked;
                const QColor s = shot.pixelColor(p);
                const QColor f = frozen.pixelColor(p);
                if (qAbs(s.red() - f.red() * kKeep) > 3
                    || qAbs(s.green() - f.green() * kKeep) > 3
                    || qAbs(s.blue() - f.blue() * kKeep) > 3)
                    ++dimBad;
            }
        }

        const bool sizeMatches = !frozen.isNull() && frozen.size() == m_overlay->size();
        const bool a1ok = annotating && onlyOverlay && sizeMatches && mismatched == 0
            && handleHits == 8 && dimChecked > 0 && dimBad == 0;
        out << "INPLACE_DEMO frozen=" << frozen.width() << "x" << frozen.height()
            << " surface=" << m_overlay->width() << "x" << m_overlay->height()
            << " exactMismatch=" << mismatched << " maxChannelDiff=" << maxDiff
            << " handles=" << handleHits << "/8 dimProbe=" << (dimChecked - dimBad) << "/"
            << dimChecked << " (跳过" << dimSkipped << ")\n";
        out << (a1ok ? "INPLACE_DEMO PASS" : "INPLACE_DEMO FAIL") << "\n";

        // ─────────────────────────────────────────────────
        // A2：自绘图标工具条 —— 位置、命中、切换、快捷键
        // 注意：这一步会合成鼠标点击，所以放在所有“画面像素”判据之后。
        // ─────────────────────────────────────────────────
        auto postMouse = [this](QEvent::Type type, const QPointF &pos, Qt::MouseButton button,
                                Qt::MouseButtons buttons) {
            QMouseEvent event(type, pos, m_overlay->mapToGlobal(pos.toPoint()), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(m_overlay.get(), &event);
        };
        auto clickAt = [&postMouse](const QPointF &pos) {
            postMouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
            postMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
            postMouse(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
        };

        const QRectF bar = m_overlay->toolbarRect();
        const QVector<Overlay::ToolButton> toolButtons = m_overlay->toolButtons();
        const QVector<Overlay::ActionButton> actionButtons = m_overlay->actionButtons();
        const QRectF surface(0, 0, m_overlay->width(), m_overlay->height());

        QStringList toolNames;
        QStringList keyLabels;
        for (const Overlay::ToolButton &button : toolButtons) {
            toolNames << toolName(button.kind);
            keyLabels << (button.keyNumber > 0 ? QString::number(button.keyNumber)
                                               : QStringLiteral("-"));
        }
        out << "INPLACE_DEMO toolbar=" << qRound(bar.x()) << "," << qRound(bar.y()) << " "
            << qRound(bar.width()) << "x" << qRound(bar.height())
            << " toolButtons=" << toolButtons.size() << " actionButtons=" << actionButtons.size()
            << "\n";
        out << "INPLACE_DEMO tools=" << toolNames.join(QLatin1Char('/')) << " keys="
            << keyLabels.join(QLatin1Char('/')) << " " << Overlay::toolIconDiagnostics() << "\n";

        const bool barOutsideSelection = !bar.intersects(localSelection);
        const bool barInsideSurface = surface.contains(bar);

        // 逐个点一下工具按钮：命中数 + 切换结果（与用户点击同一条路径）
        int toolHits = 0;
        QStringList switched;
        for (const Overlay::ToolButton &button : toolButtons) {
            clickAt(button.rect.center());
            const ToolKind now = m_overlay->currentTool();
            switched << toolName(now);
            if (now == button.kind)
                ++toolHits;
        }
        out << "INPLACE_DEMO toolHits=" << toolHits << "/" << toolButtons.size()
            << " switched=" << switched.join(QLatin1Char('/')) << "\n";

        // 快捷键 1–9
        int keyHits = 0;
        for (int i = 0; i < toolButtons.size() && i < 9; ++i) {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_1 + i, Qt::NoModifier);
            QCoreApplication::sendEvent(m_overlay.get(), &key);
            if (m_overlay->currentTool() == toolButtons.at(i).kind)
                ++keyHits;
        }
        out << "INPLACE_DEMO keyHits=" << keyHits << "/"
            << qMin(9, toolButtons.size()) << "\n";

        // 落在工具条**空隙**上（按钮之间 2px）不应切换工具（Phase 0 的判定策略）
        const ToolKind beforeGap = m_overlay->currentTool();
        bool gapQuiet = false;
        if (toolButtons.size() >= 2) {
            const QPointF gap(toolButtons.at(0).rect.right() + 1.0,
                              toolButtons.at(0).rect.center().y());
            clickAt(gap);
            gapQuiet = (m_overlay->currentTool() == beforeGap);
        }

        // hover：鼠标移到第 4 个按钮上，悬停格应为它
        int hoverCellIndex = -1;
        if (toolButtons.size() >= 4) {
            postMouse(QEvent::MouseMove, toolButtons.at(3).rect.center(), Qt::NoButton,
                      Qt::NoButton);
            hoverCellIndex = m_overlay->hoveredCellIndex();
            postMouse(QEvent::MouseMove, QPointF(2, 2), Qt::NoButton, Qt::NoButton); // 移开
        }
        const bool hoverWorks = (hoverCellIndex == 3);

        // A2 附带判据：工具条自动避让（下方放不下 → 上方；贴右边缘 → 往里收）
        SelectionResult probeSelection;
        probeSelection.mode = SelectionMode::Rect;
        probeSelection.logicalRect = QRectF(600, 900, 400, 160); // bottom=1060：下方放不下
        m_overlay->beginAnnotating(probeSelection, frozen, 1.0);
        const QRectF aboveBar = m_overlay->toolbarRect();
        const bool barGoesAbove =
            aboveBar.bottom() <= m_overlay->localSelectionRect().top() + 0.5;

        probeSelection.logicalRect = QRectF(1800, 40, 110, 120); // 贴着右边缘
        m_overlay->beginAnnotating(probeSelection, frozen, 1.0);
        const QRectF edgeBar = m_overlay->toolbarRect();
        const bool barClamped = edgeBar.right() <= m_overlay->width() - 8 + 0.5
            && edgeBar.left() >= 8 - 0.5;

        // 回到原来的选区，便于证据与日志保持一致
        m_overlay->beginAnnotating(m_pendingSelection, frozen, 1.0);

        // ─────────────────────────────────────────────────
        // A3：标注的绘制 / 命中 / 撤销（合成鼠标+键盘，与用户同一条路径）
        // ─────────────────────────────────────────────────
        auto postKey = [this](int key, const QString &text,
                              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
            QCoreApplication::sendEvent(m_overlay.get(), &event);
        };
        auto selectToolByButton = [&](ToolKind kind) {
            for (const Overlay::ToolButton &button : m_overlay->toolButtons()) {
                if (button.kind == kind) {
                    clickAt(button.rect.center());
                    return;
                }
            }
        };
        auto dragLocal = [&](const QPointF &from, const QPointF &to) {
            postMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            constexpr int kDragSteps = 6;
            for (int i = 1; i <= kDragSteps; ++i) {
                const double t = double(i) / kDragSteps;
                postMouse(QEvent::MouseMove, from + (to - from) * t, Qt::NoButton,
                          Qt::LeftButton);
            }
            postMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        };

        const QPointF origin = localSelection.topLeft();
        struct ShapeCase {
            ToolKind kind;
            QPointF from;
            QPointF to;
        };
        const QVector<ShapeCase> shapeCases{
            {ToolKind::Rectangle, {30, 30}, {200, 120}},
            {ToolKind::Ellipse, {250, 40}, {400, 140}},
            {ToolKind::Arrow, {60, 160}, {240, 260}},
            {ToolKind::Highlight, {300, 180}, {470, 215}},
            {ToolKind::Mosaic, {260, 250}, {420, 340}},
            {ToolKind::Blur, {60, 290}, {200, 360}},
        };
        int shapeHits = 0;
        QStringList drawnKinds;
        for (const ShapeCase &shapeCase : shapeCases) {
            selectToolByButton(shapeCase.kind);
            const int before = m_overlay->document().count();
            dragLocal(origin + shapeCase.from, origin + shapeCase.to);
            const int after = m_overlay->document().count();
            if (after == before + 1
                && m_overlay->document().at(after - 1).kind == shapeCase.kind) {
                ++shapeHits;
                drawnKinds << toolName(shapeCase.kind);
            }
        }
        out << "INPLACE_DEMO A3 shapes=" << shapeHits << "/" << shapeCases.size()
            << " drawn=" << drawnKinds.join(QLatin1Char('/')) << "\n";

        // 画笔（多点折线）
        bool penOk = false;
        {
            selectToolByButton(ToolKind::Pen);
            const int before = m_overlay->document().count();
            const QPointF start = origin + QPointF(430, 60);
            postMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 8; ++i) {
                postMouse(QEvent::MouseMove,
                          origin + QPointF(430 + i * 8.0, 60 + (i % 2 ? 16.0 : 0.0)),
                          Qt::NoButton, Qt::LeftButton);
            }
            postMouse(QEvent::MouseButtonRelease, origin + QPointF(500, 76), Qt::LeftButton,
                      Qt::NoButton);
            penOk = m_overlay->document().count() == before + 1
                && m_overlay->document().at(before).kind == ToolKind::Pen
                && m_overlay->document().at(before).points.size() >= 5;
        }

        // 序号（单击即得 1）
        bool numberOk = false;
        {
            selectToolByButton(ToolKind::Number);
            const int before = m_overlay->document().count();
            clickAt(origin + QPointF(490, 300));
            numberOk = m_overlay->document().count() == before + 1
                && m_overlay->document().at(before).kind == ToolKind::Number
                && m_overlay->document().at(before).text == QLatin1String("1");
        }

        // 文字（自绘输入：点击 + 合成按键 + 回车）
        bool textOk = false;
        {
            selectToolByButton(ToolKind::Text);
            const int before = m_overlay->document().count();
            clickAt(origin + QPointF(452, 348));
            postKey(Qt::Key_H, QStringLiteral("H"));
            postKey(Qt::Key_I, QStringLiteral("i"));
            postKey(Qt::Key_Space, QStringLiteral(" "));
            postKey(Qt::Key_3, QStringLiteral("3"));
            postKey(Qt::Key_Return, QStringLiteral("\r"));
            textOk = m_overlay->document().count() == before + 1
                && m_overlay->document().at(before).kind == ToolKind::Text
                && m_overlay->document().at(before).text == QLatin1String("Hi 3");
        }
        out << "INPLACE_DEMO A3 pen=" << (penOk ? 1 : 0) << " number=" << (numberOk ? 1 : 0)
            << " text=" << (textOk ? 1 : 0) << " textValue=\""
            << m_overlay->document().at(m_overlay->document().count() - 1).text << "\""
            << " total=" << m_overlay->document().count() << "\n";

        // Ctrl+Z / Ctrl+Shift+Z
        const int countBeforeUndo = m_overlay->document().count();
        postKey(Qt::Key_Z, QString(), Qt::ControlModifier);
        const int afterUndo = m_overlay->document().count();
        postKey(Qt::Key_Z, QString(), Qt::ControlModifier | Qt::ShiftModifier);
        const int afterRedo = m_overlay->document().count();
        const bool undoOk = afterUndo == countBeforeUndo - 1 && afterRedo == countBeforeUndo;
        out << "INPLACE_DEMO A3 undo=" << (undoOk ? 1 : 0) << " 计数 " << countBeforeUndo
            << "->" << afterUndo << "->" << afterRedo << "\n";

        // 选择工具：命中 + 拖动移动（矩形上边中点，不是手柄）
        bool moveOk = false;
        QRectF movedBounds;
        {
            selectToolByButton(ToolKind::Select);
            const QRectF beforeMove = m_overlay->document().at(0).bounds();
            const QPointF grip = origin + QPointF(115, 30);
            dragLocal(grip, grip + QPointF(45, 28));
            movedBounds = m_overlay->document().at(0).bounds();
            const QPointF delta = movedBounds.center() - beforeMove.center();
            moveOk = qAbs(delta.x() - 45.0) <= 2.0 && qAbs(delta.y() - 28.0) <= 2.0
                && m_overlay->selectedIndex() == 0;
        }
        out << "INPLACE_DEMO A3 move=" << (moveOk ? 1 : 0) << " bounds="
            << qRound(movedBounds.x()) << "," << qRound(movedBounds.y()) << " "
            << qRound(movedBounds.width()) << "x" << qRound(movedBounds.height()) << "\n";

        // Delete 删除选中的标注
        const int countBeforeDelete = m_overlay->document().count();
        postKey(Qt::Key_Delete, QString());
        const bool deleteOk = m_overlay->document().count() == countBeforeDelete - 1;
        out << "INPLACE_DEMO A3 delete=" << (deleteOk ? 1 : 0) << " 计数 " << countBeforeDelete
            << "->" << m_overlay->document().count() << "\n";

        // 工具条上拖拽不应产生标注（Phase 0 的判定策略在 A3 仍然成立）
        bool toolbarDragQuiet = false;
        {
            const int before = m_overlay->document().count();
            const QPointF center = m_overlay->toolbarRect().center();
            dragLocal(center - QPointF(40, 0), center + QPointF(40, 0));
            toolbarDragQuiet = m_overlay->document().count() == before;
        }

        // 预览 == 导出：屏幕上选区内的像素必须就是 renderAnnotations() 的输出
        // （先把鼠标移开工具条：悬停气泡是临时 UI，会在选区里画东西）
        postMouse(QEvent::MouseMove, QPointF(2, 2), Qt::NoButton, Qt::NoButton);
        int previewMismatch = -1;
        QImage onScreenImage;
        {
            const QImage onScreen = m_overlay->grab().toImage(); // 会重绘，顺带刷新合成缓存
            onScreenImage = onScreen;
            const QImage exported = m_overlay->renderSelection(m_overlay->frozenSelection());
            // 逐像素比：屏幕用全局坐标，导出图用选区局部坐标（两边都是“选区内容”）
            const QRect selectionRect = localSelection.toAlignedRect();
            const QRect inner(6, 6, exported.width() - 12, exported.height() - 12);
            if (!exported.isNull() && inner.isValid() && qFuzzyCompare(dpr, 1.0)) {
                previewMismatch = 0;
                int firstBadX = -1;
                int firstBadY = -1;
                for (int ly = inner.top(); ly <= inner.bottom(); ++ly) {
                    const int sy = ly + selectionRect.top();
                    if (sy < 0 || sy >= onScreen.height())
                        continue;
                    const QRgb *a = reinterpret_cast<const QRgb *>(onScreen.constScanLine(sy));
                    const QRgb *b = reinterpret_cast<const QRgb *>(exported.constScanLine(ly));
                    for (int lx = inner.left(); lx <= inner.right(); ++lx) {
                        const int sx = lx + selectionRect.left();
                        if (sx < 0 || sx >= onScreen.width())
                            continue;
                        if (a[sx] != b[lx]) {
                            ++previewMismatch;
                            if (firstBadX < 0) {
                                firstBadX = sx;
                                firstBadY = sy;
                            }
                        }
                    }
                }
                if (previewMismatch > 0 && !outPrefix.isEmpty()) {
                    // 留着现场：屏幕上的选区 vs 导出实现输出
                    onScreen.copy(selectionRect).save(outPrefix + QStringLiteral("-preview.png"));
                    exported.save(outPrefix + QStringLiteral("-export.png"));
                    const QRgb sa = reinterpret_cast<const QRgb *>(
                        onScreen.constScanLine(firstBadY))[firstBadX];
                    const QRgb sb = reinterpret_cast<const QRgb *>(exported.constScanLine(
                        firstBadY - selectionRect.top()))[firstBadX - selectionRect.left()];
                    out << "INPLACE_DEMO A3 firstBad=" << firstBadX << "," << firstBadY
                        << " screen=#" << QString::number(sa, 16) << " export=#"
                        << QString::number(sb, 16) << "\n";
                }
            }
        }
        out << "INPLACE_DEMO A3 previewMatchesExport=" << (previewMismatch == 0 ? 1 : 0)
            << " mismatch=" << previewMismatch << " toolbarDragQuiet=" << (toolbarDragQuiet ? 1 : 0)
            << "\n";
        out << "INPLACE_DEMO A3 status " << m_overlay->annotationStatus() << "\n";
        if (!outPrefix.isEmpty() && !onScreenImage.isNull()) {
            onScreenImage.copy(localSelection.toAlignedRect())
                .save(outPrefix + QStringLiteral("-annotated.png"));
            onScreenImage.save(outPrefix + QStringLiteral("-annotated-overlay.png"));
        }

        const bool a3ok = shapeHits == shapeCases.size() && penOk && numberOk && textOk && undoOk
            && moveOk && deleteOk && toolbarDragQuiet && previewMismatch == 0;
        out << (a3ok ? "INPLACE_DEMO A3 PASS" : "INPLACE_DEMO A3 FAIL") << "\n";

        // ─────────────────────────────────────────────────
        // A3.1：颜色 / 粗细（自绘弹出面板）
        // ─────────────────────────────────────────────────
        const QRectF colorCell = m_overlay->colorButtonRect();
        const QRectF widthCell = m_overlay->widthButtonRect();
        const bool cellsInBar = !colorCell.isEmpty() && !widthCell.isEmpty()
            && !colorCell.intersects(widthCell) && m_overlay->toolbarRect().contains(colorCell)
            && m_overlay->toolbarRect().contains(widthCell);

        // 颜色面板：打开 → 面板不压选区 → 选一个色块
        clickAt(colorCell.center());
        const bool colorPanelOpened = m_overlay->openPanel() == Overlay::Panel::Color;
        const QRectF colorPanel = m_overlay->panelRect();
        const bool panelOutsideSelection =
            !colorPanel.intersects(localSelection) && surface.contains(colorPanel);
        const QColor picked = Overlay::palette().at(5);
        // 留一张开着颜色面板的证据图（贴工具条那一带就够了，但整屏更直白）
        if (!outPrefix.isEmpty() && colorPanelOpened)
            m_overlay->grab().save(outPrefix + QStringLiteral("-panel-color.png"));
        clickAt(m_overlay->swatchRect(5).center());
        const bool swatchPicked = m_overlay->annotationColor() == picked
            && m_overlay->openPanel() == Overlay::Panel::None;

        // 用新颜色画一个矩形：颜色要真的落到标注里
        selectToolByButton(ToolKind::Rectangle);
        const int beforeColorDraw = m_overlay->document().count();
        dragLocal(origin + QPointF(120, 250), origin + QPointF(230, 330));
        const bool drawnColor = m_overlay->document().count() == beforeColorDraw + 1
            && m_overlay->document().at(beforeColorDraw).color == picked;
        out << "INPLACE_DEMO A31 cells=" << (cellsInBar ? 1 : 0) << " colorPanel="
            << (colorPanelOpened ? 1 : 0) << " outsideSelection="
            << (panelOutsideSelection ? 1 : 0) << " swatchPick=" << (swatchPicked ? 1 : 0)
            << " color=" << m_overlay->annotationColor().name()
            << " drawnColor=" << m_overlay->document().at(beforeColorDraw).color.name() << "\n";

        // 粗细面板：拖滑杆 → 新粗细要真的落到标注里
        clickAt(widthCell.center());
        const bool widthPanelOpened = m_overlay->openPanel() == Overlay::Panel::Width;
        const QRectF track = m_overlay->widthTrackRect();
        const QPointF sliderTarget(track.left() + track.width() * 0.7, track.center().y());
        const int expectedWidth = m_overlay->widthFromPosition(sliderTarget);
        if (!outPrefix.isEmpty() && widthPanelOpened)
            m_overlay->grab().save(outPrefix + QStringLiteral("-panel-width.png"));
        dragLocal(track.center(), sliderTarget);
        const bool sliderWorks = m_overlay->annotationWidth() == expectedWidth;
        selectToolByButton(ToolKind::Pen);
        const int beforeWidthDraw = m_overlay->document().count();
        dragLocal(origin + QPointF(300, 352), origin + QPointF(470, 362));
        const bool drawnWidth = m_overlay->document().count() == beforeWidthDraw + 1
            && m_overlay->document().at(beforeWidthDraw).width == expectedWidth;
        out << "INPLACE_DEMO A31 widthPanel=" << (widthPanelOpened ? 1 : 0) << " slider="
            << (sliderWorks ? 1 : 0) << " width=" << expectedWidth << " (实测 "
            << m_overlay->annotationWidth() << ") drawnWidth="
            << m_overlay->document().at(beforeWidthDraw).width << "\n";

        // 面板内的空白点击不产生标注；Esc 关面板而不退出；面板外点击只关面板
        const int beforeQuiet = m_overlay->document().count();
        clickAt(colorCell.center());
        const QPointF gapPoint =
            (m_overlay->swatchRect(0).bottomRight() + m_overlay->swatchRect(1).topLeft()) / 2.0;
        postMouse(QEvent::MouseButtonPress, gapPoint, Qt::LeftButton, Qt::LeftButton);
        postMouse(QEvent::MouseButtonRelease, gapPoint, Qt::LeftButton, Qt::NoButton);
        const bool panelClickQuiet = m_overlay->document().count() == beforeQuiet;
        postKey(Qt::Key_Escape, QString());
        const bool escClosesPanel =
            m_overlay->openPanel() == Overlay::Panel::None && m_overlay->isAnnotating();
        clickAt(colorCell.center());
        clickAt(origin + QPointF(400, 340));
        const bool outsideQuiet = m_overlay->openPanel() == Overlay::Panel::None
            && m_overlay->document().count() == beforeQuiet;
        out << "INPLACE_DEMO A31 panelClickQuiet=" << (panelClickQuiet ? 1 : 0) << " escCloses="
            << (escClosesPanel ? 1 : 0) << " outsideQuiet=" << (outsideQuiet ? 1 : 0) << "\n";

        const bool a31ok = cellsInBar && colorPanelOpened && panelOutsideSelection && swatchPicked
            && drawnColor && widthPanelOpened && sliderWorks && drawnWidth && panelClickQuiet
            && escClosesPanel && outsideQuiet;
        out << (a31ok ? "INPLACE_DEMO A31 PASS" : "INPLACE_DEMO A31 FAIL") << "\n";

        // ─────────────────────────────────────────────────
        // Phase B：选区可调整（8 手柄 + 4 边 + 内部拖动）与快捷键
        // ─────────────────────────────────────────────────
        const QRectF selBeforeB = m_overlay->localSelectionRect();
        const int countBeforeB = m_overlay->document().count();
        selectToolByButton(ToolKind::Rectangle);
        dragLocal(selBeforeB.topLeft() + QPointF(200, 150),
                  selBeforeB.topLeft() + QPointF(320, 240));
        const int markerIndex = m_overlay->document().count() - 1;
        const QRectF markerBefore = m_overlay->document().at(markerIndex).bounds();

        // 拖左上角手柄：往外扩 (-40,-30)，只有左上动、其余三边不动
        dragLocal(selBeforeB.topLeft(), selBeforeB.topLeft() + QPointF(-40, -30));
        const QRectF selGrown = m_overlay->localSelectionRect();
        const bool gripCorner = qAbs(selGrown.left() - (selBeforeB.left() - 40)) <= 1.5
            && qAbs(selGrown.top() - (selBeforeB.top() - 30)) <= 1.5
            && qAbs(selGrown.right() - selBeforeB.right()) <= 1.5
            && qAbs(selGrown.bottom() - selBeforeB.bottom()) <= 1.5;

        // 标注锚定**屏幕内容**：屏幕位置（选区原点 + 标注坐标）必须不变
        const QRectF markerAfter = m_overlay->document().at(markerIndex).bounds();
        const QPointF screenBefore = selBeforeB.topLeft() + markerBefore.topLeft();
        const QPointF screenAfter = selGrown.topLeft() + markerAfter.topLeft();
        const bool anchored = (screenBefore - screenAfter).manhattanLength() <= 2.0;
        const bool annotationsKept = m_overlay->document().count() == countBeforeB + 1;

        // 拖右边手柄到越过左边：宽度应被夹到最小 16px，而不是变成 0/负数
        dragLocal(QPointF(selGrown.right(), selGrown.center().y()),
                  QPointF(selGrown.left() + 4, selGrown.center().y()));
        const QRectF selMin = m_overlay->localSelectionRect();
        const bool minWidth = qAbs(selMin.width() - 16.0) <= 1.5 && selMin.height() > 2;
        out << "INPLACE_DEMO B gripCorner=" << (gripCorner ? 1 : 0) << " minWidth="
            << (minWidth ? 1 : 0) << " anchored=" << (anchored ? 1 : 0) << " annotationsKept="
            << (annotationsKept ? 1 : 0) << " 选区 " << qRound(selBeforeB.x()) << ","
            << qRound(selBeforeB.y()) << " " << qRound(selBeforeB.width()) << "x"
            << qRound(selBeforeB.height()) << " -> " << qRound(selGrown.x()) << ","
            << qRound(selGrown.y()) << " " << qRound(selGrown.width()) << "x"
            << qRound(selGrown.height()) << "\n";

        // 方向键微调（1px；Shift = 10px）
        m_overlay->applySelectionLocal(selBeforeB); // 先还原成原始选区
        const QRectF nudgeBase = m_overlay->localSelectionRect();
        postKey(Qt::Key_Right, QString());
        postKey(Qt::Key_Right, QString());
        postKey(Qt::Key_Down, QString(), Qt::ShiftModifier);
        const QRectF nudged = m_overlay->localSelectionRect();
        const bool arrowNudge = qAbs(nudged.left() - (nudgeBase.left() + 2)) <= 0.5
            && qAbs(nudged.top() - (nudgeBase.top() + 10)) <= 0.5
            && qAbs(nudged.width() - nudgeBase.width()) <= 0.5
            && qAbs(nudged.height() - nudgeBase.height()) <= 0.5;

        // Ctrl+A 全选（整屏）
        postKey(Qt::Key_A, QString(), Qt::ControlModifier);
        const bool selectAll = m_overlay->localSelectionRect() == surface;
        m_overlay->applySelectionLocal(selBeforeB); // 还原，便于后续证据/验收

        // 选区改过之后，预览仍然 == 导出（选区内逐像素）。
        // 先把鼠标移开工具条、并 Esc 取消选中：悬停气泡与选中框都是临时 UI，会画在选区里
        postMouse(QEvent::MouseMove, QPointF(2, 2), Qt::NoButton, Qt::NoButton);
        postKey(Qt::Key_Escape, QString());
        int bMismatch = -1;
        {
            const QImage onScreenB = m_overlay->grab().toImage();
            const QImage exportedB = m_overlay->renderSelection(m_overlay->frozenSelection());
            const QRect selRectB = m_overlay->localSelectionRect().toAlignedRect();
            const QRect innerB(6, 6, exportedB.width() - 12, exportedB.height() - 12);
            if (!exportedB.isNull() && innerB.isValid() && qFuzzyCompare(dpr, 1.0)) {
                bMismatch = 0;
                for (int ly = innerB.top(); ly <= innerB.bottom(); ++ly) {
                    const int sy = ly + selRectB.top();
                    if (sy < 0 || sy >= onScreenB.height())
                        continue;
                    const QRgb *a = reinterpret_cast<const QRgb *>(onScreenB.constScanLine(sy));
                    const QRgb *b = reinterpret_cast<const QRgb *>(exportedB.constScanLine(ly));
                    for (int lx = innerB.left(); lx <= innerB.right(); ++lx) {
                        const int sx = lx + selRectB.left();
                        if (sx >= 0 && sx < onScreenB.width() && a[sx] != b[lx])
                            ++bMismatch;
                    }
                }
            }
        }
        out << "INPLACE_DEMO B arrowNudge=" << (arrowNudge ? 1 : 0) << " selectAll="
            << (selectAll ? 1 : 0) << " previewStillMatchesExport="
            << (bMismatch == 0 ? 1 : 0) << " mismatch=" << bMismatch << "\n";

        const bool bok = gripCorner && minWidth && anchored && annotationsKept && arrowNudge
            && selectAll && bMismatch == 0;
        out << (bok ? "INPLACE_DEMO B PASS" : "INPLACE_DEMO B FAIL") << "\n";
        // 「完成/取消」出口：默认点「取消」；SEASHOT_INPLACE_DEMO_COMMIT=1 时改成合成 Enter，
        // 让“带标注的截图 → 现有截图链路（复制/保存/浮层）”真的跑一遍。
        const bool commitMode = qEnvironmentVariableIsSet("SEASHOT_INPLACE_DEMO_COMMIT")
            || !qEnvironmentVariable("SEASHOT_INPLACE_DEMO_ACTION").isEmpty();
        bool exitWorks = false;
        const QString actionMode = qEnvironmentVariable("SEASHOT_INPLACE_DEMO_ACTION");
        if (commitMode || !actionMode.isEmpty()) {
            if (actionMode == QLatin1String("copy"))
                postKey(Qt::Key_C, QString(), Qt::ControlModifier);
            else if (actionMode == QLatin1String("save"))
                postKey(Qt::Key_S, QString(), Qt::ControlModifier);
            else if (actionMode == QLatin1String("pin"))
                postKey(Qt::Key_T, QString(), Qt::ControlModifier);
            else if (actionMode == QLatin1String("scroll")) {
                postKey(Qt::Key_L, QString(), Qt::ControlModifier);
                // 长截图现在**要用户自己结束**（按「完成」/Esc）。演示里没人点按钮，
                // 所以 4 秒后替用户"按一下完成"，否则这条链路永远等不到 SCROLL ok。
                QTimer::singleShot(4000, this, [this]() {
                    if (m_scrollCapturer && m_scrollCapturer->isRunning())
                        m_scrollCapturer->stop();
                });
            }
            else if (actionMode == QLatin1String("ocr"))
                postKey(Qt::Key_O, QString(), Qt::ControlModifier);
            else
                postKey(Qt::Key_Return, QStringLiteral("\r"));
            exitWorks = !m_overlay->isAnnotating();
            out << "INPLACE_DEMO exit=Enter(提交) ok=" << (exitWorks ? 1 : 0) << "\n";
        } else if (actionButtons.size() == 2) {
            const Overlay::ActionButton &cancel =
                actionButtons.at(0).confirm ? actionButtons.at(1) : actionButtons.at(0);
            clickAt(cancel.rect.center());
            exitWorks = !m_overlay->isAnnotating();
        }
        const bool cancelWorks = exitWorks;

        out << "INPLACE_DEMO barOutsideSelection=" << (barOutsideSelection ? 1 : 0)
            << " barInsideSurface=" << (barInsideSurface ? 1 : 0)
            << " gapQuiet=" << (gapQuiet ? 1 : 0) << " hover=" << (hoverWorks ? 1 : 0)
            << " hoverCell=" << hoverCellIndex
            << " cancelButton=" << (cancelWorks ? 1 : 0) << "\n";
        out << "INPLACE_DEMO barGoesAboveWhenNoRoom=" << (barGoesAbove ? 1 : 0)
            << " barClampedAtRightEdge=" << (barClamped ? 1 : 0) << " edgeBar="
            << qRound(edgeBar.x()) << "," << qRound(edgeBar.y()) << " "
            << qRound(edgeBar.width()) << "x" << qRound(edgeBar.height()) << "\n";

        const bool a2ok = annotating && !toolButtons.isEmpty() && barOutsideSelection
            && barInsideSurface && toolHits == toolButtons.size() && keyHits == 9 && gapQuiet
            && hoverWorks && cancelWorks && barGoesAbove && barClamped;
        out << (a2ok ? "INPLACE_DEMO A2 PASS" : "INPLACE_DEMO A2 FAIL") << "\n";
        const bool allOk = a1ok && a2ok && a3ok && a31ok && bok;
        out << (allOk ? "INPLACE_DEMO ALL PASS" : "INPLACE_DEMO ALL FAIL") << "\n";
        out.flush();
        if (commitMode) {
            // 合成 Enter/Ctrl+C/Ctrl+S/Ctrl+T/Ctrl+L 之后这些交付都是异步的：
            // 给它几秒再退（滚动截屏要等它自己收工，所以给得更宽），退出码仍是上面的判定
            const int waitMs = qEnvironmentVariable("SEASHOT_INPLACE_DEMO_ACTION")
                    == QLatin1String("scroll")
                ? 9000
                : 4000;
            QTimer::singleShot(waitMs, this, [allOk]() { QCoreApplication::exit(allOk ? 0 : 1); });
        } else {
            QCoreApplication::exit(allOk ? 0 : 1);
        }
        }();
    };
    (*verification)(0);

    return QCoreApplication::exec();
}

int Application::runToastDragBench(int durationMs)
{
    if (!m_backend || !m_backendError.isEmpty()) {
        QTextStream(stderr) << "拖动基准需要可用的截图后端：" << m_backendError << "\n";
        return 2;
    }

    // 已实测：**同一进程**的 ScreenShot2 请求拍不到自己的 layer-shell 表面（另一个进程可以，
    // 见 docs/known-issues.md ISSUE-007）。所以观测交给子进程 --bench-capture，
    // 并把帧时间戳带回来跟本进程记录的目标轨迹对账，算出「显示位置落后目标多少像素/毫秒」。
    const QRectF region = m_options.hasRect ? m_options.rect
                                            : QRectF(virtualGeometry().topLeft(), QSize(600, 400));
    QImage image(qMax(120, int(region.width())), qMax(90, int(region.height())),
                 QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x % 256, y % 256, 140));
    }

    const QRect anchor = region.toAlignedRect();
    m_toast->setDragMarkerEnabled(true);
    m_toast->setAutoHideMs(0);
    m_toast->showResult(image, QString(), QStringLiteral("拖动基准"), QString(), anchor);
    m_toast->resetCommitCount();
    // 观测时要用卡片宽度把"品红框质心"反推成左上角
    auto cardWidth = std::make_shared<int>(qMax(1, m_toast->currentGeometry().width()));
    auto cpuStart = std::make_shared<struct rusage>();
    auto cpuEnd = std::make_shared<struct rusage>();
    ::getrusage(RUSAGE_SELF, cpuStart.get());

    auto trajectory = std::make_shared<QVector<QPair<qint64, int>>>();
    auto reports = std::make_shared<QVector<QString>>();
    auto startX = std::make_shared<int>(m_toast->currentGeometry().x());
    auto step = std::make_shared<int>(0);
    auto timer = new QTimer(this);
    // 事件频率可以用 SEASHOT_BENCH_STEP_MS 调（4ms ≈ 240Hz 鼠标，默认 16ms ≈ 60Hz）
    const int stepMs = qMax(1, qEnvironmentVariableIntValue("SEASHOT_BENCH_STEP_MS") > 0
                                   ? qEnvironmentVariableIntValue("SEASHOT_BENCH_STEP_MS")
                                   : 16);
    const int stepPx = 1;    // 每个事件 1px：240Hz 下仍是 ~250px/s
    const int captureEvery = qMax(1, int(320.0 / stepMs));

    QTextStream out(stdout);
    out << "DRAGBENCH_START startX=" << *startX << " stepMs=" << stepMs << " stepPx=" << stepPx
        << " durationMs=" << durationMs << " captureEvery=" << captureEvery << "\n";
    out.flush();

    // 走真实鼠标路径（press → move → move ... → release）：早先直接用 dragTo()
    // 绕过了 mousePress/Move/Release，结果漏掉了"松手瞬移回原位"的 bug。
    const QPoint startGlobal(m_toast->currentGeometry().x(), m_toast->currentGeometry().y());
    const QPoint pressPos = startGlobal + QPoint(40, 20);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(40, 20),
                           QPointF(pressPos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_toast.get(), &pressEvent);

    connect(timer, &QTimer::timeout, this, [=]() {
        ++(*step);
        const int targetX = *startX + (*step) * stepPx;
        trajectory->append({QDateTime::currentMSecsSinceEpoch(), targetX});
        QElapsedTimer applyTimer;
        applyTimer.start();
        // 鼠标相对按下点的位移 = 目标位移
        const QPointF local(40 + (*step) * stepPx, 20);
        const QPointF global(pressPos.x() + (*step) * stepPx, pressPos.y());
        QMouseEvent moveEvent(QEvent::MouseMove, local, global, Qt::NoButton, Qt::LeftButton,
                              Qt::NoModifier);
        QCoreApplication::sendEvent(m_toast.get(), &moveEvent);
        const qint64 applyUs = applyTimer.nsecsElapsed() / 1000;

        if ((*step) % captureEvery != 0)
            return;

        const QString path = QStringLiteral("/tmp/seashot-bench-obs-%1.png").arg(*step);
        auto *child = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SEASHOT_DISABLE_SELF_EXCLUSION"), QStringLiteral("1"));
        child->setProcessEnvironment(env);
        child->setProgram(QCoreApplication::applicationFilePath());
        child->setArguments({QStringLiteral("--bench-capture"), path, QStringLiteral("--backend"),
                             m_backend->name()});
        connect(child, &QProcess::finished, this, [=](int, QProcess::ExitStatus) {
            const QString childOut = QString::fromLocal8Bit(child->readAllStandardOutput());
            child->deleteLater();
            qint64 frameAt = 0;
            const QRegularExpression re(QStringLiteral("BENCH_CAPTURE t=([0-9]+)"));
            const auto match = re.match(childOut);
            if (match.hasMatch())
                frameAt = match.captured(1).toLongLong();

            const QImage frame(path);
            if (frame.isNull() || frameAt == 0) {
                reports->append(QStringLiteral("step=%1 观测失败").arg(*step));
                return;
            }
            // 用质心而不是最左像素：桌面上别的地方（图标/头像）也可能有品红像素，
            // 用最左值会被它们带偏。质心对少量杂点鲁棒，且品红像素总数本身能反映拖影。
            qint64 sumX = 0;
            qint64 count = 0;
            for (int y = 0; y < frame.height(); y += 2) {
                const QRgb *line = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
                for (int x = 0; x < frame.width(); ++x) {
                    const QColor color = QColor::fromRgb(line[x]);
                    if (color.red() > 200 && color.green() < 60 && color.blue() > 200) {
                        sumX += x;
                        ++count;
                    }
                }
            }
            if (count == 0) {
                reports->append(QStringLiteral("step=%1 没找到品红框").arg(*step));
                return;
            }
            const int centroidX = int(sumX / count);
            // 卡片左上角 = 质心 - 半宽（质心是框的中心）
            const int observedX = centroidX - *cardWidth / 2;
            int targetAtFrame = 0;
            for (const auto &point : *trajectory) {
                if (point.first <= frameAt)
                    targetAtFrame = point.second;
                else
                    break;
            }
            const int lagPx = targetAtFrame - observedX;
            reports->append(QStringLiteral("step=%1 observed=%2 targetAtFrame=%3 lagPx=%4 lagMs=%5 "
                                           "applyUs=%6 magenta=%7")
                                .arg(*step)
                                .arg(observedX)
                                .arg(targetAtFrame)
                                .arg(lagPx)
                                .arg(lagPx * stepMs / stepPx)
                                .arg(applyUs)
                                .arg(count));
        });
        child->start();
    });
    if (!qEnvironmentVariableIsSet("SEASHOT_BENCH_STATIC"))
        timer->start(stepMs);
    else
        QTextStream(stdout) << "DRAGBENCH_STATIC 只显示合成卡片（验收对照用）\n";
        std::fflush(stdout);

    QTimer::singleShot(durationMs + 2000, this, [=]() {
        timer->stop();
        QTextStream summary(stdout);

        // 松手：走真实的 release，并断言落点没有回到按下时的位置
        const int expectedX = *startX + (*step) * stepPx;
        const QPointF releaseLocal(40 + (*step) * stepPx, 20);
        const QPointF releaseGlobal(pressPos.x() + (*step) * stepPx, pressPos.y());
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, releaseLocal, releaseGlobal,
                                 Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_toast.get(), &releaseEvent);
        const int finalX = m_toast->currentGeometry().x();
        const bool releaseOk = qAbs(finalX - expectedX) <= 2;
        summary << "DRAGBENCH_RELEASE expected=" << expectedX << " final=" << finalX
                << " startWas=" << *startX << " ok=" << (releaseOk ? 1 : 0) << "\n";
        int lagSum = 0;
        int lagMax = 0;
        int valid = 0;
        for (const QString &report : *reports) {
            summary << "DRAGBENCH_SAMPLE " << report << "\n";
            const QRegularExpression re(QStringLiteral("lagPx=(-?[0-9]+)"));
            const auto match = re.match(report);
            if (match.hasMatch()) {
                const int lag = match.captured(1).toInt();
                lagSum += lag;
                lagMax = qMax(lagMax, lag);
                ++valid;
            }
        }
        if (valid > 0) {
            summary << "DRAGBENCH samples=" << valid
                    << " lagPxAvg=" << double(lagSum) / valid << " lagPxMax=" << lagMax
                    << " lagMsAvg=" << (double(lagSum) / valid) * stepMs / stepPx << "\n";
            ::getrusage(RUSAGE_SELF, cpuEnd.get());
            const auto ms = [](const timeval &t) {
                return double(t.tv_sec) * 1000.0 + double(t.tv_usec) / 1000.0;
            };
            summary << "DRAGBENCH_CPU userMs="
                    << (ms(cpuEnd->ru_utime) - ms(cpuStart->ru_utime))
                    << " sysMs=" << (ms(cpuEnd->ru_stime) - ms(cpuStart->ru_stime)) << "\n";
            const QVector<qint64> times = m_toast->commitTimestamps();
            if (times.size() > 2) {
                qint64 sum = 0;
                qint64 minGap = 1LL << 62;
                qint64 maxGap = 0;
                for (int i = 1; i < times.size(); ++i) {
                    const qint64 gap = times.at(i) - times.at(i - 1);
                    sum += gap;
                    minGap = qMin(minGap, gap);
                    maxGap = qMax(maxGap, gap);
                }
                const double avg = double(sum) / (times.size() - 1);
                double variance = 0;
                for (int i = 1; i < times.size(); ++i) {
                    const double diff = double(times.at(i) - times.at(i - 1)) - avg;
                    variance += diff * diff;
                }
                variance /= (times.size() - 1);
                summary << "DRAGBENCH_CADENCE commits=" << times.size() << " avgGapMs=" << avg
                        << " minGapMs=" << minGap << " maxGapMs=" << maxGap
                        << " jitterStddevMs=" << std::sqrt(variance) << "\n";
            }
            summary << "DRAGBENCH_CHURN steps=" << *step << " commits=" << m_toast->commitCount()
                    << " elapsedMs=" << durationMs << " commitsPerSec="
                    << double(m_toast->commitCount()) * 1000.0 / qMax(1, durationMs) << "\n";
        } else {
            summary << "DRAGBENCH_EMPTY 没有有效观测\n";
        }
        summary.flush();
        m_toast->hide();
        QCoreApplication::quit();
    });

    return QCoreApplication::exec();
}

int Application::runBenchCapture(const QString &path)
{
    if (!m_backend || !m_backendError.isEmpty()) {
        QTextStream(stderr) << "BENCH_CAPTURE_ERROR " << m_backendError << "\n";
        return 2;
    }
    const qint64 frameAt = QDateTime::currentMSecsSinceEpoch();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool ok = false;
    m_backend->captureRect(QRectF(virtualGeometry()), [&](const CaptureImage &frame) {
        ok = frame.ok() && frame.image.save(path, "PNG");
        loop.quit();
    });
    timeout.start(20000);
    loop.exec();
    QTextStream out(stdout);
    out << "BENCH_CAPTURE t=" << frameAt << " path=" << path << " ok=" << (ok ? 1 : 0) << "\n";
    out.flush();
    return ok ? 0 : 1;
}

int Application::runPortalScreencastProbe(const QString &tokenFile)
{
    QTextStream out(stdout);
    QString token;
    if (!tokenFile.isEmpty()) {
        QFile file(tokenFile);
        if (file.exists() && file.open(QIODevice::ReadOnly))
            token = QString::fromUtf8(file.readAll()).trimmed();
        out << "PORTAL_SCREENCAST_TOKEN_FILE " << tokenFile << " exists="
            << (token.isEmpty() ? 0 : 1) << "\n";
    }

    PortalScreencast portal;
    QEventLoop loop;
    g_nestedLoop = &loop;
    PortalScreencast::Result result;
    portal.start(
        token, /*persistMode=*/2,
        [&](const PortalScreencast::Result &r) {
            result = r;
            loop.quit();
        },
        m_options.probePortalNoStart);
    // 弹窗要等用户点，给足时间；静默的话这里几乎立刻就返回
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    loop.exec();
    g_nestedLoop = nullptr;

    out << "PORTAL_SCREENCAST_RESULT " << result.describe() << "\n";
    const bool silent = result.startLatencyMs >= 0 && result.startLatencyMs < 1000;
    if (result.startLatencyMs >= 0)
        out << "PORTAL_SCREENCAST_DIALOG_LIKELY " << (silent ? 0 : 1) << " (startLatencyMs="
            << result.startLatencyMs << ")\n";

    if (result.ok && !m_options.probePortalNoStart) {
        QString pipewireError;
        int fd = portal.openPipeWireRemote(&pipewireError);
        out << "PORTAL_SCREENCAST_PIPEWIRE_FD ok=" << (fd >= 0 ? 1 : 0) << " fd=" << fd
            << (pipewireError.isEmpty() ? QString() : QStringLiteral(" error=%1").arg(pipewireError))
            << "\n";
        out.flush();
        // 接帧验证：把 fd 交给 gst-launch 去真的取几帧。
        // 注意必须在 closeSession() 之前做 —— 会话一关流就没了。
        if (fd >= 0 && m_options.probePipewireLib > 0) {
            runPipeWireLibProbe(fd, result.streams.isEmpty() ? 0 : result.streams.first().nodeId,
                                m_options.probePipewireLib, m_options.probePipewireOut);
        }
        if (fd >= 0 && m_options.probePipewireFrames > 0) {
            uint nodeId = 0;
            if (!result.streams.isEmpty())
                nodeId = result.streams.first().nodeId;
            out << "PIPEWIRE_PROBE_NODEID " << nodeId << "（来自 portal 应答的 streams）\n";
            out.flush();
            runPipeWireFrameProbe(fd, m_options.probePipewireFrames, m_options.probePipewireOut,
                                  nodeId);
        }
        if (fd >= 0)
            ::close(fd);
    }

    if (!result.restoreToken.isEmpty() && !tokenFile.isEmpty()) {
        QDir().mkpath(QFileInfo(tokenFile).absolutePath());
        QFile file(tokenFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(result.restoreToken.toUtf8());
            out << "PORTAL_SCREENCAST_TOKEN_SAVED " << tokenFile << "\n";
        }
    }
    portal.closeSession();
    out.flush();
    return result.ok ? 0 : 1;
}

int Application::runPipeWireFrameProbe(int fd, int frameCount, const QString &outDir, uint nodeId)
{
    QTextStream out(stdout);
    const QString program = QStandardPaths::findExecutable(QStringLiteral("gst-launch-1.0"));
    if (program.isEmpty()) {
        out << "PIPEWIRE_PROBE_NO_GSTREAMER 终端里没有 gst-launch-1.0，无法做接帧验证\n";
        out.flush();
        return 4;
    }
    if (!QDir().mkpath(outDir)) {
        out << "PIPEWIRE_PROBE_ERROR 无法创建 " << outDir << "\n";
        out.flush();
        return 3;
    }
    for (const QString &old : QDir(outDir).entryList({QStringLiteral("frame-*")}, QDir::Files))
        QFile::remove(outDir + QLatin1Char('/') + old);

    // dup() 出来的 fd 不带 CLOEXEC，所以子进程能直接继承（Qt 不会去关它）。
    const int childFd = ::dup(fd);
    if (childFd < 0) {
        out << "PIPEWIRE_PROBE_ERROR dup(fd) 失败\n";
        out.flush();
        return 5;
    }

    // 落盘方式：avenc_png 把每帧编成一个 PNG，filesink 把它们首尾相接写进同一个文件。
    // （本机没装 gst-plugins-good，所以 multifilesink/pngenc 用不了。）
    // 帧数 = 文件里 PNG 签名的出现次数 —— 这个计数很可靠。
    const QString rawPath = outDir + QStringLiteral("/frames.png");

    // pipewiresrc 光给 fd 不够：它会用 PW_ID_ANY 去连，服务端报
    //   "stream error: target not found"（就是 KDE 日志里那句，实测确认不是拆卸噪音）。
    // 所以要先找到 portal 刚建的那个视频源节点。
    // 这里用 pw-dump 枚举（会话存活期间它在 PipeWire 图里可见），
    // 比手工解析 D-Bus 的 a(ua{sv}) 稳 —— 后者会让 Qt 在读嵌套 QVariantMap 时 abort。
    QString target;
    {
        QProcess dump;
        dump.start(QStringLiteral("pw-dump"), {});
        if (!dump.waitForFinished(6000)) {
            dump.kill();
        } else {
            const QJsonDocument doc =
                QJsonDocument::fromJson(dump.readAllStandardOutput());
            for (const QJsonValue &entry : doc.array()) {
                const QJsonObject object = entry.toObject();
                if (object.value(QStringLiteral("type")).toString()
                    != QLatin1String("PipeWire:Interface:Node"))
                    continue;
                const QJsonObject props = object.value(QStringLiteral("info"))
                                             .toObject()
                                             .value(QStringLiteral("props"))
                                             .toObject();
                if (props.value(QStringLiteral("media.class")).toString()
                    != QLatin1String("Video/Source"))
                    continue;
                const int id = object.value(QStringLiteral("id")).toInt();
                const QString name = props.value(QStringLiteral("node.name")).toString();
                out << "PIPEWIRE_PROBE_TARGET id=" << id << " name=" << name
                    << " description="
                    << props.value(QStringLiteral("node.description")).toString() << "\n";
                target = QString::number(id);
                if (!name.isEmpty())
                    target = name;
                break;
            }
        }
    }
    if (target.isEmpty() && nodeId > 0) {
        // pw-dump 看不到 portal 的节点（它只存在于那条 fd 连接里），
        // 所以用 portal 应答里给的 node id 作为目标。
        target = QString::number(nodeId);
        out << "PIPEWIRE_PROBE_TARGET 用 portal 给的 node id=" << nodeId << "\n";
    }
    if (target.isEmpty())
        out << "PIPEWIRE_PROBE_TARGET (没有可用目标)\n";

    QStringList args{QStringLiteral("-q"),
                     QStringLiteral("pipewiresrc"),
                     QStringLiteral("fd=%1").arg(childFd),
                     QStringLiteral("num-buffers=%1").arg(frameCount),
                     QStringLiteral("!"),
                     QStringLiteral("videoconvert"),
                     QStringLiteral("!"),
                     QStringLiteral("avenc_png"),
                     QStringLiteral("!"),
                     QStringLiteral("filesink"),
                     QStringLiteral("location=%1").arg(rawPath)};
    if (!target.isEmpty()) {
        // path = 数字节点 id（已废弃但收 id）；target-object 要的是节点名/序列号。
        args.insert(3, QStringLiteral("path=%1").arg(target));
    }
    out << "PIPEWIRE_PROBE_CMD gst-launch-1.0 " << args.join(QLatin1Char(' ')) << "\n";
    out.flush();

    QElapsedTimer timer;
    timer.start();
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(5000)) {
        out << "PIPEWIRE_PROBE_ERROR gst-launch 启动失败: " << proc.errorString() << "\n";
        out.flush();
        ::close(childFd);
        return 6;
    }

    qint64 firstFrameMs = -1;
    QString firstFrameSize;
    static const QByteArray pngMagic("\x89PNG\r\n\x1a\n", 8);
    const auto countFrames = [&]() -> int {
        QFile file(rawPath);
        if (!file.open(QIODevice::ReadOnly))
            return 0;
        const QByteArray blob = file.readAll();
        int count = 0;
        for (int at = blob.indexOf(pngMagic); at >= 0; at = blob.indexOf(pngMagic, at + 1))
            ++count;
        return count;
    };

    while (proc.state() != QProcess::NotRunning) {
        proc.waitForFinished(100);
        if (firstFrameMs < 0 && countFrames() > 0) {
            firstFrameMs = timer.elapsed();
            const QImage first(rawPath); // 多帧文件里读第一帧
            if (!first.isNull())
                firstFrameSize = QStringLiteral("%1x%2").arg(first.width()).arg(first.height());
        }
        if (timer.elapsed() > 20000) { // 兜底：不能把探测卡死
            proc.kill();
            break;
        }
    }
    const QString gstOutput = QString::fromLocal8Bit(proc.readAll()).simplified();
    const int frames = countFrames();

    out << "PIPEWIRE_PROBE frames=" << frames << " firstFrameMs=" << firstFrameMs
        << " elapsedMs=" << timer.elapsed() << " exit=" << proc.exitCode() << " bytes="
        << QFileInfo(rawPath).size() << "\n";
    if (frames > 0)
        out << "PIPEWIRE_PROBE_FIRST " << rawPath << " size=" << firstFrameSize << "\n";
    else
        out << "PIPEWIRE_PROBE_GST_OUTPUT " << gstOutput.left(700) << "\n";
    out.flush();
    ::close(childFd);
    return frames > 0 ? 0 : 1;
}

int Application::runPipeWireLibProbe(int fd, uint nodeId, int frameCount, const QString &outDir)
{
    QTextStream out(stdout);
    if (!PipeWireFrameSource::isAvailable()) {
        out << "PIPEWIRE_LIB_UNAVAILABLE 编译时没有 libpipewire-0.3\n";
        out.flush();
        return 4;
    }
    // 挑一个输出（取第一个）。区域取该输出的中间 1/2：能直观看出裁剪位置对不对。
    QRectF monitor;
    if (m_backend) {
        const QVector<OutputInfo> outputs = m_backend->outputs();
        if (!outputs.isEmpty())
            monitor = QRectF(outputs.first().logicalGeometry);
    }
    if (monitor.isEmpty()) {
        out << "PIPEWIRE_LIB_ERROR 拿不到输出几何，无法把帧裁到选区\n";
        out.flush();
        return 5;
    }
    const QRectF region(monitor.x() + monitor.width() / 4, monitor.y() + monitor.height() / 4,
                        monitor.width() / 2, monitor.height() / 2);
    out << "PIPEWIRE_LIB_REGION monitor=" << monitor.width() << "x" << monitor.height()
        << " region=" << region.width() << "x" << region.height() << "\n";
    out.flush();

    PipeWireFrameSource source;
    source.setTargetFps(15); // 与录制器一致：流按刷新率推帧，这里只留 15fps
    QString startError;
    if (!source.start(fd, nodeId, monitor, region, &startError)) {
        out << "PIPEWIRE_LIB_START_FAILED " << startError << "\n";
        out.flush();
        return 6;
    }

    QElapsedTimer timer;
    timer.start();
    qint64 firstFrameMs = -1;
    quint64 seen = 0;
    while (timer.elapsed() < 15000) {
        const quint64 got = source.receivedFrames();
        if (got != seen) {
            seen = got;
            if (firstFrameMs < 0)
                firstFrameMs = timer.elapsed();
            if (frameCount > 0 && int(got) >= frameCount)
                break;
        }
        if (!source.error().isEmpty())
            break;
        QThread::msleep(5);
    }
    const QImage frame = source.latestFrame();
    const QString format = source.negotiatedFormat();
    const QString streamError = source.error();
    source.stop();

    const double seconds = qMax(0.001, double(timer.elapsed()) / 1000.0);
    out << "PIPEWIRE_LIB frames=" << seen << " firstFrameMs=" << firstFrameMs
        << " elapsedMs=" << timer.elapsed() << " fps=" << QString::number(double(seen) / seconds, 'f', 2)
        << " format=\"" << format << "\" image=" << frame.width() << "x" << frame.height() << "\n";
    if (!streamError.isEmpty())
        out << "PIPEWIRE_LIB_STREAM_ERROR " << streamError << "\n";
    if (!frame.isNull() && !outDir.isEmpty()) {
        QDir().mkpath(outDir);
        const QString path = outDir + QStringLiteral("/lib-last-frame.png");
        if (frame.save(path, "PNG"))
            out << "PIPEWIRE_LIB_SAVED " << path << "\n";
    }
    out.flush();
    return seen > 0 ? 0 : 1;
}

int Application::run()
{
    QTextStream out(stdout);

    if (m_options.selftest) {
        out << "BACKEND_CHOICE " << m_backendReason << "\n";
        out.flush();
        return runSelfTest();
    }
    if (m_options.probePortal)
        return runPortalScreencastProbe(m_options.portalTokenFile);
    if (!m_options.benchCapturePath.isEmpty())
        return runBenchCapture(m_options.benchCapturePath);
    if (m_options.toastDragBenchMs > 0)
        return runToastDragBench(m_options.toastDragBenchMs);
    if (!m_options.inplaceDemoOut.isEmpty())
        return runInplaceDemo(m_options.inplaceDemoOut);
    if (!m_options.pinDemoOut.isEmpty())
        return runPinDemo(m_options.pinDemoOut);
    if (!m_options.pinPath.isEmpty())
        return runPinFile(m_options.pinPath);
    if (m_options.printConfig)
        return runPrintConfig();
    if (m_options.hotkeyStatus) {
        // ⚠️ 这里**不要** applyHotkey()：查询命令不该改全局状态。
        //    之前它会顺手把 KGlobalAccel 那条注册回来，于是"查一下状态"就把
        //    capture=Ctrl+Alt+A 又绑上了 —— 系统设置里又出现两条（实测踩过）。
        // Phase F：顺带静默报告门户那条路的状态（谁在真正生效，一眼就能看出来）
        {
            uint version = 0;
            QString portalError;
            if (GlobalShortcutsPortal::available(&version, &portalError)) {
                GlobalShortcutsPortal portal;
                QEventLoop loop;
                QStringList triggers;
                int boundCount = 0;
                connect(&portal, &GlobalShortcutsPortal::listed, &loop,
                        [&triggers, &boundCount, &loop](const GlobalShortcutsPortal::Result &result) {
                            // 直接数"有 trigger 的"条目，别拿显示字符串去 contains（全角/半角括号会坑）
                            for (const auto &shortcut : result.shortcuts) {
                                if (!shortcut.trigger.isEmpty())
                                    ++boundCount;
                                triggers << QStringLiteral("%1=%2").arg(
                                    shortcut.id, shortcut.trigger.isEmpty()
                                                     ? QStringLiteral("(unbound)")
                                                     : shortcut.trigger);
                            }
                            loop.quit();
                        });
                QTimer::singleShot(8000, &loop, &QEventLoop::quit);
                portal.listShortcuts();
                loop.exec();
                portal.closeSession();
                const bool authorized = boundCount > 0;
                out << "HOTKEY_PORTAL_STATE available=1 version=" << version
                    << " authorized=" << (authorized ? 1 : 0) << " shortcuts="
                    << (triggers.isEmpty() ? QStringLiteral("(空)") : triggers.join(QLatin1Char(',')))
                    << "\n";
            } else {
                out << "HOTKEY_PORTAL_STATE available=0 error=" << portalError << "\n";
            }
        }
        out << "HOTKEY configured="
            << (m_settings->hotkey().isEmpty() ? QStringLiteral("(none)") : m_settings->hotkey())
            << " actual=" << (m_backend ? m_backend->hotkeyStatus() : QStringLiteral("-")) << "\n";
        out.flush();
        return 0;
    }
    if (!m_options.setHotkey.isEmpty()) {
        const QString sequence = m_options.setHotkey == QLatin1String("none")
            ? QString()
            : m_options.setHotkey;
        m_portalHotkeyActive = false; // 用户显式指定 → KGlobalAccel 那条路说了算
        m_settings->setHotkey(sequence);
        applyHotkey(sequence);
        out << "HOTKEY_SET " << (sequence.isEmpty() ? QStringLiteral("(none)") : sequence)
            << " actual=" << (m_backend ? m_backend->hotkeyStatus() : QStringLiteral("-")) << "\n";
        out.flush();
        return 0;
    }
    if (m_options.settings) {
        // 配置界面独立进程：已有常驻实例时也允许（它只读写配置，不动截图链路）
        openSettings();
        return QCoreApplication::exec();
    }
    if (m_options.recordStop) {
        if (sendToExistingInstance(QStringLiteral("stop-record"))) {
            out << "RECORD_STOP_FORWARDED\n";
            out.flush();
            return 0;
        }
        out << "RECORD_STOP no-running-instance\n";
        out.flush();
        return 1;
    }
    if (m_options.record) {
        RecordingRequest request;
        request.hasRect = m_options.hasRect;
        request.rect = m_options.rect;
        request.seconds = m_options.seconds;
        request.saveTo = m_options.saveTo.isEmpty() ? QString()
                                                    : QFileInfo(m_options.saveTo).absoluteFilePath();
        request.quitWhenFinished = true;
        QJsonObject message{{QStringLiteral("command"), QStringLiteral("record")},
                             {QStringLiteral("seconds"), request.seconds},
                             {QStringLiteral("saveTo"), request.saveTo}};
        if (request.hasRect) {
            message.insert(QStringLiteral("rect"), QJsonArray{request.rect.x(), request.rect.y(),
                                                               request.rect.width(), request.rect.height()});
        }
        const QString command = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
        if (sendToExistingInstance(command)) {
            out << "RECORD_FORWARDED " << (request.hasRect ? "rect" : "interactive") << "\n";
            out.flush();
            return 0;
        }

        if (!startServer() || !startRecordingRequest(request))
            return 3;
        return QCoreApplication::exec();
    }
    if (m_options.hotkeyPortalStatus)
        return runHotkeyPortalStatus();
    if (m_options.hotkeyPortalRequest)
        return runHotkeyPortalRequest();
    if (!m_options.ocrPath.isEmpty())
        return runOcrCli(m_options.ocrPath);
    if (m_options.scrollCapture)
        return runScrollCaptureCli();
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
        // 编辑器还开着：继续跑事件循环，等它关掉
        if (m_editorOpened)
            return QCoreApplication::exec();
        // 浮层还在：等它被关闭或自动消失。
        // ⚠️ 两层保险，缺一个都会挂住：
        //   1) 进循环前先看 isVisible()：浮层可能已经在上一段（runNonInteractiveCapture
        //      内部的）嵌套循环里被关掉了（例如点「保存」后 dismissAfter(1200)），
        //      那时再等就没人来叫醒；
        //   2) 用**局部 QEventLoop** 而不是 QCoreApplication::exec()：后者在这种
        //      “外部已经退过一次循环”的场景下实测不会真正返回（进程永远 poll 着），
        //      局部循环的 quit 目标明确，行为可预测。
        if (m_keepAliveForToast && m_toast && m_toast->isVisible()) {
            QEventLoop wait;
            connect(m_toast.get(), &Toast::dismissed, &wait, &QEventLoop::quit);
            if (m_toast->isVisible())
                wait.exec();
        }
        // ⚠️ 等待期间用户可能点了「编辑」——编辑器是那一瞬才打开的，所以必须重新判断。
        //    否则浮层消失后直接 return code，进程退出会把刚打开的编辑器一起带走
        //    （实测日志：先“等浮层关闭”，再“EDITOR_OPEN”，再“直接返回”。）
        if (m_editorOpened)
            return QCoreApplication::exec();
        return code;
    }
    if (m_options.overlayHoldMs > 0) {
        if (sendToExistingInstance(QStringLiteral("capture"))) {
            out << "FORWARDED_TO_RUNNING_INSTANCE\n";
            return 0;
        }
        if (!startServer())
            return 1;
        return showOverlayAndHold();
    }
    if (m_options.capture) {
        // 已有常驻实例 → 交给它；否则自己截一次
        if (sendToExistingInstance(QStringLiteral("capture"))) {
            out << "FORWARDED_TO_RUNNING_INSTANCE\n";
            out.flush();
            return 0;
        }
        if (!startServer())
            return 1;
        startCapture();
        return QCoreApplication::exec();
    }

    // 无参数：常驻
    if (sendToExistingInstance(QStringLiteral("capture-already-running-check"))) {
        out << "SEASHOT_ALREADY_RUNNING\n";
        out.flush();
        return 0;
    }
    if (!startServer())
        return 1;
    startResident();
    out << "SEASHOT_READY backend=" << (m_backend ? m_backend->name() : QStringLiteral("none"))
        << " hotkey=" << (m_settings->hotkey().isEmpty() ? QStringLiteral("(未绑定)")
                                                         : m_settings->hotkey())
        << "\n";
    out.flush();
    return QCoreApplication::exec();
}

} // namespace seashot
