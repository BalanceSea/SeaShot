#include "app/Toast.h"

#include "util/Log.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QTextStream>

#include <LayerShellQt/Window>

namespace seashot {

namespace {
constexpr int kBarHeight = 34;
constexpr int kScreenMargin = 16;
constexpr int kAnchorGap = 3;
constexpr int kButtonHeight = 24;
constexpr int kButtonPadding = 12;
constexpr int kPositionFlushMs = 8; ///< 拖动时合并提交间隔
constexpr int kDismissDelayMs = 1200; ///< 保存完成后再展示多久

QString buttonLabel(int id)
{
    // Button 枚举里 None 是 0，所以这里用 1..4
    switch (id) {
    case 1: return QStringLiteral("编辑");
    case 2: return QStringLiteral("另存为");
    case 3: return QStringLiteral("复制");
    case 4: return QStringLiteral("关闭");
    default: return QString();
    }
}
} // namespace

Toast::Toast(QWidget *parent)
    : QWidget(parent)
{
    // 不用 Qt::Tool：layer-shell 需要普通 toplevel（实测 Tool 窗口在全屏应用前面显示不出来）
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(QStringLiteral("seashot-toast"));

    m_autoHide.setSingleShot(true);
    connect(&m_autoHide, &QTimer::timeout, this, &Toast::closeAndNotify);

    m_positionFlush.setSingleShot(true);
    connect(&m_positionFlush, &QTimer::timeout, this, [this]() {
        m_positionPending = false;
        applyPosition(m_pendingTopLeft);
    });
}

int Toast::barHeight() const
{
    return kBarHeight;
}

void Toast::setEditEnabled(bool enabled)
{
    if (m_editEnabled == enabled)
        return;
    m_editEnabled = enabled;
    invalidateCache();
}

void Toast::setAutoHideMs(int milliseconds)
{
    m_autoHideMs = qMax(0, milliseconds);
}

void Toast::invalidateCache()
{
    m_cacheDirty = true;
    update();
}

void Toast::showResult(const QImage &image, const QString &savedPath, const QString &message,
                       const QString &copyError, const QRect &anchorGlobalRect)
{
    m_source = image;
    m_savedPath = savedPath;
    m_message = message;
    m_copyError = copyError;
    m_lastAnchor = anchorGlobalRect;
    m_cacheDirty = true;

    // 1) 原尺寸优先；屏幕可用区放不下才等比缩小（并标明）
    QScreen *screen = QGuiApplication::screenAt(anchorGlobalRect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int maxWidth = available.width() - kScreenMargin * 2;
    const int maxHeight = available.height() - kScreenMargin * 2;

    QSize display = image.size();
    m_scaledToFit = false;
    if (display.width() > maxWidth || display.height() > maxHeight) {
        display.scale(maxWidth, maxHeight, Qt::KeepAspectRatio);
        m_scaledToFit = true;
    }
    m_display = display == image.size()
        ? image
        : image.scaled(display, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 2) 卡片 == 图片尺寸（按钮条浮在图片上），但至少留够按钮宽度
    const int buttonsWidth = buttons().isEmpty() ? 0 : (buttons().last().second.right() + 1);
    const int cardWidth = qMax(m_display.width(), buttonsWidth + kButtonPadding);
    const int cardHeight = m_display.height() + barHeight();
    resize(qMax(cardWidth, 260), qMax(cardHeight, 120));

    reposition(anchorGlobalRect);
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);

    QTextStream out(stdout);
    out << "TOAST image=" << image.width() << "x" << image.height()
        << " display=" << m_display.width() << "x" << m_display.height()
        << " scaled=" << (m_scaledToFit ? 1 : 0)
        << " anchor=" << anchorGlobalRect.x() << "," << anchorGlobalRect.y() << " "
        << anchorGlobalRect.width() << "x" << anchorGlobalRect.height()
        << " geometry=" << x() << "," << y() << " " << width() << "x" << height()
        << " autoHide=" << m_autoHideMs << "\n";
    out.flush();
    SS_INFO() << "[toast] visible=" << isVisible() << "layerShell=" << m_layerShellApplied
              << "geometry=" << geometry() << "autoHideMs=" << m_autoHideMs;

    maybeStartAutoHide();
}

void Toast::updateStatus(const QString &message, const QString &savedPath, const QString &copyError)
{
    m_message = message;
    m_savedPath = savedPath;
    m_copyError = copyError;
    invalidateCache();
}

void Toast::dismissAfter(int milliseconds)
{
    m_autoHide.stop();
    m_autoHide.start(qMax(0, milliseconds));
}

void Toast::maybeStartAutoHide()
{
    m_autoHide.stop();
    if (m_autoHideMs > 0)
        m_autoHide.start(m_autoHideMs);
}

void Toast::closeAndNotify()
{
    hide();
    emit dismissed();
}

QRect Toast::barRect() const
{
    return QRect(0, height() - barHeight(), width(), barHeight());
}

QVector<QPair<Toast::Button, QRect>> Toast::buttons() const
{
    const QVector<Button> order{m_editEnabled ? Button::Edit : Button::None, Button::SaveAs,
                                Button::Copy, Button::Close};
    QVector<QPair<Button, QRect>> result;
    const QFontMetrics metrics(font());
    int x = width() - kButtonPadding;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        if (*it == Button::None)
            continue;
        const int textWidth = metrics.horizontalAdvance(buttonLabel(int(*it)));
        const int buttonWidth = textWidth + 18;
        x -= buttonWidth;
        result.prepend({*it, QRect(x, height() - barHeight() + (barHeight() - kButtonHeight) / 2,
                                   buttonWidth, kButtonHeight)});
        x -= 6;
    }
    return result;
}

