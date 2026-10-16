#include "record/PipeWireFrameSource.h"

#include "util/Log.h"

#include <QElapsedTimer>

#ifdef SEASHOT_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#endif

namespace seashot {

#ifdef SEASHOT_HAVE_PIPEWIRE
namespace {

/// QImage 能直接映射的 SPA 像素格式（按内存字节序）
QImage::Format spaToQImageFormat(spa_video_format format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return QImage::Format_RGB32;   // 内存 B,G,R,X
    case SPA_VIDEO_FORMAT_BGRA: return QImage::Format_ARGB32;  // 内存 B,G,R,A
    case SPA_VIDEO_FORMAT_RGBx: return QImage::Format_RGBA8888; // 内存 R,G,B,X
    case SPA_VIDEO_FORMAT_RGBA: return QImage::Format_RGBA8888;
    default: return QImage::Format_Invalid;
    }
}

const char *spaFormatName(spa_video_format format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    default: return "其它";
    }
}

} // namespace
#endif

/// 让 C 回调能拿到私有实现的桥。
/// ⚠️ 回调必须写成**这个结构的静态成员**：friend 只对它的成员生效，
///    写成匿名命名空间里的自由函数会报“handleProcess is private”。
struct PipeWireFrameSourceAccess {
    static PipeWireFrameSource *self(void *data) { return static_cast<PipeWireFrameSource *>(data); }
    static void process(void *data) { self(data)->handleProcess(); }
    static void paramChanged(void *data, uint32_t id, const spa_pod *param);
    static void stateChanged(void *data, int state, const char *error)
    {
        self(data)->handleState(state, error);
    }
};

struct PipeWireFrameSource::Impl
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    pw_main_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook streamHook{};
    spa_video_info_raw video{};
    bool haveFormat = false;
    std::atomic<bool> failed{false};
#else
    int placeholder = 0;
#endif
};

#ifdef SEASHOT_HAVE_PIPEWIRE
namespace {

void onStreamProcess(void *data)
{
    PipeWireFrameSourceAccess::process(data);
}

void onStreamParamChanged(void *data, uint32_t id, const spa_pod *param)
{
    if (!param || id != SPA_PARAM_Format)
        return;
    PipeWireFrameSourceAccess::paramChanged(data, id, param);
}

void onStreamStateChanged(void *data, pw_stream_state, pw_stream_state state, const char *error)
{
    PipeWireFrameSourceAccess::stateChanged(data, int(state), error);
}

} // namespace

// 定义在类作用域之外也是成员，所以能访问私有成员
void PipeWireFrameSourceAccess::paramChanged(void *data, uint32_t id, const spa_pod *param)
{
    Q_UNUSED(id)
    self(data)->handleFormat(const_cast<spa_pod *>(param));
}
#endif

PipeWireFrameSource::PipeWireFrameSource(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

PipeWireFrameSource::~PipeWireFrameSource()
{
    stop();
}

bool PipeWireFrameSource::isAvailable()
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    return true;
#else
    return false;
#endif
}

bool PipeWireFrameSource::start(int fd, uint nodeId, const QRectF &monitorLogicalRect,
                                const QRectF &region, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        stop();
        return false;
    };

#ifndef SEASHOT_HAVE_PIPEWIRE
    Q_UNUSED(fd)
    Q_UNUSED(nodeId)
    Q_UNUSED(monitorLogicalRect)
    Q_UNUSED(region)
    return fail(QStringLiteral("编译时没有 libpipewire-0.3（装 libpipewire 后重新构建即可）"));
