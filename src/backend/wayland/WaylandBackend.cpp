#include "backend/wayland/WaylandBackend.h"

#include "util/Log.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <KGlobalAccel>

namespace seashot {

namespace {
constexpr int kPermissionProbeTimeoutMs = 6000;
}

WaylandBackend::WaylandBackend(QObject *parent)
    : CaptureBackend()
{
    setParent(parent);
}

WaylandBackend::~WaylandBackend()
{
    shutdown();
}

void WaylandBackend::collectOutputs()
{
    m_outputs.clear();
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        OutputInfo info;
        // QScreen::name() 在 KWin 下就是输出名（如 DP-3），与 ScreenShot2 的 CaptureScreen 参数一致
        info.name = screen->name();
        info.logicalGeometry = screen->geometry();
        info.scale = screen->devicePixelRatio();
        m_outputs.append(info);
    }
    m_mapper.setOutputs(m_outputs);
}

bool WaylandBackend::probePermission(QString *detail)
{
    bool ok = false;
    QString message;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    m_shot2.captureRect(QRect(0, 0, 8, 8), {}, [&](const CaptureImage &result) {
        ok = result.ok();
        message = result.error;
        loop.quit();
    });
    timeout.start(kPermissionProbeTimeoutMs);
    loop.exec();

    if (!ok && message.isEmpty())
        message = QStringLiteral("截图探测超时（%1ms）").arg(kPermissionProbeTimeoutMs);

    if (detail) {
        if (ok)
            *detail = QStringLiteral("ScreenShot2 授权通过（8x8 探测截图成功）");
        else if (m_shot2.lastNoAuthorized())
            *detail = QStringLiteral(
                          "ScreenShot2 拒绝授权（NoAuthorized）。KWin 用"
                          "「调用方 PID → /proc/<pid>/exe → desktop 文件 → "
                          "X-KDE-DBUS-Restricted-Interfaces」判定，请先执行 ./install.sh "
                          "并确认 ~/.local/share/applications/seashot.desktop 的 Exec 指向"
                          "正在运行的这个二进制，然后运行 kbuildsycoca6。原始错误：%1")
                          .arg(message);
        else
            *detail = QStringLiteral("截图探测失败：%1").arg(message);
    }
    return ok;
}

bool WaylandBackend::initialize(QString *error)
{
    collectOutputs();

    if (!m_shot2.serviceAvailable()) {
        if (error)
            *error = QStringLiteral("会话总线上没有 org.kde.KWin：当前不是 KWin 会话");
        return false;
    }
    m_shot2Version = m_shot2.version();
    SS_INFO() << "[wayland] ScreenShot2 Version =" << m_shot2Version;

    m_permissionOk = probePermission(&m_permissionDetail);
    if (!m_permissionOk) {
        if (error)
            *error = m_permissionDetail;
        return false;
    }

    QString bridgeError;
    if (!m_bridge.start(&bridgeError)) {
        if (error)
            *error = QStringLiteral("KWin 助手脚本启动失败：%1").arg(bridgeError);
        return false;
    }

    // 自排除：让覆盖层/浮层不出现在任何截图里（Phase 0 实测：不设的话覆盖层会被拍到）
    if (qEnvironmentVariableIsSet("SEASHOT_DISABLE_SELF_EXCLUSION")) {
        SS_WARN() << "[wayland] SEASHOT_DISABLE_SELF_EXCLUSION 已置位：跳过自排除";
    } else {
        QString excludeError;
        if (!m_bridge.requestExcludeOwnWindows(true, &excludeError))
            SS_WARN() << "[wayland] 自排除请求失败：" << excludeError;
    }

    return true;
}

void WaylandBackend::shutdown()
{
    m_bridge.stop();
}

void WaylandBackend::refresh()
{
    // 等一次新快照，最多 1.2s（足够脚本 120ms 节流 + 50ms 命令轮询跑完）
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&m_bridge, &KWinBridge::windowsChanged, &loop, &QEventLoop::quit);
    timeout.start(1200);
    loop.exec();
}

bool WaylandBackend::excludeOwnWindows(bool value, QString *error)
{
    return m_bridge.requestExcludeOwnWindows(value, error);
}

QImage WaylandBackend::cropToOpaqueBounds(const QImage &image)
{
    // 阴影是半透明的，窗口实体（标题栏/边框/客户区）是不透明的 →
    // 用 alpha>=240 的不透明包围盒把阴影边距裁掉（Phase 0 实测精度 ±1px）。
    if (image.isNull())
        return image;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int minX = argb.width();
    int minY = argb.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) >= 240) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY)
        return image; // 全透明（异常情况）→ 原样返回
    return image.copy(QRect(QPoint(minX, minY), QPoint(maxX, maxY)));
}

