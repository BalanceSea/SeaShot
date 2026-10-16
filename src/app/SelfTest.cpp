#include "app/SelfTest.h"

#include "app/Settings.h"
#include "backend/CaptureBackend.h"
#include "util/Clipboard.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

namespace seashot {

namespace {

constexpr const char *kDesktopFileName = "seashot.desktop";
constexpr const char *kRequiredProperty = "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2";

QString desktopFilePath()
{
    const QStringList dirs{QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation),
                           QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                               + QStringLiteral("/applications")};
    for (const QString &dir : dirs) {
        const QString path = dir + QLatin1Char('/') + QLatin1String(kDesktopFileName);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

void printCheck(const SelfTest::Check &check)
{
    QTextStream out(stdout);
    const QString status = check.ok ? QStringLiteral("OK  ") : (check.fatal ? QStringLiteral("FAIL")
                                                                           : QStringLiteral("WARN"));
    out << QStringLiteral("[%1] %2\n").arg(status, check.name);
    if (!check.detail.isEmpty()) {
        const QStringList lines = check.detail.split(QLatin1Char('\n'));
        for (const QString &line : lines)
            out << QStringLiteral("       ") << line << QLatin1Char('\n');
    }
    out.flush();
}

} // namespace

int SelfTest::run(CaptureBackend *backend, const BackendChoice &choice, Settings *settings,
                  bool x11Requested)
{
    QVector<Check> checks;
    QTextStream out(stdout);

    out << QStringLiteral("SeaShot 自检（Phase 1）\n") << QStringLiteral("========================\n");
    out.flush();

    // 1) 后端解析
    {
        Check check;
        check.name = QStringLiteral("后端选择");
        check.ok = backend != nullptr;
        check.detail = choice.reason;
        checks.append(check);
    }

    // 2) desktop 文件与授权属性（Wayland 必需）
    {
        Check check;
        check.name = QStringLiteral("desktop 文件授权属性");
        const QString path = desktopFilePath();
        if (path.isEmpty()) {
            check.ok = false;
            check.detail = QStringLiteral(
                "未找到 %1。KWin 会用「调用方 PID → /proc/<pid>/exe → desktop 文件 → "
                "X-KDE-DBUS-Restricted-Interfaces」判定截图授权；请先执行 ./install.sh")
                               .arg(QLatin1String(kDesktopFileName));
            check.fatal = (choice.kind == BackendKind::Wayland);
        } else {
            QFile file(path);
            file.open(QIODevice::ReadOnly);
            const QString content = QString::fromUtf8(file.readAll());
            const bool hasProperty = content.contains(QLatin1String(kRequiredProperty));
            const bool execMatches = content.contains(QCoreApplication::applicationFilePath());
            check.ok = hasProperty && execMatches;
            check.detail = QStringLiteral("%1\n属性: %2\nExec 指向当前二进制: %3")
                               .arg(path, hasProperty ? QStringLiteral("有") : QStringLiteral("缺失"),
                                    execMatches ? QStringLiteral("是") : QStringLiteral("否"));
            check.fatal = (choice.kind == BackendKind::Wayland);
        }
        checks.append(check);
    }

    // 3) 后端初始化结果 / 输出与缩放
    if (backend) {
        Check check;
        check.name = QStringLiteral("输出与缩放");
        const QVector<OutputInfo> outputs = backend->outputs();
        check.ok = !outputs.isEmpty();
        QStringList lines;
        for (const OutputInfo &o : outputs) {
            lines << QStringLiteral("%1 %2x%3+%4+%5 scale=%6")
                         .arg(o.name)
                         .arg(o.logicalGeometry.width())
                         .arg(o.logicalGeometry.height())
                         .arg(o.logicalGeometry.x())
                         .arg(o.logicalGeometry.y())
                         .arg(o.scale);
        }
        check.detail = lines.join(QLatin1Char('\n'));
        checks.append(check);

        Check windowsCheck;
        windowsCheck.name = QStringLiteral("窗口枚举");
        backend->refresh();
        const QVector<WindowInfo> windows = backend->windows();
        windowsCheck.ok = !windows.isEmpty();
        QStringList windowLines;
        int shown = 0;
        for (const WindowInfo &w : windows) {
            if (shown++ >= 6)
                break;
            windowLines << QStringLiteral("%1 appId=%2 frame=%3,%4 %5x%6 stack=%7")
                               .arg(w.id, w.appId)
                               .arg(w.frameGeometry.x())
                               .arg(w.frameGeometry.y())
                               .arg(w.frameGeometry.width())
                               .arg(w.frameGeometry.height())
                               .arg(w.stackIndex);
        }
        windowLines.prepend(QStringLiteral("共 %1 个窗口").arg(windows.size()));
        windowsCheck.detail = windowLines.join(QLatin1Char('\n'));
        checks.append(windowsCheck);

        Check captureCheck;
        captureCheck.name = QStringLiteral("区域截图（32x32 探测）");
        bool ok = false;
        QString detail;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        backend->captureRect(QRectF(0, 0, 32, 32), [&](const CaptureImage &result) {
            ok = result.ok();
            detail = result.ok() ? QStringLiteral("得到 %1x%2，scale=%3")
                                       .arg(result.image.width())
                                       .arg(result.image.height())
                                       .arg(result.scale)
                                 : result.error;
            loop.quit();
        });
        timeout.start(15000);
        loop.exec();
        captureCheck.ok = ok;
        captureCheck.detail = detail;
        checks.append(captureCheck);
    }

    // 4) 剪贴板：wl-copy 可用 + 真实往返
    {
        Check check;
        check.name = QStringLiteral("剪贴板（wl-copy）");
        check.ok = Clipboard::wlCopyAvailable();
        check.detail = check.ok ? QStringLiteral("找到 wl-copy") : QStringLiteral("未找到 wl-copy");
        checks.append(check);

        if (check.ok) {
            Check roundTrip;
            roundTrip.name = QStringLiteral("剪贴板内容往返");
            QImage probe(4, 4, QImage::Format_ARGB32);
            probe.fill(QColor(12, 34, 56));
            QString copyError;
            const bool copied = Clipboard::copyImage(probe, &copyError);
            bool hasPng = false;
            if (copied) {
                QProcess paste;
                paste.start(QStringLiteral("wl-paste"), {QStringLiteral("--list-types")});
                if (paste.waitForFinished(3000))
                    hasPng = QString::fromLocal8Bit(paste.readAllStandardOutput())
                                 .contains(QLatin1String("image/png"));
            }
            roundTrip.ok = copied && hasPng;
            roundTrip.detail = roundTrip.ok
                ? QStringLiteral("wl-paste --list-types 见到 image/png")
                : QStringLiteral("复制=%1 读到 image/png=%2 错误=%3")
                      .arg(copied ? QStringLiteral("成功") : QStringLiteral("失败"),
                           hasPng ? QStringLiteral("是") : QStringLiteral("否"), copyError);
            checks.append(roundTrip);

            QProcess::startDetached(QStringLiteral("wl-copy"), {QStringLiteral("--clear")});
        }
    }

    // 5) 后端限制说明（非致命提示）
    if (backend) {
        Check check;
        check.name = QStringLiteral("后端限制（务必阅读）");
        check.ok = true;
        check.fatal = false;
        check.detail = backend->limitations();
        if (x11Requested)
            check.detail += QStringLiteral("\n注意：本次是 X11 后端：**X11 会话未实测**，仅 XWayland 冒烟。");
        checks.append(check);
    }

    if (settings) {
        Check check;
        check.name = QStringLiteral("配置");
        check.ok = true;
        check.fatal = false;
        check.detail = QStringLiteral("%1\n保存目录: %2\n截图后复制: %3\n保留阴影边距: %4\n快捷键: %5")
                           .arg(settings->configFilePath(), settings->saveDir(),
                                settings->copyAfterCapture() ? QStringLiteral("是") : QStringLiteral("否"),
                                settings->keepShadowMargin() ? QStringLiteral("是") : QStringLiteral("否"),
                                settings->hotkey().isEmpty() ? QStringLiteral("(未绑定)")
                                                             : settings->hotkey());
        checks.append(check);
    }

    out << QLatin1Char('\n');
    for (const Check &check : checks)
        printCheck(check);

    int failures = 0;
    for (const Check &check : checks) {
        if (!check.ok && check.fatal)
            ++failures;
    }
    out << QStringLiteral("\n结果: %1 项检查，%2 项致命失败\n").arg(checks.size()).arg(failures);
    out.flush();
    return failures == 0 ? 0 : 1;
}

} // namespace seashot
