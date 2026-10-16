#include "app/Toast.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace seashot {

namespace {
constexpr int kThumbMaxWidth = 260;
constexpr int kThumbMaxHeight = 170;
constexpr int kPadding = 12;
constexpr int kButtonHeight = 28;
constexpr int kButtonGap = 8;
constexpr int kAutoHideMs = 8000;
} // namespace

Toast::Toast(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("seashot-toast"));

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, [this]() {
        hide();
        emit dismissed();
    });
}

void Toast::showResult(const QImage &image, const QString &savedPath, const QString &message,
                       const QString &copyError)
{
    m_image = image;
    m_savedPath = savedPath;
    m_message = message;
    m_copyError = copyError;

    QSize thumb = image.size();
    if (thumb.width() > kThumbMaxWidth || thumb.height() > kThumbMaxHeight)
        thumb.scale(kThumbMaxWidth, kThumbMaxHeight, Qt::KeepAspectRatio);

    const int buttonsWidth = (m_editEnabled ? 4 : 3) * 74 + 3 * kButtonGap;
    const int width = qMax(thumb.width(), buttonsWidth) + kPadding * 2;
    const int height = thumb.height() + kPadding * 3 + kButtonHeight + 34;
    resize(width, height);
    layoutButtons();
    repositionToCorner();
    show();
    raise();
    m_hideTimer.start(kAutoHideMs);
}

void Toast::layoutButtons()
{
    m_buttons.clear();
    int x = kPadding;
    const int y = height() - kPadding - kButtonHeight;
    const auto add = [&](Button button, const QString &) {
        m_buttons.append({button, QRect(x, y, 74, kButtonHeight)});
        x += 74 + kButtonGap;
    };
    if (m_editEnabled)
        add(Button::Edit, QStringLiteral("编辑"));
    add(Button::SaveAs, QStringLiteral("另存为"));
    add(Button::Copy, QStringLiteral("复制"));
    add(Button::Close, QStringLiteral("关闭"));
}

void Toast::repositionToCorner()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect available = screen->availableGeometry();
    move(available.right() - width() - 16, available.bottom() - height() - 16);
}

Toast::Button Toast::buttonAt(const QPoint &position) const
{
    for (const auto &entry : m_buttons) {
        if (entry.second.contains(position))
            return entry.first;
    }
    return Button::None;
}

void Toast::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect background = rect().adjusted(0, 0, -1, -1);
    painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
    painter.setBrush(QColor(20, 20, 24, 235));
    painter.drawRoundedRect(background, 10, 10);

    // 缩略图
    const QRect thumbRect(kPadding, kPadding, width() - kPadding * 2, 0);
    if (!m_image.isNull()) {
        QSize thumb = m_image.size();
        thumb.scale(kThumbMaxWidth, kThumbMaxHeight, Qt::KeepAspectRatio);
        const QRect target(thumbRect.x() + (thumbRect.width() - thumb.width()) / 2, kPadding,
                           thumb.width(), thumb.height());
        painter.drawImage(target, m_image);
        painter.setPen(QPen(QColor(255, 255, 255, 60), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(target);
    }

    // 文本
    const int textTop = kPadding + qMin(kThumbMaxHeight, thumbRect.width()) + 4;
    painter.setPen(QColor(240, 240, 240));
    painter.drawText(QRect(kPadding, textTop, width() - kPadding * 2, 18), Qt::AlignLeft,
                     m_message.isEmpty() ? QStringLiteral("已截图") : m_message);
    if (!m_copyError.isEmpty()) {
        painter.setPen(QColor(255, 160, 120));
        painter.drawText(QRect(kPadding, textTop + 16, width() - kPadding * 2, 18), Qt::AlignLeft,
                         m_copyError);
    }

    // 按钮
    const QFontMetrics metrics(painter.font());
    for (const auto &entry : m_buttons) {
        static const QHash<int, QString> labels{
            {int(Button::Edit), QStringLiteral("编辑")},
            {int(Button::SaveAs), QStringLiteral("另存为")},
            {int(Button::Copy), QStringLiteral("复制")},
            {int(Button::Close), QStringLiteral("关闭")},
        };
        const QRect &buttonRect = entry.second;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 26));
        painter.drawRoundedRect(buttonRect, 6, 6);
        painter.setPen(QColor(235, 235, 235));
        painter.drawText(buttonRect, Qt::AlignCenter, labels.value(int(entry.first)));
    }

    if (!m_savedPath.isEmpty()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(QRect(kPadding, height() - kPadding - kButtonHeight - 16, width() - kPadding * 2, 16),
                         Qt::AlignLeft, QFileInfo(m_savedPath).fileName());
    }
    Q_UNUSED(metrics)
}

void Toast::mousePressEvent(QMouseEvent *event)
{
    switch (buttonAt(event->position().toPoint())) {
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

void Toast::mouseMoveEvent(QMouseEvent *)
{
    m_hideTimer.stop(); // 鼠标在浮层上时不自动消失
}

void Toast::enterEvent(QEnterEvent *)
{
    m_hideTimer.stop();
}

void Toast::leaveEvent(QEvent *)
{
    m_hideTimer.start(kAutoHideMs);
}

} // namespace seashot
