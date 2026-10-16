// 截图结果浮层
//
// 行为约定（按验收反馈定稿）：
//  - **原尺寸显示**：卡片 == 截图尺寸（1:1）；屏幕可用区放不下才等比缩小并注明
//  - **贴着截图框左下角**：首选卡片左上角 = 选区左下角；下方放不下则左下角对齐往上盖
//  - **描边**：外圈 1px 暗描边 + 内圈 2px 高亮色边框（截图内容不再"溶进"桌面背景）
//  - **拖动**：坐标更新按 8ms 合并提交（layer-shell 的定位是每次 setMargins 一次 commit，
//    逐事件提交会卡）；卡片内容缓存成 QPixmap，拖动时不再重画整张截图
//  - **消失时机**：默认**不自动消失**，只在用户操作后消失（Enter/Esc、另存为完成、编辑、
//    关闭）。`ui/toastAutoHideMs` 可设一个兜底超时（0 = 不自动消失）
#pragma once

#include <QImage>
#include <QPixmap>
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

    void setEditEnabled(bool enabled);
    /// 0 = 不自动消失（默认）
    void setAutoHideMs(int milliseconds);
    /// 更新状态文字（复制/保存完成后调用）
    void updateStatus(const QString &message, const QString &savedPath, const QString &copyError);
    /// 短暂展示后消失（例如另存为完成）
    void dismissAfter(int milliseconds);

    /// 诊断用：浮层边框改成高饱和品红（便于截屏定位实测拖动延迟）
    void setDragMarkerEnabled(bool enabled);
    /// 诊断用：直接走拖动时的定位路径（不经过鼠标事件）
    void dragTo(const QPoint &globalTopLeft) { applyPosition(globalTopLeft); }
    QRect currentGeometry() const { return geometry(); }

signals:
    void editRequested();
    void saveAsRequested();
    void copyRequested();
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Button { None, Edit, SaveAs, Copy, Close };

    int barHeight() const;
    QRect barRect() const;
    QVector<QPair<Button, QRect>> buttons() const;
    Button buttonAt(const QPoint &position) const;
    void rebuildCache();
    void invalidateCache();
    void reposition(const QRect &anchorGlobalRect);
    void applyPosition(const QPoint &globalTopLeft);
    void ensureLayerShell();
    void closeAndNotify();
    void maybeStartAutoHide();

    QImage m_source;
    QImage m_display;
    bool m_scaledToFit = false;
    QString m_savedPath;
    QString m_message;
    QString m_copyError;

    QPixmap m_cache;      ///< 卡片内容缓存（拖动时只做一次 drawPixmap）
    bool m_cacheDirty = true;

    QTimer m_autoHide;
    /// 上一次实际提交过的位置（重复位置不再提交）
    QPoint m_lastAppliedTopLeft{-1, -1};

    bool m_editEnabled = true;
    bool m_layerShellApplied = false;
    bool m_dragging = false;
    bool m_dragMarker = false;
    QPoint m_dragOrigin;
    QRect m_lastAnchor;
    int m_autoHideMs = 0;
};

} // namespace seashot
