#include "backend/wayland/KWinBridge.h"

#include "util/Log.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QCoreApplication>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

namespace seashot {

namespace {
constexpr const char *kServiceTemplate = "org.seashot.SeaShot.p%1";
constexpr const char *kServicePlaceholder = "__SEASHOT_SERVICE__";
constexpr const char *kObjectPath = "/KWinBridge";
constexpr const char *kKWinService = "org.kde.KWin";
constexpr const char *kScriptingPath = "/Scripting";
constexpr const char *kScriptingIface = "org.kde.kwin.Scripting";

QString serviceNameFor(qint64 pid)
{
    return QString::fromLatin1(kServiceTemplate).arg(pid);
}

QRectF rectFromJson(const QJsonObject &obj)
{
    if (obj.isEmpty())
        return {};
    return QRectF(obj.value(QStringLiteral("x")).toDouble(), obj.value(QStringLiteral("y")).toDouble(),
                  obj.value(QStringLiteral("width")).toDouble(),
                  obj.value(QStringLiteral("height")).toDouble());
}

QString scriptResourcePath()
{
    return QStringLiteral(":/seashot/resources/seashot-kwin.js");
}
} // namespace

KWinBridge::KWinBridge(QObject *parent)
    : QObject(parent)
{
    m_pluginName = QStringLiteral("seashot-%1").arg(QCoreApplication::applicationPid());
}

QString KWinBridge::serviceName()
{
    return serviceNameFor(QCoreApplication::applicationPid());
}

KWinBridge::~KWinBridge()
{
    stop();
}

bool KWinBridge::writeScriptToCache(QString *path, QString *error) const
{
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) {
        if (error)
            *error = QStringLiteral("拿不到缓存目录（XDG_CACHE_HOME）");
        return false;
    }
    QDir().mkpath(cacheDir);
    // 脚本里带本进程的 D-Bus 服务名 → 文件名也按 pid 区分，避免多实例互相覆盖
    const QString target = cacheDir
        + QStringLiteral("/seashot-kwin-%1.js").arg(QCoreApplication::applicationPid());
    QFile source(scriptResourcePath());
    if (!source.exists() || !source.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("内嵌的 KWin 脚本资源缺失: %1").arg(scriptResourcePath());
        return false;
    }
    QByteArray content = source.readAll();
    source.close();

    const QByteArray placeholder(kServicePlaceholder);
    if (!content.contains(placeholder)) {
        if (error)
            *error = QStringLiteral("KWin 脚本里找不到服务名占位符 %1").arg(kServicePlaceholder);
        return false;
    }
    content.replace(placeholder, serviceName().toUtf8());

    // 内容相同就不重写，避免每次启动都改 mtime
    QFile existing(target);
    if (existing.exists() && existing.open(QIODevice::ReadOnly)
        && existing.readAll() == content) {
        existing.close();
        *path = target;
        return true;
    }
    existing.close();

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 %1: %2").arg(target, out.errorString());
        return false;
    }
    out.write(content);
    out.close();
    *path = target;
    return true;
}