Toast::Button Toast::buttonAt(const QPoint &position) const
{
    for (const auto &entry : buttons()) {
        if (entry.second.contains(position))
            return entry.first;
    }
    return Button::None;
}

void Toast::reposition(const QRect &anchorGlobalRect)
{
    QScreen *screen = QGuiApplication::screenAt(anchorGlobalRect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

    int px = anchorGlobalRect.left();
    int py = anchorGlobalRect.bottom() + kAnchorGap;
    if (py + height() > available.bottom() - kScreenMargin / 2)
        py = anchorGlobalRect.bottom() - height(); // 下方放不下 → 左下角对齐，往上盖
    if (py < available.top() + kScreenMargin / 2)
        py = available.top() + kScreenMargin / 2;
    px = qBound(available.left() + kScreenMargin / 2, px,
                available.right() - width() - kScreenMargin / 2);
    applyPosition(QPoint(px, py));
}

void Toast::ensureLayerShell()
{
    if (m_layerShellApplied)
        return;
    createWinId();
    QWindow *handle = windowHandle();
    if (!handle) {
        SS_WARN() << "[toast] 拿不到 QWindow，无法应用 layer-shell";
        return;
    }
    if (auto *layerShell = LayerShellQt::Window::get(handle)) {
        layerShell->setLayer(LayerShellQt::Window::LayerOverlay);
        layerShell->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop
                                                             | LayerShellQt::Window::AnchorLeft));
        layerShell->setExclusiveZone(-1);
        layerShell->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityOnDemand);
        m_layerShellApplied = true;
        SS_INFO() << "[toast] layer-shell role 已应用（LayerOverlay）";
    } else {
        SS_WARN() << "[toast] LayerShellQt 不可用，退化为普通窗口（X11 路径）";
    }
}

void Toast::applyPosition(const QPoint &globalTopLeft)
{
    ensureLayerShell();
    if (m_layerShellApplied) {
        QScreen *screen =
            QGuiApplication::screenAt(globalTopLeft + QPoint(width() / 2, height() / 2));
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        const QRect output = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
        if (auto *handle = windowHandle()) {
            if (auto *layerShell = LayerShellQt::Window::get(handle)) {
                layerShell->setMargins(QMargins(globalTopLeft.x() - output.x(),
                                                globalTopLeft.y() - output.y(), 0, 0));
            }
        }
    }
    move(globalTopLeft); // 非 layer-shell（X11）下才有效
}

