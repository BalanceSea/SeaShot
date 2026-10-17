// X11 后端（xcb + xcb-ewmh）。
//
// 现状（PLAN 已定）：**代码级 + XWayland 冒烟**。本机没有可登录的 X11 会话
// （/usr/share/xsessions 为空），所有输出都会标注「X11 会话未实测」。
//
// 已知限制：
//  - 窗口截图用 XGetImage 取 frame 矩形 → 窗口被遮挡时会截到遮挡物
//  - X11 没有 excludeFromCapture 的等价物 → 上层在截图前隐藏覆盖层
#pragma once

#include "backend/CaptureBackend.h"

#include <QAbstractNativeEventFilter>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_ewmh.h>
#include <span>

namespace seashot {

class X11Backend : public CaptureBackend, public QAbstractNativeEventFilter
{
public:
    X11Backend();
    ~X11Backend() override;

    QString name() const override { return QStringLiteral("x11"); }
    bool initialize(QString *error) override;
    void shutdown() override;
    QVector<OutputInfo> outputs() const override { return m_outputs; }
    QVector<WindowInfo> windows() const override;
    void refresh() override {}
    void captureWindow(const WindowInfo &window, ImageCallback callback) override;
    void captureRect(const QRectF &logicalRect, ImageCallback callback) override;
    bool setHotkey(const QKeySequence &sequence, QString *error) override;
    QString hotkeyStatus() const override;
    QString limitations() const override;
    QString diagnostics() const override;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    /// 每个 keycode 占连续的 perKeycode 个 keysym；找不到时返回 0。
    static quint32 findKeycode(std::span<const quint32> keysyms, int perKeycode,
                               quint8 firstKeycode, quint32 keysym);

private:
    QByteArray getProperty(quint32 window, xcb_atom_t property, xcb_atom_t type, bool *ok = nullptr) const;
    QRect clientGeometryOf(quint32 window) const;
    QVector<WindowInfo> collectWindows() const;
    void ungrabHotkey();
    bool grabHotkey(const QKeySequence &sequence, QString *error);
    void releaseShm();
    bool ensureShm(int bytes);
    bool captureViaShm(const QRect &rect, CaptureImage *result);

    xcb_connection_t *m_connection = nullptr;
    xcb_ewmh_connection_t m_ewmh{};
    xcb_screen_t *m_screen = nullptr;
    QVector<OutputInfo> m_outputs;
    bool m_hotkeyGrabbed = false;
    quint32 m_hotkeyKeycode = 0;
    quint32 m_hotkeyModifiers = 0;

    // MIT-SHM：实测 XWayland 下普通 GetImage 返回 BadMatch，ShmGetImage 正常
    bool m_shmAvailable = false;
    xcb_shm_seg_t m_shmSeg = 0;
    int m_shmId = -1;
    void *m_shmAddr = nullptr;
    int m_shmSize = 0;
};

} // namespace seashot
