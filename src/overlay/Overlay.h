// 选择模式的覆盖层（layer-shell 全屏 + QPainter 绘制）
//
// 两个阶段（Phase A1 起）：
//   Selecting ：整屏压暗 + 悬停窗口虚线框 + 拖拽选区 + 尺寸提示
//               左键单击=截该窗口；按下拖动=矩形选区；Esc/右键=取消
//   Annotating：选区已定，**不打开独立窗口**，就地 1:1 绘制冻结屏幕 + 选区边框/8 手柄
//               Enter/双击=完成；Esc/右键=取消
//
// Annotating 只在**交互式**截图里启用（见 setAnnotationEnabled）：
// `--rect` / `--window` / `--save` 这些脚本式调用必须保持原有行为不变，
// 否则 Phase 1 验收与所有自动化会全崩。
#pragma once

#include "core/Types.h"

#include <QPointF>
#include <QWidget>

namespace seashot {

class Overlay : public QWidget
{
    Q_OBJECT

public:
    /// 选择模式的用途：截图或录制 GIF（只影响提示文字与上层如何处理结果）
    enum class Intent { Screenshot, Record };

    explicit Overlay(QWidget *parent = nullptr);

    void setIntent(Intent intent);
    Intent intent() const { return m_intent; }

    void setWindows(const QVector<WindowInfo> &windows);
    void setVirtualGeometry(const QRect &logicalGeometry);

    /// 交互式截图是否允许进入 Annotating 阶段。默认 false（脚本式调用走原路径）。
    void setAnnotationEnabled(bool enabled) { m_annotationEnabled = enabled; }
    bool annotationEnabled() const { return m_annotationEnabled; }
    bool isAnnotating() const { return m_stage == Stage::Annotating; }

    /// 显示覆盖层（会自动套 layer-shell role / X11 绕过 WM）
    void startSelection();
    void stopSelection();

    /// 进入就地标注阶段：选区已定，冻结屏幕按**逻辑像素 1:1** 画在覆盖层上。
    /// frozenPhysical 是整块虚拟桌面的截图（物理像素），frozenScale 是它的缩放因子。
    /// 调用后覆盖层保持可见（必要时重新 grabKeyboard），不会打开任何新窗口。
    void beginAnnotating(const SelectionResult &selection, const QImage &frozenPhysical,
                         double frozenScale = 1.0);

    bool layerShellApplied() const { return m_layerShellApplied; }
    QString diagnostics() const;

    // ── 诊断/验收（--inplace-demo）──────────────────────────
    /// Annotating 阶段的关键量：annotating=0/1 frozen=WxH selection=x,y WxH stage=...
    QString annotationDiagnostics() const;
    /// 冻结屏幕（已归一到逻辑像素）——验收用它跟覆盖层画面做**逐像素 1:1 比对**
    const QImage &frozenFrame() const { return m_frozenFrame; }
    /// 当前选区在覆盖层本地坐标下的矩形
    QRectF localSelectionRect() const { return toLocal(m_selection.logicalRect); }

signals:
    void selectionFinished(const SelectionResult &result);
    void selectionCancelled();
    /// Annotating 阶段：用户确认（Enter/双击）
    void annotationConfirmed(const SelectionResult &result);
    /// Annotating 阶段：用户取消（Esc/右键）
    void annotationCancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Stage { Selecting, Annotating };

    void applyLayerShell();
    void paintAnnotating(QPainter &painter);
    /// 选区装饰（边框 + 8 手柄）的 8 个点（覆盖层本地坐标）
    QVector<QPointF> handlePoints() const;
    QPointF toLogical(const QPointF &local) const;
    QRectF toLocal(const QRectF &logical) const;
    void updateHover(const QPointF &local);
    QRectF currentDragRect() const;
    void drawInfoBox(QPainter &painter, const QPoint &anchor, const QString &title,
                     const QString &detail) const;

    QVector<WindowInfo> m_windows;
    Intent m_intent = Intent::Screenshot;
    QRect m_virtualGeometry;
    WindowInfo m_hover;
    bool m_hasHover = false;
    bool m_dragging = false;
    QPointF m_dragStart;
    QPointF m_dragCurrent;
    bool m_layerShellApplied = false;

    Stage m_stage = Stage::Selecting;
    bool m_annotationEnabled = false;
    QImage m_frozenFrame;        ///< 整屏冻结图（已归一到逻辑像素，1:1 可画）
    SelectionResult m_selection; ///< Annotating 阶段固定的选区
};

} // namespace seashot
