#include "editor/EditorWindow.h"

#include "app/Settings.h"
#include "ui/Theme.h"
#include "util/Clipboard.h"
#include "util/Log.h"

#include <QAction>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace seashot {

namespace {

QIcon toolIcon(ToolKind kind)
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
    return {};
}

QIcon swatchIcon(const QColor &color)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QColor(255, 255, 255, 120));
    painter.drawRect(0, 0, pixmap.width() - 1, pixmap.height() - 1);
    return QIcon(pixmap);
}
} // namespace

EditorWindow::EditorWindow(const QImage &image, const QString &sourcePath, QWidget *parent)
    : QWidget(parent)
    , m_sourcePath(sourcePath)
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowTitle(QStringLiteral("SeaShot 编辑器"));
    // 编辑器自己也在 KWin 的「排除出截图」名单里（appId 含 seashot）

    m_canvas = new AnnotationCanvas(this);
    m_canvas->setImage(image);
    buildUi();

    connect(m_canvas, &AnnotationCanvas::documentChanged, this, [this]() {
        m_dirty = true;
        refreshActions();
    });
    connect(m_canvas, &AnnotationCanvas::selectionChanged, this, [this](int) { refreshActions(); });
    connect(m_canvas, &AnnotationCanvas::statusMessage, this,
            [this](const QString &message) { m_status->setText(message); });
    connect(m_canvas, &AnnotationCanvas::zoomChanged, this, [this](double) { updateZoomUi(); });
    connect(m_canvas, &AnnotationCanvas::panRequested, this, [this](const QPoint &delta) {
        if (!m_scroll)
            return;
        // 中键拖动平移 = 反向拨动滚动条
        if (QScrollBar *bar = m_scroll->horizontalScrollBar())
            bar->setValue(bar->value() - delta.x());
        if (QScrollBar *bar = m_scroll->verticalScrollBar())
            bar->setValue(bar->value() - delta.y());
    });

    setTool(ToolKind::Rectangle);
    updateZoomUi();
    refreshActions();
    updateTitle();
    // 窗口按**图片本身**定尺寸（用户反馈：画布留白太多、图片吊在中间），
    // 但也不能窄到让工具栏把它后面的工具折进 ">" 溢出菜单里 —— 所以取两者较大值。
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int chrome = 64; // 工具栏 + 状态栏的大致高度
    const int toolbarWidth = m_toolbar ? m_toolbar->sizeHint().width() + 24 : 0;
    const int wantW = qMax(760, qMax(image.width() + 40, toolbarWidth));
    const int wantH = qMax(420, image.height() + chrome + 24);
    resize(qMin(avail.width() - 60, wantW), qMin(avail.height() - 80, wantH));
}

