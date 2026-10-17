#include "overlay/Overlay.h"

#include "core/SelectionModel.h"
#include "editor/AnnotationRenderer.h"
#include "geometry/ScreenMapper.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <LayerShellQt/Window>

#include <cmath>

namespace seashot {

namespace {
constexpr int kDimAlpha = 120;
const QColor kAccent(0x3d, 0xae, 0xff);

// ── A2 自绘工具条的尺寸与配色（紧凑：图标为主）────────────────
constexpr int kBarHeight = 40;
constexpr int kBarPad = 6;
constexpr int kBtnSize = 30;
constexpr int kBtnGap = 2;
constexpr int kGroupGap = 10; ///< 分隔区宽度（含那条竖线两侧的留白）
constexpr int kIconInset = 6; ///< 图标相对按钮的内缩（30 - 2*6 = 18px 图标）
constexpr double kHitTolerance = 5.0;  ///< 标注命中容差（图像像素，1:1 所以 == 屏幕像素）
constexpr double kHandleRadius = 5.0;  ///< 手柄命中半径（屏幕像素）

// ── A3.1 颜色/粗细弹出面板 ────────────────────────────────
constexpr int kSwatchSize = 26;
constexpr int kSwatchGap = 6;
constexpr int kSwatchPad = 8;
constexpr int kSwatchColumns = 5;
constexpr int kPanelColorWidth = 2 * kSwatchPad + kSwatchColumns * kSwatchSize
    + (kSwatchColumns - 1) * kSwatchGap;
constexpr int kPanelColorHeight = 2 * kSwatchPad + 2 * kSwatchSize + kSwatchGap;

const QColor kBarBg(26, 28, 33, 238);
const QColor kBarBorder(255, 255, 255, 30);
const QColor kBtnHover(255, 255, 255, 30);
const QColor kBtnPress(255, 255, 255, 58);
const QColor kBtnActive(0x3d, 0xae, 0xff, 72);
const QColor kIconColor(0xe4, 0xe8, 0xf0);
const QColor kSizeTextColor(0xc2, 0xc9, 0xd6);
const QColor kSeparator(255, 255, 255, 38);

/// 工具图标：统一走 ui/Theme 的**语义图标表**（与编辑器工具栏同一套），
/// 主题里取不到时返回空 QIcon，调用方回退到自绘图形（不出现空白按钮）
QIcon toolIconFor(ToolKind kind)
{
    switch (kind) {
    case ToolKind::Select: return ui::iconToolSelect();
    case ToolKind::Arrow: return ui::iconToolArrow();
    case ToolKind::Rectangle: return ui::iconToolRectangle();
    case ToolKind::Ellipse: return ui::iconToolEllipse();
    case ToolKind::Pen: return ui::iconToolPen();
    case ToolKind::Text: return ui::iconToolText();
    case ToolKind::Highlight: return ui::iconToolHighlight();
    case ToolKind::Blur: return ui::iconToolBlur();
    case ToolKind::Mosaic: return ui::iconToolMosaic();
    case ToolKind::Number: return ui::iconToolNumber();
    }
    return QIcon();
}

/// 把主题图标染成单色：图标主题给的都是符号图标，染色后与自绘图标观感一致，
/// 并且能直接用强调色表示“当前工具”
QPixmap tintedIcon(const QIcon &icon, const QSize &size, qreal dpr, const QColor &color)
{
    if (icon.isNull() || size.isEmpty())
        return QPixmap();
    const QSize deviceSize(qMax(1, int(size.width() * dpr)), qMax(1, int(size.height() * dpr)));
    QPixmap pixmap = icon.pixmap(deviceSize, dpr, QIcon::Normal, QIcon::Off);
    if (pixmap.isNull())
        return QPixmap();
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    painter.end();
    return pixmap;
}
} // namespace

Overlay::Overlay(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    // 工具条要 hover 高亮：不按键时也要收到 mouseMove
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("seashot-overlay"));
    hide();
}

