#include "hotkey/GlobalShortcutsPortal.h"

#include "util/Log.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

namespace seashot {

namespace {
constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kShortcutsIface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char *kSessionIface = "org.freedesktop.portal.Session";
constexpr const char *kRequestIface = "org.freedesktop.portal.Request";
constexpr const char *kHostAppId = "seashot";

/// BindShortcuts 的第二个参数是 a(sa{sv})，在 Qt 里就是 QList<QPair<QString, QVariantMap>>。
/// ⚠️ 这个类型必须显式注册，否则调用直接失败：
///    "Marshalling failed: Unregistered type QList<std::pair<QString,QMap<QString,QVariant>>>"
///    （ScreenCast 那边注册的是 QList<QPair<uint,QVariantMap>>，不是同一个类型。）
void ensureShortcutListTypeRegistered()
{
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalShortcutEntry>();
        qDBusRegisterMetaType<QList<PortalShortcutEntry>>();
        return true;
    }();
    Q_UNUSED(registered)
}

/// 与 ScreenCast 同一套身份处理：portal 的 app id 关联是**按 D-Bus 连接**的，
/// 且一条连接一生只能关联一次 —— 所以开私有连接，让它上面的第一个调用就是 Register。
QDBusConnection portalBus()
{
    static QDBusConnection connection = [] {
        QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                            QStringLiteral("seashot-portal"));
        if (!bus.isConnected()) {
            SS_WARN() << "[hotkey-portal] 私有 D-Bus 连接建立失败，退回默认连接";
            return QDBusConnection::sessionBus();
        }
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
            QStringLiteral("org.freedesktop.host.portal.Registry"), QStringLiteral("Register"));
        msg << QString::fromLatin1(kHostAppId) << QVariantMap();
        const QDBusMessage reply = bus.call(msg, QDBus::Block, 5000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            // "already associated" 也算成功（说明身份已经定了）
            if (!reply.errorMessage().contains(QLatin1String("already")))
                SS_WARN() << "[hotkey-portal] Register appId 失败:" << reply.errorMessage();
        }
        return bus;
    }();
    return connection;
}
} // namespace

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcutEntry &entry)
{
    argument.beginStructure();
    argument << entry.id << entry.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcutEntry &entry)
{
    argument.beginStructure();
    argument >> entry.id >> entry.properties;
    argument.endStructure();
    return argument;
}

QString GlobalShortcutsPortal::Result::describe() const
{
    QStringList parts;
    parts << QStringLiteral("ok=%1").arg(ok ? 1 : 0) << QStringLiteral("denied=%1").arg(denied ? 1 : 0);
    for (const Shortcut &shortcut : shortcuts) {
        parts << QStringLiteral("%1=%2").arg(shortcut.id,
                                             shortcut.trigger.isEmpty() ? QStringLiteral("(未绑定)")
                                                                        : shortcut.trigger);
    }
    if (latencyMs >= 0)
        parts << QStringLiteral("latencyMs=%1").arg(latencyMs);
    if (!error.isEmpty())
        parts << QStringLiteral("error=%1").arg(error);
    return parts.join(QLatin1Char(' '));
}

GlobalShortcutsPortal::GlobalShortcutsPortal(QObject *parent)
    : QObject(parent)
    , m_bus(portalBus())
{
    ensureShortcutListTypeRegistered();
}

bool GlobalShortcutsPortal::available(uint *version, QString *error)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg << QString::fromLatin1(kShortcutsIface) << QStringLiteral("version");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error)
            *error = QStringLiteral("portal 没有 GlobalShortcuts 接口：%1").arg(reply.errorMessage());
        return false;
    }
    if (version) {
        const QVariant value = reply.arguments().value(0).value<QDBusVariant>().variant();
        *version = value.toUInt();
    }
    if (error)
        error->clear();
    return true;
}