void EditorWindow::buildUi()
{
    auto *toolbar = new QToolBar(this);
    m_toolbar = toolbar;
    toolbar->setMovable(false);
    // 图标与文字都显示：用户反馈“顶栏图标太小、看不懂”，
    // 统一走 ui::spec::iconSize（与浮层/配置界面同一份规格）。
    toolbar->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // 选中态要一眼能看出：QToolButton 在 Fusion/Breeze 下勾选态太淡
    const auto colors = ui::cardColors(palette());
    toolbar->setStyleSheet(
        QStringLiteral("QToolBar { spacing: %1px; padding: 4px; }\n"
                       "QToolButton { padding: 4px 8px; border-radius: %2px; }\n"
                       "QToolButton:hover { background: %3; }\n"
                       "QToolButton:checked { background: %4; color: %5; "
                       "border: 1px solid %6; }\n")
            .arg(ui::spec::gap)
            .arg(ui::spec::buttonRadius)
            .arg(colors.button.lighter(ui::spec::hoverLighten).name())
            .arg(colors.primary.name(), colors.primaryText.name(), colors.primary.darker(110).name()));

    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    const QVector<ToolKind> tools{ToolKind::Select, ToolKind::Arrow,   ToolKind::Rectangle,
                                  ToolKind::Ellipse, ToolKind::Pen,     ToolKind::Highlight,
                                  ToolKind::Text,   ToolKind::Number,  ToolKind::Blur,
                                  ToolKind::Mosaic};
    int shortcut = 1;
    for (ToolKind tool : tools) {
        // 「模糊/马赛克」另起一组（它们是区域效果，与绘制工具性质不同）
        if (tool == ToolKind::Blur)
            toolbar->addSeparator();
        auto *button = new QToolButton(toolbar);
        button->setText(toolName(tool));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(toolIcon(tool));
        button->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
        button->setShortcut(QKeySequence(QString::number(shortcut++)));
        button->setToolTip(QStringLiteral("%1（快捷键 %2）")
                               .arg(toolName(tool))
                               .arg(shortcut - 1));
        group->addButton(button, int(tool));
        toolbar->addWidget(button);
        m_toolButtons.insert(int(tool), button);
        connect(button, &QToolButton::clicked, this, [this, tool]() { setTool(tool); });
    }

    toolbar->addSeparator();
    m_colorButton = new QToolButton(toolbar);    m_colorButton->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
    m_colorButton->setText(QStringLiteral("颜色"));
    m_colorButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_colorButton->setIcon(swatchIcon(m_canvas->color()));
    connect(m_colorButton, &QToolButton::clicked, this, &EditorWindow::chooseColor);
    toolbar->addWidget(m_colorButton);

    toolbar->addWidget(new QLabel(QStringLiteral(" 粗细/字号 "), toolbar));
    m_widthBox = new QSpinBox(toolbar);
    m_widthBox->setRange(1, 48);
    m_widthBox->setValue(m_canvas->strokeWidth());
    m_widthBox->setToolTip(QStringLiteral("形状为线宽；文字工具时它是字号档位（实际字号 = 档位×4）"));
    connect(m_widthBox, &QSpinBox::valueChanged, this,
            [this](int value) { m_canvas->setStrokeWidth(value); });
    toolbar->addWidget(m_widthBox);

    toolbar->addSeparator();
    m_undoAction = toolbar->addAction(ui::iconUndo(), QStringLiteral("撤销"), this, [this]() {
        if (m_canvas->document().undo()) {
            m_dirty = true;
            m_canvas->invalidateComposedCache();
            m_canvas->update();
            refreshActions();
        }
    });
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = toolbar->addAction(ui::iconRedo(), QStringLiteral("重做"), this, [this]() {
        if (m_canvas->document().redo()) {
            m_dirty = true;
            m_canvas->invalidateComposedCache();
            m_canvas->update();
            refreshActions();
        }
    });
    m_redoAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+Z")),
                                QKeySequence(QStringLiteral("Ctrl+Y"))});

    QAction *deleteAction = toolbar->addAction(ui::iconDelete(), QStringLiteral("删除选中"), this,
                                               [this]() { m_canvas->deleteSelected(); });
    deleteAction->setShortcut(QKeySequence::Delete);

    toolbar->addSeparator();
    QAction *copyAction = toolbar->addAction(ui::iconCopy(), QStringLiteral("复制"), this,
                                             &EditorWindow::copyToClipboard);
    copyAction->setShortcut(QKeySequence::Copy);
    QAction *saveAction = toolbar->addAction(ui::iconSave(), QStringLiteral("保存"), this,
                                             &EditorWindow::save);
    saveAction->setShortcut(QKeySequence::Save);
    QAction *saveAsAction = toolbar->addAction(ui::iconSaveAs(), QStringLiteral("另存为"), this,
                                               &EditorWindow::saveAs);
    saveAsAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+S"))});

    toolbar->addSeparator();
    m_fitAction = toolbar->addAction(ui::icon({QStringLiteral("zoom-fit-best")}),
                                     QStringLiteral("适应窗口"), this,
                                     [this]() { m_canvas->zoomToFit(); });
    m_fitAction->setCheckable(true);
    m_fitAction->setChecked(true);
    m_fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    QAction *zoomOutAction = toolbar->addAction(ui::icon({QStringLiteral("zoom-out")}),
                                                QStringLiteral("缩小"), this,
                                                [this]() { m_canvas->zoomOut(); });
    zoomOutAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
    m_zoomLabel = new QLabel(this);
    m_zoomLabel->setMinimumWidth(56);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    toolbar->addWidget(m_zoomLabel);
    QAction *zoomInAction = toolbar->addAction(ui::icon({QStringLiteral("zoom-in")}),
                                               QStringLiteral("放大"), this,
                                               [this]() { m_canvas->zoomIn(); });
    zoomInAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+=")),
                                QKeySequence(QStringLiteral("Ctrl++"))});
    QAction *hundredAction = toolbar->addAction(QStringLiteral("100%"), this,
                                                [this]() { m_canvas->zoomTo100(); });
    hundredAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));

    m_status = new QLabel(this);
    m_status->setText(QStringLiteral("就绪"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setWidgetResizable(true);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->setFrameShape(QFrame::NoFrame);

    layout->addWidget(toolbar);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_status);

    m_canvas->setFocus();
}

