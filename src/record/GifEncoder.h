// GIF 编码器：两遍 ffmpeg
//   第一遍：原始帧（来自我们自己采集的截图）经 stdin 写成无损中间文件（FFV1/MKV）
//   第二遍：palettegen + paletteuse 生成调色板优化后的 GIF
//
// 为什么两遍：GIF 只有 256 色，单遍编码会明显发灰；palettegen 需要先看完整段视频。
// 中间文件在收尾后删除；录制期间只要磁盘够，就没有时长/体积上限。
#pragma once

#include <QImage>
#include <QProcess>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

namespace seashot {

class GifEncoder
{
public:
    GifEncoder();
    ~GifEncoder();

    bool start(const QSize &size, int fps, QString *error);
    bool writeFrame(const QImage &image, QString *error);
    bool finish(const QString &gifPath, QString *error);
    void abort();

    bool isActive() const { return m_encoder && m_encoder->state() != QProcess::NotRunning; }
    int frameCount() const { return m_frames; }
    QString intermediatePath() const { return m_intermediatePath; }

    // ── 可单测的纯函数 ─────────────────────────────────────
    static QStringList rawInputArgs(const QSize &size, int fps, const QString &intermediatePath);
    static QStringList gifEncodeArgs(const QString &intermediatePath, const QString &gifPath, int fps);
    static bool ffmpegAvailable();
    static QString ffmpegPath();

private:
    std::unique_ptr<QProcess> m_encoder;
    QString m_intermediatePath;
    QSize m_size;
    int m_fps = 15;
    int m_frames = 0;
};

} // namespace seashot
