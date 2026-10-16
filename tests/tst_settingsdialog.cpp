// 配置界面：控件改动是否即时写回配置（offscreen 平台下跑，无需真实显示）
#include "app/Settings.h"
#include "app/SettingsDialog.h"

#include <QCheckBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QtTest>

using namespace seashot;

class TestSettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("seashot-dialog-test"));
        QCoreApplication::setOrganizationName(QStringLiteral("seashot-dialog-test"));
        QSettings().clear();
        QSettings().sync();
    }

    void init()
    {
        QSettings().clear();
        QSettings().sync();
    }

    void widgetsReflectCurrentSettings()
    {
        Settings settings;
        settings.setCopyAfterCapture(false);
        settings.setToastAutoHideMs(3000);
        settings.setGifFps(20);
        settings.setSaveDir(QStringLiteral("/tmp/seashot-dialog"));

        SettingsDialog dialog(&settings, nullptr);
        QCOMPARE(dialog.findChild<QCheckBox *>(QStringLiteral("copyAfterCapture"))->isChecked(), false);
        QVERIFY(dialog.findChild<QCheckBox *>(QStringLiteral("keepShadowMargin"))->isChecked());
        QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("toastAutoHideMs"))->value(), 3000);
        QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("gifFps"))->value(), 20);
        QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("saveDir"))->text(),
                 QStringLiteral("/tmp/seashot-dialog"));
        // 打开时不应把控件值又写回配置（m_loading 守卫）
        QCOMPARE(settings.gifFps(), 20);
    }

    void checkboxChangesApplyImmediately()
    {
        Settings settings;
        SettingsDialog dialog(&settings, nullptr);

        auto *copyBox = dialog.findChild<QCheckBox *>(QStringLiteral("copyAfterCapture"));
        auto *editorBox = dialog.findChild<QCheckBox *>(QStringLiteral("openEditorAfterCapture"));
        auto *autoSaveBox = dialog.findChild<QCheckBox *>(QStringLiteral("autoSaveAfterCapture"));
        auto *shadowBox = dialog.findChild<QCheckBox *>(QStringLiteral("keepShadowMargin"));
        auto *trayBox = dialog.findChild<QCheckBox *>(QStringLiteral("trayPersistent"));
        QVERIFY(copyBox && editorBox && autoSaveBox && shadowBox && trayBox);

        copyBox->setChecked(false);
        editorBox->setChecked(true);
        autoSaveBox->setChecked(true);
        shadowBox->setChecked(false);
        trayBox->setChecked(false);

        Settings reread;
        QVERIFY(!reread.copyAfterCapture());
        QVERIFY(reread.openEditorAfterCapture());
        QVERIFY(reread.autoSaveAfterCapture());
        QVERIFY(!reread.keepShadowMargin());
        QVERIFY(!reread.trayPersistent());
    }

    void numericAndTextChangesApplyImmediately()
    {
        Settings settings;
        SettingsDialog dialog(&settings, nullptr);

        dialog.findChild<QSpinBox *>(QStringLiteral("toastAutoHideMs"))->setValue(2500);
        dialog.findChild<QSpinBox *>(QStringLiteral("gifFps"))->setValue(24);
        dialog.findChild<QLineEdit *>(QStringLiteral("saveDir"))->setText(QStringLiteral("/tmp/x"));
        dialog.findChild<QLineEdit *>(QStringLiteral("filenameTemplate"))->setText(QStringLiteral("s_%1"));

        Settings reread;
        QCOMPARE(reread.toastAutoHideMs(), 2500);
        QCOMPARE(reread.gifFps(), 24);
        QCOMPARE(reread.saveDir(), QStringLiteral("/tmp/x"));
        QCOMPARE(reread.filenameTemplate(), QStringLiteral("s_%1"));
    }

    void hotkeyEditWritesAndEmits()
    {
        Settings settings;
        SettingsDialog dialog(&settings, nullptr);
        QSignalSpy spy(&dialog, &SettingsDialog::hotkeyChanged);

        auto *hotkeyEdit = dialog.findChild<QKeySequenceEdit *>(QStringLiteral("hotkey"));
        QVERIFY(hotkeyEdit);
        hotkeyEdit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
        QMetaObject::invokeMethod(hotkeyEdit, "editingFinished"); // 界面里失焦/超时才会触发

        QCOMPARE(spy.count(), 1);
        QCOMPARE(settings.hotkey(), QKeySequence(QStringLiteral("Ctrl+Alt+S"))
                                        .toString(QKeySequence::NativeText));

        // 「清除」按钮 = 解绑
        dialog.findChild<QPushButton *>(QStringLiteral("clearHotkey"))->click();
        QCOMPARE(settings.hotkey(), QString());
        QCOMPARE(spy.count(), 2);
    }

    void resetRestoresDefaults()
    {
        Settings settings;
        settings.setCopyAfterCapture(false);
        settings.setGifFps(30);
        settings.setHotkey(QStringLiteral("Ctrl+Alt+S"));
        settings.setSaveDir(QStringLiteral("/tmp/custom"));

        SettingsDialog dialog(&settings, nullptr);
        dialog.findChild<QPushButton *>(QStringLiteral("resetDefaults"))->click();

        Settings reread;
        QVERIFY(reread.copyAfterCapture());          // 回到默认 true
        QCOMPARE(reread.gifFps(), 15);
        QVERIFY(reread.hotkey().isEmpty());
        QVERIFY(reread.saveDir() != QStringLiteral("/tmp/custom"));
        // 界面也要跟着回到默认
        QVERIFY(dialog.findChild<QCheckBox *>(QStringLiteral("copyAfterCapture"))->isChecked());
        QCOMPARE(dialog.findChild<QSpinBox *>(QStringLiteral("gifFps"))->value(), 15);
    }
};

QTEST_MAIN(TestSettingsDialog)
#include "tst_settingsdialog.moc"
