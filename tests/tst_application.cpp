#include "app/Application.h"
#include "app/Settings.h"
#include "backend/CaptureBackend.h"
#include "record/GifRecorder.h"
#include "record/GifEncoder.h"
#include "record/RecordingIndicator.h"
#include "scroll/ScrollCapturer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace seashot {

class ChangingBackend : public CaptureBackend
{
public:
    QString name() const override { return QStringLiteral("test"); }
    bool initialize(QString *) override { return true; }
    bool excludeOwnWindows(bool, QString *) override { return true; }
    QVector<OutputInfo> outputs() const override { return {{"test", QRect(0, 0, 800, 600), scale}}; }
    QVector<WindowInfo> windows() const override { return {}; }
    void captureWindow(const WindowInfo &, ImageCallback callback) override
    {
        captureRect(QRectF(100, 100, 64, 48), std::move(callback));
    }
    void captureRect(const QRectF &rect, ImageCallback callback) override
    {
        if (beforeCapture)
            beforeCapture();
        CaptureImage image;
        image.scale = scale;
        image.image = QImage(QSize(qRound(rect.width() * scale), qRound(rect.height() * scale)),
                              QImage::Format_ARGB32);
        image.image.fill(++captures == 1 ? Qt::red : Qt::blue);
        if (delayMs > 0)
            QTimer::singleShot(delayMs, this, [callback = std::move(callback), image]() { callback(image); });
        else
            callback(image);
    }
    int captures = 0;
    double scale = 1.0;
    int delayMs = 0;
    std::function<void()> beforeCapture;
};

