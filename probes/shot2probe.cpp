// SeaShot Phase 0 探测程序：调用 org.kde.KWin.ScreenShot2 并落盘 PNG。
//
// 用法：
//   shot2probe screen <输出名> <out.png>
//   shot2probe area <x> <y> <w> <h> <out.png>
//   shot2probe window <窗口 handle(uuid)> <out.png>
//   shot2probe activescreen <out.png>
//   shot2probe activewindow <out.png>
//   shot2probe workspace <out.png>
//   shot2probe interactive <kind> <out.png>
// 可选末位参数为 options，形如 "include-cursor=true"
//
// ⚠️ 两条 Phase 0 实测实现约束（都写进 PLAN.md）：
//  1) 必须「异步调用 + 边读管道边等响应」：像素由 KWin 写入我们传入的
//     管道，若先阻塞等响应再读管道，屏幕级图像会撑满管道缓冲区 → 双方
//     互等 → 死锁。
//  2) **不能等 EOF**：KWin 取完图后不会及时关闭它那份写端副本，等 EOF 会
//     一直等到超时（实测空等 60s）。应当用响应里的 width/height/stride
//     算出精确字节数，读满即停。

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QImage>
#include <QTextStream>
#include <QVariantMap>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static QTextStream out(stdout);

static QImage::Format mapFormat(uint f)
{
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

static QVariantMap parseOptions(const QString &spec)
{
    QVariantMap map;
    if (spec.isEmpty())
        return map;
    const QStringList parts = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = part.left(eq).trimmed();
        const QString value = part.mid(eq + 1).trimmed();
        if (value == QLatin1String("true") || value == QLatin1String("1"))
            map.insert(key, true);
        else if (value == QLatin1String("false") || value == QLatin1String("0"))
            map.insert(key, false);
        else
            map.insert(key, value);
    }
    return map;
}

