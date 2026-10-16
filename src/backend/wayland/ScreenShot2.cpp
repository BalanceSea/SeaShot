#include "backend/wayland/ScreenShot2.h"

#include "util/Log.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace seashot {

namespace {
constexpr const char *kService = "org.kde.KWin";
constexpr const char *kPath = "/org/kde/KWin/ScreenShot2";
constexpr const char *kIface = "org.kde.KWin.ScreenShot2";
constexpr int kReplyTimeoutMs = 20000;

QImage::Format mapFormat(uint f)
{
    // 对应 QImage::Format 枚举值（KWin 返回的是这个枚举）
    switch (f) {
    case 4: return QImage::Format_RGB32;
    case 5: return QImage::Format_ARGB32;
    case 6: return QImage::Format_ARGB32_Premultiplied;
    case 7: return QImage::Format_RGB16;
    case 8: return QImage::Format_RGB666;
    case 9: return QImage::Format_ARGB8565_Premultiplied;
    default: return QImage::Format_Invalid;
    }
}

QVariantMap toMap(const QDBusMessage &reply)
{
    QVariantMap result;
    if (reply.arguments().isEmpty())
        return result;
    const QVariant arg = reply.arguments().at(0);
    result = arg.value<QVariantMap>();
    if (result.isEmpty())
        result = qdbus_cast<QVariantMap>(arg);
    return result;
}
} // namespace

struct ScreenShot2::Request {
    int fd = -1;
    QSocketNotifier *notifier = nullptr;
    QDBusPendingCallWatcher *watcher = nullptr;
    QByteArray data;
    qint64 expected = -1;
    QVariantMap result;
    Callback callback;
    QElapsedTimer timer;
    bool replySeen = false;
    bool finished = false;
    bool stallTimerArmed = false;
    QString method;
};

ScreenShot2::ScreenShot2(QObject *parent)
    : QObject(parent)
{
}

ScreenShot2::~ScreenShot2() = default;

bool ScreenShot2::serviceAvailable() const
{
    if (!QDBusConnection::sessionBus().isConnected())
        return false;
    QDBusConnectionInterface *iface = QDBusConnection::sessionBus().interface();
    return iface && iface->isServiceRegistered(QString::fromLatin1(kService));
}

int ScreenShot2::version() const
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), QString::fromLatin1(kPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg << QString::fromLatin1(kIface) << QStringLiteral("Version");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
        return -1;
    return reply.arguments().first().value<QDBusVariant>().variant().toInt();
}

void ScreenShot2::captureWindow(const QString &handle, const QVariantMap &options, Callback callback)
{
    QVariantList args;
    args << handle;
    start(QStringLiteral("CaptureWindow"), args, options, std::move(callback));
}

void ScreenShot2::captureRect(const QRect &logicalRect, const QVariantMap &options, Callback callback)
{
    QVariantList args;
    args << logicalRect.x() << logicalRect.y()
         << static_cast<uint>(qMax(1, logicalRect.width()))
         << static_cast<uint>(qMax(1, logicalRect.height()));
    start(QStringLiteral("CaptureArea"), args, options, std::move(callback));
}

void ScreenShot2::captureScreen(const QString &outputName, const QVariantMap &options, Callback callback)
{
    QVariantList args;
    args << outputName;
    start(QStringLiteral("CaptureScreen"), args, options, std::move(callback));
}

