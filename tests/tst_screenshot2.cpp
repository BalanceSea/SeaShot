#include "backend/wayland/ScreenShot2.h"
#include "backend/wayland/WaylandBackend.h"

#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QSocketNotifier>
#include <QTimer>
#include <QtTest>
#include <unistd.h>

namespace seashot {

class FakeScreenShotService : public QDBusVirtualObject
{
public:
    int delayMs = 0;
    int completed = 0;
    QDBusMessage lastRequest;

    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        lastRequest = message;
        const auto fd = message.arguments().last().value<QDBusUnixFileDescriptor>();
        const int writer = ::dup(fd.fileDescriptor());
        if (writer < 0)
            return false;
        message.setDelayedReply(true);
        QTimer::singleShot(delayMs, this, [this, message, connection, writer]() {
            const QByteArray pixels(8 * 8 * 4, char(0xff));
            const ssize_t written = ::write(writer, pixels.constData(), size_t(pixels.size()));
            Q_UNUSED(written)
            ::close(writer);
            const QVariantMap result{{"width", 8}, {"height", 8}, {"stride", 32},
                                      {"format", 5u}, {"scale", 1.0}};
            connection.send(message.createReply(QVariantList{result}));
            ++completed;
        });
        return true;
    }
};

class TestScreenShot2 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        m_connection = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(
            qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("fake-kwin")));
        QVERIFY(m_connection->registerService(QStringLiteral("org.kde.KWin")));
        QVERIFY(m_connection->registerVirtualObject(QStringLiteral("/org/kde/KWin/ScreenShot2"),
                                                     &m_service));
    }

    void completedRequestsReleaseCallbacksAndTimers()
    {
        ScreenShot2 shot;
        int callbacks = 0;
        auto marker = std::make_shared<int>(42);
        const std::weak_ptr<int> weak = marker;
        for (int i = 0; i < 32; ++i) {
            shot.captureRect(QRect(0, 0, 8, 8), {}, [&, marker](const CaptureImage &result) {
                QVERIFY(result.ok());
                QCOMPARE(result.image.size(), QSize(8, 8));
                ++callbacks;
            });
        }
        marker.reset();
        QTRY_COMPARE(callbacks, 32);
        QTRY_VERIFY(weak.expired());
        QTRY_VERIFY(shot.findChildren<QTimer *>().isEmpty());
        QTRY_VERIFY(shot.findChildren<QSocketNotifier *>().isEmpty());
    }

    void areaKeepsLogicalCoordinatesAndOwnDialogs_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("fractional") << 1.25;
        QTest::newRow("hidpi") << 2.0;
    }

    void areaKeepsLogicalCoordinatesAndOwnDialogs()
    {
        QFETCH(double, scale);
        WaylandBackend backend;
        backend.m_mapper.setOutputs({{"test", QRect(-1600, 0, 1600, 1000), scale}});
        const QRectF selected(-1500, 125, 320, 240);
        bool completed = false;
        backend.captureRect(selected, [&](const CaptureImage &image) {
            QVERIFY(image.ok());
            completed = true;
        });
        QTRY_VERIFY(completed);
        const auto args = m_service.lastRequest.arguments();
        QCOMPARE(m_service.lastRequest.member(), QStringLiteral("CaptureArea"));
        QCOMPARE(args[0].toInt(), -1500);
        QCOMPARE(args[1].toInt(), 125);
        QCOMPARE(args[2].toUInt(), 320u);
        QCOMPARE(args[3].toUInt(), 240u);
        const auto options = qdbus_cast<QVariantMap>(args[4]);
        QVERIFY(options.contains(QStringLiteral("hide-caller-windows")));
        QVERIFY(!options.value(QStringLiteral("hide-caller-windows")).toBool());
        QVERIFY(options.value(QStringLiteral("native-resolution")).toBool());
    }

    void permissionResponseAfterTimeoutIsSafe()
    {
        m_service.delayMs = 6500;
        const int before = m_service.completed;
        WaylandBackend backend;
        QString detail;
        QVERIFY(!backend.probePermission(&detail));
        QVERIFY(detail.contains(QStringLiteral("超时")));
        QTRY_COMPARE_WITH_TIMEOUT(m_service.completed, before + 1, 2000);
        QTRY_VERIFY(backend.m_shot2.findChildren<QTimer *>().isEmpty());
        m_service.delayMs = 0;
    }

private:
    FakeScreenShotService m_service;
    std::unique_ptr<QDBusConnection> m_connection;
};

} // namespace seashot

QTEST_MAIN(seashot::TestScreenShot2)
#include "tst_screenshot2.moc"
