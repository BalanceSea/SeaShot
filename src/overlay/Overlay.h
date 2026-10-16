// 选择模式的覆盖层（layer-shell 全屏 + QPainter 绘制）
//
// 画：整屏压暗 + 悬停窗口的虚线框（贴 frameGeometry，PLAN 已定）+ 拖拽选区 + 尺寸提示
// 交互：左键单击=截该窗口；按下拖动=矩形选区；Esc/右键=取消
#pragma once

#include "core/Types.h"

#include <QPointF>
#include <QWidget>

namespace seashot {

class Overlay : public QWidget
{
    Q_OBJECT

public:
    explicit Overlay(QWidget *parent = nullptr);

    void setWindows(const QVector<WindowInfo> &windows);
    void setVirtualGeometry(const QRect &logicalGeometry);

    /// 显示覆盖层（会自动套 layer-shell role / X11 绕过 WM）
    void startSelection();
    void stopSelection();

    bool layerShellApplied() const { return m_layerShellApplied; }
    QString diagnostics() const;

signals:
    void selectionFinished(const SelectionResult &result);
    void selectionCancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyLayerShell();
    QPointF toLogical(const QPointF &local) const;
    QRectF toLocal(const QRectF &logical) const;
    void updateHover(const QPointF &local);
    QRectF currentDragRect() const;
    void drawInfoBox(QPainter &painter, const QPoint &anchor, const QString &title,
                     const QString &detail) const;

    QVector<WindowInfo> m_windows;
    QRect m_virtualGeometry;
    WindowInfo m_hover;
    bool m_hasHover = false;
    bool m_dragging = false;
    QPointF m_dragStart;
    QPointF m_dragCurrent;
    bool m_layerShellApplied = false;
};

} // namespace seashot
