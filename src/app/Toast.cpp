#include "app/Toast.h"

#include "ui/Theme.h"
#include "util/Log.h"

#include <QApplication>
#include <QDateTime>
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
constexpr int kScreenMargin = 16;
constexpr int kAnchorGap = 3;
constexpr int kButtonPadding = 12;
} // namespace

QString Toast::labelFor(Button button)
{
    switch (button) {
    case Button::Edit: return QStringLiteral("编辑");
    case Button::Save: return QStringLiteral("保存");
    case Button::SaveAs: return QStringLiteral("另存为");
    case Button::Copy: return QStringLiteral("复制");
    case Button::Close: return QStringLiteral("关闭");
    case Button::ExportGif: return QStringLiteral("导出 GIF");
    case Button::ExportMp4: return QStringLiteral("导出 MP4");
    case Button::CopyGif: return QStringLiteral("复制 GIF");
    case Button::None: break;
    }
    return QString();
}

QIcon Toast::iconFor(Button button)
{
    switch (button) {
    case Button::Edit: return ui::iconEdit();
    case Button::Save: return ui::iconSave();
    case Button::SaveAs: return ui::iconSaveAs();
    case Button::Copy: return ui::iconCopy();
    case Button::Close: return ui::iconClose();
    case Button::ExportGif: return ui::iconSave();
    case Button::ExportMp4: return ui::iconSaveAs();
    case Button::CopyGif: return ui::iconCopy();
    case Button::None: break;
    }
    return QIcon();
}

Toast::Toast(QWidget *parent)
    : QWidget(parent)
{
    // 不用 Qt::Tool：layer-shell 需要普通 toplevel（实测 Tool 窗口在全屏应用前面显示不出来）
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(QStringLiteral("seashot-toast"));
    m_fixedSurface = !qEnvironmentVariableIsSet("SEASHOT_TOAST_MOVING_SURFACE");

    // 用户反馈字号偏小：在系统字号基础上加一点（“按钮和字号都放大”）
    QFont toastFont = font();
    if (toastFont.pointSizeF() > 0)
        toastFont.setPointSizeF(toastFont.pointSizeF() + ui::spec::fontDelta);
    setFont(toastFont);

    m_autoHide.setSingleShot(true);
    connect(&m_autoHide, &QTimer::timeout, this, &Toast::closeAndNotify);
}

