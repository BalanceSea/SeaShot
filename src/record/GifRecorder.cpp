#include "record/GifRecorder.h"

#include "backend/CaptureBackend.h"
#include "record/GifEncoder.h"
#include "util/Log.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

namespace seashot {

GifRecorder::GifRecorder(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &GifRecorder::tick);
}

GifRecorder::~GifRecorder()
{
    abort();
}

bool GifRecorder::startFromProvider(FrameProvider provider, const QSize &frameSize,
                                  const QRectF &logicalRect, int fps, QString *error)
{
    if (m_recording) {
        if (error)
            *error = QStringLiteral("已经在录制中");
        return false;
    }
    if (!provider) {
        if (error)
            *error = QStringLiteral("没有可用的帧来源");
        return false;
    }
    if (frameSize.width() < 2 || frameSize.height() < 2) {
        if (error)
            *error = QStringLiteral("帧尺寸无效");
        return false;
    }

    m_backend = nullptr;
    m_frameProvider = std::move(provider);
    m_region = logicalRect;
    m_fps = qBound(1, fps, 60);
    m_stats = Stats();
    m_frameSize = frameSize;
    m_lastFrame = QImage();
    m_inFlight = false;
    m_finishing = false;

    // 等第一帧（流刚从 portal 起来时可能有几十毫秒延迟）
    QImage first;
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        first = m_frameProvider();
        if (!first.isNull())
            break;
        QThread::msleep(10);
    }
    if (first.isNull()) {
        m_frameProvider = nullptr;
        if (error)
            *error = QStringLiteral("等待 PipeWire 首帧超时");
        return false;
    }
    if (!beginRecording(first, error)) {
        m_frameProvider = nullptr;
        return false;
    }
    SS_INFO() << "[record] 开始录制（PipeWire 帧源）" << m_region << m_fps << "fps 帧尺寸"
              << m_frameSize;
    return true;
}

bool GifRecorder::beginRecording(const QImage &firstFrame, QString *error)
{
    m_encoder = std::make_unique<GifEncoder>();
    if (!m_encoder->start(m_frameSize, m_fps, error)) {
        m_encoder.reset();
        return false;
    }
    if (!m_encoder->writeFrame(firstFrame, error)) {
        m_encoder->abort();
        m_encoder.reset();
        return false;
    }
    m_stats.requested = 1;
    m_stats.captured = 1;
    m_lastFrame = firstFrame;
    m_elapsed.start();
    m_recording = true;
    m_timer.start(1000 / m_fps);
    emit started(m_region, m_fps);
    return true;
}

bool GifRecorder::start(CaptureBackend *backend, const QRectF &logicalRect, int fps, QString *error)
{
    if (m_recording) {
        if (error)
            *error = QStringLiteral("已经在录制中");
        return false;
    }
    if (!backend) {
        if (error)
            *error = QStringLiteral("没有可用的截图后端");
        return false;
    }
    if (logicalRect.width() < 2 || logicalRect.height() < 2) {
        if (error)
            *error = QStringLiteral("录制区域太小");
        return false;
    }

    m_backend = backend;
    m_region = logicalRect;
    m_fps = qBound(1, fps, 60);
    m_stats = Stats();
    m_frameSize = QSize();
    m_lastFrame = QImage();
    m_inFlight = false;
    m_finishing = false;

    // 先用一帧确定尺寸（同时也验证这一块真的截得到）
    CaptureImage first;
    bool got = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    backend->captureRect(m_region, [&](const CaptureImage &image) {
        first = image;
        got = true;
        loop.quit();
    });
    // 回调可能是同步的（例如测试用的假后端）：已拿到就不必再进事件循环
    if (!got) {
        timeout.start(15000);
        loop.exec();
    }
    if (!got || !first.ok()) {        if (error)
            *error = first.error.isEmpty() ? QStringLiteral("首帧截图超时") : first.error;
        return false;
    }

    m_frameSize = first.image.size();
    m_encoder = std::make_unique<GifEncoder>();
    if (!m_encoder->start(m_frameSize, m_fps, error)) {
        m_encoder.reset();
        return false;
    }
    if (!m_encoder->writeFrame(first.image, error)) {
        m_encoder->abort();
        m_encoder.reset();
        return false;
    }

    m_stats.requested = 1;
    m_stats.captured = 1;
    m_lastFrame = first.image;
    m_elapsed.start();
    m_recording = true;
    m_timer.start(1000 / m_fps);

    SS_INFO() << "[record] 开始录制" << m_region << m_fps << "fps 帧尺寸" << m_frameSize
              << "输出" << (m_outputPath.isEmpty() ? QStringLiteral("(未指定)") : m_outputPath);
    emit started(m_region, m_fps);
    return true;
}

