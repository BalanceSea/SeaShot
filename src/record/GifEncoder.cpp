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
    // 从 stdin 读 BGRA 原始帧 → FFV1 无损中间文件（快、且不与 GIF 调色板耦合）
    return {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("rawvideo"),
            QStringLiteral("-pixel_format"), QStringLiteral("bgra"),
            QStringLiteral("-video_size"), QStringLiteral("%1x%2").arg(size.width()).arg(size.height()),
            QStringLiteral("-framerate"), QString::number(qMax(1, fps)),
            QStringLiteral("-i"), QStringLiteral("-"),
            QStringLiteral("-c:v"), QStringLiteral("ffv1"),
            QStringLiteral("-level"), QStringLiteral("3"),
            QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
            QStringLiteral("-y"), intermediatePath};
}

QStringList GifEncoder::gifEncodeArgs(const QString &intermediatePath, const QString &gifPath, int fps)
{
    const QString filter = QStringLiteral("fps=%1,split[s0][s1];[s0]palettegen=stats_mode=diff[p];"
                                         "[s1][p]paletteuse=dither=bayer:bayer_scale=3")
                               .arg(qMax(1, fps));
    return {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), intermediatePath,
            QStringLiteral("-vf"), filter,
            QStringLiteral("-loop"), QStringLiteral("0"),
            QStringLiteral("-y"), gifPath};
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

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(dir);
    m_intermediatePath = QStringLiteral("%1/seashot-record-%2.mkv")
                             .arg(dir)
                             .arg(QCoreApplication::applicationPid());

    m_encoder = std::make_unique<QProcess>();
    m_encoder->setProgram(ffmpegPath());
    m_encoder->setArguments(rawInputArgs(m_size, m_fps, m_intermediatePath));
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
    gifProcess.setArguments(gifEncodeArgs(m_intermediatePath, gifPath, m_fps));
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
