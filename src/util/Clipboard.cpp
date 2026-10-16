#include "util/Clipboard.h"

#include "util/Log.h"

#include <QBuffer>
#include <QFile>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>

namespace seashot {

namespace {
QByteArray encodePng(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!image.save(&buffer, "PNG"))
        return {};
    return bytes;
}
} // namespace

bool Clipboard::wlCopyAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty();
}

bool Clipboard::copyFile(const QString &path, const QString &mimeType, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        if (error)
            *error = QStringLiteral("文件是空的：%1").arg(path);
        return false;
    }
    const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
    if (wlCopy.isEmpty()) {
        if (error)
            *error = QStringLiteral("找不到 wl-copy");
        return false;
    }
    // ⚠️ 必须用 startDetached + shell 重定向，不能用 QProcess 自己写管道：
    //    wl-copy 靠 fork 出的子进程持有选区，而"复制 GIF"之后浮层会自动关闭 → 进程退出，
    //    QProcess 一析构、管道一关，那个持有选区的子进程就跟着死了 ——
    //    表现就是命令报成功、剪贴板却是空的（实测 "Nothing is copied" 且没有 wl-copy 进程）。
    //    detached 之后 wl-copy 完全脱离我们，进程退出后仍能粘贴。
    QString escaped = path;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    const QString command = QStringLiteral("%1 --type %2 < '%3'")
                                .arg(QStringLiteral("wl-copy"), mimeType, escaped);
    qint64 pid = 0;
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command}, QString(),
                                 &pid)) {
        if (error)
            *error = QStringLiteral("无法启动 wl-copy");
        return false;
    }
    SS_INFO() << "[clipboard] 已把文件写入剪贴板（detached pid=" << pid << "）" << mimeType
              << bytes.size() << "字节";
    return true;
}

bool Clipboard::copyImage(const QImage &image, QString *error)
{
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("空图像，未写入剪贴板");
        return false;
    }

    const QByteArray png = encodePng(image);
    if (png.isEmpty()) {
        if (error)
            *error = QStringLiteral("PNG 编码失败");
        return false;
    }

    const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
    if (!wlCopy.isEmpty()) {
        QProcess process;
        process.setProgram(wlCopy);
        process.setArguments({QStringLiteral("--type"), QStringLiteral("image/png")});
        process.start();
        if (process.waitForStarted(2000)) {
            process.write(png);
            process.closeWriteChannel();
            if (process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit
                && process.exitCode() == 0) {
                SS_INFO() << "[clipboard] 已通过 wl-copy 写入" << png.size() << "字节";
                return true;
            }
            if (error) {
                *error = QStringLiteral("wl-copy 失败(exit=%1): %2")
                             .arg(process.exitCode())
                             .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
            }
        } else if (error) {
            *error = QStringLiteral("wl-copy 启动失败");
        }
        SS_WARN() << "[clipboard] wl-copy 路径失败，回退 QClipboard：" << (error ? *error : QString());
    } else if (error) {
        *error = QStringLiteral("未找到 wl-copy（wl-clipboard 未安装）");
    }

    // 回退：QClipboard（进程退出后内容可能丢失，但至少有内容）
    if (QGuiApplication::clipboard()) {
        QGuiApplication::clipboard()->setImage(image);
        return true;
    }
    return false;
}

} // namespace seashot
