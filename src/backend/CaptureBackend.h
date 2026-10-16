// 截图后端抽象：Wayland(KWin) 与 X11 两套实现共用上层 UI/编辑逻辑
#pragma once

#include "core/Types.h"

#include <QKeySequence>
#include <QObject>
#include <QString>

#include <functional>

namespace seashot {

class CaptureBackend : public QObject
{
    Q_OBJECT

public:
    using ImageCallback = std::function<void(const CaptureImage &)>;
    using HotkeyCallback = std::function<void()>;

    explicit CaptureBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~CaptureBackend() override = default;

    /// 后端名（"--backend" 与自检输出用）
    virtual QString name() const = 0;

    /// 建立连接/加载必要组件（KWin 助手脚本、X11 连接…）
    virtual bool initialize(QString *error) = 0;
    virtual void shutdown() {}

    virtual QVector<OutputInfo> outputs() const = 0;

    /// 当前窗口快照（Wayland 来自 KWin 脚本推送，X11 来自 EWMH）
    virtual QVector<WindowInfo> windows() const = 0;
    virtual void refresh() {}

    /// 把本进程自己的窗口排除出截图（Wayland: KWin 脚本设 excludeFromCapture）
    virtual bool excludeOwnWindows(bool value, QString *error)
    {
        Q_UNUSED(value)
        if (error)
            *error = QStringLiteral("该后端不支持自排除");
        return false;
    }

    /// 截取窗口：约定 **所见即所得**（含标题栏/边框/圆角/阴影）
    virtual void captureWindow(const WindowInfo &window, ImageCallback callback) = 0;

    /// 按逻辑坐标矩形截取屏幕
    virtual void captureRect(const QRectF &logicalRect, ImageCallback callback) = 0;

    /// 全局快捷键（空序列 = 不绑定）。Wayland 走 KGlobalAccel，X11 走 XGrabKey。
    virtual bool setHotkey(const QKeySequence &sequence, QString *error)
    {
        Q_UNUSED(sequence)
        if (error)
            *error = QStringLiteral("该后端不支持快捷键");
        return false;
    }

    /// 快捷键被按下时的回调（由上层注册）
    void setHotkeyCallback(HotkeyCallback callback) { m_hotkeyCallback = std::move(callback); }

    /// 当前实际生效的快捷键（读回注册后的结果），用于设置界面与自检
    virtual QString hotkeyStatus() const { return {}; }

    /// 路径依赖的说明（X11 下窗口被遮挡等），用于 --selftest 与文档
    virtual QString limitations() const { return {}; }

    virtual QString diagnostics() const { return {}; }

protected:
    HotkeyCallback m_hotkeyCallback;
};

} // namespace seashot
