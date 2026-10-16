// Wayland(KWin) 后端：
//  - 窗口枚举/几何来自 KWin 助手脚本
//  - 截取走 org.kde.KWin.ScreenShot2（窗口图带 include-decoration=true 才是所见即所得）
#pragma once

#include "backend/CaptureBackend.h"
#include "backend/wayland/KWinBridge.h"
#include "backend/wayland/ScreenShot2.h"
#include "geometry/ScreenMapper.h"

#include <QAction>

namespace seashot {

class WaylandBackend : public CaptureBackend
{
    Q_OBJECT

public:
    explicit WaylandBackend(QObject *parent = nullptr);
    ~WaylandBackend() override;

    QString name() const override { return QStringLiteral("wayland"); }
    bool initialize(QString *error) override;
    void shutdown() override;
    QVector<OutputInfo> outputs() const override { return m_outputs; }
    QVector<WindowInfo> windows() const override { return m_bridge.windows(); }
    void refresh() override;
    bool excludeOwnWindows(bool value, QString *error) override;
    void captureWindow(const WindowInfo &window, ImageCallback callback) override;
    void captureRect(const QRectF &logicalRect, ImageCallback callback) override;
    bool setHotkey(const QKeySequence &sequence, QString *error) override;
    QString hotkeyStatus() const override;
    QString limitations() const override;
    QString diagnostics() const override;

    /// 窗口图是否保留阴影边距（PLAN 已定：默认保留；关闭则按 alpha 裁到窗口实体）
    void setKeepShadowMargin(bool keep) { m_keepShadowMargin = keep; }

    /// 授权是否通过（自检用）
    bool permissionOk() const { return m_permissionOk; }
    QString permissionDetail() const { return m_permissionDetail; }
    int screenShotVersion() const { return m_shot2Version; }

private:
    bool probePermission(QString *detail);
    void collectOutputs();
    static QImage cropToOpaqueBounds(const QImage &image);

    KWinBridge m_bridge;
    ScreenShot2 m_shot2;
    ScreenMapper m_mapper;
    QVector<OutputInfo> m_outputs;
    QAction *m_hotkeyAction = nullptr;
    bool m_keepShadowMargin = true;
    bool m_permissionOk = false;
    QString m_permissionDetail;
    int m_shot2Version = -1;
};

} // namespace seashot
