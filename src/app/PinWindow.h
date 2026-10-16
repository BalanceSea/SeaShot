// 贴图 / 钉图浮窗（Phase C）：把一张图钉在屏幕上，可拖动 / 滚轮缩放 / 右键菜单关闭。
//
// 与 Toast 同一套 layer-shell 经验（ISSUE-001 四轮迭代的结论）：
//  - **表面铺满输出**，不动表面本身，只改图在表面内的偏移 ——
//    移动 layer-shell 表面会让 KWin 重排该层，240Hz 鼠标下就是每秒几百次重排 → 卡顿/发涩。
//  - 输入区用 **QWindow::setMask**（Qt 的 Wayland 后端映射成 wl_surface.set_input_region）
//    限制在「贴图 ∪ 右键菜单」上：贴图以外的地方点击**穿透到桌面**，不会挡住正常工作。
//  - 任何 layer-shell 表面都必须 **show() 之前**设好 shell（见 docs/known-issues.md ISSUE-007），
//    否则报 "Cannot set shell integration while there's already a shell surface created"，
//    而且 layerShell=1 / get() 返回非空都是"假成功"。
//  - 整个表面每次全量重绘（一个贴图不大），避免 Toast 那套"脏区 + 帧缓冲轮换"的复杂度：
//    全量重绘时每个 shm 缓冲都会被写全，不会出现拖动拖影。
#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

namespace seashot {

class PinWindow : public QWidget
{
    Q_OBJECT

public:
    /// 右键菜单项（自绘，不用 QMenu：layer-shell overlay 层上的弹出窗口层级不可控）
    enum class MenuItem { None, Copy, Save, Close };

    PinWindow(const QImage &image, const QPoint &globalTopLeft, QWidget *parent = nullptr);

    /// 显示（自动套 layer-shell role）。anchor 已由构造函数给出。
    void showPin();
    void closePin();

    QImage image() const { return m_image; }
    double zoom() const { return m_zoom; }
    /// 贴图在屏幕上的矩形（全局坐标）
    QRect pinRect() const;
    bool layerShellApplied() const { return m_layerShellApplied; }
    bool menuOpen() const { return m_menuOpen; }
    MenuItem menuItemAt(const QPoint &localPos) const;
    QString diagnostics() const;

    /// 诊断/验收：直接指定承载表面（一般不手工调用）
    void setSurfaceForTest(const QRect &surface);

    static constexpr double kMinZoom = 0.2;
    static constexpr double kMaxZoom = 5.0;

signals:
    void copyRequested(const QImage &image);
    void saveRequested(const QImage &image);
    void closed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QSize pinSize() const; ///< 缩放后的贴图尺寸（逻辑像素）
    QRect pinRectLocal() const;
    QRect menuRectLocal() const;
    QVector<QPair<MenuItem, QRect>> menuItems() const;
    void setZoomAnchored(double zoom, const QPoint &anchorLocal);
    void clampOffset();
    void updateInputMask();
    void ensureLayerShell();
    QRect surfaceGeometry() const;
    void activate(MenuItem item);

    QImage m_image;
    QRect m_surface; ///< 承载贴图的固定表面（= 所在输出的几何）
    double m_zoom = 1.0;
    QPoint m_offset{0, 0}; ///< 贴图左上角（表面局部坐标）
    bool m_layerShellApplied = false;

    bool m_dragging = false;
    QPoint m_dragOrigin;
    QPoint m_dragStartPos;

    bool m_menuOpen = false;
    QPoint m_menuAnchor; ///< 菜单锚点（表面局部坐标）
    MenuItem m_hoverItem = MenuItem::None;
    MenuItem m_pressedItem = MenuItem::None;
};

} // namespace seashot