void Overlay::applyLayerShell()
{
    // 顺序很重要：先定窗口标志，再建立原生窗口，最后套 layer-shell role
    if (QGuiApplication::platformName().startsWith(QLatin1String("xcb"))) {
        // X11：绕过窗口管理器（覆盖层不进 WM 管理、不被装饰）
        setWindowFlags(windowFlags() | Qt::X11BypassWindowManagerHint);
    }
    createWinId();
    QWindow *handle = windowHandle();
    if (!handle) {
        SS_WARN() << "[overlay] 拿不到 QWindow，layer-shell 无法应用";
        return;
    }
    if (auto *layerShell = LayerShellQt::Window::get(handle)) {
        layerShell->setLayer(LayerShellQt::Window::LayerOverlay);
        layerShell->setAnchors(LayerShellQt::Window::Anchors(
            LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layerShell->setExclusiveZone(-1); // 不占布局空间
        layerShell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        m_layerShellApplied = true;
        SS_INFO() << "[overlay] layer-shell role 已应用（LayerOverlay，四边锚定）";
    } else {
        SS_WARN() << "[overlay] LayerShellQt::Window::get 返回空（非 Wayland 或 compositor 不支持），"
                     "退化为普通全屏窗口";
    }
}

void Overlay::setIntent(Intent intent)
{
    m_intent = intent;
    if (isVisible())
        update();
}

void Overlay::setWindows(const QVector<WindowInfo> &windows)
{
    m_windows = windows;
    if (isVisible())
        update();
}

void Overlay::setVirtualGeometry(const QRect &logicalGeometry)
{
    m_virtualGeometry = logicalGeometry;
    setGeometry(logicalGeometry);
}

void Overlay::startSelection()
{
    m_dragging = false;
    m_hasHover = false;
    m_hover = WindowInfo();
    m_stage = Stage::Selecting;
    m_frozenFrame = QImage();
    m_selection = SelectionResult();
    m_currentTool = ToolKind::Select;
    m_hoverCell = -1;
    m_pressedCell = -1;
    m_document.reset({});
    m_frozenSelection = QImage();
    m_composedSelection = QImage();
    m_draft = Annotation();
    m_drawing = false;
    m_annotDragActive = false;
    m_selectedIndex = -1;
    m_handleIndex = -1;
    m_textDraftActive = false;
    m_textDraft.clear();
    m_textEditIndex = -1;
    applyLayerShell();
    if (!m_virtualGeometry.isEmpty())
        setGeometry(m_virtualGeometry);
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    grabKeyboard();
}

void Overlay::stopSelection()
{
    releaseKeyboard();
    hide();
    m_stage = Stage::Selecting;
    m_frozenFrame = QImage();
    m_hoverCell = -1;
    m_pressedCell = -1;
}

void Overlay::beginAnnotating(const SelectionResult &selection, const QImage &frozenPhysical,
                              double frozenScale)
{
    m_dragging = false;
    m_hasHover = false;
    m_hover = WindowInfo();
    m_selection = selection;
    m_stage = Stage::Annotating;
    m_hoverCell = -1;
    m_pressedCell = -1;
    m_document.reset({});
    m_selectedIndex = -1;
    m_draft = Annotation();
    m_drawing = false;
    m_annotDragActive = false;
    m_handleIndex = -1;
    m_textDraftActive = false;
    m_textDraft.clear();
    m_textEditIndex = -1;
    m_composedDirty = true;

    // 冻结屏幕归一到**逻辑像素**：这样绘制就是严格的 1:1，不需要在 paint 里做任何缩放。
    // 本机实测 scale=1（单输出），此时这一步是零成本的直通。
    const double scale = (frozenScale > 0.0) ? frozenScale : 1.0;
    if (frozenPhysical.isNull()) {
        m_frozenFrame = QImage();
    } else if (qFuzzyCompare(scale, 1.0)) {
        m_frozenFrame = frozenPhysical;
    } else {
        const QSize logicalSize(qMax(1, int(qRound(frozenPhysical.width() / scale))),
                                qMax(1, int(qRound(frozenPhysical.height() / scale))));
        m_frozenFrame = frozenPhysical.scaled(logicalSize, Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation);
    }

    // 保持同一个表面（layer-shell 已应用），只是继续画：不起新窗口、不重设 shell。
    if (!isVisible()) {
        show();
        raise();
        activateWindow();
        setFocus(Qt::OtherFocusReason);
        grabKeyboard();
    }
    setCursor(Qt::CrossCursor);

    // A3：裁出导出坐标系的那块源图（标注坐标就活在它里面）
    const QRectF selectionLocal = toLocal(m_selection.logicalRect);
    m_frozenSelection = m_frozenFrame.isNull()
        ? QImage()
        : m_frozenFrame.copy(selectionLocal.toAlignedRect().intersected(m_frozenFrame.rect()));
    refreshComposed();
    update();

    SS_INFO() << "[overlay] 进入就地标注阶段：选区" << m_selection.logicalRect << "冻结图"
              << m_frozenFrame.width() << "x" << m_frozenFrame.height() << "(scale=" << scale
              << ") 表面" << width() << "x" << height() << "选区图" << m_frozenSelection.width()
              << "x" << m_frozenSelection.height();
}

QVector<QPointF> Overlay::handlePoints() const
{
    const QRectF r = toLocal(m_selection.logicalRect);
    if (r.isEmpty())
        return {};
    return {r.topLeft(),
            QPointF(r.center().x(), r.top()),
            r.topRight(),
            QPointF(r.left(), r.center().y()),
            QPointF(r.right(), r.center().y()),
            r.bottomLeft(),
            QPointF(r.center().x(), r.bottom()),
            r.bottomRight()};
}

void Overlay::paintAnnotating(QPainter &painter)
{
    const QRectF full(0, 0, width(), height());
    const QRectF selection = toLocal(m_selection.logicalRect);

    // 1) 冻结屏幕 1:1（左上角对齐表面原点 = 虚拟桌面原点）
    if (!m_frozenFrame.isNull())
        painter.drawImage(QPointF(0, 0), m_frozenFrame);
    else
        painter.fillRect(full, QColor(20, 22, 26));

    // 2) 只压暗选区**之外**的部分：选区里看到的是 1:1 原像素（与 QQ/PixPin 一致）
    const QColor dim(0, 0, 0, kDimAlpha);
    painter.fillRect(QRectF(full.left(), full.top(), full.width(), selection.top() - full.top()),
                     dim);
    painter.fillRect(QRectF(full.left(), selection.bottom() + 1, full.width(),
                            full.bottom() - selection.bottom()),
                     dim);
    painter.fillRect(QRectF(full.left(), selection.top(), selection.left() - full.left(),
                            selection.height()),
                     dim);
    painter.fillRect(QRectF(selection.right() + 1, selection.top(),
                            full.right() - selection.right(), selection.height()),
                     dim);

    // 3) 标注（A3）：合成图 + 正在画的草稿/选中框，都裁在选区内
    drawAnnotations(painter, selection);

    // 4) 选区边框 + 8 手柄
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(kAccent, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selection);

    painter.setPen(QPen(QColor(255, 255, 255), 1.0));
    painter.setBrush(kAccent);
    for (const QPointF &point : handlePoints())
        painter.drawRect(QRectF(point.x() - 3.5, point.y() - 3.5, 7.0, 7.0));

    // 5) 自绘图标工具条（A2）：贴选区下方，放不下则上方
    drawToolbar(painter);

    // 6) 颜色/粗细弹出面板（A3.1）：画在最上层
    drawPanel(painter);
}

// ─────────────────────────────────────────────────────────────
// A3：标注绘制 / 命中 / 撤销（语义对齐 src/editor/AnnotationCanvas，
//     但自绘、不用子控件；坐标 = 选区图像坐标）
// ─────────────────────────────────────────────────────────────

QPointF Overlay::toImage(const QPointF &local) const
{
    return local - toLocal(m_selection.logicalRect).topLeft();
}

// ─────────────────────────────────────────────────────────────
// Phase B：选区可调整（8 手柄 + 4 边 + 内部拖动；标注锚定屏幕内容）
// ─────────────────────────────────────────────────────────────

Overlay::Grip Overlay::gripAt(const QPointF &local) const
{
    const QRectF rect = toLocal(m_selection.logicalRect);
    if (!rect.isValid())
        return Grip::None;
    const double t = kGripTolerance;
    const bool nearLeft = qAbs(local.x() - rect.left()) <= t;
    const bool nearRight = qAbs(local.x() - rect.right()) <= t;
    const bool nearTop = qAbs(local.y() - rect.top()) <= t;
    const bool nearBottom = qAbs(local.y() - rect.bottom()) <= t;
    const bool insideX = local.x() >= rect.left() - t && local.x() <= rect.right() + t;
    const bool insideY = local.y() >= rect.top() - t && local.y() <= rect.bottom() + t;
    if (!insideX || !insideY)
        return Grip::None;

    // 四个角优先（对角手柄比边更难命中，先判它们）
    if (nearLeft && nearTop)
        return Grip::TopLeft;
    if (nearRight && nearTop)
        return Grip::TopRight;
    if (nearLeft && nearBottom)
        return Grip::BottomLeft;
    if (nearRight && nearBottom)
        return Grip::BottomRight;
    if (nearLeft)
        return Grip::Left;
    if (nearRight)
        return Grip::Right;
    if (nearTop)
        return Grip::Top;
    if (nearBottom)
        return Grip::Bottom;
    if (rect.contains(local))
        return Grip::Inside;
    return Grip::None;
}

Qt::CursorShape Overlay::cursorForGrip(Grip grip)
{
    switch (grip) {
    case Grip::TopLeft:
    case Grip::BottomRight:
        return Qt::SizeFDiagCursor;
    case Grip::TopRight:
    case Grip::BottomLeft:
        return Qt::SizeBDiagCursor;
    case Grip::Left:
    case Grip::Right:
        return Qt::SizeHorCursor;
    case Grip::Top:
    case Grip::Bottom:
        return Qt::SizeVerCursor;
    case Grip::Inside:
        return Qt::SizeAllCursor;
    case Grip::None:
        break;
    }
    return Qt::CrossCursor;
}

void Overlay::updateCursorFor(const QPointF &local)
{
    const Grip grip = gripAt(local);
    if (grip == Grip::Inside && m_currentTool != ToolKind::Select)
        setCursor(Qt::CrossCursor); // 画图工具在选区内部是“画”，不是“搬”
    else if (grip != Grip::None)
        setCursor(cursorForGrip(grip));
    else
        setCursor(m_currentTool == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
}

void Overlay::selectionPress(const QPointF &local, Grip grip)
{
    m_grip = grip;
    m_selectionDrag = true;
    m_gripStartRect = toLocal(m_selection.logicalRect).toAlignedRect();
    m_gripStartPos = local;
}

void Overlay::selectionMoveTo(const QPointF &local)
{
    if (!m_selectionDrag)
        return;
    const QPointF delta = local - m_gripStartPos;
    QRectF next = m_gripStartRect;
    switch (m_grip) {
    case Grip::Inside:
        next.translate(delta);
        break;
    case Grip::TopLeft:
        next.setTopLeft(m_gripStartRect.topLeft() + delta);
        break;
    case Grip::Top:
        next.setTop(m_gripStartRect.top() + delta.y());
        break;
    case Grip::TopRight:
        next.setTopRight(m_gripStartRect.topRight() + delta);
        break;
    case Grip::Left:
        next.setLeft(m_gripStartRect.left() + delta.x());
        break;
    case Grip::Right:
        next.setRight(m_gripStartRect.right() + delta.x());
        break;
    case Grip::BottomLeft:
        next.setBottomLeft(m_gripStartRect.bottomLeft() + delta);
        break;
    case Grip::Bottom:
        next.setBottom(m_gripStartRect.bottom() + delta.y());
        break;
    case Grip::BottomRight:
        next.setBottomRight(m_gripStartRect.bottomRight() + delta);
        break;
    case Grip::None:
        return;
    }
    // 越界翻转时 normalized() 会把它转回正矩形；再由 applySelectionLocal 做最小尺寸/夹取
    applySelectionLocal(next.normalized().toAlignedRect());
}

void Overlay::applySelectionLocal(const QRectF &localRect)
{
    const QRectF full(0, 0, width(), height());
    const QRectF aligned = localRect.toAlignedRect();
    const double x = qBound(0.0, aligned.left(), qMax(0.0, full.width() - kMinSelection));
    const double y = qBound(0.0, aligned.top(), qMax(0.0, full.height() - kMinSelection));
    const double w = qBound(kMinSelection, aligned.width(), qMax(kMinSelection, full.width() - x));
    const double h = qBound(kMinSelection, aligned.height(), qMax(kMinSelection, full.height() - y));
    const QRectF next(x, y, w, h);

    const QRectF oldLocal = toLocal(m_selection.logicalRect).toAlignedRect();
    if (next == oldLocal)
        return;

    // 标注锚定的是**屏幕内容**：选区原点一动，标注的相对坐标要反向平移
    // （撤销/重做栈里的快照也一起平移，否则之后撤销会跳到另一个坐标系）
    const QPointF delta = oldLocal.topLeft() - next.topLeft();
    m_document.translateAll(delta);

    m_selection.logicalRect = QRectF(next.topLeft() + QPointF(m_virtualGeometry.x(),
                                                             m_virtualGeometry.y()),
                                     next.size());

    // 重裁“导出坐标系”的源图 + 重烘预览
    m_frozenSelection = m_frozenFrame.isNull()
        ? QImage()
        : m_frozenFrame.copy(next.toAlignedRect().intersected(m_frozenFrame.rect()));
    refreshComposed();

    // 选区变小/变位后，选中项可能已经不在选区里了，但坐标仍然有效 —— 保留选中，不丢数据
    emit selectionChanged(m_selection.logicalRect);
    update();
    SS_INFO() << "[overlay] 选区调整为" << m_selection.logicalRect << "（标注已随屏内容平移"
              << delta << "）";
}

void Overlay::selectAllSurface()
{
    applySelectionLocal(QRectF(0, 0, width(), height()));
}

void Overlay::nudgeSelection(double dx, double dy)
{
    if (dx == 0.0 && dy == 0.0)
        return;
    QRectF next = toLocal(m_selection.logicalRect).toAlignedRect();
    next.translate(dx, dy);
    // 微调时保持尺寸：超出边界就停在边界上
    const QRectF full(0, 0, width(), height());
    if (next.left() < 0)
        next.moveLeft(0);
    if (next.top() < 0)
        next.moveTop(0);
    if (next.right() > full.right())
        next.moveRight(full.right() + 1);
    if (next.bottom() > full.bottom())
        next.moveBottom(full.bottom() + 1);
    applySelectionLocal(next);
}

// ─────────────────────────────────────────────────────────────
// A3.1：颜色 / 粗细（自绘弹出面板）
// ─────────────────────────────────────────────────────────────

QVector<QColor> Overlay::palette()
{
    // 常用标注色：前 8 个是彩色（红=默认，与编辑器一致），最后是白/黑
    return {QColor(255, 64, 64),  QColor(255, 140, 0),  QColor(255, 214, 10),
            QColor(60, 200, 90),  QColor(0, 200, 220),  QColor(61, 174, 255),
            QColor(150, 110, 255), QColor(255, 90, 190), QColor(255, 255, 255),
            QColor(24, 24, 28)};
}

void Overlay::setAnnotationColor(const QColor &color)
{
    if (!color.isValid() || color == m_annotationColor)
        return;
    m_annotationColor = color;
    SS_INFO() << "[overlay] 标注颜色" << color.name();
    update();
}

void Overlay::setAnnotationWidth(int width)
{
    const int bounded = qBound(minWidth(), width, maxWidth());
    if (bounded == m_annotationWidth)
        return;
    m_annotationWidth = bounded;
    SS_INFO() << "[overlay] 标注粗细" << m_annotationWidth << "px";
    update();
}

QRectF Overlay::panelAnchorRect(Panel panel) const
{
    const QVector<BarCell> cells = barCells();
    const BarCell::Kind kind = (panel == Panel::Color) ? BarCell::Kind::ColorSwatch
                                                       : BarCell::Kind::WidthPicker;
    for (const BarCell &cell : cells) {
        if (cell.kind == kind)
            return cell.rect;
    }
    return toolbarRect();
}

QRectF Overlay::colorButtonRect() const
{
    return panelAnchorRect(Panel::Color);
}

QRectF Overlay::widthButtonRect() const
{
    return panelAnchorRect(Panel::Width);
}

QRectF Overlay::swatchRect(int index) const
{
    if (index < 0)
        return QRectF();
    const QRectF panel = panelRect();
    if (panel.isEmpty())
        return QRectF();
    const int column = index % kSwatchColumns;
    const int row = index / kSwatchColumns;
    return QRectF(panel.left() + kSwatchPad + column * (kSwatchSize + kSwatchGap),
                  panel.top() + kSwatchPad + row * (kSwatchSize + kSwatchGap), kSwatchSize,
                  kSwatchSize);
}

QRectF Overlay::panelRect() const
{
    if (m_panel == Panel::None)
        return QRectF();
    const QSizeF size = (m_panel == Panel::Color)
        ? QSizeF(kPanelColorWidth, kPanelColorHeight)
        : QSizeF(kPanelColorWidth, 54);
    const QRectF anchor = panelAnchorRect(m_panel);
    const QRectF bar = toolbarRect();
    const QRectF selection = toLocal(m_selection.logicalRect);
    const bool barBelow = bar.top() >= selection.bottom();

    // 与悬停气泡同一套避让：先放到远离选区、且不压工具条的那一侧
    double y = barBelow ? bar.bottom() + 6 : bar.top() - size.height() - 6;
    if (y < 4 || y + size.height() > height() - 4)
        y = barBelow ? bar.top() - size.height() - 6 : bar.bottom() + 6;
    if (y < 4 || y + size.height() > height() - 4)
        y = qBound(4.0, y, qMax(4.0, double(height()) - size.height() - 4));

    double x = anchor.center().x() - size.width() / 2.0;
    x = qBound(4.0, x, qMax(4.0, double(width()) - size.width() - 4));
    return QRectF(x, y, size.width(), size.height());
}

int Overlay::colorIndexAt(const QPointF &position) const
{
    if (m_panel != Panel::Color)
        return -1;
    const QVector<QColor> colors = palette();
    for (int i = 0; i < colors.size(); ++i) {
        if (swatchRect(i).contains(position))
            return i;
    }
    return -1;
}

QRectF Overlay::widthTrackRect() const
{
    const QRectF panel = panelRect();
    if (m_panel != Panel::Width || panel.isEmpty())
        return QRectF();
    return QRectF(panel.left() + 16, panel.top() + 36, panel.width() - 32, 4);
}

QRectF Overlay::widthHandleRect() const
{
    const QRectF track = widthTrackRect();
    if (track.isEmpty())
        return QRectF();
    const double span = qMax(1, maxWidth() - minWidth());
    const double t = double(m_annotationWidth - minWidth()) / double(span);
    const double x = track.left() + t * track.width();
    return QRectF(x - 7.0, track.center().y() - 7.0, 14.0, 14.0);
}

int Overlay::widthFromPosition(const QPointF &position) const
{
    const QRectF track = widthTrackRect();
    if (track.isEmpty())
        return m_annotationWidth;
    const double t = qBound(0.0, (position.x() - track.left()) / qMax(1.0, track.width()), 1.0);
    return minWidth() + qRound(t * (maxWidth() - minWidth()));
}

Annotation Overlay::makeAnnotation(ToolKind kind, const QPointF &at) const
{
    Annotation annotation;
    annotation.kind = kind;
    annotation.color = m_annotationColor;
    annotation.width = m_annotationWidth;
    annotation.points = {at, at};
    return annotation;
}

int Overlay::nextNumber() const
{
    int next = 1;
    for (int i = 0; i < m_document.count(); ++i) {
        if (m_document.at(i).kind == ToolKind::Number)
            ++next;
    }
    return next;
}

int Overlay::hitAnnotation(const QPointF &imagePos) const
{
    // 后画的在上层 → 从后往前找
    for (int i = m_document.count() - 1; i >= 0; --i) {
        if (m_document.at(i).hitTest(imagePos, kHitTolerance))
            return i;
    }
    return -1;
}

int Overlay::hitHandle(int index, const QPointF &imagePos) const
{
    if (index < 0 || index >= m_document.count())
        return -1;
    const QVector<QPointF> handles = m_document.at(index).handles();
    for (int i = 0; i < handles.size(); ++i) {
        if (QLineF(handles.at(i), imagePos).length() <= kHandleRadius)
            return i;
    }
    return -1;
}

void Overlay::annotationPress(const QPointF &local, Qt::MouseButton button)
{
    const QPointF imagePos = toImage(local);
    m_pressImagePos = imagePos;

    if (button == Qt::RightButton) {
        // 右键：先取消正在做的事情，实在没东西可取消才退出（与 Esc 同语义）
        if (!cancelTransient())
            emit annotationCancelled();
        return;
    }
    if (button != Qt::LeftButton)
        return;

    if (m_textDraftActive)
        commitTextDraft(); // 在别处点一下 = 先把文字落下来

    if (m_currentTool == ToolKind::Select) {
        m_handleIndex = hitHandle(m_selectedIndex, imagePos);
        if (m_handleIndex >= 0) {
            m_annotDragActive = true;
            m_dragOriginal = m_document.at(m_selectedIndex);
            m_draft = m_dragOriginal;
            refreshComposed(m_selectedIndex); // 拖动时先把原件从合成图里拿掉，免得重影
            update();
            return;
        }
        const int index = hitAnnotation(imagePos);
        m_selectedIndex = index;
        if (index >= 0) {
            m_annotDragActive = true;
            m_handleIndex = -1;
            m_dragOriginal = m_document.at(index);
            m_draft = m_dragOriginal;
            refreshComposed(index);
        }
        update();
        return;
    }

    if (m_currentTool == ToolKind::Text) {
        m_textDraftActive = true;
        m_textDraft.clear();
        m_textAnchor = imagePos;
        m_textEditIndex = -1;
        SS_INFO() << "[overlay] 开始输入文字（回车确认 / Esc 取消）";
        update();
        return;
    }

    if (m_currentTool == ToolKind::Number) {
        Annotation badge = makeAnnotation(ToolKind::Number, imagePos);
        badge.points = {imagePos};
        badge.text = QString::number(nextNumber());
        m_document.add(badge);
        m_selectedIndex = m_document.count() - 1;
        refreshComposed();
        SS_INFO() << "[overlay] 新增标注" << toolName(badge.kind) << "共" << m_document.count();
        update();
        return;
    }

    m_draft = makeAnnotation(m_currentTool, imagePos);
    m_drawing = true;
    update();
}

void Overlay::annotationMove(const QPointF &local)
{
    if (!m_drawing && !m_annotDragActive)
        return;
    const QPointF imagePos = toImage(local);
    if (m_drawing) {
        if (m_draft.kind == ToolKind::Pen || m_draft.kind == ToolKind::Highlight) {
            if (QLineF(m_draft.points.last(), imagePos).length() >= 1.5)
                m_draft.points.append(imagePos);
        } else if (m_draft.points.size() >= 2) {
            m_draft.points[1] = imagePos;
        }
        update();
        return;
    }

    m_draft = m_dragOriginal;
    if (m_handleIndex >= 0)
        m_draft.moveHandle(m_handleIndex, imagePos);
    else
        m_draft.translate(imagePos - m_pressImagePos);
    update();
}

void Overlay::annotationRelease(const QPointF &local, Qt::MouseButton button)
{
    if (button != Qt::LeftButton)
        return;
    const QPointF imagePos = toImage(local);

    if (m_drawing) {
        if (m_draft.kind == ToolKind::Pen || m_draft.kind == ToolKind::Highlight)
            m_draft.points.append(imagePos);
        else if (m_draft.points.size() >= 2)
            m_draft.points[1] = imagePos;
        m_drawing = false;
        commitDraft();
        return;
    }

    if (m_annotDragActive) {
        m_annotDragActive = false;
        const int index = m_selectedIndex;
        m_handleIndex = -1;
        if (index >= 0 && m_draft.isValid()) {
            m_document.update(index, m_draft);
            SS_INFO() << "[overlay] 移动/缩放标注" << toolName(m_draft.kind);
        }
        m_draft = Annotation();
        refreshComposed();
        update();
    }
}

void Overlay::commitDraft()
{
    if (m_draft.isValid()) {
        m_document.add(m_draft);
        m_selectedIndex = m_document.count() - 1;
        refreshComposed();
        SS_INFO() << "[overlay] 新增标注" << toolName(m_draft.kind) << "共" << m_document.count();
    }
    m_draft = Annotation();
    update();
}

bool Overlay::cancelTransient()
{
    if (m_selectionDrag) {
        m_selectionDrag = false;
        m_grip = Grip::None;
        update();
        return true;
    }
    if (m_textDraftActive) {
        m_textDraftActive = false;
        m_textDraft.clear();
        m_textEditIndex = -1;
        update();
        return true;
    }
    if (m_drawing || m_annotDragActive) {
        m_drawing = false;
        m_annotDragActive = false;
        m_draft = Annotation();
        m_handleIndex = -1;
        refreshComposed();
        update();
        return true;
    }
    if (m_selectedIndex >= 0) {
        m_selectedIndex = -1;
        update();
        return true;
    }
    return false;
}

void Overlay::commitTextDraft()
{
    if (!m_textDraftActive)
        return;
    const QString text = m_textDraft;
    const int editIndex = m_textEditIndex;
    m_textDraftActive = false;
    m_textDraft.clear();
    m_textEditIndex = -1;

    if (!text.trimmed().isEmpty()) {
        Annotation annotation;
        annotation.kind = ToolKind::Text;
        annotation.text = text;
        annotation.color = m_annotationColor;
        annotation.width = m_annotationWidth;
        if (editIndex >= 0 && editIndex < m_document.count()) {
            annotation.points = {m_document.at(editIndex).points.value(0)};
            m_document.update(editIndex, annotation);
            m_selectedIndex = editIndex;
        } else {
            annotation.points = {m_textAnchor};
            m_document.add(annotation);
            m_selectedIndex = m_document.count() - 1;
        }
        refreshComposed();
        SS_INFO() << "[overlay] 文字标注已提交" << text << "共" << m_document.count();
    }
    update();
}

void Overlay::undoAnnotation()
{
    if (m_drawing || m_annotDragActive || m_textDraftActive) {
        cancelTransient(); // 先取消进行中的操作，避免状态错位
        return;
    }
    if (!m_document.undo()) {
        SS_INFO() << "[overlay] 没有可撤销的标注";
        return;
    }
    if (m_selectedIndex >= m_document.count())
        m_selectedIndex = -1;
    refreshComposed();
    update();
    emit annotationsChanged();
    SS_INFO() << "[overlay] 撤销，剩余标注" << m_document.count();
}

void Overlay::redoAnnotation()
{
    if (m_drawing || m_annotDragActive || m_textDraftActive) {
        cancelTransient();
        return;
    }
    if (!m_document.redo()) {
        SS_INFO() << "[overlay] 没有可重做的标注";
        return;
    }
    if (m_selectedIndex >= m_document.count())
        m_selectedIndex = -1;
    refreshComposed();
    update();
    emit annotationsChanged();
    SS_INFO() << "[overlay] 重做，标注" << m_document.count();
}

void Overlay::deleteSelectedAnnotation()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= m_document.count())
        return;
    const QString name = toolName(m_document.at(m_selectedIndex).kind);
    m_document.removeAt(m_selectedIndex);
    m_selectedIndex = -1;
    refreshComposed();
    update();
    emit annotationsChanged();
    SS_INFO() << "[overlay] 删除标注" << name << "剩余" << m_document.count();
}

QImage Overlay::renderSelection(const QImage &source) const
{
    // 导出/复制与预览共用这一条出路（renderAnnotations），不另写一套画法
    return renderAnnotations(source, m_document.items());
}

void Overlay::refreshComposed(int skipIndex)
{
    m_composedDirty = false;
    if (m_frozenSelection.isNull()) {
        m_composedSelection = QImage();
        return;
    }
    if (skipIndex < 0) {
        // 预览直接就是「导出」那条路的输出（renderAnnotations），不做任何第二套画法
        m_composedSelection = renderAnnotations(m_frozenSelection, m_document.items());
        return;
    }
    QVector<Annotation> items = m_document.items();
    if (skipIndex < items.size())
        items.removeAt(skipIndex);
    m_composedSelection = renderAnnotations(m_frozenSelection, items);
}

void Overlay::drawAnnotations(QPainter &painter, const QRectF &selection) const
{
    painter.save();
    painter.setClipRect(selection);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!m_composedSelection.isNull())
        painter.drawImage(selection.topLeft(), m_composedSelection);

    // 草稿：区域特效给虚线提示框（拖动中不做实时模糊，免得卡）
    if (m_draft.isValid()) {
        if (isRegionEffect(m_draft.kind)) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 180), 1.0, Qt::DashLine));
            painter.drawRect(m_draft.bounds().translated(selection.topLeft()));
        } else {
            painter.save();
            painter.translate(selection.topLeft());
            m_draft.draw(painter);
            painter.restore();
        }
    }

    if (m_textDraftActive) {
        painter.save();
        painter.translate(selection.topLeft());
        drawTextDraft(painter);
        painter.restore();
    }

    if (m_selectedIndex >= 0 && !m_annotDragActive && !m_drawing
        && m_selectedIndex < m_document.count())
        drawAnnotationSelection(painter, m_document.at(m_selectedIndex));

    painter.restore();
}

