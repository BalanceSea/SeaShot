// 选择模式的覆盖层（layer-shell 全屏 + QPainter 绘制）
//
// 两个阶段（Phase A1 起）：
//   Selecting ：整屏压暗 + 悬停窗口虚线框 + 拖拽选区 + 尺寸提示
//               左键单击=截该窗口；按下拖动=矩形选区；Esc/右键=取消
//   Annotating：选区已定，**不打开独立窗口**，就地 1:1 绘制冻结屏幕 + 选区边框/8 手柄
//               + 自绘图标工具条（A2）+ 在选区上画标注 / 撤销（A3）
//               Enter/双击=完成；Esc/右键=取消；1–9 切工具；Ctrl+Z/Y=撤销重做
//
// 坐标约定（A3 起很关键）：
//   * 标注存**选区图像坐标**（原点 = 选区左上角），与即将导出的那张图同一个像素空间，
//     所以「预览 == 导出」不需要任何换算：预览用的就是 renderAnnotations() 的输出
//     （m_composedSelection），导出时对抓到的选区图再调一次同一个函数。
//   * m_frozenSelection 是冻结屏幕裁到选区的那一块（= 导出坐标系的源图）。
//
// Annotating 只在**交互式**截图里启用（见 setAnnotationEnabled）：
// `--rect` / `--window` / `--save` 这些脚本式调用必须保持原有行为不变，
// 否则 Phase 1 验收与所有自动化会全崩。
//
// 工具条为什么**自绘**而不用子控件：layer-shell 上子控件的输入区/合成在本项目反复咬人
// （ISSUE-001 拖动卡顿、ISSUE-007 自排除），`Toast` 用自绘走通了，照它做。
// 命中判定要点（Phase 0 已验证）：工具条矩形**优先**于标注逻辑；落在工具条内但不在按钮上
// 也算“工具条”，不开始标注。
#pragma once

#include "core/Types.h"
#include "editor/AnnotationDocument.h"

#include <QPointF>
#include <QSizeF>
#include <QWidget>

namespace seashot {

class Overlay : public QWidget
{
    Q_OBJECT

public:
    /// 选择模式的用途：截图或录制 GIF（只影响提示文字与上层如何处理结果）
    enum class Intent { Screenshot, Record };

    /// 「完成」时要求上层做哪种交付（对应 QQ/PixPin 的 Enter / Ctrl+C / Ctrl+S / 贴图）
    enum class ConfirmAction { Plain, Copy, Save, Pin, Scroll, Ocr };
    Q_ENUM(ConfirmAction)

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

    // ── A2 自绘工具条（绘制 + 命中 + 验收共用同一套几何）────────
    struct ToolButton {
        ToolKind kind = ToolKind::Select;
        QRectF rect; ///< 覆盖层局部坐标
        int keyNumber = 0; ///< 1–9；0 = 无快捷键（目前只有「模糊」）
    };
    struct ActionButton {
        bool confirm = true; ///< true = 完成，false = 取消
        QRectF rect;
    };
    QRectF toolbarRect() const;
    QVector<ToolButton> toolButtons() const;
    QVector<ActionButton> actionButtons() const;
    ToolKind currentTool() const { return m_currentTool; }
    /// 当前悬停/按下的按钮矩形（-1 = 不在工具条上）——仅在诊断里用
    int hoveredCellIndex() const { return m_hoverCell; }
    /// 工具条图标来源：主题语义图标命中几个、缺哪些（缺的走自绘回退）
    static QString toolIconDiagnostics();

    // ── A3 标注（绘制 / 命中 / 撤销）─────────────────────
    /// 已提交的标注（图像坐标 = 选区坐标）
    const AnnotationDocument &document() const { return m_document; }
    /// 冻结屏幕裁到选区的那一块（= 导出坐标系的源图，未带标注）
    const QImage &frozenSelection() const { return m_frozenSelection; }
    /// 预览用的合成图：frozenSelection + 已提交标注（由 renderAnnotations 产出）
    const QImage &composedSelection() const { return m_composedSelection; }
    /// 把标注渲染进任意一张选区图（导出/复制调它；与预览同一实现，不允许绕过）
    QImage renderSelection(const QImage &source) const;
    int selectedIndex() const { return m_selectedIndex; }
    /// A3 验收用的一行状态
    QString annotationStatus() const;

