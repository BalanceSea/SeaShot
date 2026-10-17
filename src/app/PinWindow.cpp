#include "app/PinWindow.h"

#include "util/Log.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

#include <LayerShellQt/Window>

namespace seashot {

namespace {
constexpr int kMenuWidth = 132;
constexpr int kMenuRowHeight = 30;
constexpr int kMenuPad = 6;
constexpr int kBorder = 2;
const QColor kMenuBg(26, 28, 33, 242);
const QColor kMenuBorder(255, 255, 255, 34);
const QColor kMenuHover(0x3d, 0xae, 0xff, 90);
const QColor kMenuText(232, 236, 242);
} // namespace

PinWindow::PinWindow(const QImage &image, const QPoint &globalTopLeft, QWidget *parent)
    : QWidget(parent)
    , m_image(image)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setWindowTitle(QStringLiteral("seashot-pin"));

    // 先按目标位置算好表面与偏移（保证 ensureLayerShell 时几何已定）。
    // ⚠️ 这里不要用 pinRect() 去算屏幕：pinRect() 依赖表面，会无限递归（实测直接段错误）。
    QPoint center = globalTopLeft + QPoint(m_image.width() / 2, m_image.height() / 2);
    QScreen *screen = QGuiApplication::screenAt(center);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    m_surface = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    resize(m_surface.size());
    move(m_surface.topLeft());
    m_offset = globalTopLeft - m_surface.topLeft();
    clampOffset();
}

QRect PinWindow::surfaceGeometry() const
{
    if (m_surface.isValid())
        return m_surface;
    QScreen *screen = QGuiApplication::primaryScreen();
    return screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
}

QSize PinWindow::pinSize() const
{
    return QSize(qMax(1, qRound(m_image.width() * m_zoom)),
                 qMax(1, qRound(m_image.height() * m_zoom)));
}

QRect PinWindow::pinRectLocal() const
{
    return QRect(m_offset, pinSize());
}

QRect PinWindow::pinRect() const
{
    return QRect(surfaceGeometry().topLeft() + m_offset, pinSize());
}

void PinWindow::setSurfaceForTest(const QRect &surface)
{
    m_surface = surface;
}

void PinWindow::clampOffset()
{
    const QSize size = pinSize();
    const QRect full(0, 0, width(), height());
    // 至少留 24px 在屏幕内，避免拖动时“把贴图甩丢”
    const int keep = 24;
    m_offset.setX(qBound(-size.width() + keep, m_offset.x(), full.width() - keep));
    m_offset.setY(qBound(-size.height() + keep, m_offset.y(), full.height() - keep));
}