void EditorWindow::setTool(ToolKind tool)
{
    m_canvas->setTool(tool);
    if (auto *button = m_toolButtons.value(int(tool)))
        button->setChecked(true);
    m_status->setText(tool == ToolKind::Select
                          ? QStringLiteral("选择：单击选中标注，拖动移动，拖角点改变大小，双击文字可改字")
                          : QStringLiteral("%1：在图上拖动绘制").arg(toolName(tool)));
}

void EditorWindow::chooseColor()
{
    const QColor picked = QColorDialog::getColor(m_canvas->color(), this,
                                                 QStringLiteral("选择标注颜色"));
    if (!picked.isValid())
        return;
    m_canvas->setColor(picked);
    m_colorButton->setIcon(swatchIcon(picked));
}

void EditorWindow::updateZoomUi()
{
    if (m_zoomLabel)
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_canvas->zoom() * 100)));
    if (m_fitAction)
        m_fitAction->setChecked(m_canvas->fitToWindow());
}

void EditorWindow::refreshActions()
{
    if (m_undoAction)
        m_undoAction->setEnabled(m_canvas->document().canUndo());
    if (m_redoAction)
        m_redoAction->setEnabled(m_canvas->document().canRedo());
    updateTitle();
}

void EditorWindow::updateTitle()
{
    const QImage image = m_canvas->image();
    setWindowTitle(QStringLiteral("SeaShot 编辑器 — %1×%2 标注 %3%4")
                       .arg(image.width())
                       .arg(image.height())
                       .arg(m_canvas->document().count())
                       .arg(m_dirty ? QStringLiteral(" *") : QString()));
}

void EditorWindow::copyToClipboard()
{
    QString error;
    const QImage rendered = m_canvas->renderedImage();
    if (Clipboard::copyImage(rendered, &error)) {
        m_status->setText(QStringLiteral("已复制 %1×%2 到剪贴板")
                              .arg(rendered.width())
                              .arg(rendered.height()));
        QTextStream(stdout) << "EDITOR_COPIED " << rendered.width() << "x" << rendered.height()
                            << "\n";
        emit copiedToClipboard();
    } else {
        m_status->setText(QStringLiteral("复制失败：%1").arg(error));
        QTextStream(stdout) << "EDITOR_COPY_ERROR " << error << "\n";
    }
}

QString EditorWindow::saveToDefaultPath()
{
    Settings settings;
    const QString path = m_savedPath.isEmpty() ? settings.defaultSavePath() : m_savedPath;
    QString error;
    const QImage rendered = m_canvas->renderedImage();
    if (!Settings::saveImage(path, rendered, &error)) {
        m_status->setText(QStringLiteral("保存失败：%1").arg(error));
        QTextStream(stdout) << "EDITOR_SAVE_ERROR " << error << "\n";
        return {};
    }
    m_savedPath = path;
    m_dirty = false;
    refreshActions();
    m_status->setText(QStringLiteral("已保存 %1").arg(QFileInfo(path).fileName()));
    QTextStream(stdout) << "EDITOR_SAVED " << path << " size=" << rendered.width() << "x"
                        << rendered.height() << "\n";
    emit savedTo(path);
    return path;
}

void EditorWindow::save()
{
    saveToDefaultPath();
}

void EditorWindow::saveAs()
{
    Settings settings;
    const QString suggested = m_savedPath.isEmpty() ? settings.defaultSavePath() : m_savedPath;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"), suggested,
                                                      QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;
    QString error;
    const QImage rendered = m_canvas->renderedImage();
    if (!Settings::saveImage(path, rendered, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return;
    }
    m_savedPath = path;
    m_dirty = false;
    refreshActions();
    m_status->setText(QStringLiteral("已保存 %1").arg(QFileInfo(path).fileName()));
    QTextStream(stdout) << "EDITOR_SAVED " << path << " size=" << rendered.width() << "x"
                        << rendered.height() << "\n";
    emit savedTo(path);
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    if (!m_dirty) {
        event->accept();
        return;
    }
    // 验收判据「退出未保存不留下半成品文件」：只在用户明确选择保存时才写盘
    const auto answer = QMessageBox::question(
        this, QStringLiteral("有未保存的标注"),
        QStringLiteral("当前标注尚未保存，要保存后再关闭吗？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (answer == QMessageBox::Save) {
        if (saveToDefaultPath().isEmpty()) {
            event->ignore();
            return;
        }
    }
    QTextStream(stdout) << "EDITOR_CLOSED dirty=" << (m_dirty ? 1 : 0) << " saved="
                        << (m_savedPath.isEmpty() ? QStringLiteral("(none)") : m_savedPath) << "\n";
    event->accept();
}

} // namespace seashot
