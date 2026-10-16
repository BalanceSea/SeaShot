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

    setTool(ToolKind::Rectangle);
    refreshActions();
    updateTitle();
    resize(qMin(1280, qMax(900, image.width() + 80)), qMin(860, qMax(600, image.height() + 140)));
}

void EditorWindow::buildUi()
{
    auto *toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    const QVector<ToolKind> tools{ToolKind::Select, ToolKind::Arrow, ToolKind::Rectangle,
                                  ToolKind::Ellipse, ToolKind::Pen, ToolKind::Text};
    int shortcut = 1;
    for (ToolKind tool : tools) {
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
    m_colorButton = new QToolButton(toolbar);
    m_colorButton->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
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
            m_canvas->update();
            refreshActions();
        }
    });
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = toolbar->addAction(ui::iconRedo(), QStringLiteral("重做"), this, [this]() {
        if (m_canvas->document().redo()) {
            m_dirty = true;
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

    m_status = new QLabel(this);
    m_status->setText(QStringLiteral("就绪"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(m_canvas, 1);
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
