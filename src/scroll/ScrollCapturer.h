// 滚动截屏的取帧循环（Phase D）
//
// 与录制的分工不同：这里**不注入滚动**，让用户自己滚（鼠标滚轮/触控板/拖滚动条都行），
// 我们定时抓取选区、交给 ScrollStitcher 拼接。这样不需要合成输入事件、也不依赖具体应用。
//
// 结束条件（任一）：
//   · 连续 N 帧"没滚动"       → 用户停下了 / 到底了（正常结束）
//   · 连续 N 帧被拒绝         → 页面一直在动（视频/动画），拼不出干净结果（带说明结束）
//   · 长图高度到上限 / 时长到上限
//   · 用户点指示器上的「完成」/按 Esc
//   · 抓帧失败
#pragma once

#include "core/Types.h"
#include "scroll/ScrollStitcher.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>

namespace seashot {

class CaptureBackend;
class RecordingIndicator;

class ScrollCapturer : public QObject
{
    Q_OBJECT

public:
    struct Config {
        QRectF region;                 ///< 抓取区域（逻辑坐标）
        int intervalMs = 250;          ///< 抓帧间隔
        int idleFramesToStop = 4;      ///< 连续这么多帧没滚动就收工
        int rejectedFramesToStop = 10; ///< 连续这么多帧对不上（页面一直在动）
        int maxDurationMs = 120000;    ///< 兜底时长
        ScrollStitcher::Config stitcher;
    };

    explicit ScrollCapturer(CaptureBackend *backend, QObject *parent = nullptr);

    void setIndicator(RecordingIndicator *indicator) { m_indicator = indicator; }
    void start(const Config &config);
    /// 用户主动结束：把已经拼好的部分交付出去
    void stop();
    bool isRunning() const { return m_running; }
    /// 诊断/验收用
    QString report() const { return m_report; }

signals:
    void progress(int heightPx, int frames);
    void finished(const QImage &image, const QString &report);
    void failed(const QString &error);

private:
    void tick();
    void onCaptured(const CaptureImage &image);
    void finish(bool ok, const QString &note);

    CaptureBackend *m_backend = nullptr;
    QPointer<RecordingIndicator> m_indicator;
    Config m_config;
    ScrollStitcher m_stitcher;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    QImage m_lastFrame;
    bool m_running = false;
    bool m_capturing = false;
    int m_idle = 0;
    int m_rejected = 0;
    int m_frames = 0;
    QString m_report;
};

} // namespace seashot
