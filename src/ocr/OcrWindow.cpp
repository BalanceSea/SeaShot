#include "ocr/OcrWindow.h"

#include "ui/Theme.h"
#include "util/Clipboard.h"
#include "util/Log.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextStream>
#include <QVBoxLayout>

namespace seashot {

OcrWindow::OcrWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("SeaShot · 文字识别"));
    setWindowIcon(ui::appIcon());
    resize(720, 480);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::spec::cardPadding, ui::spec::cardPadding,
                              ui::spec::cardPadding, ui::spec::cardPadding);
    layout->setSpacing(ui::spec::gap);

    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    m_editor = new QPlainTextEdit(this);
    m_editor->setReadOnly(true);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_editor, 1);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(ui::spec::gap);
    auto *copyAll = new QPushButton(QStringLiteral("复制全部"), this);
    copyAll->setIcon(ui::iconCopy());
    auto *copySelection = new QPushButton(QStringLiteral("复制选中"), this);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setIcon(ui::iconClose());
    buttons->addWidget(copyAll);
    buttons->addWidget(copySelection);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(copyAll, &QPushButton::clicked, this, &OcrWindow::copyAll);
    connect(copySelection, &QPushButton::clicked, this, &OcrWindow::copySelection);
    connect(close, &QPushButton::clicked, this, &OcrWindow::close);
}

void OcrWindow::setResult(const OcrResult &result)
{
    m_editor->setPlainText(result.ok ? result.text : result.error);
    m_status->setText(result.ok
        ? QStringLiteral("识别完成 · %1 · 共 %2 行").arg(result.summary()).arg(result.lines.size())
        : QStringLiteral("识别失败 · %1").arg(result.error));
    QTextStream(stdout) << "OCR_WINDOW ok=" << (result.ok ? 1 : 0) << " lines=" << result.lines.size()
                        << " elapsedMs=" << result.elapsedMs << "\n";
    QTextStream(stdout).flush();
}

QString OcrWindow::text() const
{
    return m_editor ? m_editor->toPlainText() : QString();
}

QString OcrWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void OcrWindow::copyAll()
{
    const QString text = m_editor->toPlainText();
    QString error;
    const bool ok = Clipboard::copyText(text, &error);
    QTextStream out(stdout);
    out << (ok ? "OCR_COPY ok chars=" : "OCR_COPY_ERROR ") << (ok ? QString::number(text.size()) : error)
        << "\n";
    out.flush();
    m_status->setText(ok ? QStringLiteral("已复制全部 %1 字").arg(text.size())
                         : QStringLiteral("复制失败：%1").arg(error));
}

void OcrWindow::copySelection()
{
    const QString selected = m_editor->textCursor().selectedText();
    if (selected.isEmpty()) {
        // 没选就退化成"复制光标所在行"（右键粘贴/逐行复制的常见预期）
        const QString line = m_editor->textCursor().block().text();
        QString error;
        const bool ok = Clipboard::copyText(line, &error);
        QTextStream out(stdout);
        out << (ok ? "OCR_COPY_LINE ok chars=" : "OCR_COPY_LINE_ERROR ")
            << (ok ? QString::number(line.size()) : error) << "\n";
        out.flush();
        m_status->setText(ok ? QStringLiteral("已复制当前行（%1 字）").arg(line.size())
                             : QStringLiteral("复制失败：%1").arg(error));
        return;
    }
    QString error;
    const bool ok = Clipboard::copyText(selected, &error);
    QTextStream out(stdout);
    out << (ok ? "OCR_COPY_SELECTION ok chars=" : "OCR_COPY_SELECTION_ERROR ")
        << (ok ? QString::number(selected.size()) : error) << "\n";
    out.flush();
    m_status->setText(ok ? QStringLiteral("已复制选中的 %1 字").arg(selected.size())
                         : QStringLiteral("复制失败：%1").arg(error));
}

void OcrWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_C
        && m_editor->textCursor().hasSelection()) {
        copySelection();
        return;
    }
    QWidget::keyPressEvent(event);
}

void OcrWindow::closeEvent(QCloseEvent *event)
{
    emit closed();
    QWidget::closeEvent(event);
}

} // namespace seashot
