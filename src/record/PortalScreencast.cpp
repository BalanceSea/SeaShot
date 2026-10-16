#include "record/PortalScreencast.h"

#include "util/Log.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>

#include <unistd.h>

namespace seashot {

namespace {
constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kScreenCastIface = "org.freedesktop.portal.ScreenCast";
constexpr const char *kSessionIface = "org.freedesktop.portal.Session";
constexpr const char *kRequestIface = "org.freedesktop.portal.Request";

// 会话类型：1=显示器(monitor) 2=窗口 4=虚拟
constexpr uint kSourceTypeMonitor = 1;
// 光标模式：0=隐藏 1=嵌入 2=元数据
constexpr uint kCursorModeEmbedded = 1;
} // namespace

QString PortalScreencast::newToken(const QString &prefix)
{
    // portal 要求 token 只含 [A-Za-z0-9_]（会当成 D-Bus 对象路径元素校验）。
    // ⚠️ 注意 QString::arg(value, 10) 的第二个参数是**字段宽度**不是进制，
    //    之前这么写会生成 "seashot_  61130904"（带空格）→ portal 报 Invalid token。
    const quint32 value = QRandomGenerator::global()->generate();
    return QStringLiteral("%1_%2").arg(prefix, QString::number(value, 16));
}

QString PortalScreencast::requestPathFor(const QString &token)
{
    // 规范：/org/freedesktop/portal/desktop/request/<sender>/<handle_token>。
    // ⚠️ 实测（dbus-monitor）：portal 对唯一名的处理是**去掉开头的 ':'**，再把 '.' 换成 '_'
    //    —— ":1.50529" → "1_50529"；如果按"把 ':' 也换成 '_'"来猜（"_1_50529"）就会订阅错路径，
    //    于是 portal 发了 Response 也收不到（表现为一直挂着）。
    QString sender = QDBusConnection::sessionBus().baseService();
    if (sender.startsWith(QLatin1Char(':')))
        sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

QString PortalScreencast::Result::describe() const
{
    QStringList parts;
    parts << QStringLiteral("ok=%1").arg(ok ? 1 : 0);
    parts << QStringLiteral("session=%1").arg(sessionPath.isEmpty() ? QStringLiteral("(none)")
                                                                   : sessionPath);
    parts << QStringLiteral("streams=%1").arg(streams.size());
    parts << QStringLiteral("streamsPresent=%1").arg(streamsPresent ? 1 : 0);
    for (const StreamInfo &stream : streams)
        parts << QStringLiteral("node=%1").arg(stream.nodeId);
    parts << QStringLiteral("usedRestoreToken=%1").arg(usedRestoreToken ? 1 : 0);
    parts << QStringLiteral("restoreToken=%1")
                 .arg(restoreToken.isEmpty() ? QStringLiteral("(none)") : restoreToken);
    parts << QStringLiteral("startLatencyMs=%1").arg(startLatencyMs);
    if (!error.isEmpty())
        parts << QStringLiteral("error=%1").arg(error);
    return parts.join(QLatin1Char(' '));
}

PortalScreencast::PortalScreencast(QObject *parent)
    : QObject(parent)
{
}

int PortalScreencast::portalVersion()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg << QString::fromLatin1(kScreenCastIface) << QStringLiteral("version");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
        return -1;
    return reply.arguments().first().value<QDBusVariant>().variant().toInt();
}

void PortalScreencast::fail(const QString &message)
{
    if (m_finished)
        return;
    m_finished = true;
    m_responseHandler = nullptr;
    Result result;
    result.sessionPath = m_sessionPath;
    result.error = message;
    if (m_callback)
        m_callback(result);
}

void PortalScreencast::onPortalResponse(uint response, const QVariantMap &results)
{
    auto handler = m_responseHandler;
    m_responseHandler = nullptr;
    if (!handler)
        return;
    handler(response, results);
}

void PortalScreencast::callWithResponse(
    const QString &interface, const QString &method, const QVariantList &leadingArgs,
    const QVariantMap &options, const std::function<void(uint, const QVariantMap &)> &callback)
{
    const QString token = newToken(QStringLiteral("seashot"));
    const QString path = requestPathFor(token);

    // 先订阅应答（应答可能比方法返回更早；这是规范建议的顺序）
    m_responseHandler = callback;
    QDBusConnection::sessionBus().connect(QString::fromLatin1(kPortalService), path,
                                          QString::fromLatin1(kRequestIface),
                                          QStringLiteral("Response"), this,
                                          SLOT(onPortalResponse(uint, QVariantMap)));

    QVariantMap callOptions = options;
    callOptions.insert(QStringLiteral("handle_token"), token);

    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService),
                                                      QString::fromLatin1(kPortalPath), interface,
                                                      method);
    QVariantList args = leadingArgs;
    args << callOptions;
    msg.setArguments(args);

    QTextStream diag(stdout);
    diag << "PORTAL_SCREENCAST_REQUEST " << method << " predictedPath=" << path << "\n";
    diag.flush();

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 30000), this);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                     [this, watcher, method](QDBusPendingCallWatcher *) {
                         const QDBusMessage reply = watcher->reply();
                         watcher->deleteLater();
                         if (reply.type() == QDBusMessage::ErrorMessage) {
                             SS_WARN() << "[portal]" << method << "调用失败:" << reply.errorMessage();
                             fail(QStringLiteral("%1 调用失败：%2").arg(method, reply.errorMessage()));
                             return;
                         }
                         // 方法返回的就是请求对象的真实路径 —— 用它校验我们预测的路径
                         if (!reply.arguments().isEmpty()) {
                             const QString actual =
                                 reply.arguments().first().value<QDBusObjectPath>().path();
                             QTextStream out(stdout);
                             out << "PORTAL_SCREENCAST_REQUEST_HANDLE " << method
                                 << " actualPath=" << (actual.isEmpty() ? QStringLiteral("(empty)")
                                                                        : actual)
                                 << "\n";
                             out.flush();
                         }
                     });

    // 每步超时：portal 不回应时报明确错误，而不是一直挂着
    QTimer::singleShot(20000, this, [this, method]() {
        if (m_responseHandler != nullptr)
            fail(QStringLiteral("%1 等待应答超时（20s）—— portal 没有发出 Response 信号").arg(method));
    });
}

