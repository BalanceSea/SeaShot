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
    /// 中间格式：true=无损 FFV1（默认，转 GIF/MP4 只损失一次，但临时文件很大）；
    /// false=直接录 H.264 MP4（临时文件小得多，转 GIF 会多一次有损）。
    /// 必须在 start() 之前设置。
    void setLossless(bool lossless) { m_lossless = lossless; }
    bool lossless() const { return m_lossless; }
    bool writeFrame(const QImage &image, QString *error);
    bool finish(const QString &gifPath, QString *error);
    /// 只收尾第一遍（中间文件仍然完好、保留在原位），**不转码**。
    /// 用于"录完再让用户选导出格式"：先留下无损产物，选好再转。
    bool closeIntermediate(QString *error);
    /// 把中间文件转成 output（格式按扩展名定）。与 finish() 的第二遍是同一段逻辑。
    static bool transcode(const QString &intermediatePath, const QString &outputPath, int fps,
                          QString *error);
    void abort();

    bool isActive() const { return m_encoder && m_encoder->state() != QProcess::NotRunning; }
    int frameCount() const { return m_frames; }
    QString intermediatePath() const { return m_intermediatePath; }

    // ── 可单测的纯函数 ─────────────────────────────────────
    static QStringList rawInputArgs(const QSize &size, int fps, const QString &intermediatePath);
    /// 带中间格式选择的版本
    static QStringList rawInputArgs(const QSize &size, int fps, const QString &intermediatePath,
                                    bool lossless);
    static QStringList gifEncodeArgs(const QString &intermediatePath, const QString &gifPath, int fps);
    /// MP4（H.264）。用 libx264 + crf 18 + veryfast：体积小、CPU 低、兼容性最好。
    /// yuv420p 是为了播放器兼容（4:2:0 是唯一被普遍支持的像素格式）。
    static QStringList mp4EncodeArgs(const QString &intermediatePath, const QString &mp4Path, int fps);
    /// 按输出扩展名决定目标格式（.mp4/.m4v → MP4，其余 → GIF）
    static bool wantsMp4(const QString &path);
    static bool ffmpegAvailable();
    static QString ffmpegPath();

private:
    std::unique_ptr<QProcess> m_encoder;
    QString m_intermediatePath;
    QSize m_size;
    int m_fps = 15;
    int m_frames = 0;
    bool m_lossless = true;
};

} // namespace seashot