#else
    if (m_running)
        return fail(QStringLiteral("已经在取帧中"));
    if (fd < 0)
        return fail(QStringLiteral("无效的 PipeWire fd"));
    if (monitorLogicalRect.width() < 1 || monitorLogicalRect.height() < 1)
        return fail(QStringLiteral("输出矩形无效，无法把帧裁到选区"));
    if (region.width() < 2 || region.height() < 2)
        return fail(QStringLiteral("录制区域太小"));

    m_monitor = monitorLogicalRect;
    m_region = region;
    m_lastStoredMs = -1;
    m_clock.start();
    {
        QMutexLocker locker(&m_mutex);
        m_latest = QImage();
    }
    m_error.clear();
    m_format.clear();
    m_received.store(0);

    pw_init(nullptr, nullptr);
    m_impl->loop = pw_main_loop_new(nullptr);
    if (!m_impl->loop)
        return fail(QStringLiteral("pw_main_loop_new 失败"));
    m_impl->context = pw_context_new(pw_main_loop_get_loop(m_impl->loop), nullptr, 0);
    if (!m_impl->context)
        return fail(QStringLiteral("pw_context_new 失败"));

    // 关键：用 portal 给的 fd 连过去。这一条连接里才能看到那个录制节点
    // （实测 pw-dump 在默认图里看不到它）。
    m_impl->core = pw_context_connect_fd(m_impl->context, fd, nullptr, 0);
    if (!m_impl->core)
        return fail(QStringLiteral("pw_context_connect_fd 失败（fd 可能已关闭）"));

    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                             PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    m_impl->stream = pw_stream_new(m_impl->core, "seashot-recorder", props);
    if (!m_impl->stream)
        return fail(QStringLiteral("pw_stream_new 失败"));

    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = onStreamStateChanged;
        e.param_changed = onStreamParamChanged;
        e.process = onStreamProcess;
        return e;
    }();
    pw_stream_add_listener(m_impl->stream, &m_impl->streamHook, &events, this);

    // 不约束格式：录制节点自己决定，我们只接受能直接映射成 QImage 的 RGB 系格式
    uint8_t buffer[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), nullptr));

    // ⚠️ 目标必须用 nodeId 显式指定：不带目标时服务端报 "target not found"
    //    （这就是 KDE/pipewire 日志里那两行的来源，实测已确认不是拆卸噪音）。
    if (pw_stream_connect(m_impl->stream, PW_DIRECTION_INPUT, nodeId,
                          pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                          params, 1) < 0) {
        return fail(QStringLiteral("pw_stream_connect 失败"));
    }

    m_running = true;
    m_thread = std::thread([this] {
        pw_main_loop_run(m_impl->loop);
    });
    return true;
#endif
}

void PipeWireFrameSource::stop()
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    if (m_impl->loop)
        pw_main_loop_quit(m_impl->loop);
    if (m_thread.joinable())
        m_thread.join();
    if (m_impl->stream) {
        pw_stream_destroy(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->core) {
        pw_core_disconnect(m_impl->core);
        m_impl->core = nullptr;
    }
    if (m_impl->context) {
        pw_context_destroy(m_impl->context);
        m_impl->context = nullptr;
    }
    if (m_impl->loop) {
        pw_main_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
    m_impl->haveFormat = false;
    pw_deinit();
#endif
    m_running = false;
}

QImage PipeWireFrameSource::latestFrame() const
{
    QMutexLocker locker(&m_mutex);
    return m_latest;
}

QString PipeWireFrameSource::negotiatedFormat() const
{
    QMutexLocker locker(&m_mutex);
    return m_format;
}

QString PipeWireFrameSource::error() const
{
    QMutexLocker locker(&m_mutex);
    return m_error;
}

void PipeWireFrameSource::setError(const QString &message)
{
    QMutexLocker locker(&m_mutex);
    if (m_error.isEmpty())
        m_error = message;
}

void PipeWireFrameSource::storeFrame(const QImage &frame)
{
    QMutexLocker locker(&m_mutex);
    m_latest = frame;
    m_received.fetch_add(1);
}

void PipeWireFrameSource::handleFormat(void *param)
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(static_cast<spa_pod *>(param), &info) < 0) {
        setError(QStringLiteral("无法解析流的格式"));
        return;
    }
    m_impl->video = info;
    m_impl->haveFormat = true;
    {
        QMutexLocker locker(&m_mutex);
        m_format = QStringLiteral("%1 %2x%3")
                       .arg(QString::fromLatin1(spaFormatName(info.format)))
                       .arg(info.size.width)
                       .arg(info.size.height);
    }
    if (spaToQImageFormat(info.format) == QImage::Format_Invalid) {
        setError(QStringLiteral("不支持的像素格式 %1（只做 BGRx/BGRA/RGBx/RGBA）")
                     .arg(QString::fromLatin1(spaFormatName(info.format))));
    }
