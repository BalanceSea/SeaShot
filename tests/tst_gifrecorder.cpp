// 录制器：定时采集 / 丢帧策略 / 停止后编码（用假后端，不真录屏）
#include "record/GifRecorder.h"

#include "backend/CaptureBackend.h"
#include "record/GifEncoder.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

using namespace seashot;

namespace {

/// 假后端：按设定延迟返回固定尺寸的纯色帧
class StubBackend : public CaptureBackend
{
public:
    explicit StubBackend(int delayMs = 0)
        : CaptureBackend(nullptr)
        , m_delayMs(delayMs)
    {
    }

    QString name() const override { return QStringLiteral("stub"); }
    bool initialize(QString *) override { return true; }
    QVector<OutputInfo> outputs() const override
    {
        return {{QStringLiteral("stub-0"), QRect(0, 0, 800, 600), 1.0}};
    }
    QVector<WindowInfo> windows() const override { return {}; }
    void captureWindow(const WindowInfo &, ImageCallback) override {}

    void captureRect(const QRectF &, ImageCallback callback) override
    {
        ++requests;
        const auto deliver = [this, callback]() {
            CaptureImage image;
            image.image = QImage(m_size, QImage::Format_ARGB32);
            image.image.fill(QColor(++m_frame % 255, 60, 120));
            image.scale = 1.0;
            callback(image);
        };
        if (m_delayMs > 0)
            QTimer::singleShot(m_delayMs, this, deliver);
        else
            deliver();
    }

    int requests = 0;
    QSize m_size{64, 48};

private:
    int m_delayMs = 0;
    int m_frame = 0;
};

} // namespace

class TestGifRecorder : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<GifRecorder::Stats>("seashot::GifRecorder::Stats");
    }

    void capturesAtRequestedRate()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("本机没有 ffmpeg，跳过（录制需要它编码）");

        StubBackend backend(0);
        GifRecorder recorder;
        const QString gifPath = QDir::tempPath() + QStringLiteral("/seashot-tst-recorder.gif");
        QFile::remove(gifPath);
        recorder.setOutputPath(gifPath);
        QString error;
        QVERIFY2(recorder.start(&backend, QRectF(0, 0, 64, 48), 30, &error), qPrintable(error));
        QVERIFY(recorder.isRecording());
        QCOMPARE(recorder.fps(), 30);

        QSignalSpy finished(&recorder, &GifRecorder::finished);
        QTest::qWait(320);
        recorder.stop();
        QVERIFY(finished.wait(30000));

        const auto stats = recorder.stats();
        // 30fps 跑 ~320ms：期望 ~10 帧，允许 ±3（定时器精度 + 首帧同步采集）
        QVERIFY2(stats.captured >= 7 && stats.captured <= 14,
                 qPrintable(QStringLiteral("captured=%1").arg(stats.captured)));
        QCOMPARE(stats.failed, 0);
        QVERIFY(stats.elapsedMs >= 250);
        QCOMPARE(finished.first().at(2).toString(), QString()); // 无错误
        QCOMPARE(finished.first().at(0).toString(), gifPath);
        QVERIFY2(QFile::exists(gifPath), qPrintable(gifPath));
        QFile::remove(gifPath);
        QVERIFY(!recorder.isRecording());
    }

    void dropsFramesInsteadOfQueueing()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("本机没有 ffmpeg，跳过");

        // 每帧要 60ms，但按 30fps 触发（33ms）→ 必然出现"上一帧还没回来"
        StubBackend backend(60);
        GifRecorder recorder;
        const QString gifPath = QDir::tempPath() + QStringLiteral("/seashot-tst-recorder-drop.gif");
        QFile::remove(gifPath);
        recorder.setOutputPath(gifPath);
        QString error;
        QVERIFY2(recorder.start(&backend, QRectF(0, 0, 64, 48), 30, &error), qPrintable(error));

        QSignalSpy finished(&recorder, &GifRecorder::finished);
        QTest::qWait(400);
        recorder.stop();
        QVERIFY(finished.wait(30000));

        const auto stats = recorder.stats();
        QVERIFY2(stats.dropped > 0, "慢后端下必须出现丢帧，而不是把请求排队堆积");
        QVERIFY(stats.requested > stats.captured);
        QVERIFY(stats.dropRate() > 0.2);
        QFile::remove(gifPath);
    }

    void rejectsTinyRegion()
    {
        StubBackend backend(0);
        GifRecorder recorder;
        QString error;
        QVERIFY(!recorder.start(&backend, QRectF(0, 0, 1, 1), 15, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!recorder.isRecording());
    }
};

QTEST_MAIN(TestGifRecorder)
#include "tst_gifrecorder.moc"