static QVariantMap replyMap(const QDBusMessage &reply)
{
    QVariantMap result;
    if (reply.arguments().isEmpty())
        return result;
    const QVariant rawArg = reply.arguments().at(0);
    result = rawArg.value<QVariantMap>();
    if (result.isEmpty())
        result = qdbus_cast<QVariantMap>(rawArg);
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 3) {
        out << "usage: shot2probe <screen|area|window|activescreen|activewindow|workspace|interactive> ... <out.png>\n";
        return 2;
    }

    const QString mode = a.at(1);
    QString method;
    QVariantList args;
    QString outPath;
    QVariantMap options;

    if (mode == QLatin1String("screen")) {
        method = QStringLiteral("CaptureScreen");
        args << a.value(2);
        outPath = a.value(3);
        options = parseOptions(a.value(4));
    } else if (mode == QLatin1String("area")) {
        method = QStringLiteral("CaptureArea");
        args << a.value(2).toInt() << a.value(3).toInt()
             << a.value(4).toUInt() << a.value(5).toUInt();
        outPath = a.value(6);
        options = parseOptions(a.value(7));
    } else if (mode == QLatin1String("window")) {
        method = QStringLiteral("CaptureWindow");
        args << a.value(2);
        outPath = a.value(3);
        options = parseOptions(a.value(4));
    } else if (mode == QLatin1String("activescreen")) {
        method = QStringLiteral("CaptureActiveScreen");
        outPath = a.value(2);
        options = parseOptions(a.value(3));
    } else if (mode == QLatin1String("activewindow")) {
        method = QStringLiteral("CaptureActiveWindow");
        outPath = a.value(2);
        options = parseOptions(a.value(3));
    } else if (mode == QLatin1String("workspace")) {
        method = QStringLiteral("CaptureWorkspace");
        outPath = a.value(2);
        options = parseOptions(a.value(3));
    } else if (mode == QLatin1String("interactive")) {
        method = QStringLiteral("CaptureInteractive");
        args << a.value(2).toUInt();
        outPath = a.value(3);
        options = parseOptions(a.value(4));
    } else {
        out << "unknown mode: " << mode << "\n";
        return 2;
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        out << "pipe() failed: " << ::strerror(errno) << "\n";
        return 1;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/org/kde/KWin/ScreenShot2"),
        QStringLiteral("org.kde.KWin.ScreenShot2"),
        method);
    QVariantList callArgs = args;
    callArgs << options;
    callArgs << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
    msg.setArguments(callArgs);

    out << "== method: " << method << "\n";
    out << "== options: " << QVariant(options).toString() << "\n";
    out.flush();

    QElapsedTimer timer;
    timer.start();
    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(msg, 60000);
    ::close(fds[1]); // 写端已由 D-Bus 复制给 KWin

    QByteArray raw;
    char buf[65536];
    bool eof = false;
    qint64 replyAtMs = -1;
    qint64 lastDataAtMs = 0;
    qint64 expected = -1;
    QVariantMap result;

    while (timer.elapsed() < 90000) {
        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pr = ::poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = ::read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                raw.append(buf, int(n));
                lastDataAtMs = timer.elapsed();
            } else if (n == 0) {
                eof = true;
            } else if (errno != EINTR && errno != EAGAIN) {
                eof = true;
            }
        }
        if (replyAtMs < 0 && pending.isFinished()) {
            replyAtMs = timer.elapsed();
            const QDBusMessage reply = pending.reply();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                out << "== ERROR name=" << reply.errorName()
                    << " message=" << reply.errorMessage()
                    << " (replyAt=" << replyAtMs << "ms)\n";
                ::close(fds[0]);
                out.flush();
                return 1;
            }
            result = replyMap(reply);
            const int w = result.value(QStringLiteral("width")).toInt();
            const int h = result.value(QStringLiteral("height")).toInt();
            const int s = result.value(QStringLiteral("stride")).toInt();
            if (w > 0 && h > 0 && s > 0)
                expected = qint64(s) * h;
        }
        if (expected > 0 && raw.size() >= expected)
            break; // 像素已读满，不再等 EOF（见文件头注释 2）
        if (eof)
            break;
        if (replyAtMs >= 0 && lastDataAtMs > 0 && timer.elapsed() - lastDataAtMs > 5000)
            break; // 数据流已停 5s
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    const qint64 totalMs = timer.elapsed();
    ::close(fds[0]);

    out << "== reply map:\n";
    for (auto it = result.cbegin(); it != result.cend(); ++it)
        out << "   " << it.key() << " = " << it.value().toString()
            << " (type=" << it.value().typeName() << ")\n";

    const int width = result.value(QStringLiteral("width")).toInt();
    const int height = result.value(QStringLiteral("height")).toInt();
    const int stride = result.value(QStringLiteral("stride")).toInt();
    const uint fmtRaw = result.value(QStringLiteral("format")).toUInt();
    out << "== timing: replyAt=" << replyAtMs << "ms total=" << totalMs
        << "ms eof=" << (eof ? "yes" : "no") << "\n";
    out << "== raw bytes: " << raw.size() << " expected=" << expected << "\n";

    if (raw.isEmpty()) {
        out << "== 管道未读到任何像素\n";
        return 1;
    }
    const QImage::Format fmt = mapFormat(fmtRaw);
    QImage img;
    if (width > 0 && height > 0 && stride > 0 && fmt != QImage::Format_Invalid)
        img = QImage(reinterpret_cast<const uchar *>(raw.constData()), width, height, stride, fmt).copy();
    if (img.isNull()) {
        out << "== QImage construction FAILED (format=" << fmtRaw << ")\n";
        return 1;
    }
    if (!img.save(outPath, "PNG")) {
        out << "== save FAILED: " << outPath << "\n";
        return 1;
    }
    out << "== saved " << outPath << " size=" << img.width() << "x" << img.height() << "\n";
    out.flush();
    return 0;
}
