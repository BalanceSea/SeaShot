// 截图结果浮层
//
// Phase 2 后按你的要求改过：
//  - **按原尺寸显示**（截图多大卡片就多大；屏幕放不下时才等比缩小，并在文字里说明）
//  - **贴着截图框的左下角**：首选挂在选区下方（卡片左上角 = 选区左下角），
//    下方放不下就改成卡片的左下角与选区左下角对齐（往上盖）
//  - 外观跟随系统主题（用 QPalette 取色，Breeze 亮/暗主题都跟着走）
//  - 按钮条浮在图片底部，卡片尺寸 == 图片尺寸（不再有额外留白）
#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace seashot {

class Toast : public QWidget
{
    Q_OBJECT

public:
    explicit Toast(QWidget *parent = nullptr);

    /// anchor 是截图源在屏幕上的矩形（窗口 frameGeometry 或选区矩形，全局坐标）
    void showResult(const QImage &image, const QString &savedPath, const QString &message,
                    const QString &copyError, const QRect &anchorGlobalRect);
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
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Button { None, Edit, SaveAs, Copy, Close };

    int barHeight() const;
    QRect barRect() const;
    QRect buttonRect(Button button) const;
    Button buttonAt(const QPoint &position) const;
    QVector<QPair<Button, QRect>> buttons() const;
    void reposition(const QRect &anchorGlobalRect);

    QImage m_source;   ///< 原图（用于显示尺寸说明）
    QImage m_display;  ///< 实际显示的图（1:1 或等比缩小）
    bool m_scaledToFit = false;
    bool m_layerShellApplied = false;
    void ensureLayerShell();
    void applyPosition(const QPoint &globalTopLeft);
    QString m_savedPath;
    QString m_message;
    QString m_copyError;
    QTimer m_hideTimer;
    bool m_editEnabled = true;
    bool m_dragging = false;
    QPoint m_dragOrigin;   ///< 拖动时鼠标相对窗口左上角的偏移
    QRect m_lastAnchor;
};

} // namespace seashot