int Toast::barHeight() const
{
    return ui::spec::barHeight;
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

void Toast::setDragMarkerEnabled(bool enabled)
{
    m_dragMarker = enabled;
    invalidateCache();
}

void Toast::invalidateCache()
{
    m_cacheDirty = true;
    update();
}

QRect Toast::surfaceGeometry() const
{
    QScreen *screen = QGuiApplication::screenAt(m_lastAnchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
}

QPoint Toast::cardTopLeftGlobal() const
{
    if (!m_fixedSurface)
        return m_lastAppliedTopLeftFallback;
    return surfaceGeometry().topLeft() + m_cardOffset;
}

QRect Toast::currentGeometry() const
{
    return QRect(cardTopLeftGlobal(), m_cardSize);
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

    // 2) 卡片宽度：图片宽度 与「按钮条所需宽度」取大者
    //    （按钮变多变大后，卡片可能比图片宽；这个量必须直接算，不能用按钮位置反推）
    m_cardSize = display; // 先按图片尺寸算
    m_cardSize.setWidth(qMax(m_cardSize.width(), barNeededWidth()));
    m_cardSize.setHeight(m_cardSize.height() + barHeight());
    m_cardSize.setWidth(qMax(m_cardSize.width(), 320));
    m_cardSize.setHeight(qMax(m_cardSize.height(), 150));

    // 3) 表面：固定模式铺满输出；旧模式就是卡片本身
    if (m_fixedSurface) {
        const QRect surface = surfaceGeometry();
        resize(surface.size());
        move(surface.topLeft());
    } else {
        resize(m_cardSize);
    }

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
        << " card=" << m_cardSize.width() << "x" << m_cardSize.height()
        << " bar=" << barHeight() << " buttons=" << buttons().size()
        << " cardTopLeft=" << cardTopLeftGlobal().x() << "," << cardTopLeftGlobal().y()
        << " fixedSurface=" << (m_fixedSurface ? 1 : 0)
        << " autoHide=" << m_autoHideMs << "\n";
    out.flush();
    SS_INFO() << "[toast] visible=" << isVisible() << "layerShell=" << m_layerShellApplied
              << "cardTopLeft=" << cardTopLeftGlobal() << "autoHideMs=" << m_autoHideMs;

    maybeStartAutoHide();
    scheduleAutoClick();
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
    return QRect(0, m_cardSize.height() - barHeight(), m_cardSize.width(), barHeight());
}

QVector<Toast::Button> Toast::buttonOrder() const
{
    if (m_exportMode)
        return {Button::ExportGif, Button::ExportMp4, Button::CopyGif, Button::Close};
    return {m_editEnabled ? Button::Edit : Button::None, Button::Save, Button::SaveAs, Button::Copy,
            Button::Close};
}

void Toast::setExportMode(bool enabled, const QString &info)
{
    m_exportMode = enabled;
    m_exportInfo = info;
    invalidateCache();
}

void Toast::setExportBusy(bool busy)
{
    m_exportBusy = busy;
    invalidateCache();
}

int Toast::barNeededWidth() const
{
    const QFontMetrics metrics(font());
    int total = ui::spec::cardPadding * 2;
    bool first = true;
    for (Button button : buttonOrder()) {
        if (button == Button::None)
            continue;
        if (!first)
            total += ui::spec::gap;
        first = false;
        const QIcon ic = iconFor(button);
        const int iconPart = ic.isNull() ? 0 : ui::spec::iconSize + ui::spec::gap / 2;
        total += metrics.horizontalAdvance(labelFor(button)) + iconPart + ui::spec::buttonPadX * 2;
    }
    return total;
}

QVector<QPair<Toast::Button, QRect>> Toast::buttons() const
{
    const QVector<Button> order = buttonOrder();
    QVector<QPair<Button, QRect>> result;
    const QFontMetrics metrics(font());
    const int width = qMax(260, m_cardSize.width());
    const int height = qMax(120, m_cardSize.height());
    int x = width - ui::spec::cardPadding;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        if (*it == Button::None)
            continue;
        const QIcon ic = iconFor(*it);
        const int textWidth = metrics.horizontalAdvance(labelFor(*it));
        const int iconPart = ic.isNull() ? 0 : ui::spec::iconSize + ui::spec::gap / 2;
        const int buttonWidth = textWidth + iconPart + ui::spec::buttonPadX * 2;
        x -= buttonWidth;
        result.prepend({*it, QRect(x,
                                   height - barHeight()
                                       + (barHeight() - ui::spec::buttonHeight) / 2,
                                   buttonWidth, ui::spec::buttonHeight)});
        x -= ui::spec::gap;
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
    if (py + m_cardSize.height() > available.bottom() - kScreenMargin / 2)
        py = anchorGlobalRect.bottom() - m_cardSize.height(); // 下方放不下 → 左下角对齐，往上盖
    if (py < available.top() + kScreenMargin / 2)
        py = available.top() + kScreenMargin / 2;
    px = qBound(available.left() + kScreenMargin / 2, px,
                available.right() - m_cardSize.width() - kScreenMargin / 2);
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
        layerShell->setScope(QStringLiteral("on-screen-display"));
        if (m_fixedSurface) {
            // 表面铺满输出、不设边距：拖动只改卡片在表面内的偏移，KWin 无需重排图层
            layerShell->setAnchors(LayerShellQt::Window::Anchors(
                LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
            layerShell->setMargins(QMargins(0, 0, 0, 0));
        } else {
            layerShell->setAnchors(LayerShellQt::Window::Anchors(
                LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft));
        }
        layerShell->setExclusiveZone(-1);
        layerShell->setKeyboardInteractivity(
            LayerShellQt::Window::KeyboardInteractivityOnDemand);
        m_layerShellApplied = true;
        SS_INFO() << "[toast] layer-shell role 已应用"
                  << (m_fixedSurface ? "（铺满输出的固定表面）" : "（旧：移动表面）");
    } else {
        SS_WARN() << "[toast] LayerShellQt 不可用，退化为普通窗口（X11 路径）";
    }
}

void Toast::applyPosition(const QPoint &globalTopLeft)
{
    ensureLayerShell();
    ++m_commitCount;
    if (m_commitTimes.size() < 20000)
        m_commitTimes.append(QDateTime::currentMSecsSinceEpoch());

    if (!m_fixedSurface) {
        // 旧实现：移动表面本身（每次 set_margin 都会让 KWin 重排该层，仅用于 A/B 对照）
        if (m_layerShellApplied) {
            if (QScreen *screen =
                    QGuiApplication::screenAt(globalTopLeft + QPoint(m_cardSize.width() / 2,
                                                                     m_cardSize.height() / 2))) {
                if (auto *handle = windowHandle()) {
                    if (auto *layerShell = LayerShellQt::Window::get(handle)) {
                        layerShell->setMargins(QMargins(globalTopLeft.x() - screen->geometry().x(),
                                                        globalTopLeft.y() - screen->geometry().y(), 0,
                                                        0));
                    }
                }
            }
        }
        move(globalTopLeft);
        m_lastAppliedTopLeftFallback = globalTopLeft;
        if (m_layerShellApplied)
            update(QRect(0, 0, 1, 1)); // 只标 1px 脏区，促成提交但不重传内容
        return;
    }

    // 固定表面：只挪卡片在表面内的偏移，弄脏「旧位置 + 新位置」两块矩形
    const QRect surface = surfaceGeometry();
    const QPoint newOffset = globalTopLeft - surface.topLeft();
    if (newOffset == m_cardOffset) {
        --m_commitCount; // 位置没变，不算一次提交
        return;
    }
    const QRect oldCard(m_cardOffset, m_cardSize);
    const QRect newCard(newOffset, m_cardSize);
    m_cardOffset = newOffset;
    m_lastCardRect = newCard;

    // 输入区域只保留卡片本身（Wayland 下映射为 wl_surface.set_input_region），
    // 其余地方点击穿透到桌面。
    // ⚠️ 必须用 QWindow::setMask（只动平台窗口），**不能用 QWidget::setMask**：
    // 后者是"控件可见区域"裁剪，会阻止 Qt 绘制卡片以外的像素 —— 旧位置的边框
    // 就永远擦不掉，拖动时会留下一条实心拖影（实测就是这样，排查了很久）。
    if (QWindow *handle = windowHandle())
        handle->setMask(QRegion(newCard));

    // ⚠️ 脏区必须覆盖「最近几次」的卡片矩形，不能只标旧+新：
    // Qt 的 Wayland shm 后端有 2~3 个缓冲轮换，只重绘脏区时，轮到的那个缓冲里
    // 残留的是更早位置上的卡片像素 —— 表现就是拖动时出现拖影/闪烁（实测观测图里
    // 品红框被"拉长"成 526/566 宽即此）。取最近 3 次位置的并集即可让每个缓冲都写全。
    m_recentCardRects.append(newCard);
    while (m_recentCardRects.size() > 3)
        m_recentCardRects.removeFirst();
    QRect damage;
    for (const QRect &rect : std::as_const(m_recentCardRects))
        damage = damage.isNull() ? rect : damage.united(rect);
    Q_UNUSED(oldCard)
    // 脏区要含阴影外扩部分，否则拖动时阴影会留下残影
    update(damage.adjusted(-kShadowMargin, -kShadowMargin, kShadowMargin, kShadowMargin));
}

void Toast::rebuildCache()
{
    const QRect card = cardRect();
    const qreal dpr = devicePixelRatioF();
    m_cache = QPixmap(shadowRect().size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    m_cache.fill(Qt::transparent);

    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 一律用卡片本地坐标：阴影会画到卡片外面（缓存已经比卡片大一圈），
    // 所以必须先把坐标系移到卡片原点，否则阴影会整体偏到左上 ✗
    // （上一版就是先画阴影、后 translate，实测左、上多出一条黑边，右、下没有。）
    painter.translate(kShadowMargin, kShadowMargin);

    // 阴影：卡片外一圈渐渐变淡的圆角矩形。
    // ⚠️ 必须画成"1px 描边环"而不是"填充矩形叠层"：填充会互相重叠、
    //    alpha 逐层累加，靠近卡片处会积到接近纯黑，变成一条硬黑边
    //    （实测边缘处背景从 (22,29,43) 被压到 (2,2,3)，看起来就像加了黑框）。
    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i <= kShadowMargin; ++i) {
        const double fade = 1.0 - double(i) / double(kShadowMargin + 1);
        const int alpha = int(72.0 * fade * fade);
        if (alpha <= 0)
            continue;
        painter.setPen(QPen(QColor(0, 0, 0, alpha), 1));
        painter.drawRoundedRect(QRectF(card).adjusted(-i, -i + 3, i, i + 3),
                                ui::spec::cardRadius + i, ui::spec::cardRadius + i);
    }

    const ui::CardColors colors = ui::cardColors(palette());
    const QColor window = colors.card;
    const QColor text = colors.cardText;
    const QColor button = colors.button;
    const QColor buttonText = colors.buttonText;
    const QColor highlight = colors.primary;
    const QColor highlightedText = colors.primaryText;
    const QColor border = colors.border;

    const QRectF cardF = QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(window);
    painter.drawRoundedRect(cardF, ui::spec::cardRadius, ui::spec::cardRadius);

    // 图片（原尺寸或等比缩小）
    if (!m_display.isNull()) {
        const QRect imageArea(0, 0, card.width(), card.height() - barHeight());
        const QPoint imagePos((card.width() - m_display.width()) / 2,
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
    QString info = m_exportMode && !m_exportInfo.isEmpty() ? m_exportInfo : m_message;
    if (m_exportBusy)
        info = QStringLiteral("正在导出，请稍候…");
    if (m_scaledToFit && !m_source.isNull()) {
        info += QStringLiteral("（屏幕放不下，已缩到 %1×%2 显示）")
                    .arg(m_display.width())
                    .arg(m_display.height());
    }
    if (!m_copyError.isEmpty())
        info += QStringLiteral(" · ") + m_copyError;
    else if (!m_savedPath.isEmpty())
        info += QStringLiteral(" · ") + QFileInfo(m_savedPath).fileName();
    const int textRight = buttons().isEmpty() ? card.width() - kButtonPadding
                                              : buttons().first().second.left() - 8;
    const int textWidth = qMax(0, textRight - kButtonPadding);
    // 按钮多（导出模式有 4 个）时留给文字的空间会很小，直接画会被硬裁成半截
    // （实测出现过"已录制：64 帧 / 4.2 秒 · 选导出格"）。这里按宽度做省略号。
    const QString shown = painter.fontMetrics().elidedText(info, Qt::ElideRight, textWidth);
    painter.drawText(QRect(kButtonPadding, bar.top(), textWidth, bar.height()),
                     Qt::AlignVCenter | Qt::AlignLeft, shown);

    // 按钮：第一个（编辑/或首个可用）为主按钮；带图标 + hover/pressed 态
    const Button primaryButton = m_editEnabled ? Button::Edit : Button::SaveAs;
    for (const auto &entry : buttons()) {
        const QRect &rect = entry.second;
        const bool primary = entry.first == primaryButton;
        const bool hovered = m_hoverButton == entry.first;
        const bool pressed = m_pressedButton == entry.first;
        QColor background = primary ? highlight : button;
        if (pressed)
            background = background.darker(ui::spec::pressedDarker);
        else if (hovered)
            background = background.lighter(ui::spec::hoverLighten);
        const QColor foreground = primary ? highlightedText : buttonText;
        painter.setPen(QPen(primary ? highlight.darker(110) : border, 1));
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), ui::spec::buttonRadius,
                                ui::spec::buttonRadius);

        const QIcon ic = iconFor(entry.first);
        const QString label = labelFor(entry.first);
        const int textWidth = painter.fontMetrics().horizontalAdvance(label);
        const int iconPart = ic.isNull() ? 0 : ui::spec::iconSize + ui::spec::gap / 2;
        const int contentWidth = textWidth + iconPart;
        const int startX = rect.left() + (rect.width() - contentWidth) / 2;
        if (!ic.isNull()) {
            const QRect iconRect(startX, rect.top() + (rect.height() - ui::spec::iconSize) / 2,
                                 ui::spec::iconSize, ui::spec::iconSize);
            ic.paint(&painter, iconRect, Qt::AlignCenter, QIcon::Normal,
                     pressed ? QIcon::On : QIcon::Off);
        }
        painter.setPen(foreground);
        painter.drawText(QRect(startX + iconPart, rect.top(), textWidth + 2, rect.height()),
                         Qt::AlignVCenter | Qt::AlignLeft, label);
    }

    // 边框：外圈 3px 深色 + 内圈 2px 主题高亮色（浅色/深色桌面都能一眼找到）
    painter.setBrush(Qt::NoBrush);
    if (m_dragMarker) {
        painter.setPen(QPen(QColor(255, 0, 255), 4));
        painter.drawRect(QRectF(card).adjusted(2, 2, -2, -2));
        painter.end();
        m_cacheDirty = false;
        return;
    }
    painter.setPen(QPen(colors.frameOuter, ui::spec::frameOuterWidth));
    painter.drawRoundedRect(QRectF(card).adjusted(1.0, 1.0, -1.0, -1.0), ui::spec::cardRadius,
                            ui::spec::cardRadius);
    painter.setPen(QPen(colors.frameInner, ui::spec::frameInnerWidth));
    painter.drawRoundedRect(QRectF(card).adjusted(3.0, 3.0, -3.0, -3.0),
                            ui::spec::cardRadius - 2, ui::spec::cardRadius - 2);

    painter.end();
    m_cacheDirty = false;
}

void Toast::paintEvent(QPaintEvent *event)
{
    if (m_cacheDirty
        || m_cache.size() != (QSizeF(shadowRect().size()) * devicePixelRatioF()).toSize())
        rebuildCache();

    QPainter painter(this);
    if (!m_fixedSurface) {
        painter.drawPixmap(0, 0, m_cache);
        return;
    }

    // 固定表面：把「脏区大小的整帧」整体画上去（里面含透明像素），
    // 而不是先 fillRect(transparent) 再 drawPixmap —— 实测透明填充会被优化掉/不生效，
    // 导致旧位置像素残留成拖影。整帧绘制则是确定的：透明像素也会被写入。
    const QRect damage = event->rect();
    const qreal dpr = devicePixelRatioF();
    const QSize wanted = (QSizeF(damage.size()) * dpr).toSize();
    if (m_frameBuffer.size() != wanted) {
        m_frameBuffer = QPixmap(wanted);
        m_frameBuffer.setDevicePixelRatio(dpr);
    }
    m_frameBuffer.fill(Qt::transparent);
    {
        QPainter framePainter(&m_frameBuffer);
        framePainter.drawPixmap(m_cardOffset - QPoint(kShadowMargin, kShadowMargin) - damage.topLeft(),
                                m_cache);
    }
    painter.drawPixmap(damage.topLeft(), m_frameBuffer);
}

void Toast::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    invalidateCache();
}

void Toast::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint localInCard = event->position().toPoint()
        - (m_fixedSurface ? m_cardOffset : QPoint(0, 0));
    const Button hit = buttonAt(localInCard);
    if (hit != Button::None && event->button() == Qt::LeftButton) {
        m_pressedButton = hit;
        invalidateCache();
    }
    if (hit == Button::None && event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOrigin = event->globalPosition().toPoint() - cardTopLeftGlobal();
        return;
    }
    switch (hit) {
    case Button::Edit:
    case Button::Save:
    case Button::SaveAs:
    case Button::Copy:
    case Button::Close:
        activate(hit);
        break;
    case Button::None:
        break;
    }
}

void Toast::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        // 悬停态：跟随调色板的按钮高亮
        const QPoint localInCard = event->position().toPoint()
            - (m_fixedSurface ? m_cardOffset : QPoint(0, 0));
        const Button hovered = buttonAt(localInCard);
        if (hovered != m_hoverButton) {
            m_hoverButton = hovered;
            invalidateCache();
        }
        return;
    }
    // 逐事件提交：固定表面模式下每次只是「弄脏旧位置 + 新位置两块矩形 + 更新输入区域」，
    // 没有图层重排，也没有整块缓冲重传，所以不需要节流。
    applyPosition(event->globalPosition().toPoint() - m_dragOrigin);
}