QString GlobalShortcutsPortal::toXdgTrigger(const QString &qtSequence)
{
    if (qtSequence.trimmed().isEmpty())
        return QString();
    QString xdg = qtSequence.trimmed().toUpper();
    xdg.replace(QLatin1Char(' '), QString());
    return xdg;
}

QString GlobalShortcutsPortal::shortcutId()
{
    return QStringLiteral("screenshot");
}

QString GlobalShortcutsPortal::newToken(const QString &prefix)
{
    const quint32 value = QRandomGenerator::global()->generate();
    return QStringLiteral("%1_%2").arg(prefix, QString::number(value, 16));
}

QString GlobalShortcutsPortal::requestPathFor(const QString &token)
{
    QString sender = portalBus().baseService();
    if (sender.startsWith(QLatin1Char(':')))
        sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

QVector<GlobalShortcutsPortal::Shortcut> GlobalShortcutsPortal::parseShortcuts(const QVariant &value)
{
    QVector<Shortcut> shortcuts;
    if (!value.canConvert<QDBusArgument>())
        return shortcuts;
    QDBusArgument argument = value.value<QDBusArgument>();
    QList<PortalShortcutEntry> entries;
    argument >> entries;
    for (const PortalShortcutEntry &entry : entries) {
        Shortcut shortcut;
        shortcut.id = entry.id;
        for (const QString &key : {QStringLiteral("trigger_description"), QStringLiteral("trigger"),
                                   QStringLiteral("preferred_trigger")}) {
            if (entry.properties.contains(key)) {
                shortcut.trigger = entry.properties.value(key).toString();
                break;
            }
        }
        shortcuts.append(shortcut);
    }
    return shortcuts;
}

void GlobalShortcutsPortal::finishResult(Result result)
{
    if (result.denied)
        emit denied(result.error);
    emit bound(result);
}

void GlobalShortcutsPortal::callWithResponse(
    const QString &method, const QVariantList &leadingArgs, const QVariantMap &options,
    const std::function<void(uint, const QVariantMap &)> &callback, int timeoutMs)
{
    const QString token = newToken(QStringLiteral("seashot"));
    const QString path = requestPathFor(token);
    m_responseHandler = callback;
    m_pendingMethod = method;
    const quint64 generation = ++m_generation;

    // 先订阅再调用（应答可能早于方法返回 —— 规范建议的顺序）
    m_bus.connect(QString::fromLatin1(kPortalService), path, QString::fromLatin1(kRequestIface),
                  QStringLiteral("Response"), this, SLOT(onPortalResponse(uint, QVariantMap)));

    QVariantMap callOptions = options;
    callOptions.insert(QStringLiteral("handle_token"), token);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kShortcutsIface), method);
    QVariantList args = leadingArgs;
    args << callOptions;
    msg.setArguments(args);

    QTextStream diag(stdout);
    diag << "HOTKEY_PORTAL_REQUEST " << method << " predictedPath=" << path
         << " args=" << args.size() << " signature=" << msg.signature() << "\n";
    diag.flush();

    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(msg, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, method]() {
        const QDBusMessage reply = watcher->reply();
        watcher->deleteLater();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            SS_WARN() << "[hotkey-portal]" << method << "调用失败:" << reply.errorMessage();
            if (m_responseHandler) {
                m_responseHandler = nullptr;
                Result result;
                result.error = QStringLiteral("%1 调用失败：%2").arg(method, reply.errorMessage());
                finishResult(result);
            }
        }
    });

    QTimer::singleShot(timeoutMs, this, [this, method, timeoutMs, generation]() {
        // ⚠️ 每次调用都有自己的 generation：否则前一次调用留下的定时器会在 20s 后
        //    打到**当前**这次调用的处理函数上（实测：明明卡在 BindShortcuts，却报
        //    "CreateSession 等待用户应答超时" —— 排查起来很误导）。
        if (m_responseHandler == nullptr || generation != m_generation)
            return;
        m_responseHandler = nullptr;
        Result result;
        result.denied = true; // 没等到应答：当作"没授权"，不是崩溃
        result.error = QStringLiteral("%1 等待用户应答超时（%2ms）—— 授权对话框没被处理？")
                           .arg(method)
                           .arg(timeoutMs);
        finishResult(result);
    });
}

