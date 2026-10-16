// 配置：文件名模板、保存路径唯一化、图像落盘
#include "app/Settings.h"

#include <QSettings>
#include <QtTest>

using namespace seashot;

class TestSettings : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("seashot-test"));
        QCoreApplication::setOrganizationName(QStringLiteral("seashot-test"));
        // 每次跑测试都从干净配置开始（否则上一次 valuesRoundTrip 写下的值会污染默认值断言）
        QSettings().clear();
        QSettings().sync();
        m_dir = QDir::tempPath() + QStringLiteral("/seashot-settings-test");
        QDir().mkpath(m_dir);
    }

    void defaultsAreSane()
    {
        Settings settings;
        QVERIFY(settings.copyAfterCapture());
        QVERIFY(settings.keepShadowMargin());
        QVERIFY(settings.trayPersistent());
        QVERIFY(!settings.autoSaveAfterCapture());
        QCOMPARE(settings.gifFps(), 15);
        QVERIFY(!settings.saveDir().isEmpty());
    }

    void valuesRoundTrip()
    {
        Settings settings;
        settings.setSaveDir(m_dir);
        settings.setFilenameTemplate(QStringLiteral("shot_%1"));
        settings.setAutoSaveAfterCapture(true);
        settings.setKeepShadowMargin(false);
        settings.setGifFps(24);

        Settings reloaded;
        QCOMPARE(reloaded.saveDir(), m_dir);
        QCOMPARE(reloaded.filenameTemplate(), QStringLiteral("shot_%1"));
        QVERIFY(reloaded.autoSaveAfterCapture());
        QVERIFY(!reloaded.keepShadowMargin());
        QCOMPARE(reloaded.gifFps(), 24);
    }

    void defaultSavePathUsesTemplateAndAvoidsOverwrite()
    {
        Settings settings;
        settings.setSaveDir(m_dir);
        settings.setFilenameTemplate(QStringLiteral("t_%1"));

        const QString first = settings.defaultSavePath(QStringLiteral("png"));
        QVERIFY(first.startsWith(m_dir));
        QVERIFY(first.endsWith(QStringLiteral(".png")));

        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QString error;
        QVERIFY(Settings::saveImage(first, image, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(first));

        // 同名时自动加序号，不覆盖
        const QString second = settings.defaultSavePath(QStringLiteral("png"));
        QVERIFY(second != first);
        QVERIFY(second.contains(QStringLiteral("_1.")));
    }

    void saveImageRejectsEmptyImage()
    {
        QString error;
        QVERIFY(!Settings::saveImage(m_dir + QStringLiteral("/empty.png"), QImage(), &error));
        QVERIFY(!error.isEmpty());
    }

private:
    QString m_dir;
};

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
