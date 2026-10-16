// 截图结果浮层
//
// 行为约定：
//  - **原尺寸显示**：卡片 == 截图尺寸（1:1）；屏幕可用区放不下才等比缩小并注明
//  - **贴着截图框左下角**：卡片左上角 = 选区左下角；下方放不下则左下角对齐往上盖
//  - **描边**：外圈 3px 深色 + 内圈 2px 高亮色（截图内容不"溶进"桌面背景）
//  - **消失时机**：默认**不自动消失**，只在用户操作后消失（Enter/Esc、另存为完成、编辑、关闭）；
//    `ui/toastAutoHideMs` 可设兜底超时
//
// 拖动实现（ISSUE-001 四轮迭代后的最终形态）：
//   表面**固定铺满输出**（layer-shell 四边锚定、不设边距），卡片只是画在表面内的一个偏移位置，
//   拖动 = 移动这个偏移量。这样 KWin 不需要重排图层，只需要重绘被弄脏的那两块小矩形。
//   早先几版是"移动 layer-shell 表面"（改 set_margin），每改一次 KWin 都要重排该层，
//   240Hz 鼠标下就是每秒几百次重排 —— 表现为卡顿/闪烁/发涩，怎么调节流都不对。
//   输入区域用 setMask 限制在卡片矩形上（Qt 的 Wayland 后端把它映射为
//   wl_surface.set_input_region），所以卡片以外的地方点击会穿透到桌面。
//   设 SEASHOT_TOAST_MOVING_SURFACE=1 可退回旧的"移动表面"实现做 A/B 对比。
#pragma once

#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace seashot {

class Toast : public QWidget
{
    Q_OBJECT

public:
    explicit Toast(QWidget *parent = nullptr);

    /// anchor 是截图源在屏幕上的矩形（窗口 frameGeometry 或选区矩形，全局坐标）
    void showResult(const QImage &image, const QString &savedPath, const QString &message,
                    const QString &copyError, const QRect &anchorGlobalRect);

    void setEditEnabled(bool enabled);
    /// 0 = 不自动消失（默认）
    void setAutoHideMs(int milliseconds);
    /// 更新状态文字（复制/保存完成后调用）
    void updateStatus(const QString &message, const QString &savedPath, const QString &copyError);
    /// 短暂展示后消失（例如另存为完成）
    void dismissAfter(int milliseconds);

    /// 诊断用：浮层边框改成高饱和品红（便于截屏定位实测拖动延迟）
    void setDragMarkerEnabled(bool enabled);
    /// 诊断用：直接走拖动时的定位路径（不经过鼠标事件）
    void dragTo(const QPoint &globalTopLeft) { applyPosition(globalTopLeft); }
    /// 卡片的全局矩形（诊断/基准用）
    QRect currentGeometry() const;
    /// 诊断用：累计提交次数（每次 applyPosition 一次）
    int commitCount() const { return m_commitCount; }
    void resetCommitCount() { m_commitCount = 0; m_commitTimes.clear(); }
    /// 诊断用：每次提交的时间戳（毫秒），用来量提交节奏抖动
    QVector<qint64> commitTimestamps() const { return m_commitTimes; }

signals:
    void editRequested();
    void saveAsRequested();
    void copyRequested();
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Button { None, Edit, SaveAs, Copy, Close };

    // ── 几何 ──────────────────────────────────────────────
    QRect cardRect() const { return QRect(QPoint(0, 0), m_cardSize); } ///< 卡片本地坐标
    int barHeight() const;
    QRect barRect() const;
    QVector<QPair<Button, QRect>> buttons() const;
    Button buttonAt(const QPoint &position) const;
    QPoint cardTopLeftGlobal() const;                  ///< 卡片左上角（全局）
    QRect surfaceGeometry() const;                     ///< 表面覆盖的输出矩形

    // ── 生命周期 ──────────────────────────────────────────
    void rebuildCache();
    void invalidateCache();
    void reposition(const QRect &anchorGlobalRect);
    void applyPosition(const QPoint &globalTopLeft);
    void ensureLayerShell();
    void closeAndNotify();
    void maybeStartAutoHide();

    // ── 状态 ──────────────────────────────────────────────
    QImage m_source;
    QImage m_display;
    bool m_scaledToFit = false;
    QString m_savedPath;
    QString m_message;
    QString m_copyError;

    QSize m_cardSize;      ///< 卡片尺寸（固定表面模式下与表面尺寸不同）
    QPoint m_cardOffset{0, 0}; ///< 卡片在表面内的偏移
    QRect m_lastCardRect;  ///< 上一次的卡片本地矩形（用于弄脏旧位置）
    QVector<QRect> m_recentCardRects; ///< 最近几次的卡片矩形（缓冲轮换需要的并集脏区）
    QPoint m_lastAppliedTopLeftFallback{0, 0}; ///< 旧模式（移动表面）下的卡片左上角

    QPixmap m_cache;       ///< 卡片内容缓存（拖动时只做一次 blit）
    QPixmap m_frameBuffer; ///< 固定表面模式下用来整帧提交脏区（含透明像素）
    bool m_cacheDirty = true;

    QTimer m_autoHide;
    bool m_editEnabled = true;
    bool m_layerShellApplied = false;
    bool m_fixedSurface = true; ///< false = 旧实现（移动表面本身，仅用于 A/B）
    bool m_dragging = false;
    bool m_dragMarker = false;
    QPoint m_dragOrigin;   ///< 拖动时鼠标相对卡片左上角的偏移
    QRect m_lastAnchor;
    int m_autoHideMs = 0;
    int m_commitCount = 0;
    QVector<qint64> m_commitTimes;
};

} // namespace seashot
