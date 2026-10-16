#include "record/GifRecorder.h"

#include "backend/CaptureBackend.h"
#include "record/GifEncoder.h"
#include "util/Log.h"

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
    emit progress();
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