QString PortalScreencast::hostAppId()
{
    // 与桌面文件名保持一致：~/.local/share/applications/seashot.desktop → "seashot"
    return QStringLiteral("seashot");
}

bool PortalScreencast::registerHostAppId(const QString &appId, QString *error)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QStringLiteral("org.freedesktop.host.portal.Registry"), QStringLiteral("Register"));
    msg << appId << QVariantMap {};
    // 用同步调用 + 超时，避免又出现"挂死"（这个接口在旧版 portal 上不存在）
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error)
            *error = reply.errorMessage();
        return false;
    }
    return true;
}

void PortalScreencast::start(const QString &restoreToken, uint persistMode, Callback callback,
                             bool stopBeforeStart)
{
    {
        QString registerError;
        const bool registered = registerHostAppId(hostAppId(), &registerError);
        QTextStream out(stdout);
        out << "PORTAL_SCREENCAST_REGISTER appId=" << hostAppId()
            << " ok=" << (registered ? 1 : 0);
        if (!registered)
            out << " error=" << registerError;
        out << "\n";
        out.flush();
    }

    m_callback = std::move(callback);
    m_finished = false;
    const bool usingToken = !restoreToken.isEmpty();

    QTextStream out(stdout);
    out << "PORTAL_SCREENCAST_BEGIN restoreToken="
        << (usingToken ? restoreToken : QStringLiteral("(none)")) << " persistMode=" << persistMode
        << " portalVersion=" << portalVersion() << "\n";
    out.flush();

    QVariantMap createOptions;
    if (persistMode > 0)
        createOptions.insert(QStringLiteral("persist_mode"), persistMode);
    if (usingToken)
        createOptions.insert(QStringLiteral("restore_token"), restoreToken);
    createOptions.insert(QStringLiteral("session_handle_token"), newToken(QStringLiteral("seashot_s")));

    callWithResponse(
        QString::fromLatin1(kScreenCastIface), QStringLiteral("CreateSession"), {}, createOptions,
        [this, usingToken, stopBeforeStart](uint code, const QVariantMap &results) {
            if (code != 0) {
                fail(QStringLiteral("CreateSession 被拒绝或取消（response=%1）").arg(code));
                return;
            }
            {
                QTextStream diag(stdout);
                diag << "PORTAL_SCREENCAST_RESPONSE CreateSession code=" << code << " keys="
                     << results.keys().join(QLatin1Char(',')) << " session_handleType="
                     << results.value(QStringLiteral("session_handle")).typeName() << "\n";
                diag.flush();
            }
            m_sessionPath = results.value(QStringLiteral("session_handle")).toString();
            if (m_sessionPath.isEmpty()) {
                // 某些版本以对象路径类型返回
                m_sessionPath =
                    results.value(QStringLiteral("session_handle")).value<QDBusObjectPath>().path();
            }
            if (m_sessionPath.isEmpty()) {
                fail(QStringLiteral("CreateSession 没有返回 session_handle"));
                return;
            }

            QVariantMap selectOptions;
            selectOptions.insert(QStringLiteral("types"), kSourceTypeMonitor);
            selectOptions.insert(QStringLiteral("multiple"), false);
            selectOptions.insert(QStringLiteral("cursor_mode"), kCursorModeEmbedded);

            callWithResponse(
                QString::fromLatin1(kScreenCastIface), QStringLiteral("SelectSources"),
                {QVariant::fromValue(QDBusObjectPath(m_sessionPath))}, selectOptions,
                [this, usingToken, stopBeforeStart](uint selectCode, const QVariantMap &) {
                    if (selectCode != 0) {
                        fail(QStringLiteral("SelectSources 失败（response=%1）").arg(selectCode));
                        return;
                    }
                    if (stopBeforeStart) {
                        m_finished = true;
                        Result result;
                        result.sessionPath = m_sessionPath;
                        result.usedRestoreToken = usingToken;
                        result.ok = true; // 链路自检：走到 SelectSources 就算通
                        QTextStream(stdout)
                            << "PORTAL_SCREENCAST_DRY_RUN ok=1 session=" << m_sessionPath
                            << "（未 Start，所以没有弹授权对话框）\n";
                        QTextStream(stdout).flush();
                        if (m_callback)
                            m_callback(result);
                        return;
                    }
                    auto timer = std::make_shared<QElapsedTimer>();
                    timer->start();
                    callWithResponse(
                        QString::fromLatin1(kScreenCastIface), QStringLiteral("Start"),
                        {QVariant::fromValue(QDBusObjectPath(m_sessionPath)), QString()}, {},
                        [this, usingToken, timer](uint startCode, const QVariantMap &startResults) {
                            if (m_finished)
                                return;
                            m_finished = true;
                            Result result;
                            result.sessionPath = m_sessionPath;
                            result.startLatencyMs = timer->elapsed();
                            result.usedRestoreToken = usingToken;
                            result.streamsPresent = startResults.contains(QStringLiteral("streams"));
                            {
                                QTextStream diag(stdout);
                                diag << "PORTAL_SCREENCAST_RESPONSE Start code=" << startCode
                                     << " keys=" << startResults.keys().join(QLatin1Char(','))
                                     << " startLatencyMs=" << result.startLatencyMs << "\n";
                                diag.flush();
                            }
                            if (startCode != 0) {
                                result.error =
                                    QStringLiteral("Start 被取消或失败（response=%1）").arg(startCode);
                                if (m_callback)
                                    m_callback(result);
                                return;
                            }
                            result.restoreToken =
                                startResults.value(QStringLiteral("restore_token")).toString();
                            // 注意：这里**故意不手工迭代 streams**（a(ua{sv})）。
                            // 实测读取嵌套结构时 Qt 会直接 abort：
                            //   "QDBusArgument: write from a read-only object" + SIGABRT。
                            // 诊断阶段只需要知道"有没有流"，用 OpenPipeWireRemote 判定即可
                            // （没有流时它会报 No streams available，已实测）。
                            result.ok = !result.sessionPath.isEmpty() && result.streamsPresent;
                            if (!result.ok && result.error.isEmpty())
                                result.error = QStringLiteral("Start 成功但没有 streams");
                            SS_INFO() << "[portal] ScreenCast 会话建立:" << result.describe();
                            if (m_callback)
                                m_callback(result);
                        });
                });
        });
}