void Overlay::drawAnnotationSelection(QPainter &painter, const Annotation &annotation) const
{
    const QRectF bounds = annotation.bounds();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kAccent, 1.0, Qt::DashLine));
    painter.drawRect(bounds);

    painter.setPen(QPen(QColor(255, 255, 255), 1.0));
    painter.setBrush(kAccent);
    for (const QPointF &handle : annotation.handles())
        painter.drawEllipse(handle, kHandleRadius, kHandleRadius);
}

void Overlay::drawTextDraft(QPainter &painter) const
{
    Annotation preview;
    preview.kind = ToolKind::Text;
    preview.points = {m_textAnchor};
    preview.text = m_textDraft;
    preview.color = m_annotationColor;
    preview.width = m_annotationWidth;
    preview.draw(painter); // 与最终渲染同一套画法（所见即所得）

    const QFontMetricsF metrics(preview.textFont());
    const double width = metrics.horizontalAdvance(m_textDraft);
    painter.setPen(QPen(m_annotationColor, 1.5));
    painter.drawLine(QPointF(m_textAnchor.x() + width + 1.0, m_textAnchor.y()),
                     QPointF(m_textAnchor.x() + width + 1.0,
                             m_textAnchor.y() + metrics.height()));
}

QString Overlay::annotationStatus() const
{
    static const auto panelName = [](Panel panel) {
        switch (panel) {
        case Panel::Color: return QStringLiteral("Color");
        case Panel::Width: return QStringLiteral("Width");
        case Panel::None: break;
        }
        return QStringLiteral("None");
    };
    return QStringLiteral("标注=%1 选中=%2 绘制中=%3 拖动中=%4 文字草稿=%5 选区图=%6x%7 颜色=%8 粗细=%9 面板=%10")
        .arg(m_document.count())
        .arg(m_selectedIndex)
        .arg(m_drawing ? 1 : 0)
        .arg(m_annotDragActive ? 1 : 0)
        .arg(m_textDraftActive ? 1 : 0)
        .arg(m_frozenSelection.width())
        .arg(m_frozenSelection.height())
        .arg(m_annotationColor.name())
        .arg(m_annotationWidth)
        .arg(panelName(m_panel));
}

