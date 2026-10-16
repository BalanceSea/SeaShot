#include "record/RecordingIndicator.h"

#include "ui/Theme.h"
#include "util/Log.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QTimer>

#include <LayerShellQt/Window>

namespace seashot {

namespace {
constexpr int kHeight = 40;
constexpr int kMinWidth = 300;
constexpr int kPadding = 10;
constexpr int kStopWidth = 60;
constexpr int kGap = 6;

QString formatElapsed(qint64 ms)
{
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

RecordingIndicator::RecordingIndicator(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(QStringLiteral("seashot-recording"));

    // 红点闪烁：纯视觉，1s 一次
    m_blink = new QTimer(this);
    m_blink->setInterval(500);
    connect(m_blink, &QTimer::timeout, this, [this]() {
        m_dotOn = !m_dotOn;
        invalidateCache();
    });
    hide();
}

void RecordingIndicator::showFor(const QRectF &region, int fps, const QString &outputPath)
{
    m_region = region;
    m_fps = fps;
    m_outputPath = outputPath;
    m_captured = 0;
    m_dropped = 0;
    m_elapsedMs = 0;

    resize(kMinWidth, kHeight);
    reposition(region);
    show();
    raise();
    activateWindow();
    m_blink->start();

    QTextStream(stdout) << "RECORDING_INDICATOR region=" << region.x() << "," << region.y() << " "
                        << region.width() << "x" << region.height() << " fps=" << fps
                        << " output=" << outputPath << "\n";
    QTextStream(stdout).flush();
}

void RecordingIndicator::updateStats(int captured, int dropped, qint64 elapsedMs)
{
    const bool changed = captured != m_captured || dropped != m_dropped
        || (elapsedMs / 1000) != (m_elapsedMs / 1000);
    m_captured = captured;
    m_dropped = dropped;
    m_elapsedMs = elapsedMs;
    if (changed)
        invalidateCache();
}

void RecordingIndicator::hideIndicator()
{
    m_blink->stop();
    hide();
}

void RecordingIndicator::invalidateCache()
{
    m_cacheDirty = true;
    update();
}

void RecordingIndicator::ensureLayerShell()
{
    if (m_layerShellApplied)
        return;
    createWinId();
    QWindow *handle = windowHandle();
    if (!handle)
        return;
    if (auto *layerShell = LayerShellQt::Window::get(handle)) {
        layerShell->setLayer(LayerShellQt::Window::LayerOverlay);
        layerShell->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop
                                                             | LayerShellQt::Window::AnchorLeft));
        layerShell->setExclusiveZone(-1);
        layerShell->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityOnDemand);
        m_layerShellApplied = true;
        SS_INFO() << "[record] 指示器已套 layer-shell";
    }
}

void RecordingIndicator::applyPosition(const QPoint &globalTopLeft)
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
    move(globalTopLeft);
}

void RecordingIndicator::reposition(const QRectF &region)
{
    QScreen *screen = QGuiApplication::screenAt(region.center().toPoint());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

    int px = int(region.left());
    int py = int(region.top()) - height() - kGap; // 默认贴在区域上方之外
    if (py < available.top() + kGap)
        py = int(region.bottom()) + kGap;         // 上面没地方 → 放区域下方之外
    if (py + height() > available.bottom() - kGap)
        py = qMax(available.top() + kGap, available.bottom() - height() - kGap);
    px = qBound(available.left() + kGap, px, available.right() - width() - kGap);
    applyPosition(QPoint(px, py));

    // 证据：指示器必须落在录制区域**之外**（否则没有自排除的后端会把它录进去）
    QTextStream(stdout) << "RECORDING_INDICATOR_GEOMETRY x=" << px << " y=" << py
                        << " w=" << width() << " h=" << height() << " regionTop="
                        << int(region.top()) << " regionBottom=" << int(region.bottom()) << "\n";
    QTextStream(stdout).flush();
}

QRect RecordingIndicator::stopButtonRect() const
{
    return QRect(width() - kPadding - kStopWidth, (height() - 24) / 2, kStopWidth, 24);
}

void RecordingIndicator::rebuildCache()
{
    const qreal dpr = devicePixelRatioF();
    m_cache = QPixmap(size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    m_cache.fill(Qt::transparent);

    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ui::CardColors colors = ui::cardColors(palette());
    const QColor text = colors.cardText;
    const QColor highlight = colors.primary;
    const QColor highlightedText = colors.primaryText;
    QColor card = colors.card;
    card.setAlpha(240);
    painter.setPen(QPen(colors.frameOuter, ui::spec::frameOuterWidth));
    painter.setBrush(card);
    painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), ui::spec::cardRadius,
                            ui::spec::cardRadius);
    painter.setPen(QPen(colors.frameInner, ui::spec::frameInnerWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(3.0, 3.0, -3.0, -3.0),
                            ui::spec::cardRadius - 2, ui::spec::cardRadius - 2);

    // 闪烁红点
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_dotOn ? QColor(230, 60, 60) : QColor(230, 60, 60, 90));
    painter.drawEllipse(QPointF(kPadding + 6, height() / 2.0), 6, 6);

    // 计时 + 帧数
    painter.setPen(text);
    const QString info = QStringLiteral("REC %1   已录 %2 帧%3")
                             .arg(formatElapsed(m_elapsedMs))
                             .arg(m_captured)
                             .arg(m_dropped > 0 ? QStringLiteral("（丢 %1）").arg(m_dropped)
                                                : QString());
    painter.drawText(QRect(kPadding + 18, 0, stopButtonRect().left() - kPadding - 24, height()),
                     Qt::AlignVCenter | Qt::AlignLeft, info);

    // 停止按钮
    const QRect stopRect = stopButtonRect();
    painter.setPen(QPen(highlight.darker(110), 1));
    painter.setBrush(highlight);
    painter.drawRoundedRect(QRectF(stopRect).adjusted(0.5, 0.5, -0.5, -0.5), ui::spec::buttonRadius,
                            ui::spec::buttonRadius);
    const QIcon stopIcon = ui::iconStop();
    const QString stopLabel = QStringLiteral("停止");
    const int stopTextWidth = painter.fontMetrics().horizontalAdvance(stopLabel);
    const int stopIconPart = stopIcon.isNull() ? 0 : ui::spec::iconSize + ui::spec::gap / 2;
    const int stopStartX = stopRect.left() + (stopRect.width() - stopTextWidth - stopIconPart) / 2;
    if (!stopIcon.isNull()) {
        stopIcon.paint(&painter,
                       QRect(stopStartX, stopRect.top() + (stopRect.height() - ui::spec::iconSize) / 2,
                             ui::spec::iconSize, ui::spec::iconSize),
                       Qt::AlignCenter, QIcon::Normal, QIcon::Off);
    }
    painter.setPen(highlightedText);
    painter.drawText(QRect(stopStartX + stopIconPart, stopRect.top(), stopTextWidth + 2,
                           stopRect.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, stopLabel);
    painter.end();
    m_cacheDirty = false;
}

void RecordingIndicator::paintEvent(QPaintEvent *)
{
    if (m_cacheDirty || m_cache.size() != size() * devicePixelRatioF())
        rebuildCache();
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_cache);
}

void RecordingIndicator::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    invalidateCache();
}

void RecordingIndicator::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && stopButtonRect().contains(event->position().toPoint()))
        emit stopRequested();
}

void RecordingIndicator::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        emit stopRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RecordingIndicator::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        invalidateCache();
    QWidget::changeEvent(event);
}

} // namespace seashot
