// 录制器：PipeWire 或截图轮询取帧；按实际经过时间补帧，保持导出时长。
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

    /// 帧来源：PipeWire 路径用它替换"后端轮询截图"。同步返回"当前最新一帧"，
    /// 返回空图表示还没出帧（算丢帧）。
    using FrameProvider = std::function<QImage()>;
    /// 推送式帧源（PipeWire）的录制入口：
    /// 与 start() 的区别只有"帧从哪来"与"首帧已知尺寸"，其余逻辑（编码/统计/停止）完全共用。
    bool startFromProvider(FrameProvider provider, const QSize &frameSize,
                           const QRectF &logicalRect, int fps, QString *error);
    void stop();  ///< 停止采集 → 异步编码 → finished
    void abort(); ///< 丢弃当前录制

    bool isRecording() const { return m_recording; }
    bool isFinishing() const { return m_finishing; }
    Stats stats() const { return m_stats; }
    QRectF region() const { return m_region; }
    int fps() const { return m_fps; }
    QString outputPath() const { return m_outputPath; }
    void setOutputPath(const QString &path) { m_outputPath = path; }
    /// 延迟导出：停止录制时**只收尾中间文件、不转码**，把格式选择留给用户。
    /// 之后用 exportTo(path) 按扩展名导出（.gif / .mp4），导出完中间文件自动清掉。
    void setDeferExport(bool defer) { m_deferExport = defer; }
    /// 中间格式（无损 / MP4），由配置决定；须在 start*/ 之前设置
    void setLossless(bool lossless) { m_lossless = lossless; }
    bool deferExport() const { return m_deferExport; }
    /// 把已保留的无损中间文件导出成 path（按扩展名定格式）。仅延迟导出模式下有意义。
    bool exportTo(const QString &path, QString *error);
    bool hasPendingExport() const;
    QImage lastFrame() const { return m_lastFrame; }

    friend class TestGifRecorder;

signals:
    /// 临时空间不足，已提前停止采集（参数是当时的可用字节数）
    void stoppedForSpace(qint64 availableBytes);
    void started(const QRectF &region, int fps);
    void progress();
    void finished(const QString &gifPath, const seashot::GifRecorder::Stats &stats,
                  const QString &error);

private:
    void tick();
    bool beginRecording(const QImage &firstFrame, QString *error);
    bool writeFrameAtTime(const QImage &frame, qint64 elapsedMs, QString *error);
    void onFrame(const CaptureImage &image);
    void finishEncoding();

    CaptureBackend *m_backend = nullptr;
    FrameProvider m_frameProvider; ///< 非空则走推送式帧源
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
    bool m_deferExport = false;
    bool m_lossless = true;
    qint64 m_lastSpaceCheckMs = 0;
    quint64 m_sessionId = 0;
    int m_fps = 15;
};

} // namespace seashot

// 让 QSignalSpy / 队列连接能携带 Stats
Q_DECLARE_METATYPE(seashot::GifRecorder::Stats)