// ─────────────────────────────────────────────────────────────
// A2 工具条：几何（绘制与命中共用同一套 cell 列表）
// ─────────────────────────────────────────────────────────────

QVector<ToolKind> Overlay::toolOrder()
{
    // 前 9 个对应快捷键 1–9（对齐 QQ/PixPin：选择/矩形/椭圆/箭头/画笔/高亮/文字/序号/马赛克），
    // 「模糊」排在最后、没有数字键（点击可达）—— 详见 docs/plan-inplace-editor.md 第 2.4 节
    return {ToolKind::Select, ToolKind::Rectangle, ToolKind::Ellipse, ToolKind::Arrow,
            ToolKind::Pen,    ToolKind::Highlight, ToolKind::Text,      ToolKind::Number,
            ToolKind::Mosaic, ToolKind::Blur};
}

QString Overlay::selectionSizeText() const
{
    return QStringLiteral("%1 × %2")
        .arg(ScreenMapper::roundCoord(m_selection.logicalRect.width()))
        .arg(ScreenMapper::roundCoord(m_selection.logicalRect.height()));
}

QVector<Overlay::BarCell> Overlay::buildBarCells() const
{
    // 以 (0,0) 为左上角排一排；toolbarSize()/barCells() 都从这里推导，避免两处尺寸打架
    QVector<BarCell> cells;
    const double top = (kBarHeight - kBtnSize) / 2.0;
    double x = kBarPad;

    const QVector<ToolKind> tools = toolOrder();
    for (int i = 0; i < tools.size(); ++i) {
        BarCell cell;
        cell.kind = BarCell::Kind::Tool;
        cell.tool = tools.at(i);
        cell.keyNumber = (i < 9) ? i + 1 : 0;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }
    x += kGroupGap - kBtnGap; // 收掉最后一个按钮间距，换成组间距

    { // 颜色
        BarCell cell;
        cell.kind = BarCell::Kind::ColorSwatch;
        cell.separatorBefore = 1;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 粗细
        BarCell cell;
        cell.kind = BarCell::Kind::WidthPicker;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 选区尺寸（“540 × 380”贴在工具右侧，和 QQ 一致）
        BarCell cell;
        cell.kind = BarCell::Kind::SizeText;
        cell.separatorBefore = 1;
        const QFontMetricsF metrics(font());
        // 宽度按「8888 × 8888」定死：Phase B 拖手柄改选区时尺寸文字会变，
        // 不固定的话整条工具条会跟着抖
        const double textWidth =
            qMax(metrics.horizontalAdvance(selectionSizeText()),
                 metrics.horizontalAdvance(QStringLiteral("8888 × 8888")));
        cell.rect = QRectF(x, 0, textWidth + kGroupGap, kBarHeight);
        cells.append(cell);
        x = cell.rect.right() + kGroupGap - kBtnGap;
    }

    { // 文字识别（OCR）
        BarCell cell;
        cell.kind = BarCell::Kind::Ocr;
        cell.separatorBefore = 1;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 长截图（滚动截屏）
        BarCell cell;
        cell.kind = BarCell::Kind::Scroll;
        cell.separatorBefore = 0;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 贴图（钉到屏幕上）
        BarCell cell;
        cell.kind = BarCell::Kind::Pin;
        cell.separatorBefore = 0;
        cell.separatorBefore = 1;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 完成
        BarCell cell;
        cell.kind = BarCell::Kind::Confirm;
        cell.separatorBefore = 0;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
        x += kBtnSize + kBtnGap;
    }

    { // 取消
        BarCell cell;
        cell.kind = BarCell::Kind::Cancel;
        cell.rect = QRectF(x, top, kBtnSize, kBtnSize);
        cells.append(cell);
    }
    return cells;
}

