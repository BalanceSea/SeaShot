// 选区纯逻辑：悬停命中 / 拖拽归一化 / 单击阈值
#include "core/SelectionModel.h"

#include <QtTest>

using namespace seashot;

namespace {
WindowInfo makeWindow(const QString &id, const QRectF &frame, int stack, bool active = false,
                      const QString &appId = QStringLiteral("app"), bool minimized = false)
{
    WindowInfo info;
    info.id = id;
    info.appId = appId;
    info.title = id;
    info.frameGeometry = frame;
    info.clientGeometry = frame;
    info.stackIndex = stack;
    info.active = active;
    info.minimized = minimized;
    info.normal = true;
    info.visible = true;
    return info;
}
} // namespace

class TestSelection : public QObject
{
    Q_OBJECT

private slots:
    void picksTopmostWindowUnderCursor()
    {
        const QVector<WindowInfo> windows{
            makeWindow(QStringLiteral("bottom"), QRectF(0, 0, 500, 500), 0),
            makeWindow(QStringLiteral("middle"), QRectF(100, 100, 300, 300), 1),
            makeWindow(QStringLiteral("top"), QRectF(200, 200, 200, 200), 2),
        };
        QCOMPARE(pickWindowAt(windows, QPointF(250, 250)).id, QStringLiteral("top"));
        QCOMPARE(pickWindowAt(windows, QPointF(150, 150)).id, QStringLiteral("middle"));
        QCOMPARE(pickWindowAt(windows, QPointF(50, 50)).id, QStringLiteral("bottom"));
        QVERIFY(!pickWindowAt(windows, QPointF(900, 900)).isValid());
    }

    void skipsMinimizedAndOwnWindows()
    {
        const QVector<WindowInfo> windows{
            makeWindow(QStringLiteral("minimized"), QRectF(0, 0, 500, 500), 5, false, QStringLiteral("app"), true),
            makeWindow(QStringLiteral("ours"), QRectF(0, 0, 500, 500), 4, false, QStringLiteral("seashot")),
            makeWindow(QStringLiteral("usable"), QRectF(0, 0, 500, 500), 1),
        };
        QCOMPARE(pickWindowAt(windows, QPointF(10, 10)).id, QStringLiteral("usable"));
    }

    void fallsBackToListOrderWhenStackUnknown()
    {
        const QVector<WindowInfo> windows{
            makeWindow(QStringLiteral("first"), QRectF(0, 0, 500, 500), -1),
            makeWindow(QStringLiteral("second"), QRectF(0, 0, 500, 500), -1),
        };
        // 栈序未知时列表里靠后的优先（KWin 列表按自下而上追加）
        QCOMPARE(pickWindowAt(windows, QPointF(10, 10)).id, QStringLiteral("second"));
    }

    void frameGeometryDecidesHitTest()
    {
        // 标题栏在 frame 里而不是 client 里：点标题栏也必须命中窗口
        WindowInfo info = makeWindow(QStringLiteral("w"), QRectF(100, 100, 400, 428), 0);
        info.clientGeometry = QRectF(100, 128, 400, 400);
        QVector<WindowInfo> windows{info};
        QCOMPARE(pickWindowAt(windows, QPointF(300, 110)).id, QStringLiteral("w"));
        QCOMPARE(pickWindowAt(windows, QPointF(300, 300)).id, QStringLiteral("w"));
        QVERIFY(!pickWindowAt(windows, QPointF(300, 600)).isValid());
    }

    void normalizesDragRectRegardlessOfDirection()
    {
        QCOMPARE(normalizeDragRect(QPointF(10, 10), QPointF(110, 60)), QRectF(10, 10, 100, 50));
        QCOMPARE(normalizeDragRect(QPointF(110, 60), QPointF(10, 10)), QRectF(10, 10, 100, 50));
        QCOMPARE(normalizeDragRect(QPointF(50, 50), QPointF(50, 50)), QRectF(50, 50, 0, 0));
    }

    void distinguishesClickFromDrag()
    {
        QVERIFY(!dragExceedsClickThreshold(QPointF(100, 100), QPointF(102, 103)));
        QVERIFY(dragExceedsClickThreshold(QPointF(100, 100), QPointF(120, 100)));
        QVERIFY(dragExceedsClickThreshold(QPointF(100, 100), QPointF(100, -120)));
    }
};

QTEST_MAIN(TestSelection)
#include "tst_selection.moc"
