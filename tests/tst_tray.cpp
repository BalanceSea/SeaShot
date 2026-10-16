// 托盘：菜单项与注册状态（offscreen 下跑；不依赖面板是否真的显示了图标）
#include "app/Tray.h"

#include <QtTest>

using namespace seashot;

class TestTray : public QObject
{
    Q_OBJECT

private slots:
    void menuHasExpectedActions()
    {
        Tray tray;
        const QStringList texts = tray.menuActionTexts();
        QVERIFY2(texts.contains(QStringLiteral("截图")), qPrintable(texts.join(QLatin1Char('|'))));
        QVERIFY(texts.contains(QStringLiteral("设置…")));
        QVERIFY(texts.contains(QStringLiteral("KDE 快捷键设置…")));
        QVERIFY(texts.contains(QStringLiteral("退出 SeaShot")));
        // 顺序也要稳定：截图 → 设置 → 快捷键 → 退出
        QCOMPARE(texts.indexOf(QStringLiteral("截图")) < texts.indexOf(QStringLiteral("设置…")), true);
        QCOMPARE(texts.indexOf(QStringLiteral("设置…"))
                     < texts.indexOf(QStringLiteral("KDE 快捷键设置…")),
                 true);
        QCOMPARE(texts.indexOf(QStringLiteral("KDE 快捷键设置…"))
                     < texts.indexOf(QStringLiteral("退出 SeaShot")),
                 true);
    }

    void itemIsCreated()
    {
        Tray tray;
        QVERIFY(tray.available());
    }
};

QTEST_MAIN(TestTray)
#include "tst_tray.moc"