QSizeF Overlay::toolbarSize() const
{
    const QVector<BarCell> cells = buildBarCells();
    double right = 0.0;
    for (const BarCell &cell : cells)
        right = qMax(right, cell.rect.right());
    return QSizeF(right + kBarPad, kBarHeight);
}

QRectF Overlay::toolbarRect() const
{
    const QSizeF size = toolbarSize();
    const QRectF selection = toLocal(m_selection.logicalRect);
    const QRectF full(0, 0, width(), height());
    const double margin = 8.0;

    // 优先贴选区**下方**，下方放不下则放上方（两边都不行就挑能放的那边）
    double y = selection.bottom() + margin;
    if (y + size.height() > full.bottom() - margin)
        y = selection.top() - size.height() - margin;
    if (y < margin)
        y = qBound(margin, selection.bottom() + margin,
                   qMax(margin, full.bottom() - size.height() - margin));

    double x = selection.left();
    x = qBound(margin, x, qMax(margin, full.right() - size.width() - margin));
    return QRectF(x, y, size.width(), size.height());
}

QVector<Overlay::BarCell> Overlay::barCells() const
{
    QVector<BarCell> cells = buildBarCells();
    const QPointF origin = toolbarRect().topLeft();
    for (BarCell &cell : cells)
        cell.rect.translate(origin);
    return cells;
}

QVector<Overlay::ToolButton> Overlay::toolButtons() const
{
    QVector<ToolButton> buttons;
    const QVector<BarCell> cells = barCells();
    for (const BarCell &cell : cells) {
        if (cell.kind == BarCell::Kind::Tool)
            buttons.append({cell.tool, cell.rect, cell.keyNumber});
    }
    return buttons;
}

QVector<Overlay::ActionButton> Overlay::actionButtons() const
{
    QVector<ActionButton> buttons;
    const QVector<BarCell> cells = barCells();
    for (const BarCell &cell : cells) {
        if (cell.kind == BarCell::Kind::Confirm)
            buttons.append({true, cell.rect});
        else if (cell.kind == BarCell::Kind::Cancel)
            buttons.append({false, cell.rect});
    }
    return buttons;
}

int Overlay::cellIndexAt(const QPointF &position) const
{
    const QVector<BarCell> cells = barCells();
    for (int i = 0; i < cells.size(); ++i) {
        if (cells.at(i).rect.contains(position))
            return i;
    }
    return -1;
}

void Overlay::activateCell(int index)
{
    const QVector<BarCell> cells = barCells();
    if (index < 0 || index >= cells.size()) {
        update();
        return;
    }
    const BarCell &cell = cells.at(index);
    switch (cell.kind) {
    case BarCell::Kind::Tool:
        if (m_textDraftActive)
            commitTextDraft(); // 切工具 = 先把已输入的文字落下来，不丢用户输入
        if (m_currentTool != cell.tool) {
            m_currentTool = cell.tool;
            SS_INFO() << "[overlay] 工具切换到" << toolName(cell.tool);
        }
        m_panel = Panel::None; // 换工具时收起面板
        setCursor(cell.tool == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        update();
        break;
    case BarCell::Kind::Ocr:
        emit annotationConfirmed(m_selection, ConfirmAction::Ocr);
        break;
    case BarCell::Kind::Scroll:
        emit annotationConfirmed(m_selection, ConfirmAction::Scroll);
        break;
    case BarCell::Kind::Pin:
        emit annotationConfirmed(m_selection, ConfirmAction::Pin);
        break;
    case BarCell::Kind::Confirm:
        emit annotationConfirmed(m_selection, ConfirmAction::Plain);
        break;
    case BarCell::Kind::Cancel:
        emit annotationCancelled();
        break;
    case BarCell::Kind::SizeText:
        update();
        break;
    case BarCell::Kind::ColorSwatch:
        m_panel = (m_panel == Panel::Color) ? Panel::None : Panel::Color;
        m_hoverSwatch = -1;
        update();
        break;
    case BarCell::Kind::WidthPicker:
        m_panel = (m_panel == Panel::Width) ? Panel::None : Panel::Width;
        m_hoverWidthHandle = false;
        update();
        break;
    }
}

void Overlay::drawToolbar(QPainter &painter) const
{
    const QRectF bar = toolbarRect();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kBarBorder, 1.0));
    painter.setBrush(kBarBg);
    painter.drawRoundedRect(bar, 8, 8);

    const QVector<BarCell> cells = barCells();
    const QRectF iconBox = QRectF(0, 0, kBtnSize - 2 * kIconInset, kBtnSize - 2 * kIconInset);
    for (int i = 0; i < cells.size(); ++i) {
        const BarCell &cell = cells.at(i);
        if (cell.separatorBefore) {
            const double sx = cell.rect.left() - kGroupGap / 2.0;
            painter.setPen(QPen(kSeparator, 1.0));
            painter.drawLine(QPointF(sx, bar.top() + 8), QPointF(sx, bar.bottom() - 8));
        }

        const bool hovered = (i == m_hoverCell);
        const bool pressed = (i == m_pressedCell);
        QRectF box = cell.rect;
        box.moveTopLeft(cell.rect.topLeft() + QPointF(kIconInset, kIconInset));
        box.setSize(iconBox.size());

        switch (cell.kind) {
        case BarCell::Kind::Tool: {
            const bool active = (cell.tool == m_currentTool);
            if (pressed || active || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress : (active ? kBtnActive : kBtnHover));
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            // 图标优先用 ui/Theme 的语义图标（与编辑器工具栏一致），取不到才自绘回退
            const QColor tint = active ? kAccent : kIconColor;
            const QPixmap themeIcon = tintedIcon(toolIconFor(cell.tool), box.size().toSize(),
                                                devicePixelRatioF(), tint);
            if (!themeIcon.isNull())
                painter.drawPixmap(box.topLeft(), themeIcon);
            else
                drawToolIcon(painter, cell.tool, box, tint);
            break;
        }
        case BarCell::Kind::ColorSwatch: {
            if (pressed || m_panel == Panel::Color || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress
                                         : (m_panel == Panel::Color ? kBtnActive : kBtnHover));
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            const double inset = kIconInset - 1.0;
            drawSwatch(painter,
                       cell.rect.adjusted(inset, inset, -inset, -inset), m_annotationColor, false,
                       false);
            break;
        }
        case BarCell::Kind::WidthPicker: {
            if (pressed || m_panel == Panel::Width || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress
                                         : (m_panel == Panel::Width ? kBtnActive : kBtnHover));
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            // 一条按当前粗细画的预览线（粗细上限 24，所以画的时候先夹到图标高度内）
            const double thickness = qBound(2.0, double(m_annotationWidth), box.height());
            painter.setPen(Qt::NoPen);
            painter.setBrush(kIconColor);
            painter.drawRoundedRect(QRectF(box.left(), box.center().y() - thickness / 2.0,
                                          box.width(), thickness),
                                   thickness / 2.0, thickness / 2.0);
            break;
        }
        case BarCell::Kind::SizeText:
            painter.setPen(kSizeTextColor);
            painter.drawText(cell.rect, Qt::AlignCenter, selectionSizeText());
            break;
        case BarCell::Kind::Ocr: {
            if (pressed || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress : kBtnHover);
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            drawOcrIcon(painter, box, kIconColor);
            break;
        }
        case BarCell::Kind::Scroll: {
            if (pressed || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress : kBtnHover);
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            drawScrollIcon(painter, box, kIconColor);
            break;
        }
        case BarCell::Kind::Pin: {
            if (pressed || hovered) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress : kBtnHover);
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            drawPinIcon(painter, box, kIconColor);
            break;
        }
        case BarCell::Kind::Confirm:
        case BarCell::Kind::Cancel: {
            const bool confirm = (cell.kind == BarCell::Kind::Confirm);
            if (confirm) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? QColor(0x2f, 0x92, 0xdb) : kAccent);
                painter.drawRoundedRect(cell.rect, 6, 6);
            } else if (hovered || pressed) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pressed ? kBtnPress : kBtnHover);
                painter.drawRoundedRect(cell.rect, 6, 6);
            }
            drawActionIcon(painter, confirm, box, confirm ? QColor(255, 255, 255) : kIconColor);
            break;
        }
        }
    }

    // 面板开着时不画悬停气泡：两者会挤在同一位置，而且面板本身已经写明颜色/粗细
    if (m_hoverCell >= 0 && m_hoverCell < cells.size() && m_pressedCell < 0
        && m_panel == Panel::None)
        drawCellBubble(painter, cells.at(m_hoverCell));
    painter.restore();
}

