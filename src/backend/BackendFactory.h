// 后端选择（选择逻辑是纯函数，便于单元测试与 --selftest 解释原因）
#pragma once

#include "backend/CaptureBackend.h"

namespace seashot {

enum class BackendKind { Auto, Wayland, X11 };

struct BackendChoice {
    BackendKind kind = BackendKind::Auto;
    QString reason;
};

class BackendFactory
{
public:
    /// 依据显式请求 + 会话环境决定用哪个后端。
    /// - requested == X11：总是选 X11（即使当前是 Wayland 会话，用于 XWayland 冒烟）
    /// - requested == Wayland：总是选 Wayland（KWin 缺失时初始化会失败并给出原因）
    /// - requested == Auto：WAYLAND_DISPLAY 非空且会话是 KDE → Wayland；否则 X11
    static BackendChoice resolve(BackendKind requested, const QString &sessionType,
                                const QString &waylandDisplay, const QString &currentDesktop);

    /// 创建后端实例；失败时返回 nullptr 并写 *error。
    static CaptureBackend *create(const BackendChoice &choice, QString *error);
};

} // namespace seashot
