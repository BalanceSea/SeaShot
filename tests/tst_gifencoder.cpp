// GIF 编码器：命令行构造 + 真跑一遍 ffmpeg 的端到端校验
#include "record/GifEncoder.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace seashot;

class TestGifEncoder : public QObject
{
    Q_OBJECT

private slots:
    void oddSizedMp4IsPadded_data()
    {
        QTest::addColumn<bool>("lossless");
        QTest::newRow("lossless-to-mp4") << true;
        QTest::newRow("direct-mp4") << false;
    }

    void oddSizedMp4IsPadded()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("ffmpeg required");
        QFETCH(bool, lossless);
        QTemporaryDir output;
        const QString path = output.filePath(QStringLiteral("odd.mp4"));
        GifEncoder encoder;
        encoder.setLossless(lossless);
        QString error;
        QVERIFY2(encoder.start(QSize(63, 47), 10, &error), qPrintable(error));
        QImage frame(63, 47, QImage::Format_ARGB32);
        frame.fill(Qt::red);
        QVERIFY2(encoder.writeFrame(frame, &error), qPrintable(error));
        QVERIFY2(encoder.finish(path, &error), qPrintable(error));
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"), {"-v", "error", "-show_entries",
                    "stream=codec_name,width,height", "-of", "default=noprint_wrappers=1", path});
        QVERIFY(probe.waitForFinished(10000));
        QCOMPARE(probe.exitCode(), 0);
        const QByteArray metadata = probe.readAllStandardOutput();
        QVERIFY(metadata.contains("codec_name=h264"));
        QVERIFY(metadata.contains("width=64"));
        QVERIFY(metadata.contains("height=48"));
    }

    void rawInputArgsCarrySizeAndFps()
    {
        const QStringList args = GifEncoder::rawInputArgs(QSize(320, 240), 15, QStringLiteral("/tmp/x.mkv"));
        QVERIFY(args.contains(QStringLiteral("rawvideo")));
        QVERIFY(args.contains(QStringLiteral("bgra")));
        QVERIFY(args.contains(QStringLiteral("320x240")));
        QVERIFY(args.contains(QStringLiteral("15")));
        QVERIFY(args.contains(QStringLiteral("ffv1")));
        QCOMPARE(args.last(), QStringLiteral("/tmp/x.mkv"));
    }

    void gifArgsUsePaletteTwoPass()
    {
        const QStringList args =
            GifEncoder::gifEncodeArgs(QStringLiteral("/tmp/x.mkv"), QStringLiteral("/tmp/x.gif"), 12);
        const QString filter = args.at(args.indexOf(QStringLiteral("-vf")) + 1);
        QVERIFY(filter.contains(QStringLiteral("fps=12")));
        QVERIFY(filter.contains(QStringLiteral("palettegen")));
        QVERIFY(filter.contains(QStringLiteral("paletteuse")));
        QVERIFY(args.contains(QStringLiteral("-loop")));
        QCOMPARE(args.last(), QStringLiteral("/tmp/x.gif"));
    }

    void encodesRealGifWithFfmpeg()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("本机没有 ffmpeg，跳过真实编码验证");

        const QSize size(64, 48);
        const int fps = 10;
        const int frames = 10;
        const QString gifPath = QDir::tempPath() + QStringLiteral("/seashot-tst-encoder.gif");
        QFile::remove(gifPath);

        GifEncoder encoder;
        QString error;
        QVERIFY2(encoder.start(size, fps, &error), qPrintable(error));
        const QString intermediate = encoder.intermediatePath();
        QVERIFY(!intermediate.isEmpty());

        for (int frame = 0; frame < frames; ++frame) {
            QImage image(size, QImage::Format_ARGB32);
            image.fill(QColor(frame * 20, 40, 200 - frame * 10));
            QVERIFY2(encoder.writeFrame(image, &error), qPrintable(error));
        }
        QCOMPARE(encoder.frameCount(), frames);
        QVERIFY2(encoder.finish(gifPath, &error), qPrintable(error));

        // 中间文件必须被清掉，GIF 必须真存在
        QVERIFY2(!QFile::exists(intermediate), "FFV1 中间文件应当被删除");
        QVERIFY(QFile::exists(gifPath));
        QVERIFY(QFileInfo(gifPath).size() > 0);

        // 用 ffprobe 校验尺寸与帧数（真正落到 GIF 里的东西）
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                     QStringLiteral("v:0"), QStringLiteral("-show_entries"),
                     QStringLiteral("stream=width,height,nb_frames"), QStringLiteral("-of"),
                     QStringLiteral("default=noprint_wrappers=1"), gifPath});
        QVERIFY(probe.waitForFinished(20000));
        const QString out = QString::fromLocal8Bit(probe.readAllStandardOutput());
        QVERIFY2(out.contains(QStringLiteral("width=64")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("height=48")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("nb_frames=10")), qPrintable(out));

        QFile::remove(gifPath);
    }

    void rejectsInconsistentFrameSize()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("本机没有 ffmpeg，跳过");

        GifEncoder encoder;
        QString error;
        QVERIFY2(encoder.start(QSize(32, 32), 10, &error), qPrintable(error));
        QImage wrong(16, 16, QImage::Format_ARGB32);
        wrong.fill(Qt::red);
        QVERIFY(!encoder.writeFrame(wrong, &error)); // 尺寸变化必须被拒绝
        QVERIFY(!error.isEmpty());
        encoder.abort();
    }
};

QTEST_MAIN(TestGifEncoder)
#include "tst_gifencoder.moc"