void Overlay::drawCellBubble(QPainter &painter, const BarCell &cell) const
{
    QString text;
    switch (cell.kind) {
    case BarCell::Kind::Tool:
        text = toolName(cell.tool);
        if (cell.keyNumber > 0)
            text += QStringLiteral("  %1").arg(cell.keyNumber);
        break;
    case BarCell::Kind::Confirm:
        text = QStringLiteral("完成  Enter");
        break;
    case BarCell::Kind::Pin:
        text = QStringLiteral("贴图（钉到屏幕上）");
        break;
    case BarCell::Kind::Scroll:
        text = QStringLiteral("长截图（滚动截屏）");
        break;
    case BarCell::Kind::Ocr:
        text = QStringLiteral("文字识别（OCR）");
        break;
    case BarCell::Kind::ColorSwatch:
        text = QStringLiteral("颜色  ") + m_annotationColor.name();
        break;
    case BarCell::Kind::WidthPicker:
        text = QStringLiteral("粗细  %1 px").arg(m_annotationWidth);
        break;
    case BarCell::Kind::Cancel:
        text = QStringLiteral("取消  Esc");
        break;
    case BarCell::Kind::SizeText:
        return;
    }

    const QFontMetricsF metrics(font());
    const double w = metrics.horizontalAdvance(text) + 18;
    const double h = metrics.height() + 9;

    // 气泡放到**远离选区**的那一侧，免得盖住正在标注的内容
    const QRectF bar = toolbarRect();
    const QRectF selection = toLocal(m_selection.logicalRect);
    const bool barBelowSelection = bar.top() >= selection.bottom();
    double y = barBelowSelection ? bar.bottom() + 6 : bar.top() - h - 6;
    if (y < 4 || y + h > height() - 4)
        y = barBelowSelection ? bar.top() - h - 6 : bar.bottom() + 6; // 兜底：另一边
    if (y < 4 || y + h > height() - 4)
        y = qBound(4.0, y, qMax(4.0, double(height()) - h - 4));

    QRectF box(cell.rect.center().x() - w / 2.0, y, w, h);
    box.moveLeft(qBound(4.0, box.left(), qMax(4.0, double(width()) - w - 4)));

    const QColor previousPen = painter.pen().color();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 216));
    painter.drawRoundedRect(box, 5, 5);
    painter.setPen(QColor(240, 243, 248));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.setPen(previousPen);
}

void Overlay::drawSwatch(QPainter &painter, const QRectF &box, const QColor &color, bool selected,
                        bool hovered) const
{
    if (box.isEmpty())
        return;
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(box, 5, 5);
    if (selected || hovered) {
        painter.setPen(QPen(selected ? kAccent : QColor(255, 255, 255, 190), selected ? 2.5 : 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box.adjusted(-2.0, -2.0, 2.0, 2.0), 7, 7);
    }
}

void Overlay::drawPanel(QPainter &painter) const
{
    if (m_panel == Panel::None)
        return;
    const QRectF panel = panelRect();
    if (panel.isEmpty())
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kBarBorder, 1.0));
    painter.setBrush(kBarBg);
    painter.drawRoundedRect(panel, 8, 8);

    if (m_panel == Panel::Color) {
        const QVector<QColor> colors = palette();
        for (int i = 0; i < colors.size(); ++i) {
            drawSwatch(painter, swatchRect(i), colors.at(i),
                       colors.at(i) == m_annotationColor, i == m_hoverSwatch);
        }
    } else {
        const QRectF track = widthTrackRect();
        const QRectF handle = widthHandleRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 42));
        painter.drawRoundedRect(track, 2, 2);
        QRectF filled = track;
        filled.setRight(handle.center().x());
        painter.setBrush(kAccent);
        painter.drawRoundedRect(filled, 2, 2);
        painter.setBrush(m_hoverWidthHandle || m_widthDragging ? kAccent : QColor(240, 243, 248));
        painter.setPen(QPen(QColor(20, 22, 26), 1.0));
        painter.drawEllipse(handle);
        painter.setPen(kSizeTextColor);
        painter.drawText(QRectF(panel.left() + 12, panel.top() + 6, panel.width() - 24, 16),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("粗细 %1 px").arg(m_annotationWidth));
    }
    painter.restore();
}

void Overlay::drawToolIcon(QPainter &painter, ToolKind kind, const QRectF &box, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = box.width();
    const double h = box.height();
    QPen pen(color, 1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case ToolKind::Select: { // 光标箭头
        QPainterPath path;
        path.moveTo(box.left() + w * 0.30, box.top());
        path.lineTo(box.left() + w * 0.30, box.top() + h * 0.92);
        path.lineTo(box.left() + w * 0.49, box.top() + h * 0.72);
        path.lineTo(box.left() + w * 0.62, box.bottom());
        path.lineTo(box.left() + w * 0.76, box.top() + h * 0.93);
        path.lineTo(box.left() + w * 0.63, box.top() + h * 0.66);
        path.lineTo(box.left() + w * 0.86, box.top() + h * 0.63);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case ToolKind::Rectangle:
        painter.drawRoundedRect(QRectF(box.left(), box.top() + h * 0.14, w, h * 0.72), 1.5, 1.5);
        break;
    case ToolKind::Ellipse:
        painter.drawEllipse(QRectF(box.left(), box.top() + h * 0.12, w, h * 0.76));
        break;
    case ToolKind::Arrow: {
        const QPointF tail(box.left() + w * 0.12, box.bottom() - h * 0.12);
        const QPointF head(box.right() - w * 0.12, box.top() + h * 0.12);
        painter.drawLine(tail, head);
        const double angle = std::atan2(head.y() - tail.y(), head.x() - tail.x());
        const double length = w * 0.34;
        for (const double offset : {-0.55, 0.55}) {
            painter.drawLine(head, head - QPointF(std::cos(angle + offset), std::sin(angle + offset))
                                           * length);
        }
        break;
    }
    case ToolKind::Pen: { // 一笔波浪
        QPainterPath path;
        path.moveTo(box.left(), box.bottom() - h * 0.25);
        path.cubicTo(box.left() + w * 0.25, box.top() - h * 0.15,
                     box.left() + w * 0.55, box.bottom() + h * 0.15,
                     box.right(), box.top() + h * 0.25);
        painter.drawPath(path);
        break;
    }
    case ToolKind::Highlight: { // 半透明粗笔迹
        QColor highlight = color;
        highlight.setAlpha(120);
        QPen highlighter(highlight, qMax(3.0, h * 0.38));
        highlighter.setCapStyle(Qt::FlatCap);
        painter.setPen(highlighter);
        painter.drawLine(QPointF(box.left() + w * 0.08, box.bottom() - h * 0.18),
                         QPointF(box.right() - w * 0.08, box.top() + h * 0.18));
        break;
    }
    case ToolKind::Text: // “T”
        painter.drawLine(QPointF(box.left() + w * 0.08, box.top() + h * 0.12),
                         QPointF(box.right() - w * 0.08, box.top() + h * 0.12));
        painter.drawLine(QPointF(box.center().x(), box.top() + h * 0.12),
                         QPointF(box.center().x(), box.bottom() - h * 0.06));
        break;
    case ToolKind::Number: { // 实心圆徽标 + “1”
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(box.left() + w * 0.11, box.top() + h * 0.11, w * 0.78, h * 0.78));
        QPen digit(QColor(22, 24, 28), 1.3);
        digit.setJoinStyle(Qt::RoundJoin);
        digit.setCapStyle(Qt::RoundCap);
        painter.setPen(digit);
        painter.setBrush(Qt::NoBrush);
        QPainterPath one;
        one.moveTo(box.center().x() - w * 0.12, box.top() + h * 0.42);
        one.lineTo(box.center().x() + w * 0.01, box.top() + h * 0.31);
        one.lineTo(box.center().x() + w * 0.01, box.top() + h * 0.71);
        one.moveTo(box.center().x() - w * 0.10, box.top() + h * 0.71);
        one.lineTo(box.center().x() + w * 0.12, box.top() + h * 0.71);
        painter.drawPath(one);
        break;
    }
    case ToolKind::Blur: { // 框 + 柔化点
        painter.drawRoundedRect(QRectF(box.left(), box.top() + h * 0.16, w, h * 0.68), 2, 2);
        QColor soft = color;
        soft.setAlpha(110);
        painter.setPen(Qt::NoPen);
        painter.setBrush(soft);
        painter.drawEllipse(box.center(), w * 0.17, h * 0.17);
        break;
    }
    case ToolKind::Mosaic: { // 棋盘格
        const QRectF frame(box.left(), box.top() + h * 0.14, w, h * 0.72);
        painter.drawRect(frame);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const double cw = frame.width() / 2.0;
        const double ch = frame.height() / 2.0;
        painter.drawRect(QRectF(frame.left(), frame.top(), cw, ch));
        painter.drawRect(QRectF(frame.left() + cw, frame.top() + ch, cw, ch));
        break;
    }
    }
    painter.restore();
}

