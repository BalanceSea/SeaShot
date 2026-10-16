#include "overlay/Overlay.h"

#include "core/SelectionModel.h"
#include "geometry/ScreenMapper.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
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
    setCursor(Qt::ArrowCursor);
    update();

    SS_INFO() << "[overlay] 进入就地标注阶段：选区" << m_selection.logicalRect << "冻结图"
              << m_frozenFrame.width() << "x" << m_frozenFrame.height() << "(scale=" << scale
              << ") 表面" << width() << "x" << height();
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

    // 3) 选区边框 + 8 手柄
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(kAccent, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selection);

    painter.setPen(QPen(QColor(255, 255, 255), 1.0));
    painter.setBrush(kAccent);
    for (const QPointF &point : handlePoints())
        painter.drawRect(QRectF(point.x() - 3.5, point.y() - 3.5, 7.0, 7.0));

    // 4) 自绘图标工具条（A2）：贴选区下方，放不下则上方
    drawToolbar(painter);
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

    { // 完成
        BarCell cell;
        cell.kind = BarCell::Kind::Confirm;
        cell.separatorBefore = 1;
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
        if (m_currentTool != cell.tool) {
            m_currentTool = cell.tool;
            SS_INFO() << "[overlay] 工具切换到" << toolName(cell.tool);
        }
        update();
        break;
    case BarCell::Kind::Confirm:
        emit annotationConfirmed(m_selection);
        break;
    case BarCell::Kind::Cancel:
        emit annotationCancelled();
        break;
    case BarCell::Kind::SizeText:
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
        case BarCell::Kind::SizeText:
            painter.setPen(kSizeTextColor);
            painter.drawText(cell.rect, Qt::AlignCenter, selectionSizeText());
            break;
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

    if (m_hoverCell >= 0 && m_hoverCell < cells.size() && m_pressedCell < 0)
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
    case BarCell::Kind::Cancel:
        text = QStringLiteral("取消  Esc");
        break;
    case BarCell::Kind::SizeText:
        return;
    }

    const QFontMetricsF metrics(font());
    const double w = metrics.horizontalAdvance(text) + 18;
    const double h = metrics.height() + 9;
    QRectF box(cell.rect.center().x() - w / 2.0, toolbarRect().top() - h - 6, w, h);
    if (box.top() < 4)
        box.moveTop(toolbarRect().bottom() + 6);
    box.moveLeft(qBound(4.0, box.left(), qMax(4.0, double(width()) - w - 4)));

    const QColor previousPen = painter.pen().color();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 216));
    painter.drawRoundedRect(box, 5, 5);
    painter.setPen(QColor(240, 243, 248));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.setPen(previousPen);
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
        if (event->button() == Qt::RightButton) {
            emit annotationCancelled();
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        // 工具条**优先**于标注逻辑（Phase 0 已验证的判定顺序）
        const int cell = cellIndexAt(event->position());
        if (cell >= 0) {
            m_pressedCell = cell;
            update();
            return;
        }
        if (toolbarContains(event->position()))
            return; // 落在工具条矩形内但不在按钮上：也算“点在工具条上”，不开始标注
        // A3 起：这里开始画标注。A2 只负责工具切换与完成/取消。
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
        const int cell = cellIndexAt(event->position());
        if (cell != m_hoverCell) {
            m_hoverCell = cell;
            update();
        }
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
        const int pressed = m_pressedCell;
        m_pressedCell = -1;
        // 只有“按在同一格上、也在同一格上松开”才算点击（和普通按钮一致）
        if (pressed >= 0 && pressed == cellIndexAt(event->position()))
            activateCell(pressed);
        else
            update();
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
        if (event->button() == Qt::LeftButton)
            emit annotationConfirmed(m_selection);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Overlay::keyPressEvent(QKeyEvent *event)
{
    if (m_stage == Stage::Annotating) {
        const int key = event->key();
        if (key == Qt::Key_Escape) {
            emit annotationCancelled();
            return;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            emit annotationConfirmed(m_selection);
            return;
        }
        // 1–9：依次切换工具
        if (key >= Qt::Key_1 && key <= Qt::Key_9) {
            const QVector<ToolKind> tools = toolOrder();
            const int index = key - Qt::Key_1;
            if (index < tools.size() && tools.at(index) != m_currentTool) {
                m_currentTool = tools.at(index);
                SS_INFO() << "[overlay] 快捷键切换工具" << (index + 1) << toolName(m_currentTool);
                update();
            }
            return;
        }
        return; // 其余键先不接（撤销/复制等在 A3/A4）
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
    return QStringLiteral("annotating=%1 stage=%2 冻结图=%3x%4 表面=%5x%6 选区本地=%7,%8 %9x%10 选区逻辑=%11,%12 %13x%14 工具条=%15,%16 %17x%18 当前工具=%19")
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
        .arg(toolName(m_currentTool));
}

} // namespace seashot
