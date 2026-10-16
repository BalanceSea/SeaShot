#include "app/Toast.h"

#include "util/Log.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QTextStream>

#include <LayerShellQt/Window>

namespace seashot {

namespace {
constexpr int kBarHeight = 34;
constexpr int kAutoHideMs = 8000;
constexpr int kScreenMargin = 16;
constexpr int kAnchorGap = 4;
constexpr int kButtonHeight = 24;
constexpr int kButtonPadding = 12;

QString buttonLabel(int id)
{
    // 注意：Button 枚举里 None 是 0，所以这里用 1..4
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
    // 不用 Qt::Tool：layer-shell 需要普通 toplevel（Phase 2 实测：Tool 窗口既拿不到
    // layer-shell role，也不会上浮到全屏窗口之上）
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("seashot-toast"));

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, [this]() {
        hide();
        emit dismissed();
    });
}

int Toast::barHeight() const
{
    return kBarHeight;
}

void Toast::showResult(const QImage &image, const QString &savedPath, const QString &message,
                       const QString &copyError, const QRect &anchorGlobalRect)
{
    m_source = image;
    m_savedPath = savedPath;
    m_message = message;
    m_copyError = copyError;
    m_lastAnchor = anchorGlobalRect;

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

    // 2) 卡片尺寸 == 图片尺寸（按钮条浮在图片底部）；但要留够按钮宽度
    const int buttonsWidth = buttons().isEmpty() ? 0 : (buttons().last().second.right() + 1);
    const int cardWidth = qMax(m_display.width(), buttonsWidth + kButtonPadding);
    const int cardHeight = m_display.height() + barHeight();
    resize(qMax(cardWidth, 260), qMax(cardHeight, 120));

    reposition(anchorGlobalRect);
    show();
    raise();
    m_hideTimer.start(kAutoHideMs);

    // 便于自动验收：把尺寸/位置/锚点打进 stdout
    QTextStream out(stdout);
    out << "TOAST image=" << image.width() << "x" << image.height()
        << " display=" << m_display.width() << "x" << m_display.height()
        << " scaled=" << (m_scaledToFit ? 1 : 0)
        << " anchor=" << anchorGlobalRect.x() << "," << anchorGlobalRect.y() << " "
        << anchorGlobalRect.width() << "x" << anchorGlobalRect.height()
        << " geometry=" << x() << "," << y() << " " << width() << "x" << height() << "\n";
    out.flush();
    // 诊断：layer-shell 下 move() 无效，真实位置由边距决定，所以把两者都打出来
    SS_INFO() << "[toast] visible=" << isVisible() << "winId=" << winId()
              << "layerShell=" << m_layerShellApplied << "geometry=" << geometry()
              << "windowHandle=" << (windowHandle() != nullptr);
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

QRect Toast::buttonRect(Button button) const
{
    for (const auto &entry : buttons()) {
        if (entry.first == button)
            return entry.second;
    }
    return {};
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

    // 首选：挂在截图框下方（卡片左上角 = 选区左下角）
    int px = anchorGlobalRect.left();
    int py = anchorGlobalRect.bottom() + kAnchorGap;
    // 下方放不下 → 卡片左下角与选区左下角对齐（往上盖）
    if (py + height() > available.bottom() - kScreenMargin / 2)
        py = anchorGlobalRect.bottom() - height();
    if (py < available.top() + kScreenMargin / 2)
        py = available.top() + kScreenMargin / 2;
    px = qBound(available.left() + kScreenMargin / 2, px,
                available.right() - width() - kScreenMargin / 2);
    applyPosition(QPoint(px, py));
}

// 覆盖层用 layer-shell 才能保证「永远在最上一层」（全屏窗口之上）
// —— Phase 2 后实测：普通 Qt::Tool 窗口在全屏应用前面根本显示不出来。
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
        SS_WARN() << "[toast] LayerShellQt 不可用，退化为普通置顶窗口（X11 路径）";
    }
}

void Toast::applyPosition(const QPoint &globalTopLeft)
{
    ensureLayerShell();
    if (m_layerShellApplied) {
        // layer-shell 下用「锚 top|left + 边距」定位（边距相对输出左上角）
        QScreen *screen = QGuiApplication::screenAt(globalTopLeft + QPoint(width() / 2, height() / 2));
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
    // 非 layer-shell（X11）下 move() 才是有效的
    move(globalTopLeft);
}

void Toast::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 取系统调色板 → 跟随 Breeze 亮/暗主题
    const QPalette pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor buttonText = pal.color(QPalette::ButtonText);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlightedText = pal.color(QPalette::HighlightedText);
    const QColor border = pal.color(QPalette::Mid);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(border, 1));
    painter.setBrush(window);
    painter.drawRoundedRect(card, 8, 8);

    // 图片（原尺寸或等比缩小）
    if (!m_display.isNull()) {
        const QRect imageRect(0, 0, width(), height() - barHeight());
        const QPoint imagePos((width() - m_display.width()) / 2,
                              qMax(0, (imageRect.height() - m_display.height()) / 2));
        painter.drawImage(imagePos, m_display);
    }

    // 底部按钮条（浮在图片上，半透明）
    QColor barColor = window;
    barColor.setAlpha(235);
    painter.setPen(Qt::NoPen);
    painter.setBrush(barColor);
    const QRect bar = barRect();
    painter.drawRect(bar.adjusted(1, 0, -1, 0));
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
    painter.drawText(bar.adjusted(kButtonPadding, 0, -buttons().value(0).second.left() - 6, 0),
                     Qt::AlignVCenter | Qt::AlignLeft, info);

    // 按钮：第一个（编辑）为主按钮，用高亮色
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
}

void Toast::mousePressEvent(QMouseEvent *event)
{
    const Button hit = buttonAt(event->position().toPoint());
    if (hit == Button::None && event->button() == Qt::LeftButton) {
        // 按住图片区域 = 拖动浮层
        m_dragging = true;
        m_dragOrigin = event->globalPosition().toPoint() - frameGeometry().topLeft();
        return;
    }
    switch (hit) {
    case Button::Edit:
        emit editRequested();
        break;
    case Button::SaveAs:
        emit saveAsRequested();
        break;
    case Button::Copy:
        emit copyRequested();
        break;
    case Button::Close:
        hide();
        emit dismissed();
        break;
    case Button::None:
        break;
    }
}

void Toast::mouseMoveEvent(QMouseEvent *event)
{
    m_hideTimer.stop();
    if (m_dragging && (event->buttons() & Qt::LeftButton))
        applyPosition(event->globalPosition().toPoint() - m_dragOrigin);
}

void Toast::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    m_hideTimer.start(kAutoHideMs);
}

void Toast::enterEvent(QEnterEvent *)
{
    m_hideTimer.stop();
}

void Toast::leaveEvent(QEvent *)
{
    m_hideTimer.start(kAutoHideMs);
}

void Toast::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        update();
    QWidget::changeEvent(event);
}

} // namespace seashot
