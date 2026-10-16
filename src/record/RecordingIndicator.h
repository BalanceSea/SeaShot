// 录制指示器：贴在录制区域左上角外面的小条（REC + 计时 + 帧数 + 停止按钮）
//
// 放在区域**外面**（上方，没空间就下方）：这样即使在没有自排除的后端（X11）上，
// 它也不会被录进 GIF。用 layer-shell，保证在全屏应用前面也能看到/点到。
#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QWidget>

namespace seashot {

class RecordingIndicator : public QWidget
{
    Q_OBJECT

public:
    explicit RecordingIndicator(QWidget *parent = nullptr);

    void showFor(const QRectF &region, int fps, const QString &outputPath);
    void updateStats(int captured, int dropped, qint64 elapsedMs);
    void hideIndicator();

signals:
    void stopRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QRect stopButtonRect() const;
    void ensureLayerShell();
    void applyPosition(const QPoint &globalTopLeft);
    void rebuildCache();
    void invalidateCache();
    void reposition(const QRectF &region);

    QRectF m_region;
    QString m_outputPath;
    int m_fps = 15;
    int m_captured = 0;
    int m_dropped = 0;
    qint64 m_elapsedMs = 0;
    bool m_dotOn = false;
    bool m_layerShellApplied = false;
    QPixmap m_cache;
    bool m_cacheDirty = true;
    QTimer *m_blink = nullptr;
};

} // namespace seashot