void Overlay::drawOcrIcon(QPainter &painter, const QRectF &box, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = box.width();
    const double h = box.height();
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // 一个取景框 + 里面两条"文字线"（识别）
    painter.drawRoundedRect(QRectF(box.left(), box.top() + h * 0.16, w, h * 0.68), 2, 2);
    painter.drawLine(QPointF(box.left() + w * 0.18, box.top() + h * 0.38),
                     QPointF(box.right() - w * 0.18, box.top() + h * 0.38));
    painter.drawLine(QPointF(box.left() + w * 0.18, box.top() + h * 0.60),
                     QPointF(box.left() + w * 0.62, box.top() + h * 0.60));
    painter.restore();
}

void Overlay::drawScrollIcon(QPainter &painter, const QRectF &box, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = box.width();
    const double h = box.height();
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // 上下两条"页"线 + 中间向下的箭头（滚动）
    painter.drawLine(QPointF(box.left(), box.top() + h * 0.12),
                     QPointF(box.right(), box.top() + h * 0.12));
    painter.drawLine(QPointF(box.left(), box.bottom() - h * 0.12),
                     QPointF(box.right(), box.bottom() - h * 0.12));
    const double cx = box.center().x();
    painter.drawLine(QPointF(cx, box.top() + h * 0.28), QPointF(cx, box.bottom() - h * 0.32));
    painter.drawLine(QPointF(cx - w * 0.16, box.bottom() - h * 0.48),
                     QPointF(cx, box.bottom() - h * 0.32));
    painter.drawLine(QPointF(cx + w * 0.16, box.bottom() - h * 0.48),
                     QPointF(cx, box.bottom() - h * 0.32));
    painter.restore();
}

void Overlay::drawPinIcon(QPainter &painter, const QRectF &box, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = box.width();
    const double h = box.height();
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // 针脚
    painter.drawLine(QPointF(box.left() + w * 0.20, box.bottom() - h * 0.12),
                     QPointF(box.left() + w * 0.58, box.top() + h * 0.46));
    // 钉头（45° 圆角方块）
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.save();
    painter.translate(box.center().x() + w * 0.14, box.center().y() - h * 0.16);
    painter.rotate(45);
    painter.drawRoundedRect(QRectF(-w * 0.21, -h * 0.21, w * 0.42, h * 0.42), 2, 2);
    painter.restore();
    painter.restore();
}

void Overlay::drawActionIcon(QPainter &painter, bool confirm, const QRectF &box, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 2.0);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (confirm) {
        QPainterPath path;
        path.moveTo(box.left() + box.width() * 0.10, box.top() + box.height() * 0.54);
        path.lineTo(box.left() + box.width() * 0.38, box.top() + box.height() * 0.80);
        path.lineTo(box.left() + box.width() * 0.92, box.top() + box.height() * 0.18);
        painter.drawPath(path);
    } else {
        painter.drawLine(box.topLeft() + QPointF(box.width() * 0.14, box.height() * 0.14),
                         box.bottomRight() - QPointF(box.width() * 0.14, box.height() * 0.14));
        painter.drawLine(box.topRight() + QPointF(-box.width() * 0.14, box.height() * 0.14),
                         box.bottomLeft() + QPointF(box.width() * 0.14, -box.height() * 0.14));
    }
    painter.restore();
}

QPointF Overlay::toLogical(const QPointF &local) const
{
    return QPointF(local.x() + m_virtualGeometry.x(), local.y() + m_virtualGeometry.y());
}

QRectF Overlay::toLocal(const QRectF &logical) const
{
    return QRectF(logical.x() - m_virtualGeometry.x(), logical.y() - m_virtualGeometry.y(),
                  logical.width(), logical.height());
}

void Overlay::updateHover(const QPointF &local)
{
    const QPointF logical = toLogical(local);
    const WindowInfo hovered = pickWindowAt(m_windows, logical);
    const bool changed = hovered.id != m_hover.id
        || hovered.frameGeometry != m_hover.frameGeometry;
    m_hover = hovered;
    m_hasHover = hovered.isValid();
    if (changed)
        update();
}

QRectF Overlay::currentDragRect() const
{
    return toLocal(normalizeDragRect(toLogical(m_dragStart), toLogical(m_dragCurrent)));
}

void Overlay::drawInfoBox(QPainter &painter, const QPoint &anchor, const QString &title,
                          const QString &detail) const
{
    const QFontMetrics metrics(painter.font());
    const int textWidth = qMax(metrics.horizontalAdvance(title), metrics.horizontalAdvance(detail));
    const QRect box(anchor.x(), anchor.y(), textWidth + 20, metrics.height() * 2 + 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 200));
    painter.drawRoundedRect(box, 6, 6);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(box.adjusted(10, 6, -10, 0), Qt::AlignLeft | Qt::AlignTop, title);
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(box.adjusted(10, 6 + metrics.height(), -10, 0), Qt::AlignLeft | Qt::AlignTop,
                     detail);
}

void Overlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (m_stage == Stage::Annotating) {
        paintAnnotating(painter);
        return;
    }

    // 压暗整屏
    painter.fillRect(rect(), QColor(0, 0, 0, kDimAlpha));

    if (m_dragging) {
        const QRectF selection = currentDragRect();
        painter.fillRect(selection, QColor(61, 174, 255, 40));
        QPen pen(kAccent, 2.0, Qt::SolidLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selection);
        drawInfoBox(painter, QPoint(int(selection.left()), int(selection.bottom()) + 6),
                    QStringLiteral("%1 %2 × %3")
                        .arg(m_intent == Intent::Record ? QStringLiteral("录制区域")
                                                        : QStringLiteral("区域"))
                        .arg(ScreenMapper::roundCoord(selection.width()))
                        .arg(ScreenMapper::roundCoord(selection.height())),
                    m_intent == Intent::Record ? QStringLiteral("松开左键开始录制")
                                               : QStringLiteral("松开左键截图"));
        return;
    }

    if (m_hasHover) {
        const QRectF frame = toLocal(m_hover.frameGeometry);
        QPen pen(Qt::white, 2.0, Qt::DashLine);
        pen.setDashPattern({6, 4});
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frame);

        const QString title = m_hover.title.isEmpty() ? m_hover.appId : m_hover.title;
        drawInfoBox(painter, QPoint(int(frame.left()), int(frame.top()) - 48),
                    title.isEmpty() ? QStringLiteral("(无标题窗口)") : title,
                    QStringLiteral("%1 × %2 · %3")
                        .arg(ScreenMapper::roundCoord(m_hover.frameGeometry.width()))
                        .arg(ScreenMapper::roundCoord(m_hover.frameGeometry.height()))
                        .arg(m_intent == Intent::Record ? QStringLiteral("左键录制该窗口区域")
                                                        : QStringLiteral("左键单击截取该窗口")));
        return;
    }

    // 没有命中窗口时的提示（居中偏上）
    painter.setPen(QColor(255, 255, 255));
    const QString hint = m_intent == Intent::Record
        ? QStringLiteral("拖动选择录制区域 · 单击窗口录制该窗口 · Esc 或右键取消")
        : QStringLiteral("左键单击窗口截图 · 按住拖动选择区域 · Esc 或右键取消");
    const QFontMetrics metrics(painter.font());
    const QRect box((width() - metrics.horizontalAdvance(hint)) / 2 - 16, 28,
                    metrics.horizontalAdvance(hint) + 32, metrics.height() + 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 200));
    painter.drawRoundedRect(box, 8, 8);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(box, Qt::AlignCenter, hint);
}