int PortalScreencast::openPipeWireRemote(QString *error)
{
    if (m_sessionPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("还没有会话，无法取 PipeWire 远程 fd");
        return -1;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService),
                                                      QString::fromLatin1(kPortalPath),
                                                      QString::fromLatin1(kScreenCastIface),
                                                      QStringLiteral("OpenPipeWireRemote"));
    msg << QVariant::fromValue(QDBusObjectPath(m_sessionPath)) << QVariantMap();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 10000);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        if (error)
            *error = reply.type() == QDBusMessage::ErrorMessage
                ? reply.errorMessage()
                : QStringLiteral("OpenPipeWireRemote 没有返回 fd");
        return -1;
    }
    const QDBusUnixFileDescriptor descriptor = reply.arguments().first().value<QDBusUnixFileDescriptor>();
    if (!descriptor.isValid()) {
        if (error)
            *error = QStringLiteral("OpenPipeWireRemote 返回的 fd 无效");
        return -1;
    }
    return ::dup(descriptor.fileDescriptor()); // 复制一份，避免随 reply 释放而关闭
}

void PortalScreencast::closeSession()
{
    if (m_sessionPath.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kPortalService),
                                                      m_sessionPath,
                                                      QString::fromLatin1(kSessionIface),
                                                      QStringLiteral("Close"));
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
    m_sessionPath.clear();
}

} // namespace seashot
