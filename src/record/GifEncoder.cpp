#include "record/GifEncoder.h"

#include "util/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace seashot {

namespace {
constexpr int kWriteTimeoutMs = 5000;
constexpr int kFinishTimeoutMs = 60000;
// 转码：GIF 调色板量化比 H.264 慢得多（尤其大分辨率/长时长），给足时间
constexpr int kTranscodeTimeoutMs = 600000;
constexpr int kGifTimeoutMs = 120000;
} // namespace

GifEncoder::GifEncoder() = default;

GifEncoder::~GifEncoder()
{
    abort();
}

QString GifEncoder::ffmpegPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

bool GifEncoder::ffmpegAvailable()
{
    return !ffmpegPath().isEmpty();
}

QStringList GifEncoder::rawInputArgs(const QSize &size, int fps, const QString &intermediatePath)
{
    return rawInputArgs(size, fps, intermediatePath, true);
}

QStringList GifEncoder::rawInputArgs(const QSize &size, int fps, const QString &intermediatePath,
                                     bool lossless)
{
    // 从 stdin 读 BGRA 原始帧。
    //  - lossless：FFV1 无损（转 GIF/MP4 都只损失一次，代价是临时文件很大）
    //  - 否则：  直接 H.264 MP4（临时文件小，转 GIF 多一次有损）
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("error"),
                     QStringLiteral("-f"), QStringLiteral("rawvideo"),
                     QStringLiteral("-pixel_format"), QStringLiteral("bgra"),
                     QStringLiteral("-video_size"),
                     QStringLiteral("%1x%2").arg(size.width()).arg(size.height()),
                     QStringLiteral("-framerate"), QString::number(qMax(1, fps)),
                     QStringLiteral("-i"), QStringLiteral("-")};
    if (lossless) {
        args << QStringLiteral("-c:v") << QStringLiteral("ffv1") << QStringLiteral("-level")
             << QStringLiteral("3") << QStringLiteral("-pix_fmt") << QStringLiteral("bgra");
    } else {
        args << QStringLiteral("-vf") << QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2")
             << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset")
             << QStringLiteral("veryfast") << QStringLiteral("-crf") << QStringLiteral("18")
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-movflags")
             << QStringLiteral("+faststart");
    }
    args << QStringLiteral("-y") << intermediatePath;
    return args;
}

QStringList GifEncoder::gifEncodeArgs(const QString &intermediatePath, const QString &gifPath, int fps)
{
    // dither=none：抖动会给每个像素加噪，**正面破坏 GIF 的帧间差分**（只写变化区域）。
    // 实测（同一段 1920x1080 3 秒源）：
    //   dither=bayer:bayer_scale=3  → 1,405,770 字节
    //   dither=none                 → 1,098,771 字节（-22%）
    // 代价：大片渐变上可能有轻微色带；屏幕截图多为纯色区域，这个取舍值得。
    // 另：offsetting/transdiff（只写变化区域）在本机 ffmpeg 上**本来就是默认开启**的 ——
    //     实测显式加上 -gifflags 后体积一模一样，所以不必额外指定。
    const QString filter = QStringLiteral("fps=%1,split[s0][s1];[s0]palettegen=stats_mode=diff[p];"
                                         "[s1][p]paletteuse=dither=none")
                               .arg(qMax(1, fps));
    return {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), intermediatePath,
            QStringLiteral("-vf"), filter,
            QStringLiteral("-loop"), QStringLiteral("0"),
            QStringLiteral("-y"), gifPath};
}

QStringList GifEncoder::mp4EncodeArgs(const QString &intermediatePath, const QString &mp4Path, int fps)
{
    return {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), intermediatePath,
            QStringLiteral("-vf"), QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2"),
            QStringLiteral("-c:v"), QStringLiteral("libx264"),
            QStringLiteral("-preset"), QStringLiteral("veryfast"),
            QStringLiteral("-crf"), QStringLiteral("18"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-movflags"), QStringLiteral("+faststart"),
            QStringLiteral("-r"), QString::number(qMax(1, fps)),
            QStringLiteral("-y"), mp4Path};
}

QString GifEncoder::intermediateDirectory()
{
    // ⚠️ 不用 QStandardPaths::CacheLocation：它解析出来的目录取决于应用名/组织名，
    //    实测不是 ~/.cache/seashot（与本项目其它路径的约定不一致，排查了半天）。
    //    这里跟 restore token 用同一套确定性路径，行为可预期、也好清理。
    return QDir::homePath() + QStringLiteral("/.cache/seashot/recording");
}

