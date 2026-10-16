// 通过 portal 给的 PipeWire fd 取屏幕帧（libpipewire 直连，不经过 GStreamer）。
//
// 为什么是"最新帧 + 拉取"而不是回调推送：
//   GifRecorder 现有逻辑是「按自己的帧率取帧 → 喂编码器」，时序与丢帧统计都在它那边。
//   这里只负责把**已裁剪好的最新一帧**放在那里，录制器按 15fps 去拉即可 ——
//   录制侧的时序逻辑一行都不用改，而每帧的成本从"KWin 渲染一次截图"降为"读一帧已有的缓冲"。
//
// 线程模型：libpipewire 的主循环跑在独立线程；取到帧后直接在锁里写 m_latest（深拷贝），
// 不跨线程发信号，避免队列堆积。
//
// 依赖是**可选的**：没装 libpipewire-0.3 时本类仍可编译，只是 start() 明确失败并说明原因，
// 上层据此回退到 ScreenShot2 轮询。
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QRectF>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>


namespace seashot {

class PipeWireFrameSource : public QObject
{
    Q_OBJECT

public:
    explicit PipeWireFrameSource(QObject *parent = nullptr);
    ~PipeWireFrameSource() override;

    /// fd 来自 PortalScreencast::openPipeWireRemote()，nodeId 来自 portal 应答的 streams。
    /// monitorLogicalRect = 该输出在**逻辑坐标**下的矩形（用来把整屏帧裁到 region）。
    /// region 也是逻辑坐标，裁剪结果按流的分辨率取（与 ScreenShot2 路径的约定一致）。
    bool start(int fd, uint nodeId, const QRectF &monitorLogicalRect, const QRectF &region,
               QString *error);
    void stop();

    /// 目标帧率：流是按显示器刷新率推帧的（实测 ≈116fps），
    /// 不做限制就会每帧都裁剪+转换（960x540 约 230MB/s 拷贝）。
    /// 设了之后超出的帧**直接丢弃、不转换**（零成本），录制器按自己的帧率取到的仍是最新一帧。
    /// 0 = 不限（诊断用）。
    void setTargetFps(int fps) { m_targetFps = qMax(0, fps); }

    /// 当前是否在取帧
    bool isRunning() const { return m_running; }
    /// 最近一帧（已裁到 region）。还没帧时返回空图。
    QImage latestFrame() const;
    /// 收到的帧总数（含拉取方没来得及取的）
    quint64 receivedFrames() const { return m_received.load(); }
    /// 协商出来的格式摘要（诊断用）
    QString negotiatedFormat() const;
    /// 出错信息（流错误、格式不支持等）
    QString error() const;

    /// 编译期是否带上了 libpipewire（"能不能用"与"依赖在不在"是两件事）
    static bool isAvailable();

private:
    friend struct PipeWireFrameSourceAccess;
    void handleProcess();
    void handleFormat(void *param);
    void handleState(int state, const char *message);
    void storeFrame(const QImage &frame);
    void setError(const QString &message);

    struct Impl; ///< 藏 libpipewire 类型，头文件不引入 pipewire 头
    std::unique_ptr<Impl> m_impl;

    mutable QMutex m_mutex;
    QImage m_latest;
    std::atomic<quint64> m_received{0};
    std::thread m_thread;
    QString m_error;
    QString m_format;
    QRectF m_monitor;
    QRectF m_region;
    bool m_running = false;
    int m_targetFps = 0;
    qint64 m_lastStoredMs = -1;
    QElapsedTimer m_clock;
};

} // namespace seashot