bool KWinBridge::start(QString *error)
{
    if (m_running)
        return true;

    if (!QDBusConnection::sessionBus().interface()
        || !QDBusConnection::sessionBus().interface()->isServiceRegistered(QString::fromLatin1(kKWinService))) {
        if (error)
            *error = QStringLiteral("会话总线上没有 %1（当前不是 KWin 会话？）").arg(kKWinService);
        return false;
    }

    // 1) 先注册我们自己的 D-Bus 服务：脚本一加载就会往这里推快照
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString service = serviceName();
    if (!bus.registerService(service)) {
        if (error)
            *error = QStringLiteral("无法注册 D-Bus 服务 %1").arg(service);
        return false;
    }
    m_serviceRegistered = true;
    if (!bus.registerObject(QString::fromLatin1(kObjectPath), this,
                            QDBusConnection::ExportAllSlots)) {
        bus.unregisterService(service);
        m_serviceRegistered = false;
        if (error)
            *error = QStringLiteral("无法导出 D-Bus 对象 %1").arg(kObjectPath);
        return false;
    }

    // 2) 释放脚本并加载（先扫掉上次崩溃/被 kill 后留下的陈旧脚本）
    sweepStaleScripts();
    QString scriptPath;
    if (!writeScriptToCache(&scriptPath, error)) {
        stop();
        return false;
    }

    QDBusInterface scripting(QString::fromLatin1(kKWinService), QString::fromLatin1(kScriptingPath),
                             QString::fromLatin1(kScriptingIface), bus);
    if (!scripting.isValid()) {
        if (error)
            *error = QStringLiteral("%1 的 %2 接口不可用：%3")
                         .arg(kKWinService, kScriptingPath, scripting.lastError().message());
        stop();
        return false;
    }

    const QDBusReply<int> loaded = scripting.call(QStringLiteral("loadScript"), scriptPath, m_pluginName);
    if (!loaded.isValid()) {
        if (error)
            *error = QStringLiteral("loadScript 失败: %1").arg(loaded.error().message());
        stop();
        return false;
    }
    const int scriptId = loaded.value();

    // 3) 只运行我们这一个脚本（避免影响其它已加载脚本，例如别的应用的 KWin 桥）
    bool ran = false;
    const QString scriptObject = QStringLiteral("/Scripting/Script%1").arg(scriptId);
    QDBusMessage runCall = QDBusMessage::createMethodCall(QString::fromLatin1(kKWinService),
                                                         scriptObject, QString(), QStringLiteral("run"));
    const QDBusMessage runReply = bus.call(runCall, QDBus::Block, 5000);
    if (runReply.type() != QDBusMessage::ErrorMessage)
        ran = true;

    if (!ran) {
        const QDBusReply<void> started = scripting.call(QStringLiteral("start"));
        if (!started.isValid()) {
            if (error)
                *error = QStringLiteral("脚本已加载但无法运行（Script%1.run 与 start() 均失败）")
                             .arg(scriptId);
            stop();
            return false;
        }
    }

    m_running = true;
    SS_INFO() << "[kwin] 脚本已加载 pluginName=" << m_pluginName << "id=" << scriptId
              << "run方式=" << (ran ? "Script<N>.run" : "Scripting.start");

    // 4) 等首个快照（超时不算失败：界面打开时会再等一次）
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &KWinBridge::windowsChanged, &loop, &QEventLoop::quit);
    timeout.start(1500);
    if (m_windows.isEmpty())
        loop.exec();

    return true;
}

void KWinBridge::stop()
{
    if (m_serviceRegistered) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!m_pluginName.isEmpty()) {
            QDBusInterface scripting(QString::fromLatin1(kKWinService),
                                     QString::fromLatin1(kScriptingPath),
                                     QString::fromLatin1(kScriptingIface), bus);
            if (scripting.isValid())
                scripting.call(QStringLiteral("unloadScript"), m_pluginName);
        }
        bus.unregisterObject(QString::fromLatin1(kObjectPath));
        bus.unregisterService(serviceName());
        m_serviceRegistered = false;
        // 删掉本进程的脚本文件，别让它变成下一次启动要清理的「陈旧脚本」
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheDir.isEmpty())
            QFile::remove(cacheDir + QStringLiteral("/seashot-kwin-%1.js")
                                         .arg(QCoreApplication::applicationPid()));
        SS_INFO() << "[kwin] 脚本已卸载 pluginName=" << m_pluginName;
    }
    m_running = false;
}

QString KWinBridge::activeWindowId() const
{
    for (const WindowInfo &w : m_windows) {
        if (w.active)
            return w.id;
    }
    return {};
}

bool KWinBridge::requestExcludeOwnWindows(bool value, QString *error)
{
    if (!m_running) {
        if (error)
            *error = QStringLiteral("KWin 助手脚本未运行，无法设置自排除");
        return false;
    }
    QJsonObject command;
    command.insert(QStringLiteral("action"), QStringLiteral("exclude-from-capture"));
    command.insert(QStringLiteral("match"), QStringLiteral("seashot"));
    command.insert(QStringLiteral("value"), value);
    enqueue(QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact)));
    return true;
}

bool KWinBridge::ownWindowsExcluded() const
{
    bool found = false;
    for (const WindowInfo &w : m_windows) {
        if (w.captureOverlay) {
            found = true;
            if (!w.excludedFromCapture)
                return false;
        }
    }
    return found;
}

