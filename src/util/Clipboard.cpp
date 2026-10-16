#include "util/Clipboard.h"

#include "util/Log.h"

#include <QBuffer>
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
