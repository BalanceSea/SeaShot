#include "scroll/ScrollCapturer.h"

#include "backend/CaptureBackend.h"
#include "record/RecordingIndicator.h"
#include "util/Log.h"

namespace seashot {

ScrollCapturer::ScrollCapturer(CaptureBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    connect(&m_timer, &QTimer::timeout, this, &ScrollCapturer::tick);
}

void ScrollCapturer::start(const Config &config)
{
    if (m_running)
        return;
    if (!m_backend) {
        emit failed(QStringLiteral("没有可用的截图后端"));
        return;
    }
    if (config.region.width() < 30 || config.region.height() < 30) {
        emit failed(QStringLiteral("区域太小（至少要 30x30）：%1x%2")
                        .arg(config.region.width())
                        .arg(config.region.height()));
        return;
    }
    m_config = config;
    m_stitcher = ScrollStitcher(config.stitcher);
    m_running = true;
    m_capturing = false;
    m_idle = 0;
    m_rejected = 0;
    m_frames = 0;
    m_lastFrame = QImage();
    m_report.clear();
    m_elapsed.start();

    if (m_indicator)
        m_indicator->showForScroll(m_config.region, QStringLiteral("自己滚动页面 · 完成/Esc 结束"));
    m_timer.start(qMax(80, m_config.intervalMs));
    SS_INFO() << "[scroll] 开始滚动截屏：区域" << m_config.region << "间隔" << m_config.intervalMs
              << "ms";
    tick(); // 立刻抓第一帧，别让用户等一个间隔
}

void ScrollCapturer::stop()
{
    if (m_running)
        finish(true, QStringLiteral("用户结束"));
}

void ScrollCapturer::tick()
{
    if (!m_running || m_capturing)
        return; // 上一帧还没回来：跳过这一拍（抓帧是异步的）
    if (m_elapsed.elapsed() > m_config.maxDurationMs) {
        finish(true, QStringLiteral("到时长上限"));
        return;
    }
    m_capturing = true;
    m_backend->captureRect(m_config.region, [this](const CaptureImage &image) {
        m_capturing = false;
        onCaptured(image);
    });
}

void ScrollCapturer::onCaptured(const CaptureImage &image)
{
    if (!m_running)
        return;
    if (!image.ok()) {
        finish(false, QStringLiteral("抓帧失败：%1").arg(image.error));
        return;
    }
    ++m_frames;
    if (!m_stitcher.started()) {
        m_stitcher.start(image.image);
        m_lastFrame = image.image;
    } else {
        const ScrollStitcher::AbsorbResult result = m_stitcher.absorb(image.image);
        switch (result.verdict) {
        case ScrollStitcher::Verdict::Absorbed:
            m_idle = 0;
            m_rejected = 0;
            break;
        case ScrollStitcher::Verdict::NoMovement:
        case ScrollStitcher::Verdict::ScrolledBack:
            ++m_idle;
            m_rejected = 0;
            break;
        case ScrollStitcher::Verdict::Rejected:
            ++m_rejected;
            break;
        case ScrollStitcher::Verdict::TooTall:
            finish(true, QStringLiteral("到高度上限"));
            return;
        }
    }

    const int height = m_stitcher.height();
    if (m_indicator)
        m_indicator->updateScrollStats(height, m_frames);
    emit progress(height, m_frames);

    if (m_idle >= m_config.idleFramesToStop) {
        finish(true, QStringLiteral("页面不再滚动"));
        return;
    }
    if (m_rejected >= m_config.rejectedFramesToStop) {
        finish(true, QStringLiteral("页面一直在动（有动画/视频？），只能拼到这里"));
        return;
    }
}

void ScrollCapturer::finish(bool ok, const QString &note)
{
    if (!m_running)
        return;
    m_running = false;
    m_timer.stop();
    if (m_indicator)
        m_indicator->hideIndicator();

    const QImage result = m_stitcher.image();
    m_report = QStringLiteral("%1；耗时 %2 ms；%3")
                   .arg(m_stitcher.report())
                   .arg(m_elapsed.elapsed())
                   .arg(note);
    if (!ok || result.isNull() || m_stitcher.frameCount() < 1) {
        SS_WARN() << "[scroll] 结束（失败）：" << m_report;
        emit failed(m_report);
        return;
    }
    SS_INFO() << "[scroll] 结束：" << m_report;
    emit finished(result, m_report);
}

} // namespace seashot