QString KWinBridge::diagnostics() const
{
    return QStringLiteral("KWin 助手脚本: %1 (pluginName=%2)\n"
                          "快照窗口数: %3（最近 %4 次收到推送）\n"
                          "最近快照: %5 ms 前")
        .arg(m_running ? QStringLiteral("运行中") : QStringLiteral("未运行"), m_pluginName)
        .arg(m_windows.size())
        .arg(m_publishedCount)
        .arg(m_lastSnapshotMs == 0 ? QStringLiteral("-")
                                   : QString::number(QDateTime::currentMSecsSinceEpoch() - m_lastSnapshotMs));
}

void KWinBridge::sweepStaleScripts()
{
    // 进程被 kill（或崩溃）时来不及 unloadScript → KWin 里会残留一个同名脚本，
    // 而它记着的「自排除」状态会继续把之后所有 SeaShot 窗口标成排除出截图
    // （实测踩过：浮层/覆盖层再也不会被任何截图拍到，排查了很久）。
    // 这里根据缓存目录里的脚本文件回忆出旧的 pluginName，进程已死的就卸掉。
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty())
        return;
    QDir dir(cacheDir);
    const QStringList stale = dir.entryList({QStringLiteral("seashot-kwin-*.js")}, QDir::Files);
    if (stale.isEmpty())
        return;

    QDBusInterface scripting(QString::fromLatin1(kKWinService), QString::fromLatin1(kScriptingPath),
                             QString::fromLatin1(kScriptingIface), QDBusConnection::sessionBus());
    const qint64 ownPid = QCoreApplication::applicationPid();
    for (const QString &fileName : stale) {
        QString pidText = fileName;
        pidText.remove(QStringLiteral("seashot-kwin-")).remove(QStringLiteral(".js"));
        const qint64 pid = pidText.toLongLong();
        if (pid == ownPid || pid <= 0)
            continue;
        if (QFile::exists(QStringLiteral("/proc/%1").arg(pid)))
            continue; // 还活着：不是陈旧脚本（多实例并存）
        const QString pluginName = QStringLiteral("seashot-%1").arg(pid);
        if (scripting.isValid())
            scripting.call(QStringLiteral("unloadScript"), pluginName);
        QFile::remove(dir.filePath(fileName));
        SS_INFO() << "[kwin] 已清理陈旧助手脚本" << pluginName;
    }
}

void KWinBridge::enqueue(const QString &command)
{
    m_commands.append(command);
}

void KWinBridge::Publish(const QString &json)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        SS_WARN() << "[kwin] 快照 JSON 解析失败:" << parseError.errorString();
        return;
    }

    const QJsonArray array = doc.object().value(QStringLiteral("windows")).toArray();
    QVector<WindowInfo> windows;
    windows.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();
        WindowInfo info;
        info.id = obj.value(QStringLiteral("id")).toString();
        info.pid = obj.value(QStringLiteral("pid")).toInt();
        info.appId = obj.value(QStringLiteral("appId")).toString();
        info.title = obj.value(QStringLiteral("title")).toString();
        info.frameGeometry = rectFromJson(obj.value(QStringLiteral("frame")).toObject());
        info.clientGeometry = rectFromJson(obj.value(QStringLiteral("client")).toObject());
        info.stackIndex = obj.value(QStringLiteral("stackIndex")).toInt(-1);
        info.active = obj.value(QStringLiteral("active")).toBool();
        info.minimized = obj.value(QStringLiteral("minimized")).toBool();
        info.normal = obj.value(QStringLiteral("normal")).toBool();
        info.visible = obj.value(QStringLiteral("visible")).toBool(true);
        info.excludedFromCapture = obj.value(QStringLiteral("excludedFromCapture")).toBool();
        info.captureOverlay = obj.value(QStringLiteral("captureOverlay")).toBool();
        if (info.id.isEmpty() || info.frameGeometry.isEmpty())
            continue;
        windows.append(info);
    }

    m_windows = windows;
    m_lastSnapshotMs = QDateTime::currentMSecsSinceEpoch();
    ++m_publishedCount;
    emit windowsChanged();
}

QString KWinBridge::TakeCommand()
{
    if (m_commands.isEmpty())
        return {};
    return m_commands.takeFirst();
}

} // namespace seashot