    // ── A3.1 颜色 / 粗细（自绘弹出面板）─────────────────────
    enum class Panel { None, Color, Width };
    QColor annotationColor() const { return m_annotationColor; }
    int annotationWidth() const { return m_annotationWidth; }
    void setAnnotationColor(const QColor &color);
    void setAnnotationWidth(int width);
    Panel openPanel() const { return m_panel; }
    /// 预设调色板（工具条色块面板用的就是它）
    static QVector<QColor> palette();
    static int minWidth() { return 1; }
    static int maxWidth() { return 24; }
    /// 面板与面板内元素的位置（绘制/命中/验收共用）
    QRectF panelRect() const;
    QRectF colorButtonRect() const;
    QRectF widthButtonRect() const;
    int colorIndexAt(const QPointF &position) const; ///< -1 = 不在任何色块上
    QRectF widthTrackRect() const;
    QRectF widthHandleRect() const;
    int widthFromPosition(const QPointF &position) const;
    QRectF panelAnchorRect(Panel panel) const;
    QRectF swatchRect(int index) const;
    /// 把选区改到（覆盖层局部坐标）这个矩形：夹取到表面内 + 最小 16px，
    /// 并把标注（含撤销栈）反向平移，使它仍然锚定在同一块**屏幕内容**上。
    void applySelectionLocal(const QRectF &localRect);

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
    /// Annotating 阶段：用户确认（Enter/双击/✓/Ctrl+C/Ctrl+S）
    void annotationConfirmed(const SelectionResult &result, Overlay::ConfirmAction action);
    /// Annotating 阶段：用户取消（Esc/右键/✕）
    void annotationCancelled();
    /// 标注被增/删/改（撤销也算）——诊断/验收用
    void annotationsChanged();
    /// 选区被调整（拖手柄/方向键/全选）——诊断/验收用
    void selectionChanged(const QRectF &logicalRect);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Stage { Selecting, Annotating };

    /// 工具条里的一格。尺寸文字、颜色/粗细入口、分隔符也当成“格”，这样绘制/命中共用一条列表。
    struct BarCell {
        enum class Kind { Tool, ColorSwatch, WidthPicker, SizeText, Ocr, Scroll, Pin, Confirm, Cancel };
        Kind kind = Kind::Tool;
        ToolKind tool = ToolKind::Select;
        int keyNumber = 0;
        int separatorBefore = 0; ///< 1 = 本格前面画一条竖分隔线
        QRectF rect;
    };

    void applyLayerShell();
    void paintAnnotating(QPainter &painter);
    /// 选区装饰（边框 + 8 手柄）的 8 个点（覆盖层本地坐标）
    QVector<QPointF> handlePoints() const;

    // ── A3 标注交互（语义对齐 AnnotationCanvas，但自绘、不用子控件）──
    /// 覆盖层局部坐标 → 选区图像坐标
    QPointF toImage(const QPointF &local) const;
    /// 拖选区：命中哪个手柄/边/内部（Phase B）
    enum class Grip { None, TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight, Inside };
    Grip gripAt(const QPointF &local) const;
    void selectionPress(const QPointF &local, Grip grip);
    void selectionMoveTo(const QPointF &local);
    void selectAllSurface();
    void nudgeSelection(double dx, double dy);
    void updateCursorFor(const QPointF &local);
    static Qt::CursorShape cursorForGrip(Grip grip);
    void annotationPress(const QPointF &local, Qt::MouseButton button);
    void annotationMove(const QPointF &local);
    void annotationRelease(const QPointF &local, Qt::MouseButton button);
    int hitAnnotation(const QPointF &imagePos) const;
    int hitHandle(int index, const QPointF &imagePos) const;
    /// 取消当前“进行中”的东西（草稿/拖动/选中），返回 true 表示消化掉了这次 Esc
    bool cancelTransient();
    void commitTextDraft();
    void commitDraft();
    void undoAnnotation();
    void redoAnnotation();
    void deleteSelectedAnnotation();
    Annotation makeAnnotation(ToolKind kind, const QPointF &at) const;
    void refreshComposed(int skipIndex = -1);
    void drawAnnotations(QPainter &painter, const QRectF &selection) const;
    void drawAnnotationSelection(QPainter &painter, const Annotation &annotation) const;
    void drawTextDraft(QPainter &painter) const;
    /// 选区内第一个序号标注要用的下一个编号
    int nextNumber() const;

