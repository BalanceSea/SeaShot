// Render real SeaShot widgets over synthetic content. Never capture the desktop,
// register shortcuts, write the clipboard, or use the user's application settings.
#include "app/SaveExportDialog.h"
#include "app/Settings.h"
#include "app/SettingsDialog.h"
#include "overlay/Overlay.h"
#include "record/RecordingIndicator.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTextStream>

using namespace seashot;

namespace {
const QColor ink("#183748");
const QColor muted("#637e8c");
const QColor teal("#008d83");

void text(QPainter &p, QPointF at, const QString &value, int size,
          QColor color = ink, bool bold = false)
{
    QFont font(QStringLiteral("Noto Sans CJK SC"));
    font.setPixelSize(size);
    font.setBold(bold);
    p.setFont(font);
    p.setPen(color);
    p.drawText(at, value);
}

void rounded(QPainter &p, QRectF rect, QColor color, int radius = 16)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(rect, radius, radius);
}

QImage workspace()
{
    QImage image(1280, 720, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor("#f1f5f8"));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRect(0, 0, 194, 720), QColor("#ffffff"));
    p.fillRect(QRect(194, 0, 1086, 58), QColor("#ffffff"));
    rounded(p, QRect(22, 19, 30, 30), teal, 9);
    text(p, {30, 41}, QStringLiteral("S"), 20, Qt::white, true);
    text(p, {62, 42}, QStringLiteral("SeaBoard"), 21, ink, true);
    text(p, {232, 37}, QStringLiteral("工作空间  /  项目概览"), 16, muted);
    rounded(p, QRect(1068, 15, 160, 30), QColor("#edf7f4"), 15);
    text(p, {1085, 36}, QStringLiteral("演示数据 · DEMO"), 13, teal);
    rounded(p, QRect(16, 88, 160, 42), QColor("#e4f5f0"), 9);
    const QStringList nav{QStringLiteral("项目概览"), QStringLiteral("灵感收集"),
                          QStringLiteral("进行中的任务"), QStringLiteral("团队资料")};
    for (int i = 0; i < nav.size(); ++i)
        text(p, {35, qreal(116 + i * 52)}, nav[i], 17, i == 0 ? teal : muted, i == 0);
    text(p, {28, 660}, QStringLiteral("MAKE IDEAS VISIBLE"), 11, muted);

    text(p, {234, 109}, QStringLiteral("把灵感，变成下一步。"), 29, ink, true);
    text(p, {235, 137}, QStringLiteral("本周进展一览 · 从记录开始，让每一次协作更清晰"), 15, muted);
    const QStringList labels{QStringLiteral("已收集灵感"), QStringLiteral("本周完成"), QStringLiteral("协作成员")};
    const QStringList values{QStringLiteral("128"), QStringLiteral("24"), QStringLiteral("08")};
    const QStringList notes{QStringLiteral("+18  较上周"), QStringLiteral("+6  持续推进"), QStringLiteral("一起把想法落地")};
    for (int i = 0; i < 3; ++i) {
        const int x = 234 + i * 334;
        rounded(p, QRect(x, 157, 314, 116), Qt::white);
        text(p, {qreal(x + 20), 186}, labels[i], 15, muted);
        text(p, {qreal(x + 20), 234}, values[i], 38, ink, true);
        text(p, {qreal(x + 124), 231}, notes[i], 13, teal);
    }

    rounded(p, QRect(234, 295, 650, 265), Qt::white);
    text(p, {258, 330}, QStringLiteral("灵感正在发生"), 19, ink, true);
    text(p, {745, 329}, QStringLiteral("最近 7 天"), 13, muted);
    for (int i = 0; i < 4; ++i) {
        p.setPen(QColor("#edf1f4"));
        p.drawLine(260, 365 + i * 46, 854, 365 + i * 46);
    }
    const QVector<QPointF> points{{265, 492}, {357, 471}, {449, 482}, {541, 424},
                                  {633, 441}, {725, 382}, {844, 354}};
    QPainterPath line(points.first());
    for (int i = 1; i < points.size(); ++i) {
        const auto a = points[i - 1];
        const auto b = points[i];
        line.cubicTo({a.x() + 45, a.y()}, {b.x() - 45, b.y()}, b);
    }
    QPainterPath fill = line;
    fill.lineTo(844, 517);
    fill.lineTo(265, 517);
    fill.closeSubpath();
    QLinearGradient gradient(0, 350, 0, 517);
    gradient.setColorAt(0, QColor("#9de0cc"));
    gradient.setColorAt(1, QColor("#ffffff"));
    p.fillPath(fill, gradient);
    p.setPen(QPen(teal, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(line);
    for (int i = 0; i < points.size(); ++i) {
        p.setPen(QPen(Qt::white, 3));
        p.setBrush(teal);
        p.drawEllipse(points[i], 5, 5);
        text(p, {points[i].x() - 13, 543},
             QStringLiteral("周%1").arg(QStringLiteral("一二三四五六日").mid(i, 1)), 12, muted);
    }

    rounded(p, QRect(904, 295, 332, 265), Qt::white);
    text(p, {928, 330}, QStringLiteral("下一步行动"), 19, ink, true);
    const QStringList tasks{QStringLiteral("整理新一轮产品灵感"), QStringLiteral("补充使用流程说明"),
                            QStringLiteral("分享本周设计进展")};
    for (int i = 0; i < tasks.size(); ++i) {
        rounded(p, QRect(928, 355 + i * 55, 22, 22), QColor("#e4f5f0"), 6);
        text(p, {962, qreal(373 + i * 55)}, tasks[i], 15);
        text(p, {963, qreal(393 + i * 55)}, QStringLiteral("工作台 · 示例内容"), 11, muted);
    }
    rounded(p, QRect(234, 584, 1002, 88), QColor("#e0eeeb"));
    text(p, {258, 619}, QStringLiteral("记录屏幕上的重点，让沟通少一点来回。"), 19, ink, true);
    text(p, {258, 647}, QStringLiteral("所有内容均为专门制作的演示数据。"), 14, muted);
    return image;
}

QImage page(const QString &number, const QString &title, const QString &subtitle)
{
    QImage image(1440, 1000, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, 1440, 1000);
    gradient.setColorAt(0, QColor("#edf6f3"));
    gradient.setColorAt(1, QColor("#e6edf9"));
    p.fillRect(image.rect(), gradient);
    p.drawPixmap(64, 29, ui::appIconPixmap(30));
    text(p, {106, 51}, QStringLiteral("SeaShot  /  看见 · 记录 · 分享"), 15, muted);
    text(p, {64, 113}, title, 40, ink, true);
    text(p, {66, 151}, subtitle, 18, muted);
    rounded(p, QRect(1257, 56, 116, 40), Qt::white, 20);
    text(p, {1282, 83}, number + QStringLiteral(" / 04"), 17, teal, true);
    text(p, {66, 972}, QStringLiteral("SEASHOT  ·  KDE PLASMA 6"), 12, muted);
    text(p, {1030, 972}, QStringLiteral("真实控件渲染  /  合成演示内容"), 12, muted);
    return image;
}

void framed(QImage &page, const QImage &content, QPoint at, const QString &title = {})
{
    QPainter p(&page);
    p.setRenderHint(QPainter::Antialiasing);
    const int top = title.isEmpty() ? 0 : 36;
    const QRectF frame(at.x() - 12, at.y() - top - 12, content.width() + 24, content.height() + top + 24);
    for (int i = 18; i > 0; --i)
        rounded(p, frame.adjusted(-i, -i / 2.0, i, i), QColor(29, 59, 82, 2), 20 + i);
    rounded(p, frame, Qt::white, 15);
    if (top) {
        p.drawPixmap(at.x() + 1, at.y() - 29, ui::appIconPixmap(20));
        text(p, {qreal(at.x() + 30), qreal(at.y() - 13)}, title, 14, muted);
    }
    p.drawImage(at, content);
}

QImage snapshot(QWidget &widget)
{
    widget.ensurePolished();
    widget.show();
    QApplication::processEvents();
    return widget.grab().toImage();
}

void stroke(Overlay &overlay, int key, QPointF from, QPointF to, QColor color, int width)
{
    overlay.setAnnotationColor(color);
    overlay.setAnnotationWidth(width);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &press);
    QMouseEvent down(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &down);
    QApplication::sendEvent(&overlay, &move);
    QApplication::sendEvent(&overlay, &up);
}