int GifEncoder::cleanStaleIntermediates()
{
    // 上一次崩溃/kill 留下的中间文件可能有几百 MB 到几 GB，且占的是磁盘。
    // 只删"进程已经不在了"的那些：同一时刻可能有另一个实例正在录制，不能误删。
    int removed = 0;
    QStringList dirs{intermediateDirectory(),
                     QStandardPaths::writableLocation(QStandardPaths::TempLocation)};
    dirs.removeAll(QString());
    for (const QString &dir : std::as_const(dirs)) {
        QDir d(dir);
        const QStringList files =
            d.entryList({QStringLiteral("seashot-record-*")}, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            // 文件名形如 seashot-record-<pid>.<ext>
            const QString stem = name.section(QLatin1Char('.'), 0, 0);
            const QString pidText = stem.section(QLatin1Char('-'), 2, 2);
            bool isPid = false;
            const qint64 pid = pidText.toLongLong(&isPid);
            const bool alive = isPid && pid > 0 && QFile::exists(QStringLiteral("/proc/%1").arg(pid));
            if (alive)
                continue;
            if (QFile::remove(dir + QLatin1Char('/') + name)) {
                ++removed;
                SS_INFO() << "[record] 已清理残留中间文件" << dir + QLatin1Char('/') + name;
            }
        }
    }
    return removed;
}

bool GifEncoder::wantsMp4(const QString &path)
{
    return path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".m4v"), Qt::CaseInsensitive);
}

bool GifEncoder::start(const QSize &size, int fps, QString *error)
{
    if (size.isEmpty()) {
        if (error)
            *error = QStringLiteral("录制区域尺寸为空");
        return false;
    }
    if (!ffmpegAvailable()) {
        if (error)
            *error = QStringLiteral("找不到 ffmpeg（GIF 编码需要它）");
        return false;
    }

    m_size = size;
    m_fps = qMax(1, fps);
    m_frames = 0;

    const QString dir = intermediateDirectory();
    QDir().mkpath(dir);
    m_intermediatePath = QStringLiteral("%1/seashot-record-%2.%3")
                             .arg(dir)
                             .arg(QCoreApplication::applicationPid())
                             .arg(m_lossless ? QStringLiteral("mkv") : QStringLiteral("mp4"));

    m_encoder = std::make_unique<QProcess>();
    m_encoder->setProgram(ffmpegPath());
    m_encoder->setArguments(rawInputArgs(m_size, m_fps, m_intermediatePath, m_lossless));
    m_encoder->start();
    if (!m_encoder->waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("ffmpeg 启动失败（第一遍写中间文件）");
        m_encoder.reset();
        return false;
    }
    SS_INFO() << "[record] 编码器已启动" << m_size << m_fps << "fps →" << m_intermediatePath;
    return true;
}

bool GifEncoder::writeFrame(const QImage &image, QString *error)
{
    if (!isActive()) {
        if (error)
            *error = QStringLiteral("编码器不在运行状态");
        return false;
    }
    if (image.size() != m_size) {
        if (error)
            *error = QStringLiteral("帧尺寸变化（%1x%2 ≠ %3x%4）")
                         .arg(image.width())
                         .arg(image.height())
                         .arg(m_size.width())
                         .arg(m_size.height());
        return false;
    }

    // BGRA 原始帧：QImage 在小端机上 Format_ARGB32 的内存布局就是 B,G,R,A
    const QImage bgra = image.format() == QImage::Format_ARGB32
        ? image
        : image.convertToFormat(QImage::Format_ARGB32);
    const qint64 bytes = qint64(bgra.bytesPerLine()) * bgra.height();
    const char *data = reinterpret_cast<const char *>(bgra.constBits());

    qint64 written = 0;
    while (written < bytes) {
        const qint64 chunk = m_encoder->write(data + written, bytes - written);
        if (chunk <= 0) {
            if (error)
                *error = QStringLiteral("写帧失败（ffmpeg 可能已退出）：%1")
                             .arg(QString::fromLocal8Bit(m_encoder->readAllStandardError()).trimmed());
            return false;
        }
        written += chunk;
        if (!m_encoder->waitForBytesWritten(kWriteTimeoutMs)) {
            if (error)
                *error = QStringLiteral("写帧超时（编码器卡住）");
            return false;
        }
    }
    ++m_frames;
    return true;
}