void GifRecorder::tick()
{
    if (!m_recording)
        return;
    ++m_stats.requested;
    if (m_inFlight) {
        ++m_stats.dropped; // 上一帧还没回来：宁可丢帧也不排队堆积
        emit progress();
        return;
    }
    m_inFlight = true;
    if (m_frameProvider) {
        // PipeWire：帧由流推送，这里按自己的帧率拉"最新一帧"。
        // 同步返回，所以直接把结果喂给 onFrame —— 后面的统计/编码逻辑一行都不用改。
        CaptureImage frame;
        frame.image = m_frameProvider();
        frame.scale = 1.0;
        if (frame.image.isNull())
            frame.error = QStringLiteral("PipeWire 尚未出帧");
        onFrame(frame);
        return;
    }
    m_backend->captureRect(m_region, [this](const CaptureImage &image) { onFrame(image); });
}

void GifRecorder::onFrame(const CaptureImage &image)
{
    m_inFlight = false;
    if (!m_recording || !m_encoder)
        return;

    if (!image.ok()) {
        ++m_stats.failed;
        SS_WARN() << "[record] 截图失败：" << image.error;
        emit progress();
        return;
    }
    if (m_frameSize.isEmpty())
        m_frameSize = image.image.size();
    if (image.image.size() != m_frameSize) {
        ++m_stats.dropped; // 尺寸变了（缩放/输出变化）→ 丢掉，避免编码器错帧
        emit progress();
        return;
    }

    QString error;
    if (!m_encoder->writeFrame(image.image, &error)) {
        ++m_stats.failed;
        SS_WARN() << "[record] 写帧失败：" << error;
        emit progress();
        return;
    }
    ++m_stats.captured;
    m_lastFrame = image.image;
    // ⚠️ 必须每帧同步用时：指示器读的是 stats.elapsedMs，而这个字段原先只在 stop()
    //    里赋值一次 → 录制期间恒为 0 → 屏幕上秒数永远停在 00:00，只有帧数在动。
    m_stats.elapsedMs = m_elapsed.elapsed();
    emit progress();
}

bool GifRecorder::exportTo(const QString &path, QString *error)
{
    if (!m_encoder) {
        if (error)
            *error = QStringLiteral("没有可导出的录制（中间文件已释放）");
        return false;
    }
    const QString intermediate = m_encoder->intermediatePath();
    if (intermediate.isEmpty() || !QFile::exists(intermediate)) {
        if (error)
            *error = QStringLiteral("找不到中间文件：%1").arg(intermediate);
        return false;
    }
    if (!GifEncoder::transcode(intermediate, path, m_fps, error))
        return false;
    // 导出一个格式后中间文件就清掉：要两种格式应当一次问清（避免长期占用磁盘）
    m_encoder.reset();
    SS_INFO() << "[record] 已导出" << path;
    return true;
}

void GifRecorder::stop()
{
    if (!m_recording)
        return;
    m_timer.stop();
    m_recording = false;
    m_finishing = true;
    m_stats.elapsedMs = m_elapsed.elapsed();
    SS_INFO() << "[record] 停止采集：请求" << m_stats.requested << "帧 / 采到" << m_stats.captured
              << "/ 丢" << m_stats.dropped << "/ 失败" << m_stats.failed << "用时"
              << m_stats.elapsedMs << "ms";
    // 编码可能要几秒：用 0ms 定时器让 UI 先刷新（不阻塞事件循环）
    QTimer::singleShot(0, this, &GifRecorder::finishEncoding);
}

void GifRecorder::finishEncoding()
{
    QString error;
    const QString gifPath = m_outputPath;

    if (m_deferExport) {
        // 只把中间文件收尾（仍然是无损的），转码留到用户选好格式之后。
        // 收尾方式：让 ffmpeg 正常关闭写通道并退出，但不做第二遍转码 ——
        // 复用 GifEncoder 的"第一遍"收尾逻辑，把输出路径留空来跳过转码。
        const bool closed = m_encoder && m_encoder->closeIntermediate(&error);
        m_finishing = false;
        if (!closed) {
            SS_WARN() << "[record] 中间文件收尾失败：" << error;
            emit finished(QString(), m_stats, error);
            return;
        }
        SS_INFO() << "[record] 已保留无损中间文件，等用户选导出格式";
        emit finished(QString(), m_stats, QString());
        return;
    }

    const bool ok = m_encoder && m_encoder->finish(gifPath, &error);
    m_encoder.reset();
    m_finishing = false;

    Stats stats = m_stats;
    if (!ok) {
        SS_WARN() << "[record] 编码失败：" << error;
        emit finished(QString(), stats, error);
        return;
    }
    emit finished(gifPath, stats, QString());
}

void GifRecorder::abort()
{
    m_timer.stop();
    m_recording = false;
    m_finishing = false;
    if (m_encoder) {
        m_encoder->abort();
        m_encoder.reset();
    }
}

} // namespace seashot
