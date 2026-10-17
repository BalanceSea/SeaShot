#include "util/DesktopServices.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace seashot {

bool openKdeShortcutSettings(QString *error)
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("systemsettings"));
    if (program.isEmpty()) {
        if (error)
            *error = QStringLiteral("找不到 systemsettings，请手动打开：系统设置 → 快捷键");
        return false;
    }
    // Older launches may carry a process-wide layer-shell override. A child
    // application must get a normal desktop shell, otherwise its window can
    // become a fullscreen layer. Keep the parent environment unchanged.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"));
    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("kcm_keys")});
    process.setProcessEnvironment(environment);
    if (!process.startDetached()) {
        if (error)
            *error = QStringLiteral("启动 systemsettings 失败");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

} // namespace seashot