    // ── 工具条：尺寸约定、布局、绘制、命中 ────────────────
    /// 工具条的按钮顺序（前 9 个对应快捷键 1–9，「模糊」无快捷键）
    static QVector<ToolKind> toolOrder();
    /// 选区的尺寸文字（“540 × 380”），由逻辑坐标算出
    QString selectionSizeText() const;
    /// 以 (0,0) 为左上角排一排——toolbarSize()/barCells() 都从这里推导
    QVector<BarCell> buildBarCells() const;
    QSizeF toolbarSize() const;
    QVector<BarCell> barCells() const;
    int cellIndexAt(const QPointF &position) const;
    bool toolbarContains(const QPointF &position) const { return toolbarRect().contains(position); }
    void activateCell(int index);
    void drawToolbar(QPainter &painter) const;
    void drawCellBubble(QPainter &painter, const BarCell &cell) const;
    void drawPanel(QPainter &painter) const;
    void drawSwatch(QPainter &painter, const QRectF &box, const QColor &color, bool selected,
                    bool hovered) const;
    static void drawToolIcon(QPainter &painter, ToolKind kind, const QRectF &box, const QColor &color);
    static void drawActionIcon(QPainter &painter, bool confirm, const QRectF &box,
                              const QColor &color);
    static void drawPinIcon(QPainter &painter, const QRectF &box, const QColor &color);
    static void drawScrollIcon(QPainter &painter, const QRectF &box, const QColor &color);
    static void drawOcrIcon(QPainter &painter, const QRectF &box, const QColor &color);

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

    // ── A3 标注状态 ───────────────────────────────────────
    AnnotationDocument m_document;
    QImage m_frozenSelection;  ///< 冻结屏幕裁到选区（= 导出坐标系源图）
    QImage m_composedSelection; ///< 预览用：m_frozenSelection + 已提交标注
    bool m_composedDirty = true;
    QColor m_annotationColor{255, 64, 64}; ///< 与编辑器默认一致
    int m_annotationWidth = 4;
    Panel m_panel = Panel::None;   ///< 当前打开的弹出面板
    bool m_widthDragging = false; ///< 正在拖粗细滑杆
    int m_hoverSwatch = -1;       ///< 色块面板里的悬停格
    bool m_hoverWidthHandle = false;
    Annotation m_draft;       ///< 正在绘制的临时标注（不进文档）
    bool m_drawing = false;
    bool m_annotDragActive = false; ///< 标注拖动/改尺寸中（与选区拖拽 m_dragging 区分）
    int m_selectedIndex = -1;
    int m_handleIndex = -1;
    QPointF m_pressImagePos;
    Annotation m_dragOriginal;
    /// 文字草稿（自绘输入，不用 QLineEdit 子控件：layer-shell 上子控件的输入区反复咬人）
    bool m_textDraftActive = false;
    QString m_textDraft;
    QPointF m_textAnchor;
    int m_textEditIndex = -1;

    // ── Phase B：选区可调整 ──────────────────────────────
    Grip m_grip = Grip::None;
    bool m_selectionDrag = false;
    QRectF m_gripStartRect; ///< 按下时的选区（局部坐标，已对齐整数）
    QPointF m_gripStartPos;
    static constexpr double kGripTolerance = 6.0; ///< 手柄/边的命中半宽（屏幕像素）
    static constexpr double kMinSelection = 16.0; ///< 选区最小宽高

    ToolKind m_currentTool = ToolKind::Select;
    int m_hoverCell = -1;   ///< 工具条悬停格（-1 = 无）
    int m_pressedCell = -1; ///< 工具条按下格（-1 = 无）
};

} // namespace seashot
