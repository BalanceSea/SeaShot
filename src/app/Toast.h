// 截图结果浮层（右下角）：缩略图 + 「编辑 / 另存为 / 复制 / 关闭」
// 自己也在 KWin 的「排除出截图」名单里（appId 含 seashot），不会被下一次截图拍到。
#pragma once

#include <QImage>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace seashot {

class Toast : public QWidget
{
    Q_OBJECT

public:
    explicit Toast(QWidget *parent = nullptr);

    void showResult(const QImage &image, const QString &savedPath, const QString &message,
                    const QString &copyError = QString());
    void setEditEnabled(bool enabled) { m_editEnabled = enabled; }

signals:
    void editRequested();
    void saveAsRequested();
    void copyRequested();
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    enum class Button { None, Edit, SaveAs, Copy, Close };
    Button buttonAt(const QPoint &position) const;
    void layoutButtons();
    void repositionToCorner();

    QImage m_image;
    QString m_savedPath;
    QString m_message;
    QString m_copyError;
    QVector<QPair<Button, QRect>> m_buttons;
    QTimer m_hideTimer;
    bool m_editEnabled = false;
};

} // namespace seashot
