// OCR 结果面板（Phase E）：显示识别出来的文字，可整套复制，也可只复制选中的行。
//
// 用**普通窗口**（和编辑器一致）而不是 layer-shell：它是"结果"，不是覆盖层上的交互，
// 用户可能要边看边复制、甚至并排打开别的窗口。
#pragma once

#include "ocr/OcrEngine.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;

namespace seashot {

class OcrWindow : public QWidget
{
    Q_OBJECT

public:
    explicit OcrWindow(QWidget *parent = nullptr);

    void setResult(const OcrResult &result);
    /// 诊断/验收：直接取当前文本/状态
    QString text() const;
    QString statusText() const;
    void copyAll();
    void copySelection();

signals:
    void closed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace seashot