void GlobalShortcutsPortal::onPortalResponse(uint response, const QVariantMap &results)
{
    const std::function<void(uint, const QVariantMap &)> handler = m_responseHandler;
    m_responseHandler = nullptr;
    if (handler)
        handler(response, results);
}

void GlobalShortcutsPortal::createSession(const std::function<void(bool)> &then)
{
    if (!m_sessionPath.isEmpty()) {
        then(true);
        return;
    }
    QVariantMap options;
    options.insert(QStringLiteral("session_handle_token"), newToken(QStringLiteral("seashot_s")));
    callWithResponse(
        QStringLiteral("CreateSession"), {}, options,
        [this, then](uint response, const QVariantMap &results) {
            if (response != 0) {
                then(false);
                return;
            }
            m_sessionPath = results.value(QStringLiteral("session_handle")).toString();
            SS_INFO() << "[hotkey-portal] 会话已建立:" << m_sessionPath;
            then(!m_sessionPath.isEmpty());
        },
        20000);
}

void GlobalShortcutsPortal::connectActivationSignals()
{
    static bool connected = false;
    if (connected)
        return;
    connected = m_bus.connect(QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
                              QString::fromLatin1(kShortcutsIface), QStringLiteral("Activated"),
                              this, SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    SS_INFO() << "[hotkey-portal] Activated 信号订阅:" << (connected ? "成功" : "失败");
    const bool changed = m_bus.connect(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kShortcutsIface), QStringLiteral("ShortcutsChanged"), this,
        SLOT(onShortcutsChanged(QDBusObjectPath, QVariantMap)));
    SS_INFO() << "[hotkey-portal] ShortcutsChanged 信号订阅:" << (changed ? "成功" : "失败");
}

void GlobalShortcutsPortal::onShortcutsChanged(const QDBusObjectPath &session,
                                               const QVariantMap &shortcuts)
{
    Q_UNUSED(session)
    Result result;
    result.ok = true;
    result.shortcuts = parseShortcuts(QVariant::fromValue(shortcuts));
    SS_INFO() << "[hotkey-portal] 绑定有变化:" << result.describe();
    emit shortcutsChanged(result);
    emit listed(result); // 复用同一个"拿到列表"的通知，调用方少接一个信号
}

void GlobalShortcutsPortal::onActivated(const QDBusObjectPath &session, const QString &shortcutId,
                                        qulonglong timestamp, const QVariantMap &options)
{
    Q_UNUSED(timestamp)
    Q_UNUSED(options)
    if (!m_sessionPath.isEmpty() && session.path() != m_sessionPath)
        return;
    SS_INFO() << "[hotkey-portal] 快捷键被按下:" << shortcutId;
    emit activated(shortcutId);
}