void PinWindow::ensureLayerShell()
{
    if (m_layerShellApplied)
        return;
    createWinId();
    QWindow *handle = windowHandle();
    if (!handle) {
        SS_WARN() << "[pin] 拿不到 QWindow，layer-shell 无法应用";
        return;
    }
    if (auto *layerShell = LayerShellQt::Window::get(handle)) {
        layerShell->setLayer(LayerShellQt::Window::LayerOverlay);
        layerShell->setAnchors(LayerShellQt::Window::Anchors(
            LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layerShell->setMargins(QMargins(0, 0, 0, 0));
        layerShell->setExclusiveZone(-1);
        // KWin exposes this namespace as onScreenDisplay. The helper can exclude
        // auxiliary layers without also excluding ordinary SeaShot windows.
        layerShell->setScope(QStringLiteral("on-screen-display"));
        // 贴图不抢键盘：点一下它才可能拿到焦点（OnDemand），需要 Esc 关时够用
        layerShell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        m_layerShellApplied = true;
        SS_INFO() << "[pin] layer-shell role 已应用（LayerOverlay，四边锚定）";
    } else {
        SS_WARN() << "[pin] LayerShellQt 不可用，退化为普通窗口（X11 路径）";
    }
}

void PinWindow::showPin()
{
    if (m_image.isNull()) {
        SS_WARN() << "[pin] 空图，忽略";
        return;
    }
    const QRect surface = surfaceGeometry();
    if (geometry() != surface)
        setGeometry(surface);
    // ⚠️ 必须在 show() 之前设 shell（ISSUE-007）
    ensureLayerShell();
    updateInputMask();
    show();
    raise();
    update();
    SS_INFO() << "[pin] 已贴图" << m_image.size() << "at" << pinRect().topLeft()
              << "layerShell=" << m_layerShellApplied;
}

void PinWindow::closePin()
{
    hide();
    releaseKeyboard();
    emit closed();
}

void PinWindow::updateInputMask()
{
    QRegion region(pinRectLocal());
    if (m_menuOpen)
        region = region.united(menuRectLocal());
    if (QWindow *handle = windowHandle())
        handle->setMask(region); // 只动平台窗口（QWidget::setMask 会裁剪绘制，见 Toast 的教训）
}

QRect PinWindow::menuRectLocal() const
{
    if (!m_menuOpen)
        return QRect();
    const int menuHeight = kMenuPad * 2 + kMenuRowHeight * 3;
    QRect rect(m_menuAnchor.x(), m_menuAnchor.y(), kMenuWidth, menuHeight);
    const QRect surface(0, 0, width(), height());
    if (rect.right() > surface.right() - 4)
        rect.moveRight(surface.right() - 4);
    if (rect.bottom() > surface.bottom() - 4)
        rect.moveBottom(surface.bottom() - 4);
    if (rect.left() < 4)
        rect.moveLeft(4);
    if (rect.top() < 4)
        rect.moveTop(4);
    return rect;
}

QVector<QPair<PinWindow::MenuItem, QRect>> PinWindow::menuItems() const
{
    QVector<QPair<MenuItem, QRect>> items;
    const QRect menu = menuRectLocal();
    if (menu.isEmpty())
        return items;
    const QVector<MenuItem> order{MenuItem::Copy, MenuItem::Save, MenuItem::Close};
    for (int i = 0; i < order.size(); ++i) {
        items.append({order.at(i),
                      QRect(menu.left() + kMenuPad, menu.top() + kMenuPad + i * kMenuRowHeight,
                            menu.width() - 2 * kMenuPad, kMenuRowHeight)});
    }
    return items;
}

PinWindow::MenuItem PinWindow::menuItemAt(const QPoint &localPos) const
{
    for (const auto &item : menuItems()) {
        if (item.second.contains(localPos))
            return item.first;
    }
    return MenuItem::None;
}

void PinWindow::setZoomAnchored(double zoom, const QPoint &anchorLocal)
{
    const double next = qBound(kMinZoom, zoom, kMaxZoom);
    if (qFuzzyCompare(next, m_zoom))
        return;
    // 光标下的那个图像像素保持不动：imagePt = (anchor - offset) / zoom
    const QPointF imagePt((anchorLocal.x() - m_offset.x()) / m_zoom,
                          (anchorLocal.y() - m_offset.y()) / m_zoom);
    m_zoom = next;
    m_offset = QPoint(qRound(anchorLocal.x() - imagePt.x() * m_zoom),
                      qRound(anchorLocal.y() - imagePt.y() * m_zoom));
    clampOffset();
    updateInputMask();
    update();
}

void PinWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), Qt::transparent);

    const QRect pin = pinRectLocal();
    if (!pin.isEmpty()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
        painter.drawImage(pin, m_image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // 描边：外圈深色 + 内圈浅色（浅色/深色背景上都能看清边界）
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 160), kBorder + 2));
        painter.drawRect(pin.adjusted(-kBorder - 1, -kBorder - 1, kBorder, kBorder));
        painter.setPen(QPen(QColor(255, 255, 255, 150), kBorder));
        painter.drawRect(pin.adjusted(-kBorder / 2, -kBorder / 2, kBorder / 2 - 1,
                                      kBorder / 2 - 1));
    }

    if (!m_menuOpen)
        return;
    const QRect menu = menuRectLocal();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kMenuBorder, 1.0));
    painter.setBrush(kMenuBg);
    painter.drawRoundedRect(menu, 8, 8);

    const QVector<QPair<MenuItem, QRect>> items = menuItems();
    for (const auto &item : items) {
        if (item.first == m_hoverItem || item.first == m_pressedItem) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_pressedItem == item.first ? QColor(0x2f, 0x92, 0xdb) : kMenuHover);
            painter.drawRoundedRect(item.second, 5, 5);
        }
        QString label;
        switch (item.first) {
        case MenuItem::Copy: label = QStringLiteral("复制"); break;
        case MenuItem::Save: label = QStringLiteral("保存到默认目录"); break;
        case MenuItem::Close: label = QStringLiteral("关闭"); break;
        case MenuItem::None: break;
        }
        painter.setPen(kMenuText);
        painter.drawText(item.second.adjusted(10, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         label);
    }
}

