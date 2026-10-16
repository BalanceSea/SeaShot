// 录制器：按帧率轮询后端截图并把帧喂给 GifEncoder。
//
// 为什么用"轮询截图"而不是 portal ScreenCast：
//  - Phase 0 已实测 KWin 6.7 **没有**录制 D-Bus 接口；portal 路线需要 PipeWire 客户端
//    消费（libpipewire/gst 具备，但要引入新依赖与流线程/协商代码，且每次会话要用户授权）；
//  - PLAN 的 Phase 4 要求"帧采集由 SeaShot 自己完成"，轮询 `CaptureBackend::captureRect()`
//    正好落在既有链路上：Wayland 走 ScreenShot2、X11 走 MIT-SHM，两条后端一套代码；
//  - 代价是时序不完美（非严格等间隔）、CPU 更高。验收里会给出实测的丢帧率与帧率误差。
// portal ScreenCast（更平滑/更高帧率）留作后续增强，见 docs/known-issues.md。
#pragma once

#include "core/Types.h"

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTimer>

#include <memory>

namespace seashot {

class CaptureBackend;
class GifEncoder;

class GifRecorder : public QObject
{
    Q_OBJECT

public:
    struct Stats {
        int requested = 0; ///< 定时器触发的次数
        int captured = 0;  ///< 真正写进编码器的帧
        int dropped = 0;   ///< 上一帧还没回来（或帧尺寸变化）而跳过的次数
        int failed = 0;    ///< 截图失败的次数
        qint64 elapsedMs = 0;

        double dropRate() const
        {
            return requested > 0 ? double(dropped) / double(requested) : 0.0;
        }
    };

    explicit GifRecorder(QObject *parent = nullptr);
    ~GifRecorder() override;

    bool start(CaptureBackend *backend, const QRectF &logicalRect, int fps, QString *error);
    void stop();  ///< 停止采集 → 异步编码 → finished
    void abort(); ///< 丢弃当前录制

    bool isRecording() const { return m_recording; }
    bool isFinishing() const { return m_finishing; }
    Stats stats() const { return m_stats; }
    QRectF region() const { return m_region; }
    int fps() const { return m_fps; }
    QString outputPath() const { return m_outputPath; }
    void setOutputPath(const QString &path) { m_outputPath = path; }
    QImage lastFrame() const { return m_lastFrame; }

    friend class TestGifRecorder;

signals:
    void started(const QRectF &region, int fps);
    void progress();
    void finished(const QString &gifPath, const seashot::GifRecorder::Stats &stats,
                  const QString &error);

private:
    void tick();
    void onFrame(const CaptureImage &image);
    void finishEncoding();

    CaptureBackend *m_backend = nullptr;
    QTimer m_timer;
    std::unique_ptr<GifEncoder> m_encoder;
    QRectF m_region;
    QString m_outputPath;
    Stats m_stats;
    QElapsedTimer m_elapsed;
    QSize m_frameSize;
    QImage m_lastFrame;
    bool m_recording = false;
    bool m_inFlight = false;
    bool m_finishing = false;
    int m_fps = 15;
};

} // namespace seashot

// 让 QSignalSpy / 队列连接能携带 Stats
Q_DECLARE_METATYPE(seashot::GifRecorder::Stats)