void GlobalShortcutsPortal::requestBind(const QVector<Shortcut> &shortcuts, int timeoutMs)
{
    if (shortcuts.isEmpty())
        return;
    m_requested = shortcuts;
    QElapsedTimer timer;
    timer.start();
    connectActivationSignals();

    createSession([this, shortcuts, timeoutMs, timer](bool sessionOk) {
        if (!sessionOk) {
            Result result;
            result.error = QStringLiteral("无法建立 GlobalShortcuts 会话");
            finishResult(result);
            return;
        }
        // a(sa{sv})：每个快捷键 = (id, {description, preferred_trigger})
        QList<PortalShortcutEntry> list;
        for (const Shortcut &shortcut : shortcuts) {
            QVariantMap props;
            props.insert(QStringLiteral("description"), shortcut.description);
            if (!shortcut.preferredTrigger.isEmpty())
                props.insert(QStringLiteral("preferred_trigger"), shortcut.preferredTrigger);
            list.append({shortcut.id, props});
        }
        QVariantMap options;
        callWithResponse(
            QStringLiteral("BindShortcuts"),
            {QVariant::fromValue(QDBusObjectPath(m_sessionPath)), QVariant::fromValue(list), QString()},
            options,
            [this, timer](uint response, const QVariantMap &results) {
                Result result;
                result.latencyMs = timer.elapsed();
                if (response != 0) {
                    result.denied = true;
                    result.error = response == 1 ? QStringLiteral("用户拒绝了这次授权")
                                                 : QStringLiteral("授权被取消（response=%1）").arg(response);
                    SS_WARN() << "[hotkey-portal] 未授权:" << result.error;
                } else {
                    result.ok = true;
                    result.shortcuts = parseShortcuts(results.value(QStringLiteral("shortcuts")));
                    // 应答里没给 triggers 时，保留我们请求的 id 列表（trigger 留空表示"未绑定"）
                    if (result.shortcuts.isEmpty())
                        result.shortcuts = m_requested;
                    SS_INFO() << "[hotkey-portal] 已授权:" << result.describe();
                }
                finishResult(result);
            },
            timeoutMs);
    });
}

void GlobalShortcutsPortal::configure(const QString &parentWindow)
{
    if (m_sessionPath.isEmpty()) {
        Result result;
        result.error = QStringLiteral("还没有会话，先申请一次注册");
        finishResult(result);
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService),
                                                      QString::fromLatin1(kPortalPath),
                                                      QString::fromLatin1(kShortcutsIface),
                                                      QStringLiteral("ConfigureShortcuts"));
    // ⚠️ session_handle 是 D-Bus 的 'o'（对象路径），塞 QString 会**发不出去** ——
    //    而且 NoBlock 模式下连错误都看不见（实测：portal 日志里根本没有这次调用）。
    msg << QVariant::fromValue(QDBusObjectPath(m_sessionPath)) << parentWindow << QVariantMap();
    const QDBusMessage reply = m_bus.call(msg, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        SS_WARN() << "[hotkey-portal] ConfigureShortcuts 调用失败:" << reply.errorMessage();
        QTextStream stream(stdout);
        stream << "HOTKEY_PORTAL_CONFIGURE_ERROR " << reply.errorMessage() << "\n";
        stream.flush();
        return;
    }
    SS_INFO() << "[hotkey-portal] 已请求打开系统快捷键配置对话框";
    QTextStream stream(stdout);
    stream << "HOTKEY_PORTAL_CONFIGURE_SENT\n";
    stream.flush();
    // 用户改完后用 ListShortcuts 刷新一次（由调用方决定何时）
}

void GlobalShortcutsPortal::listShortcuts(int timeoutMs)
{
    createSession([this, timeoutMs](bool sessionOk) {
        if (!sessionOk) {
            Result result;
            result.error = QStringLiteral("无法建立 GlobalShortcuts 会话");
            finishResult(result);
            return;
        }
        callWithResponse(
            QStringLiteral("ListShortcuts"), {QVariant::fromValue(QDBusObjectPath(m_sessionPath))}, {},
            [this](uint response, const QVariantMap &results) {
                Result result;
                result.ok = response == 0;
                if (response != 0)
                    result.error = QStringLiteral("ListShortcuts response=%1").arg(response);
                else
                    result.shortcuts = parseShortcuts(results.value(QStringLiteral("shortcuts")));
                emit listed(result);
            },
            timeoutMs);
    });
}

void GlobalShortcutsPortal::closeSession()
{
    if (m_sessionPath.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService),
                                                      m_sessionPath,
                                                      QString::fromLatin1(kSessionIface),
                                                      QStringLiteral("Close"));
    m_bus.call(msg, QDBus::NoBlock);
    m_sessionPath.clear();
}

} // namespace seashot
