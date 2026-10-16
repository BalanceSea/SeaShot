#include "overlay/Overlay.h"

#include "core/SelectionModel.h"
#include "geometry/ScreenMapper.h"
#include "util/Log.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <LayerShellQt/Window>

namespace seashot {

namespace {
constexpr int kDimAlpha = 120;
const QColor kAccent(0x3d, 0xae, 0xff);
} // namespace

Overlay::Overlay(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
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
    if (m_dragging) {
        m_dragCurrent = event->position();
        update();
        return;
    }
    updateHover(event->position());
}

void Overlay::mouseReleaseEvent(QMouseEvent *event)
{
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

void Overlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit selectionCancelled();
        return;
    }
    QWidget::keyPressEvent(event);
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

} // namespace seashot