void PinWindow::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint pos = event->position().toPoint();

    if (event->button() == Qt::RightButton) {
        m_menuOpen = !m_menuOpen;
        m_menuAnchor = pos;
        m_hoverItem = MenuItem::None;
        m_pressedItem = MenuItem::None;
        updateInputMask();
        update();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    if (m_menuOpen) {
        const MenuItem item = menuItemAt(pos);
        if (item != MenuItem::None) {
            m_pressedItem = item;
            update();
            return;
        }
        m_menuOpen = false; // 点在菜单外：收起（不开始拖动，避免"想关菜单却把贴图拖走"）
        updateInputMask();
        update();
        return;
    }

    m_dragging = true;
    m_dragOrigin = m_offset;
    m_dragStartPos = pos;
    setCursor(Qt::ClosedHandCursor);
}

void PinWindow::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_dragging) {
        m_offset = m_dragOrigin + (pos - m_dragStartPos);
        clampOffset();
        updateInputMask();
        update();
        return;
    }
    if (m_menuOpen) {
        const MenuItem item = menuItemAt(pos);
        if (item != m_hoverItem) {
            m_hoverItem = item;
            update();
        }
    }
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_pressedItem != MenuItem::None) {
        const MenuItem pressed = m_pressedItem;
        m_pressedItem = MenuItem::None;
        if (menuItemAt(event->position().toPoint()) == pressed)
            activate(pressed);
        else
            update();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
    }
}

void PinWindow::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;
    const double factor = delta > 0 ? 1.1 : 1.0 / 1.1;
    setZoomAnchored(m_zoom * factor, event->position().toPoint());
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_menuOpen) {
            m_menuOpen = false;
            updateInputMask();
            update();
            return;
        }
        closePin();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinWindow::activate(MenuItem item)
{
    switch (item) {
    case MenuItem::Copy:
        m_menuOpen = false;
        updateInputMask();
        update();
        emit copyRequested(m_image);
        break;
    case MenuItem::Save:
        m_menuOpen = false;
        updateInputMask();
        update();
        emit saveRequested(m_image);
        break;
    case MenuItem::Close:
        closePin();
        break;
    case MenuItem::None:
        break;
    }
}

QString PinWindow::diagnostics() const
{
    const QRect pin = pinRect();
    return QStringLiteral("贴图 %1x%2 zoom=%3 矩形=%4,%5 %6x%7 layerShell=%8 菜单=%9")
        .arg(m_image.width())
        .arg(m_image.height())
        .arg(m_zoom, 0, 'f', 3)
        .arg(pin.x())
        .arg(pin.y())
        .arg(pin.width())
        .arg(pin.height())
        .arg(m_layerShellApplied ? 1 : 0)
        .arg(m_menuOpen ? 1 : 0);
}

} // namespace seashot
