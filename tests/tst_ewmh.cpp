// EWMH 属性解析（X11 后端的可测部分）
#include "backend/x11/Ewmh.h"

#include <QtTest>

using namespace seashot;

namespace {
QByteArray cardinals(std::initializer_list<quint32> values)
{
    QByteArray data;
    for (quint32 value : values) {
        data.append(char(value & 0xff));
        data.append(char((value >> 8) & 0xff));
        data.append(char((value >> 16) & 0xff));
        data.append(char((value >> 24) & 0xff));
    }
    return data;
}
} // namespace

class TestEwmh : public QObject
{
    Q_OBJECT

private slots:
    void parsesFrameExtents()
    {
        bool ok = false;
        const auto extents = ewmh::parseFrameExtents(cardinals({1, 2, 28, 3}), &ok);
        QVERIFY(ok);
        QCOMPARE(extents.left, 1);
        QCOMPARE(extents.right, 2);
        QCOMPARE(extents.top, 28);
        QCOMPARE(extents.bottom, 3);
    }

    void rejectsShortFrameExtents()
    {
        bool ok = true;
        ewmh::parseFrameExtents(QByteArray(8, '\0'), &ok);
        QVERIFY(!ok);
    }

    void frameRectExpandsClientByExtents()
    {
        ewmh::FrameExtents extents;
        extents.left = 1;
        extents.right = 1;
        extents.top = 28;
        extents.bottom = 1;
        // 客户区 600x400 @ (660,354) → 外框 602x429 @ (659,326)
        QCOMPARE(ewmh::frameRectFor(QRect(660, 354, 600, 400), extents),
                 QRectF(659, 326, 602, 429));
    }

    void parsesCardinalLists()
    {
        const QVector<quint32> ids = ewmh::parseCardinalList(cardinals({0x1000001, 0x1000002, 0x1000003}));
        QCOMPARE(ids.size(), 3);
        QCOMPARE(ids.at(0), 0x1000001u);
        QCOMPARE(ids.at(2), 0x1000003u);
        // 尾部不足 4 字节的残留要忽略
        QCOMPARE(ewmh::parseCardinalList(cardinals({7}) + QByteArray(2, '\0')).size(), 1);
    }

    void atomListLookup()
    {
        const QByteArray atoms = cardinals({100, 200, 300});
        QVERIFY(ewmh::atomListContains(atoms, 200));
        QVERIFY(!ewmh::atomListContains(atoms, 999));
        QVERIFY(!ewmh::atomListContains(atoms, 0));
    }

    void parsesPidAndNames()
    {
        QCOMPARE(ewmh::parsePid(cardinals({4242})), 4242);
        QCOMPARE(ewmh::parseUtf8String(QByteArray("访达")), QStringLiteral("访达"));
        QCOMPARE(ewmh::parsePid(QByteArray()), 0);
    }
};

QTEST_MAIN(TestEwmh)
#include "tst_ewmh.moc"
