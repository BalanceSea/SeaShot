#include "backend/wayland/ScreenShot2.h"
#include "backend/wayland/WaylandBackend.h"

#include <QDBusConnection>
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

    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
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