void save(const QDir &out, const QString &name, const QImage &image)
{
    if (!image.convertToFormat(QImage::Format_RGB32).save(out.filePath(name)))
        qFatal("Could not save README demo");
    QTextStream(stdout) << "DEMO " << out.filePath(name) << " " << image.width() << "x" << image.height() << Qt::endl;
}
} // namespace

int main(int argc, char **argv)
{
    // Enforce isolation even when invoked without an explicit platform argument.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(kwinscript);
    QCoreApplication::setApplicationName(QStringLiteral("seashot-readme-demo"));
    QCoreApplication::setOrganizationName(QStringLiteral("seashot-demo"));
    QTemporaryDir config;
    if (!config.isValid())
        return 1;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, config.path());
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Noto Sans CJK SC"));
    font.setPointSize(11);
    QApplication::setFont(font);
    QIcon::setThemeName(QStringLiteral("breeze"));
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor("#f6f8fa"));
    palette.setColor(QPalette::WindowText, ink);
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Button, QColor("#edf1f4"));
    palette.setColor(QPalette::ButtonText, ink);
    palette.setColor(QPalette::Highlight, teal);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(palette);
    const QDir out(app.arguments().value(1, QStringLiteral("docs/images")));
    if (!QDir().mkpath(out.absolutePath()))
        return 1;

    Overlay overlay;
    overlay.setVirtualGeometry(QRect(0, 0, 1280, 720));
    SelectionResult selection;
    selection.logicalRect = QRectF(216, 77, 1028, 486);
    overlay.beginAnnotating(selection, workspace());
    stroke(overlay, Qt::Key_2, {228, 151}, {553, 278}, QColor("#ed665f"), 4);
    stroke(overlay, Qt::Key_4, {996, 211}, {836, 348}, QColor("#ed665f"), 5);
    stroke(overlay, Qt::Key_8, {860, 322}, {860, 322}, QColor("#008d83"), 6);
    QKeyEvent select(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &select);
    QKeyEvent deselect(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &deselect);
    auto capture = page(QStringLiteral("01"), QStringLiteral("选中画面，直接标注。"),
                         QStringLiteral("冻结屏幕 · 自由选区 · 箭头与序号 · 完成后复制、保存或贴图"));
    framed(capture, snapshot(overlay), {80, 210});
    const QImage annotated = overlay.annotatedCapture().image;
    save(out, QStringLiteral("capture-annotate.png"), capture);
    overlay.stopSelection();

    SaveExportDialog exportDialog;
    exportDialog.resize(960, 560);
    exportDialog.setScreenshot(annotated, QStringLiteral("seashot-demo.png"));
    exportDialog.showPage(SaveExportDialog::Page::Screenshot);
    auto exportPage = page(QStringLiteral("02"), QStringLiteral("一次截图，多种交付方式。"),
                            QStringLiteral("在同一个窗口中预览结果，复制、保存、另存为，或继续编辑。"));
    framed(exportPage, snapshot(exportDialog), {240, 282}, exportDialog.windowTitle());
    save(out, QStringLiteral("save-export.png"), exportPage);
    exportDialog.hide();

    RecordingIndicator indicator;
    indicator.showFor(QRectF(100, 100, 640, 360), 15, QStringLiteral("demo.mp4"));
    indicator.updateStats(180, 0, 12000);
    auto recording = page(QStringLiteral("03"), QStringLiteral("先记录，再决定如何分享。"),
                           QStringLiteral("选区录制 · 实时计时 · 按需导出 GIF / MP4"));
    {
        QPainter p(&recording);
        text(p, {271, 238}, QStringLiteral("录制中  /  区域外指示器"), 16, muted);
        text(p, {271, 352}, QStringLiteral("录制结束  /  选择导出格式"), 16, muted);
    }
    framed(recording, snapshot(indicator), {275, 262});
    indicator.hideIndicator();
    exportDialog.resize(900, 420);
    exportDialog.setRecording(180, 12000, QStringLiteral("演示录制 · 15 fps"), true);
    exportDialog.showPage(SaveExportDialog::Page::Recording);
    framed(recording, snapshot(exportDialog), {270, 412}, exportDialog.windowTitle());
    save(out, QStringLiteral("recording-export.png"), recording);
    exportDialog.hide();

    Settings settings;
    settings.setSaveDir(QStringLiteral("~/Pictures/Screenshots"));
    SettingsDialog settingsDialog(&settings, nullptr);
    settingsDialog.resize(1060, 620);
    settingsDialog.m_configPathLabel->setText(QStringLiteral("~/.config/seashot/seashot.conf"));
    auto settingsPage = page(QStringLiteral("04"), QStringLiteral("让截图工具，适应你的习惯。"),
                              QStringLiteral("复制与保存 · 全局快捷键 · 录制格式 · 设置改动即时生效"));
    framed(settingsPage, snapshot(settingsDialog), {190, 250}, settingsDialog.windowTitle());
    save(out, QStringLiteral("settings.png"), settingsPage);
    return 0;
}
