// 滚动截屏的取帧循环（Phase D）
//
// 与录制的分工不同：这里**不注入滚动**，让用户自己滚（鼠标滚轮/触控板/拖滚动条都行），
// 我们定时抓取选区、交给 ScrollStitcher 拼接。这样不需要合成输入事件、也不依赖具体应用。
//
// 结束条件（**由用户决定**，这是 2026-10-17 按用户反馈改的）：
//   · 用户点指示器上的「完成」/按 Esc   ← 主要的结束方式
//   · 长图高度到上限（20000px）
//   · 兜底时长（30 分钟，只为不无限占内存）
//   · 抓帧失败
// **不会**再因为"连续几帧没滚动"就自动结束 —— 那会让用户还没开始滚就收工（实测反馈）。
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
        /// ⚠️ 0 = **不因为"没滚动"而结束**。
        /// 用户反馈："开启长截图后只出现一会就结束" —— 根因就是这里：
        /// 连续 4 帧（≈1 秒）没检测到滚动就收工，而用户常常还没开始滚。
        /// 现在默认 0：由用户自己按「完成」/Esc 结束。
        int idleFramesToStop = 0;
        /// 0 = 不因为"一直对不上"而结束（页面有动画时只是跳过那几帧）。
        int rejectedFramesToStop = 0;
        /// 兜底时长（默认 30 分钟，只是为了不无限占内存；正常都是用户自己结束）
        int maxDurationMs = 30 * 60 * 1000;
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
    friend class TestApplication;
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