class TestApplication : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_config.isValid());
        qApp->setQuitOnLastWindowClosed(false);
        QCoreApplication::setOrganizationName(QStringLiteral("seashot-app-test"));
        QCoreApplication::setApplicationName(QStringLiteral("seashot-app-test"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_config.path());
        const QString runtime = m_config.filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("SEASHOT_RECORD_POLLING", "1");
    }

    void init() { QSettings().clear(); }

    void confirmationKeepsFrozenPixels_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("fractional") << 1.25;
        QTest::newRow("hidpi") << 2.0;
    }

    void confirmationKeepsFrozenPixels()
    {
        QFETCH(double, scale);
        QTemporaryDir output;
        Options options;
        options.noCopy = true;
        options.saveTo = output.filePath(QStringLiteral("shot.png"));
        auto backend = std::make_unique<ChangingBackend>();
        auto *source = backend.get();
        source->scale = scale;
        Application application(options, std::move(backend), nullptr);
        application.m_overlay->setVirtualGeometry(QRect(0, 0, 800, 600));
        application.m_pendingSelection.logicalRect = QRectF(100, 100, 64, 48);
        application.enterAnnotationPhase();
        QTRY_VERIFY(application.m_overlay->isAnnotating());
        Annotation annotation;
        annotation.kind = ToolKind::Rectangle;
        annotation.color = Qt::green;
        annotation.width = 2;
        annotation.points = {QPointF(10, 10), QPointF(40, 30)};
        const_cast<AnnotationDocument &>(application.m_overlay->document()).add(annotation);
        application.m_overlay->annotationConfirmed(application.m_pendingSelection,
                                                     Overlay::ConfirmAction::Plain);
        const QImage saved(options.saveTo);
        QCOMPARE(source->captures, 1);
        QCOMPARE(saved.size(), QSize(qRound(64 * scale), qRound(48 * scale)));
        QCOMPARE(saved.pixelColor(qRound(5 * scale), qRound(5 * scale)), QColor(Qt::red));
        QCOMPARE(saved.pixelColor(int(10 * scale), int(20 * scale)), QColor(Qt::green));
    }

    void freezesBeforeOverlayCanDismissMenu_data()
    {
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("annotate");
        for (const double scale : {1.0, 1.25, 2.0}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-annotate").arg(scale))) << scale << true;
            QTest::newRow(qPrintable(QStringLiteral("%1-direct").arg(scale))) << scale << false;
        }
    }

    void freezesBeforeOverlayCanDismissMenu()
    {
        QFETCH(double, scale);
        QFETCH(bool, annotate);
        const auto old = qgetenv("SEASHOT_INPLACE_ANNOTATE");
        const auto restore = qScopeGuard([old]() {
            if (old.isNull())
                qunsetenv("SEASHOT_INPLACE_ANNOTATE");
            else
                qputenv("SEASHOT_INPLACE_ANNOTATE", old);
        });
        qputenv("SEASHOT_INPLACE_ANNOTATE", annotate ? "1" : "0");
        QTemporaryDir output;
        Options options;
        options.noCopy = true;
        options.saveTo = output.filePath(QStringLiteral("menu.png"));
        auto backend = std::make_unique<ChangingBackend>();
        auto *source = backend.get();
        source->scale = scale;
        Application application(options, std::move(backend), nullptr);
        bool capturedOverOverlay = false;
        source->beforeCapture = [&]() {
            capturedOverOverlay |= application.m_overlay->isVisible();
        };
        application.startCapture();
        QTRY_VERIFY(application.m_overlay->isVisible());
        QCOMPARE(source->captures, 1);
        QVERIFY(!capturedOverOverlay);
        QCOMPARE(application.m_overlay->frozenFrame().pixelColor(10, 200), QColor(Qt::red));
        const auto preview = application.m_overlay->grab().toImage();
        const QColor pixel = preview.pixelColor(10, qRound(200 * preview.devicePixelRatio()));
        QVERIFY(pixel.red() > pixel.blue());

        SelectionResult selection;
        selection.logicalRect = QRectF(100, 100, 64, 48);
        application.handleSelection(selection);
        if (annotate) {
            QTRY_VERIFY(application.m_overlay->isAnnotating());
            application.m_overlay->annotationConfirmed(selection, Overlay::ConfirmAction::Plain);
        }
        QTRY_VERIFY(QFileInfo::exists(options.saveTo));
        const QImage saved(options.saveTo);
        QCOMPARE(source->captures, 1);
        QCOMPARE(saved.size(), QSize(qRound(64 * scale), qRound(48 * scale)));
        QCOMPARE(saved.pixelColor(saved.width() / 2, saved.height() / 2), QColor(Qt::red));
    }

    void cancelledCaptureDoesNotReopenFromLateFrame()
    {
        Options options;
        options.noCopy = true;
        auto backend = std::make_unique<ChangingBackend>();
        auto *source = backend.get();
        source->delayMs = 30;
        Application application(options, std::move(backend), nullptr);
        application.startCapture();
        application.startCapture();
        QCOMPARE(source->captures, 1);
        QVERIFY(!application.m_overlay->isVisible());
        application.m_overlay->selectionCancelled();
        QTest::qWait(80);
        QVERIFY(!application.m_overlay->isVisible());
        application.startCapture();
        QTRY_VERIFY(application.m_overlay->isVisible());
        QCOMPARE(source->captures, 2);
        application.m_overlay->selectionCancelled();
    }

    void scrollUsesThirtyMinuteFallback()
    {
        Options options;
        options.noCopy = true;
        Application application(options, std::make_unique<ChangingBackend>(), nullptr);
        SelectionResult selection;
        selection.logicalRect = QRectF(0, 0, 64, 48);
        application.startScrollCapture(selection);
        QCOMPARE(application.m_scrollCapturer->m_config.maxDurationMs, 30 * 60 * 1000);
        application.m_scrollCapturer->disconnect(&application);
        application.m_scrollCapturer->stop();
        application.m_options.scrollMs = 5000;
        application.startScrollCapture(selection);
        QCOMPARE(application.m_scrollCapturer->m_config.maxDurationMs, 5000);
        application.m_scrollCapturer->stop();
    }

    void forwardedRecordingKeepsOptionsAndStops()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("ffmpeg required");
        QTemporaryDir output;
        Options options;
        options.noCopy = true;
        Application application(options, std::make_unique<ChangingBackend>(), nullptr);
        QVERIFY(application.startServer());
        const QString path = output.filePath(QStringLiteral("视频 export.mp4"));
        const QJsonObject message{{"command", "record"}, {"seconds", 1}, {"saveTo", path},
                                   {"rect", QJsonArray{10, 20, 64, 48}}};
        const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
        QSignalSpy finished(application.m_recorder.get(), &GifRecorder::finished);
        QLocalSocket socket;
        socket.connectToServer(Application::socketPath());
        QVERIFY(socket.waitForConnected());
        socket.write(payload.first(payload.size() / 2));
        socket.flush();
        QTest::qWait(30);
        QVERIFY(!application.m_recorder->isRecording());
        socket.write(payload.mid(payload.size() / 2) + '\n');
        socket.flush();
        QTRY_VERIFY(application.m_recorder->isRecording());
        QCOMPARE(application.m_recordingRequest.seconds, 1);
        QCOMPARE(application.m_recorder->region(), QRectF(10, 20, 64, 48));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.first().at(2).toString(), QString());
        QVERIFY(QFileInfo(path).size() > 0);
        QVERIFY(application.m_server->isListening());
        QVERIFY(application.m_options.saveTo.isEmpty());
        application.startRecordFlow();
        QCOMPARE(application.m_recordingRequest.seconds, 0);
        QVERIFY(application.m_recordingRequest.saveTo.isEmpty());
        application.m_overlay->selectionCancelled();
        QVERIFY(!application.m_recordIntent);
        QTRY_VERIFY(application.m_server->findChildren<QLocalSocket *>().isEmpty());
    }

    void recordingStartupRejectsReentrantRequests()
    {
        if (!GifEncoder::ffmpegAvailable())
            QSKIP("ffmpeg required");
        QTemporaryDir output;
        Options options;
        auto backend = std::make_unique<ChangingBackend>();
        backend->delayMs = 30;
        Application application(options, std::move(backend), nullptr);
        RecordingRequest request;
        request.hasRect = true;
        request.rect = QRectF(10, 20, 64, 48);
        request.saveTo = output.filePath(QStringLiteral("original.mp4"));
        bool attempted = false;
        bool accepted = true;
        QTimer::singleShot(0, &application, [&]() {
            attempted = true;
            RecordingRequest other = request;
            other.saveTo = output.filePath(QStringLiteral("other.mp4"));
            other.seconds = 1;
            accepted = application.startRecordingRequest(other);
        });
        QVERIFY(application.startRecordingRequest(request));
        QVERIFY(attempted);
        QVERIFY(!accepted);
        QVERIFY(!application.m_recordStarting);
        QCOMPARE(application.m_recordingRequest.saveTo, request.saveTo);
        QCOMPARE(application.m_recordingRequest.seconds, 0);
        QSignalSpy finished(application.m_recorder.get(), &GifRecorder::finished);
        application.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.first().at(2).toString(), QString());
        QVERIFY(QFileInfo(request.saveTo).size() > 0);
    }

    void anotherServerCannotRemoveLiveSocket()
    {
        Options options;
        Application first(options, std::make_unique<ChangingBackend>(), nullptr);
        QVERIFY(first.startServer());
        Application second(options, std::make_unique<ChangingBackend>(), nullptr);
        QVERIFY(!second.startServer());
        QLocalSocket socket;
        socket.connectToServer(Application::socketPath());
        QVERIFY(socket.waitForConnected());
    }

    void cliForwardsRecordingAndExits_data()
    {
        QTest::addColumn<bool>("recording");
        QTest::newRow("record-options") << true;
        QTest::newRow("existing-resident") << false;
    }

    void cliForwardsRecordingAndExits()
    {
        QFETCH(bool, recording);
        QTemporaryDir runtime;
        QLocalServer server;
        QVERIFY(server.listen(runtime.filePath(QStringLiteral("seashot.sock"))));
        QByteArray command;
        connect(&server, &QLocalServer::newConnection, this, [&]() {
            auto *socket = server.nextPendingConnection();
            const auto read = [&, socket]() { command += socket->readAll(); };
            connect(socket, &QLocalSocket::readyRead, this, read);
            read();
        });
        QProcess client;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DISPLAY"), QString());
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                            QStringLiteral("unix:path=") + runtime.filePath(QStringLiteral("no-bus")));
        client.setProcessEnvironment(environment);
        client.setWorkingDirectory(runtime.path());
        QStringList args{QStringLiteral("--backend"), QStringLiteral("x11")};
        if (recording)
            args << "--record" << "--rect" << "10,20,64,48" << "--seconds" << "7"
                 << "--save" << "out clip.mp4";
        client.start(QStringLiteral(SEASHOT_BINARY), args);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 5000);
        QCOMPARE(client.exitCode(), 0);
        QTRY_VERIFY(command.contains('\n'));
        if (recording) {
            const auto message = QJsonDocument::fromJson(command).object();
            QCOMPARE(message.value("seconds").toInt(), 7);
            QCOMPARE(message.value("saveTo").toString(), runtime.filePath("out clip.mp4"));
            QCOMPARE(message.value("rect").toArray(), QJsonArray({10, 20, 64, 48}));
        } else {
            QVERIFY(client.readAllStandardOutput().contains("SEASHOT_ALREADY_RUNNING"));
        }
    }

private:
    QTemporaryDir m_config;
};

} // namespace seashot

QTEST_MAIN(seashot::TestApplication)
#include "tst_application.moc"
