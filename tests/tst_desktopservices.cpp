#include "util/DesktopServices.h"

#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

using namespace seashot;

class TestDesktopServices : public QObject
{
    Q_OBJECT

private slots:
    void init() { m_original = QProcessEnvironment::systemEnvironment(); }

    void cleanup()
    {
        for (const QString &name : {QStringLiteral("PATH"),
                                   QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"),
                                   QStringLiteral("SEASHOT_DESKTOP_TEST_OUTPUT"),
                                   QStringLiteral("SEASHOT_DESKTOP_TEST_SENTINEL")}) {
            if (m_original.contains(name))
                qputenv(name.toUtf8().constData(), m_original.value(name).toUtf8());
            else
                qunsetenv(name.toUtf8().constData());
        }
    }

    void childUsesNormalDesktopShell_data()
    {
        QTest::addColumn<QByteArray>("shell");
        QTest::newRow("layer-shell") << QByteArray("layer-shell");
        QTest::newRow("shell-fallback-list") << QByteArray("layer-shell;xdg-shell");
        QTest::newRow("default-shell") << QByteArray();
    }

    void childUsesNormalDesktopShell()
    {
        QFETCH(QByteArray, shell);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString output = dir.filePath(QStringLiteral("child-environment.txt"));
        QFile executable(dir.filePath(QStringLiteral("systemsettings")));
        QVERIFY(executable.open(QIODevice::WriteOnly));
        const QByteArray script =
            "#!/bin/sh\n"
            "printf '%s\\n' \"${QT_WAYLAND_SHELL_INTEGRATION-unset}\" \"$1\" "
            "\"${SEASHOT_DESKTOP_TEST_SENTINEL-unset}\" > \"$SEASHOT_DESKTOP_TEST_OUTPUT\"\n";
        QCOMPARE(executable.write(script), script.size());
        executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", dir.path().toUtf8());
        if (shell.isEmpty())
            qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
        else
            qputenv("QT_WAYLAND_SHELL_INTEGRATION", shell);
        qputenv("SEASHOT_DESKTOP_TEST_OUTPUT", output.toUtf8());
        qputenv("SEASHOT_DESKTOP_TEST_SENTINEL", "preserved");

        QString error = QStringLiteral("previous error");
        QVERIFY2(openKdeShortcutSettings(&error), qPrintable(error));
        QVERIFY(error.isEmpty());
        const auto contents = [&output]() {
            QFile file(output);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray();
            return file.readAll();
        };
        QTRY_COMPARE_WITH_TIMEOUT(contents(), QByteArray("unset\nkcm_keys\npreserved\n"), 5000);
        // SeaShot must retain its own layer-shell integration for overlays.
        QCOMPARE(qgetenv("QT_WAYLAND_SHELL_INTEGRATION"), shell);
    }

    void reportsMissingSystemSettings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("PATH", dir.path().toUtf8());
        QString error;
        QVERIFY(!openKdeShortcutSettings(&error));
        QVERIFY(error.contains(QStringLiteral("找不到 systemsettings")));
    }

private:
    QProcessEnvironment m_original;
};

QTEST_GUILESS_MAIN(TestDesktopServices)
#include "tst_desktopservices.moc"