#else
    Q_UNUSED(param)
#endif
}

void PipeWireFrameSource::handleState(int state, const char *message)
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    if (state == PW_STREAM_STATE_ERROR) {
        const QString text = message ? QString::fromUtf8(message)
                                     : QStringLiteral("PipeWire 流错误");
        setError(text);
        m_impl->failed.store(true);
        SS_WARN() << "[pipewire] 流错误:" << text;
    } else if (state == PW_STREAM_STATE_STREAMING) {
        SS_INFO() << "[pipewire] 开始出帧";
    }
#else
    Q_UNUSED(state)
    Q_UNUSED(message)
#endif
}

void PipeWireFrameSource::handleProcess()
{
#ifdef SEASHOT_HAVE_PIPEWIRE
    pw_buffer *buffer = pw_stream_dequeue_buffer(m_impl->stream);
    if (!buffer)
        return;
    const auto queue = [&] {
        pw_stream_queue_buffer(m_impl->stream, buffer);
    };

    spa_buffer *spaBuffer = buffer->buffer;
    if (!spaBuffer || spaBuffer->n_datas < 1 || !m_impl->haveFormat) {
        queue();
        return;
    }
    const spa_data &data = spaBuffer->datas[0];
    const spa_chunk *chunk = data.chunk;
    if (!data.data || !chunk || chunk->size == 0) {
        queue();
        return;
    }
    const spa_video_info_raw &video = m_impl->video;
    const QImage::Format format = spaToQImageFormat(video.format);
    if (format == QImage::Format_Invalid) {
        queue(); // 已在 handleFormat 里报过错
        return;
    }

    // 按目标帧率节流：超出的帧原样退回缓冲，**不做裁剪/转换**（这是省 CPU 的关键）
    if (m_targetFps > 0) {
        const qint64 now = m_clock.isValid() ? m_clock.elapsed() : 0;
        if (m_lastStoredMs >= 0 && now - m_lastStoredMs < 1000 / m_targetFps) {
            queue();
            return;
        }
        m_lastStoredMs = now;
    }
    const int width = int(video.size.width);
    const int height = int(video.size.height);
    if (width < 1 || height < 1 || chunk->stride <= 0) {
        queue();
        return;
    }

    const uint8_t *base = static_cast<const uint8_t *>(data.data) + chunk->offset;
    const QImage view(base, width, height, chunk->stride, format);
    if (view.isNull()) {
        queue();
        return;
    }

    // 流给的是整个输出 → 按逻辑坐标换算裁到选区
    const double scale = double(width) / m_monitor.width();
    const QRectF target((m_region.x() - m_monitor.x()) * scale,
                        (m_region.y() - m_monitor.y()) * scale, m_region.width() * scale,
                        m_region.height() * scale);
    const QRect source = target.toAlignedRect().intersected(view.rect());
    if (source.isEmpty()) {
        queue();
        setError(QStringLiteral("选区不在该输出范围内"));
        return;
    }
    QImage cropped = view.copy(source);
    const QSize expected(qMax(2, qRound(target.width())), qMax(2, qRound(target.height())));
    if (cropped.size() != expected)
        cropped = cropped.scaled(expected, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    queue();

    storeFrame(cropped.convertToFormat(QImage::Format_RGB32));
#else
#endif
}

} // namespace seashot