void Toast::activate(Button button)
{
    switch (button) {
    case Button::Edit:
        hide();
        emit editRequested();
        emit dismissed();
        break;
    case Button::Save:
        emit saveRequested();
        break;
    case Button::SaveAs:
        emit saveAsRequested();
        break;
    case Button::Copy:
        emit copyRequested();
        break;
    case Button::ExportGif:
        emit exportGifRequested();
        break;
    case Button::ExportMp4:
        emit exportMp4Requested();
        break;
    case Button::CopyGif:
        emit copyGifRequested();
        break;
    case Button::Close:
        closeAndNotify();
        break;
    case Button::None:
        break;
    }
}

void Toast::scheduleAutoClick()
{
    // 验收用：SEASHOT_TOAST_AUTOCLICK=save|saveas|copy|edit|close|gif|mp4
    // 走 activate()，与真实鼠标点击完全同一条路径。
    const QString name = qEnvironmentVariable("SEASHOT_TOAST_AUTOCLICK").toLower();
    if (name.isEmpty())
        return;
    Button target = Button::None;
    if (name == QLatin1String("save"))
        target = Button::Save;
    else if (name == QLatin1String("saveas"))
        target = Button::SaveAs;
    else if (name == QLatin1String("copy"))
        target = Button::Copy;
    else if (name == QLatin1String("edit"))
        target = Button::Edit;
    else if (name == QLatin1String("close"))
        target = Button::Close;
    else if (name == QLatin1String("gif"))
        target = Button::ExportGif;
    else if (name == QLatin1String("mp4"))
        target = Button::ExportMp4;
    else if (name == QLatin1String("copygif"))
        target = Button::CopyGif;
    else {
        SS_WARN() << "[toast] 未知的 SEASHOT_TOAST_AUTOCLICK=" << name;
        return;
    }
    SS_INFO() << "[toast] 诊断：将在 500ms 后自动触发按钮" << name;
    QTimer::singleShot(500, this, [this, target] { activate(target); });
}

void Toast::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    if (m_pressedButton != Button::None) {
        m_pressedButton = Button::None;
        invalidateCache();
    }
    maybeStartAutoHide();
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
    if (m_hoverButton != Button::None) {
        m_hoverButton = Button::None;
        invalidateCache();
    }
    maybeStartAutoHide();
}

void Toast::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        invalidateCache();
    QWidget::changeEvent(event);
}

} // namespace seashot