void ScreenShot2::start(const QString &method, const QVariantList &args, const QVariantMap &options,
                        Callback callback)
{
    int fds[2];
    if (::pipe(fds) != 0) {
        CaptureImage image;
        image.error = QStringLiteral("pipe() 失败: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        if (callback)
            callback(image);
        return;
    }

    auto request = std::make_shared<Request>();
    request->fd = fds[0];
    request->callback = std::move(callback);
    request->method = method;
    request->timer.start();

    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                                      QString::fromLatin1(kPath),
                                                      QString::fromLatin1(kIface), method);
    QVariantList callArgs = args;
    callArgs << options;
    callArgs << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
    msg.setArguments(callArgs);

    const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(msg, kReplyTimeoutMs);
    ::close(fds[1]); // 写端已由 D-Bus 复制给 KWin

    request->watcher = new QDBusPendingCallWatcher(pending, this);
    connect(request->watcher, &QDBusPendingCallWatcher::finished, this,
            [this, request](QDBusPendingCallWatcher *watcher) {
                request->replySeen = true;
                const QDBusMessage reply = watcher->reply();
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    m_lastNoAuthorized = reply.errorName().contains(QLatin1String("NoAuthorized"));
                    finish(request,
                           QStringLiteral("%1: %2").arg(reply.errorName(), reply.errorMessage()));
                    return;
                }
                request->result = toMap(reply);
                const int width = request->result.value(QStringLiteral("width")).toInt();
                const int height = request->result.value(QStringLiteral("height")).toInt();
                const int stride = request->result.value(QStringLiteral("stride")).toInt();
                if (width > 0 && height > 0 && stride > 0)
                    request->expected = qint64(stride) * height;
                maybeFinish(request);
            });

    request->notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, this);
    connect(request->notifier, &QSocketNotifier::activated, this, [this, request]() {
        char buffer[65536];
        const ssize_t n = ::read(request->fd, buffer, sizeof(buffer));
        if (n > 0)
            request->data.append(buffer, int(n));
        else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            request->notifier->setEnabled(false);
        maybeFinish(request);
    });

    // 兜底：超时收尾（含"响应到了但数据不完整"的情况）
    QTimer::singleShot(kReplyTimeoutMs, this, [this, request]() { maybeFinish(request, true); });
}

void ScreenShot2::maybeFinish(const std::shared_ptr<Request> &request, bool force)
{
    if (request->finished)
        return;

    if (!request->replySeen) {
        if (force) {
            request->replySeen = true;
            finish(request, QStringLiteral("截图请求超时（%1ms 内没有响应）").arg(kReplyTimeoutMs));
        }
        return;
    }

    if (request->expected > 0 && request->data.size() >= request->expected) {
        finish(request, QString());
        return;
    }

    if (force) {
        if (request->data.isEmpty())
            finish(request, QStringLiteral("未收到像素数据（超时 %1ms）").arg(kReplyTimeoutMs));
        else
            finish(request, QStringLiteral("像素数据不完整：收到 %1 / 期望 %2 字节")
                               .arg(request->data.size()).arg(request->expected));
    }
}

void ScreenShot2::finish(const std::shared_ptr<Request> &request, const QString &error)
{
    if (request->finished)
        return;
    request->finished = true;

    if (request->notifier) {
        request->notifier->setEnabled(false);
        request->notifier->deleteLater();
        request->notifier = nullptr;
    }
    if (request->watcher) {
        request->watcher->deleteLater();
        request->watcher = nullptr;
    }
    if (request->fd >= 0) {
        ::close(request->fd);
        request->fd = -1;
    }

    CaptureImage out;
    out.error = error;
    if (error.isEmpty()) {
        const int width = request->result.value(QStringLiteral("width")).toInt();
        const int height = request->result.value(QStringLiteral("height")).toInt();
        const int stride = request->result.value(QStringLiteral("stride")).toInt();
        const uint formatRaw = request->result.value(QStringLiteral("format")).toUInt();
        out.scale = request->result.value(QStringLiteral("scale")).toDouble();
        if (out.scale <= 0.0)
            out.scale = 1.0;
        const QImage::Format format = mapFormat(formatRaw);
        if (width > 0 && height > 0 && stride > 0 && format != QImage::Format_Invalid) {
            out.image = QImage(reinterpret_cast<const uchar *>(request->data.constData()), width,
                               height, stride, format)
                            .copy();
        }
        if (out.image.isNull()) {
            out.error = QStringLiteral("无法从返回数据构造图像（format=%1, %2 字节）")
                            .arg(formatRaw)
                            .arg(request->data.size());
        }
    }

    SS_INFO() << "[shot2]" << request->method << (out.ok() ? "ok" : "失败")
              << request->timer.elapsed() << "ms" << out.image.width() << "x" << out.image.height()
              << (out.error.isEmpty() ? QString() : out.error);

    if (request->callback)
        request->callback(out);
}

} // namespace seashot
