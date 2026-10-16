#include "backend/BackendFactory.h"

#include "backend/wayland/WaylandBackend.h"
#ifdef SEASHOT_WITH_X11
#include "backend/x11/X11Backend.h"
#endif

namespace seashot {

BackendChoice BackendFactory::resolve(BackendKind requested, const QString &sessionType,
                                      const QString &waylandDisplay, const QString &currentDesktop)
{
    if (requested == BackendKind::X11)
        return {BackendKind::X11, QStringLiteral("显式请求 X11 后端")};
    if (requested == BackendKind::Wayland)
        return {BackendKind::Wayland, QStringLiteral("显式请求 Wayland 后端")};

    if (!waylandDisplay.isEmpty() && sessionType.compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0) {
        if (currentDesktop.contains(QLatin1String("KDE"), Qt::CaseInsensitive)) {
            return {BackendKind::Wayland,
                    QStringLiteral("Wayland 会话 + KDE 桌面（WAYLAND_DISPLAY=%1）").arg(waylandDisplay)};
        }
        return {BackendKind::Wayland,
                QStringLiteral("Wayland 会话但桌面非 KDE（%1）：KWin 接口可能不可用").arg(currentDesktop)};
    }
    return {BackendKind::X11, QStringLiteral("非 Wayland 会话（XDG_SESSION_TYPE=%1）").arg(sessionType)};
}

CaptureBackend *BackendFactory::create(const BackendChoice &choice, QString *error)
{
    switch (choice.kind) {
    case BackendKind::Wayland:
        return new WaylandBackend();
    case BackendKind::X11: {
#ifdef SEASHOT_WITH_X11
        return new X11Backend();
#else
        if (error)
            *error = QStringLiteral("构建时未找到 xcb/xcb-ewmh，X11 后端不可用");
        return nullptr;
#endif
    }
    case BackendKind::Auto:
    default:
        if (error)
            *error = QStringLiteral("未解析的后端类型");
        return nullptr;
    }
}

} // namespace seashot
