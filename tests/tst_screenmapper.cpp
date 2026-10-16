// 坐标转换层：逻辑 ↔ 物理 的唯一转换点（scale≠1 与多输出用合成用例覆盖）
#include "geometry/ScreenMapper.h"

#include <QtTest>

using namespace seashot;

class TestScreenMapper : public QObject
{
    Q_OBJECT

private slots:
    void roundingIsSymmetric()
    {
        QCOMPARE(ScreenMapper::roundCoord(10.4), 10);
        QCOMPARE(ScreenMapper::roundCoord(10.5), 11);
        QCOMPARE(ScreenMapper::roundCoord(-10.4), -10);
        QCOMPARE(ScreenMapper::roundCoord(-10.5), -11);
        QCOMPARE(ScreenMapper::roundCoord(536.582963768418), 537);
    }

    void scaleOneIsIdentity()
    {
        ScreenMapper mapper;
        mapper.setOutputs({{QStringLiteral("DP-3"), QRect(0, 0, 1920, 1080), 1.0}});
        const OutputInfo *output = mapper.outputAt(QPointF(100, 100));
        QVERIFY(output);
        QCOMPARE(mapper.toPhysical(QRectF(10, 20, 300, 400), *output), QRect(10, 20, 300, 400));
        QCOMPARE(mapper.toLogical(QRect(10, 20, 300, 400), *output), QRectF(10, 20, 300, 400));
    }

    void fractionalScaleScalesSizesAndOffsets()
    {
        ScreenMapper mapper;
        mapper.setOutputs({{QStringLiteral("DP-1"), QRect(0, 0, 1536, 864), 1.25}});
        const OutputInfo *output = mapper.outputAt(QPointF(10, 10));
        QVERIFY(output);
        // 逻辑 (100,100) 在 scale=1.25 下对应物理 (125,125)
        const QRect physical = mapper.toPhysical(QRectF(100, 100, 200, 200), *output);
        QCOMPARE(physical, QRect(125, 125, 250, 250));
        // 反向必须回到原点
        QCOMPARE(mapper.toLogical(physical, *output), QRectF(100, 100, 200, 200));
        QCOMPARE(mapper.toLogicalSize(QSize(250, 250), *output), QSizeF(200, 200));
    }

    void handlesOffsetsAndNegativeOrigins()
    {
        ScreenMapper mapper;
        mapper.setOutputs({{QStringLiteral("HDMI-1"), QRect(-1920, 0, 1920, 1080), 1.0},
                           {QStringLiteral("DP-3"), QRect(0, 0, 2560, 1440), 1.5}});
        const OutputInfo *left = mapper.outputAt(QPointF(-100, 100));
        const OutputInfo *right = mapper.outputAt(QPointF(100, 100));
        QVERIFY(left && right);
        QCOMPARE(left->name, QStringLiteral("HDMI-1"));
        QCOMPARE(right->name, QStringLiteral("DP-3"));
        // 物理坐标是「相对该输出左上角」的
        QCOMPARE(mapper.toPhysical(QRectF(-1870, 50, 100, 100), *left), QRect(50, 50, 100, 100));
        QCOMPARE(mapper.toPhysical(QRectF(100, 100, 100, 100), *right), QRect(150, 150, 150, 150));
    }

    void outputLookupFallsBackToFirst()
    {
        ScreenMapper mapper;
        mapper.setOutputs({{QStringLiteral("DP-3"), QRect(0, 0, 1920, 1080), 1.0}});
        QVERIFY(mapper.outputAt(QPointF(5000, 5000)) != nullptr);
        QVERIFY(mapper.outputByName(QStringLiteral("DP-3")) != nullptr);
        QVERIFY(mapper.outputByName(QStringLiteral("NOPE")) == nullptr);
    }

    void emptyMapperIsSafe()
    {
        ScreenMapper mapper;
        QVERIFY(mapper.outputAt(QPointF(1, 1)) == nullptr);
        mapper.setOutputs({});
        QVERIFY(mapper.outputs().isEmpty());
    }
};

QTEST_MAIN(TestScreenMapper)
#include "tst_screenmapper.moc"