bool GifEncoder::closeIntermediate(QString *error)
{
    if (!m_encoder) {
        if (error)
            *error = QStringLiteral("没有正在进行的编码");
        return false;
    }
    m_encoder->closeWriteChannel();
    if (!m_encoder->waitForFinished(kFinishTimeoutMs)) {
        if (error)
            *error = QStringLiteral("第一遍编码收尾超时");
        abort();
        return false;
    }
    if (m_encoder->exitCode() != 0) {
        if (error)
            *error = QStringLiteral("第一遍编码失败：%1")
                         .arg(QString::fromLocal8Bit(m_encoder->readAllStandardError()).trimmed());
        abort();
        return false;
    }
    m_encoder.reset();
    return true; // 中间文件留在原位，不清也不转
}

bool GifEncoder::transcode(const QString &intermediatePath, const QString &outputPath, int fps,
                           QString *error)
{
    if (intermediatePath.isEmpty() || !QFile::exists(intermediatePath)) {
        if (error)
            *error = QStringLiteral("找不到中间文件：%1").arg(intermediatePath);
        return false;
    }
    if (!ffmpegAvailable()) {
        if (error)
            *error = QStringLiteral("找不到 ffmpeg");
        return false;
    }
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    QProcess process;
    process.setProgram(ffmpegPath());
    process.setArguments(wantsMp4(outputPath)
                             ? mp4EncodeArgs(intermediatePath, outputPath, fps)
                             : gifEncodeArgs(intermediatePath, outputPath, fps));
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("无法启动 ffmpeg 转码");
        return false;
    }
    if (!process.waitForFinished(kTranscodeTimeoutMs)) {
        process.kill();
        if (error)
            *error = QStringLiteral("转码超时");
        return false;
    }
    if (process.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("转码失败：%1")
                         .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

bool GifEncoder::finish(const QString &gifPath, QString *error)
{
    if (gifPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("未指定 GIF 输出路径");
        abort();
        return false;
    }
    if (!m_encoder) {
        if (error)
            *error = QStringLiteral("没有正在进行的编码");
        return false;
    }

    m_encoder->closeWriteChannel();
    if (!m_encoder->waitForFinished(kFinishTimeoutMs)) {
        if (error)
            *error = QStringLiteral("第一遍编码收尾超时");
        abort();
        return false;
    }
    if (m_encoder->exitCode() != 0) {
        if (error)
            *error = QStringLiteral("第一遍编码失败：%1")
                         .arg(QString::fromLocal8Bit(m_encoder->readAllStandardError()).trimmed());
        abort();
        return false;
    }
    m_encoder.reset();

    QDir().mkpath(QFileInfo(gifPath).absolutePath());
    QProcess gifProcess;
    gifProcess.setProgram(ffmpegPath());
    // 目标格式按扩展名决定：同一条 ffmpeg 管线，只是最后一步换编码器
    // 已经是目标格式（MP4 模式录 MP4）→ 直接搬文件，避免白跑一遍有损转码
    if (!m_lossless && wantsMp4(gifPath)) {
        QDir().mkpath(QFileInfo(gifPath).absolutePath());
        QFile::remove(gifPath);
        const bool moved = QFile::rename(m_intermediatePath, gifPath);
        m_intermediatePath.clear();
        if (!moved) {
            if (error)
                *error = QStringLiteral("移动 MP4 中间文件失败");
            return false;
        }
        return true;
    }
    const bool toMp4 = wantsMp4(gifPath);
    gifProcess.setArguments(toMp4 ? mp4EncodeArgs(m_intermediatePath, gifPath, m_fps)
                                  : gifEncodeArgs(m_intermediatePath, gifPath, m_fps));
    gifProcess.start();
    if (!gifProcess.waitForStarted(5000) || !gifProcess.waitForFinished(kGifTimeoutMs)
        || gifProcess.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("GIF 编码失败：%1")
                         .arg(QString::fromLocal8Bit(gifProcess.readAllStandardError()).trimmed());
        QFile::remove(m_intermediatePath);
        m_intermediatePath.clear();
        return false;
    }

    const qint64 gifBytes = QFileInfo(gifPath).size();
    QFile::remove(m_intermediatePath); // 中间文件不留
    SS_INFO() << "[record] GIF 完成" << gifPath << m_frames << "帧" << gifBytes << "字节";
    m_intermediatePath.clear();
    return true;
}

void GifEncoder::abort()
{
    if (m_encoder) {
        m_encoder->kill();
        m_encoder->waitForFinished(3000);
        m_encoder.reset();
    }
    if (!m_intermediatePath.isEmpty()) {
        QFile::remove(m_intermediatePath);
        m_intermediatePath.clear();
    }
}

} // namespace seashot