void WaylandBackend::captureWindow(const WindowInfo &window, ImageCallback callback)
{
    QVariantMap options;
    // Phase 0 实测：无此选项时只返回客户区（不含标题栏/圆角/阴影）
    options.insert(QStringLiteral("include-decoration"), true);

    const bool keepShadow = m_keepShadowMargin;
    m_shot2.captureWindow(window.id, options, [callback, keepShadow](const CaptureImage &result) {
        if (!callback)
            return;
        if (!result.ok() || keepShadow) {
            callback(result);
            return;
        }
        CaptureImage cropped = result;
        cropped.image = cropToOpaqueBounds(result.image);
        callback(cropped);
    });
}

void WaylandBackend::captureRect(const QRectF &logicalRect, ImageCallback callback)
{
    // CaptureArea 收逻辑坐标（Phase 0 实测 scale=1 时一致）；取整策略统一走 ScreenMapper
    const OutputInfo *output = m_mapper.outputAt(logicalRect.center());
    QRect request;
    if (output)
        request = m_mapper.toPhysical(logicalRect, *output).translated(output->logicalGeometry.topLeft());
    else
        request = QRect(ScreenMapper::roundCoord(logicalRect.x()), ScreenMapper::roundCoord(logicalRect.y()),
                        qMax(1, ScreenMapper::roundCoord(logicalRect.width())),
                        qMax(1, ScreenMapper::roundCoord(logicalRect.height())));

    SS_INFO() << "[wayland] CaptureArea 请求矩形(逻辑坐标):" << request;
    m_shot2.captureRect(request, {}, std::move(callback));
}

void WaylandBackend::ensureHotkeyAction()
{
    if (m_hotkeyAction)
        return;
    m_hotkeyAction = new QAction(this);
    m_hotkeyAction->setObjectName(QStringLiteral("capture"));
    m_hotkeyAction->setText(QStringLiteral("SeaShot 截图"));
    connect(m_hotkeyAction, &QAction::triggered, this, [this]() {
        if (m_hotkeyCallback)
            m_hotkeyCallback();
    });
    // 注意：这里**不碰绑定**。以前在 setHotkey() 里建 action 时顺手 setShortcut({})，
    // 而"空 = 解绑"之后，一次纯查询就会把用户的键解掉 —— 所以拆成两个函数。
}

bool WaylandBackend::setHotkey(const QKeySequence &sequence, QString *error)
{
    ensureHotkeyAction();

    // 空序列 = **明确解绑**。以前这里是"空就什么都不做"，于是 --set-hotkey none
    // 实际上解不掉旧键（KGlobalAccel 仍按配置里的旧值生效），排查了很久。
    const QList<QKeySequence> keys =
        sequence.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{sequence};
    KGlobalAccel::self()->setDefaultShortcut(m_hotkeyAction, keys);
    KGlobalAccel::self()->setShortcut(m_hotkeyAction, keys, KGlobalAccel::NoAutoloading);

    const QList<QKeySequence> current = KGlobalAccel::self()->shortcut(m_hotkeyAction);
    QStringList readable;
    for (const QKeySequence &key : current)
        readable << key.toString(QKeySequence::NativeText);
    SS_INFO() << "[hotkey] 当前绑定:" << (readable.isEmpty() ? QStringLiteral("(未绑定)") : readable.join(", "));

    if (error)
        error->clear();
    return true;
}

QString WaylandBackend::hotkeyStatus() const
{
    // 只读：按需把 action 建出来（不改变任何绑定），这样"查状态"不会顺手改注册表
    const_cast<WaylandBackend *>(this)->ensureHotkeyAction();
    if (!m_hotkeyAction)
        return QStringLiteral("(未注册)");
    const QList<QKeySequence> keys = KGlobalAccel::self()->shortcut(m_hotkeyAction);
    if (keys.isEmpty())
        return QStringLiteral("(未绑定)");
    QStringList readable;
    for (const QKeySequence &key : keys)
        readable << key.toString(QKeySequence::NativeText);
    return readable.join(QStringLiteral(", "));
}

QString WaylandBackend::limitations() const
{
    return QStringLiteral(
        "Wayland 下窗口几何/栈序来自 KWin 助手脚本；窗口截图走 ScreenShot2 的 "
        "include-decoration（含标题栏/圆角/阴影），区域截图走 CaptureArea。");
}

QString WaylandBackend::diagnostics() const
{
    QStringList outputs;
    for (const OutputInfo &o : m_outputs) {
        outputs << QStringLiteral("%1 %2x%3+%4+%5 scale=%6")
                       .arg(o.name)
                       .arg(o.logicalGeometry.width())
                       .arg(o.logicalGeometry.height())
                       .arg(o.logicalGeometry.x())
                       .arg(o.logicalGeometry.y())
                       .arg(o.scale);
    }
    return QStringLiteral("后端: wayland\n"
                          "ScreenShot2 Version: %1\n"
                          "授权: %2\n"
                          "输出: %3\n"
                          "%4")
        .arg(m_shot2Version)
        .arg(m_permissionDetail)
        .arg(outputs.join(QStringLiteral(" | ")))
        .arg(m_bridge.diagnostics());
}

} // namespace seashot