void Overlay::mousePressEvent(QMouseEvent *event)
{
    if (m_stage == Stage::Annotating) {
        const QPointF pos = event->position();
        if (event->button() == Qt::RightButton) {
            annotationPress(pos, Qt::RightButton);
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        // 0) 弹出面板优先：面板里的色块/滑杆、面板空白处、面板外点击的语义都不同
        if (m_panel != Panel::None) {
            if (m_panel == Panel::Color) {
                const int swatch = colorIndexAt(pos);
                if (swatch >= 0) {
                    setAnnotationColor(palette().at(swatch));
                    m_panel = Panel::None; // 选完就收起来（QQ/PixPin 的习惯）
                    update();
                    return;
                }
            } else if (m_panel == Panel::Width
                       && widthTrackRect().adjusted(-8, -8, 8, 8).contains(pos)) {
                m_widthDragging = true;
                setAnnotationWidth(widthFromPosition(pos));
                return;
            }
            if (panelRect().contains(pos))
                return; // 面板空白处：吞掉，不当作“开始标注”
            if (!toolbarContains(pos)) {
                m_panel = Panel::None; // 面板之外点一下 = 关面板，并且这次点击不画东西
                update();
                return;
            }
        }

        // 工具条**优先**于标注逻辑（Phase 0 已验证的判定顺序）
        const int cell = cellIndexAt(pos);
        if (cell >= 0) {
            m_pressedCell = cell;
            update();
            return;
        }
        if (toolbarContains(pos))
            return; // 落在工具条矩形内但不在按钮上：也算“点在工具条上”，不开始标注

        // 1) 选区手柄/边：任何工具下都优先（QQ/PixPin 也是这样）
        const Grip grip = gripAt(pos);
        if (grip != Grip::None) {
            if (grip != Grip::Inside) {
                selectionPress(pos, grip);
                update();
                return;
            }
            // 选区**内部**：选择工具下先看有没有点到标注 ——
            // 点到标注就交给标注逻辑（移动标注），什么都没点到才移动整个选区
            if (m_currentTool == ToolKind::Select && hitAnnotation(toImage(pos)) < 0) {
                selectionPress(pos, Grip::Inside);
                update();
                return;
            }
        }
        annotationPress(pos, Qt::LeftButton);
        return;
    }
    if (event->button() == Qt::RightButton) {
        emit selectionCancelled();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_dragStart = event->position();
    m_dragCurrent = m_dragStart;
    update();
}

void Overlay::mouseMoveEvent(QMouseEvent *event)
{
    if (m_stage == Stage::Annotating) {
        const QPointF pos = event->position();
        if (m_selectionDrag) {
            selectionMoveTo(pos);
            return;
        }
        if (m_widthDragging) {
            setAnnotationWidth(widthFromPosition(pos));
            return;
        }
        // 面板内的悬停
        if (m_panel == Panel::Color) {
            const int swatch = colorIndexAt(pos);
            if (swatch != m_hoverSwatch) {
                m_hoverSwatch = swatch;
                update();
            }
        } else if (m_panel == Panel::Width) {
            const bool onHandle = widthHandleRect().adjusted(-6, -6, 6, 6).contains(pos);
            if (onHandle != m_hoverWidthHandle) {
                m_hoverWidthHandle = onHandle;
                update();
            }
        }
        const int cell = cellIndexAt(pos);
        if (cell != m_hoverCell) {
            m_hoverCell = cell;
            update();
        }
        updateCursorFor(pos);
        annotationMove(pos);
        return;
    }
    if (m_dragging) {
        m_dragCurrent = event->position();
        update();
        return;
    }
    updateHover(event->position());
}

void Overlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_stage == Stage::Annotating) {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_widthDragging) {
            m_widthDragging = false;
            update();
            return;
        }
        if (m_selectionDrag) {
            m_selectionDrag = false;
            m_grip = Grip::None;
            update();
            return;
        }
        const QPointF pos = event->position();
        const int pressed = m_pressedCell;
        m_pressedCell = -1;
        if (pressed >= 0) {
            // 只有“按在同一格上、也在同一格上松开”才算点击（和普通按钮一致）
            if (pressed == cellIndexAt(pos))
                activateCell(pressed);
            else
                update();
            return;
        }
        annotationRelease(pos, Qt::LeftButton);
        return;
    }
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    m_dragCurrent = event->position();

    SelectionResult result;
    result.outputName = QGuiApplication::screenAt(event->globalPosition().toPoint())
                            ? QGuiApplication::screenAt(event->globalPosition().toPoint())->name()
                            : QString();

    if (dragExceedsClickThreshold(m_dragStart, m_dragCurrent)) {
        result.mode = SelectionMode::Rect;
        result.logicalRect = normalizeDragRect(toLogical(m_dragStart), toLogical(m_dragCurrent));
        if (result.logicalRect.width() < 2 || result.logicalRect.height() < 2) {
            m_dragging = false;
            update();
            return;
        }
    } else {
        updateHover(event->position());
        if (!m_hasHover) {
            update();
            return; // 点在空白处：不截图，继续等用户操作
        }
        result.mode = SelectionMode::Window;
        result.window = m_hover;
        result.logicalRect = m_hover.frameGeometry;
    }

    update();
    emit selectionFinished(result);
}

void Overlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_stage == Stage::Annotating) {
        if (event->button() != Qt::LeftButton)
            return;
        const QPointF pos = event->position();
        if (toolbarContains(pos))
            return;
        // 选择工具下双击**文字**标注 = 改字；其余地方双击 = 完成（对齐 QQ）
        if (m_currentTool == ToolKind::Select) {
            const int index = hitAnnotation(toImage(pos));
            if (index >= 0 && m_document.at(index).kind == ToolKind::Text) {
                m_selectedIndex = index;
                m_textDraftActive = true;
                m_textDraft = m_document.at(index).text;
                m_textAnchor = m_document.at(index).points.value(0);
                m_textEditIndex = index;
                update();
                return;
            }
        }
        emit annotationConfirmed(m_selection, ConfirmAction::Plain);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Overlay::keyPressEvent(QKeyEvent *event)
{
    if (m_stage == Stage::Annotating) {
        const int key = event->key();
        const Qt::KeyboardModifiers modifiers = event->modifiers();

        // 文字草稿优先吃键：打字、退格、回车、Esc
        if (m_textDraftActive) {
            if (key == Qt::Key_Escape) {
                cancelTransient();
                return;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                commitTextDraft();
                return;
            }
            if (key == Qt::Key_Backspace) {
                if (!m_textDraft.isEmpty()) {
                    m_textDraft.chop(1);
                    update();
                }
                return;
            }
            const QString text = event->text();
            if (!text.isEmpty() && text.at(0).isPrint()) {
                m_textDraft += text;
                update();
            }
            return;
        }

        if (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) {
            if (key == Qt::Key_Z) {
                if (modifiers & Qt::ShiftModifier)
                    redoAnnotation();
                else
                    undoAnnotation();
                return;
            }
            if (key == Qt::Key_Y) {
                redoAnnotation();
                return;
            }
        }
        if (key == Qt::Key_Escape) {
            // 先收弹出面板，再谈草稿/选中/退出
            if (m_panel != Panel::None) {
                m_panel = Panel::None;
                update();
                return;
            }
            // 有草稿/选中先清它们，什么都清不掉才退出（对齐计划里的 Esc 语义）
            if (!cancelTransient())
                emit annotationCancelled();
            return;
        }
        // 方向键微调选区（Shift 加速 ×10）
        if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up
            || key == Qt::Key_Down) {
            const double step = (modifiers & Qt::ShiftModifier) ? 10.0 : 1.0;
            if (key == Qt::Key_Left)
                nudgeSelection(-step, 0);
            else if (key == Qt::Key_Right)
                nudgeSelection(step, 0);
            else if (key == Qt::Key_Up)
                nudgeSelection(0, -step);
            else
                nudgeSelection(0, step);
            return;
        }
        if (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) {
            if (key == Qt::Key_A) {
                selectAllSurface(); // 全选（整屏）
                return;
            }
            if (key == Qt::Key_C) {
                emit annotationConfirmed(m_selection, ConfirmAction::Copy);
                return;
            }
            if (key == Qt::Key_S) {
                emit annotationConfirmed(m_selection, ConfirmAction::Save);
                return;
            }
            if (key == Qt::Key_T) {
                emit annotationConfirmed(m_selection, ConfirmAction::Pin); // 贴图（钉到屏幕上）
                return;
            }
            if (key == Qt::Key_L) {
                emit annotationConfirmed(m_selection, ConfirmAction::Scroll); // 长截图
                return;
            }
            if (key == Qt::Key_O) {
                emit annotationConfirmed(m_selection, ConfirmAction::Ocr); // 文字识别
                return;
            }
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            emit annotationConfirmed(m_selection, ConfirmAction::Plain);
            return;
        }
        if (key == Qt::Key_Delete) {
            deleteSelectedAnnotation();
            return;
        }
        // 1–9：依次切换工具
        if (key >= Qt::Key_1 && key <= Qt::Key_9) {
            const QVector<ToolKind> tools = toolOrder();
            const int index = key - Qt::Key_1;
            if (index < tools.size() && tools.at(index) != m_currentTool) {
                if (m_textDraftActive)
                    commitTextDraft();
                m_currentTool = tools.at(index);
                SS_INFO() << "[overlay] 快捷键切换工具" << (index + 1) << toolName(m_currentTool);
                update();
            }
            return;
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit selectionCancelled();
        return;
    }
    QWidget::keyPressEvent(event);
}

QString Overlay::toolIconDiagnostics()
{
    QStringList missing;
    const QVector<ToolKind> tools = toolOrder();
    for (ToolKind kind : tools) {
        if (toolIconFor(kind).isNull())
            missing << toolName(kind);
    }
    return QStringLiteral("主题图标=%1/%2 缺=%3%4")
        .arg(tools.size() - missing.size())
        .arg(tools.size())
        .arg(missing.isEmpty() ? QStringLiteral("(none)") : missing.join(QLatin1Char(',')))
        .arg(missing.isEmpty() ? QString()
                               : QStringLiteral("（这些按钮走自绘回退）"));
}

QString Overlay::diagnostics() const
{
    return QStringLiteral("覆盖层: layer-shell=%1, 快照窗口数=%2, 虚拟几何=%3x%4+%5+%6")
        .arg(m_layerShellApplied ? QStringLiteral("已应用") : QStringLiteral("未应用"))
        .arg(m_windows.size())
        .arg(m_virtualGeometry.width())
        .arg(m_virtualGeometry.height())
        .arg(m_virtualGeometry.x())
        .arg(m_virtualGeometry.y());
}

QString Overlay::annotationDiagnostics() const
{
    const QRectF local = toLocal(m_selection.logicalRect);
    const QRectF bar = toolbarRect();
    return QStringLiteral("annotating=%1 stage=%2 冻结图=%3x%4 表面=%5x%6 选区本地=%7,%8 %9x%10 选区逻辑=%11,%12 %13x%14 工具条=%15,%16 %17x%18 当前工具=%19 %20")
        .arg(m_stage == Stage::Annotating ? 1 : 0)
        .arg(m_stage == Stage::Annotating ? QStringLiteral("Annotating") : QStringLiteral("Selecting"))
        .arg(m_frozenFrame.width())
        .arg(m_frozenFrame.height())
        .arg(width())
        .arg(height())
        .arg(qRound(local.x()))
        .arg(qRound(local.y()))
        .arg(qRound(local.width()))
        .arg(qRound(local.height()))
        .arg(m_selection.logicalRect.x())
        .arg(m_selection.logicalRect.y())
        .arg(m_selection.logicalRect.width())
        .arg(m_selection.logicalRect.height())
        .arg(qRound(bar.x()))
        .arg(qRound(bar.y()))
        .arg(qRound(bar.width()))
        .arg(qRound(bar.height()))
        .arg(toolName(m_currentTool))
        .arg(annotationStatus());
}

} // namespace seashot