void Toast::rebuildCache()
{
    const qreal dpr = devicePixelRatioF();
    m_cache = QPixmap(size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    m_cache.fill(Qt::transparent);

    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor buttonText = pal.color(QPalette::ButtonText);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlightedText = pal.color(QPalette::HighlightedText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor shadow = pal.color(QPalette::Shadow);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(window);
    painter.drawRoundedRect(card, 8, 8);

    // 图片（原尺寸或等比缩小）
    if (!m_display.isNull()) {
        const QRect imageArea(0, 0, width(), height() - barHeight());
        const QPoint imagePos((width() - m_display.width()) / 2,
                              qMax(0, (imageArea.height() - m_display.height()) / 2));
        painter.drawImage(imagePos, m_display);
    }

    // 底部按钮条（浮在图片上）
    QColor barColor = window;
    barColor.setAlpha(235);
    painter.setPen(Qt::NoPen);
    painter.setBrush(barColor);
    const QRect bar = barRect();
    painter.drawRect(bar.adjusted(2, 0, -2, 0));
    painter.setPen(QPen(border, 1));
    painter.drawLine(bar.topLeft() + QPoint(1, 0), bar.topRight() - QPoint(1, 0));

    // 状态文字
    painter.setPen(text);
    QString info = m_message;
    if (m_scaledToFit && !m_source.isNull()) {
        info += QStringLiteral("（屏幕放不下，已缩到 %1×%2 显示）")
                    .arg(m_display.width())
                    .arg(m_display.height());
    }
    if (!m_copyError.isEmpty())
        info += QStringLiteral(" · ") + m_copyError;
    else if (!m_savedPath.isEmpty())
        info += QStringLiteral(" · ") + QFileInfo(m_savedPath).fileName();
    const int textRight = buttons().isEmpty() ? width() - kButtonPadding
                                              : buttons().first().second.left() - 8;
    painter.drawText(bar.adjusted(kButtonPadding, 0, -(width() - textRight), 0),
                     Qt::AlignVCenter | Qt::AlignLeft, info);

    // 按钮：第一个（编辑/或首个可用）为主按钮
    bool first = true;
    for (const auto &entry : buttons()) {
        const QRect &rect = entry.second;
        const bool primary = first;
        painter.setPen(QPen(primary ? highlight.darker(110) : border, 1));
        painter.setBrush(primary ? highlight : button);
        painter.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        painter.setPen(primary ? highlightedText : buttonText);
        painter.drawText(rect, Qt::AlignCenter, buttonLabel(int(entry.first)));
        first = false;
    }

    // 边框（你的要求：截图内容容易"溶进"背景）
    // 外圈 3px 深色 + 内圈 2px 主题高亮色：浅色与深色桌面上都能一眼找到
    painter.setBrush(Qt::NoBrush);
    QColor darkRing(0, 0, 0);
    darkRing.setAlpha(115);
    painter.setPen(QPen(darkRing, 3));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 8, 8);
    painter.setPen(QPen(highlight.darker(105), 2));
    painter.drawRoundedRect(QRectF(rect()).adjusted(4.0, 4.0, -4.0, -4.0), 6, 6);

    painter.end();
    m_cacheDirty = false;
}

void Toast::paintEvent(QPaintEvent *)
{
    if (m_cacheDirty || m_cache.size() != size() * devicePixelRatioF())
        rebuildCache();
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_cache);
}

void Toast::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    invalidateCache();
}

void Toast::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const Button hit = buttonAt(event->position().toPoint());
    if (hit == Button::None && event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOrigin = event->globalPosition().toPoint() - frameGeometry().topLeft();
        m_pendingTopLeft = frameGeometry().topLeft();
        return;
    }
    switch (hit) {
    case Button::Edit:
        hide();
        emit editRequested();
        emit dismissed();
        break;
    case Button::SaveAs:
        emit saveAsRequested();
        break;
    case Button::Copy:
        emit copyRequested();
        break;
    case Button::Close:
        closeAndNotify();
        break;
    case Button::None:
        break;
    }
}

void Toast::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton))
        return;
    // 合并提交：layer-shell 每次 setMargins 都是一次 commit，逐事件提交会明显卡顿
    m_pendingTopLeft = event->globalPosition().toPoint() - m_dragOrigin;
    if (!m_positionPending) {
        m_positionPending = true;
        m_positionFlush.start(kPositionFlushMs);
    }
}

void Toast::mouseReleaseEvent(QMouseEvent *)
{
    if (m_dragging && m_positionPending) {
        m_positionFlush.stop();
        m_positionPending = false;
        applyPosition(m_pendingTopLeft);
    }
    m_dragging = false;
}

void Toast::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
        closeAndNotify();
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void Toast::enterEvent(QEnterEvent *)
{
    m_autoHide.stop();
}

void Toast::leaveEvent(QEvent *)
{
    maybeStartAutoHide();
}

void Toast::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        invalidateCache();
    QWidget::changeEvent(event);
}

} // namespace seashot
