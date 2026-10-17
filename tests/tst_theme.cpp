#include "ui/Theme.h"

#include <QPainter>
#include <QtTest>
#include <cmath>

using namespace seashot;

class TestTheme : public QObject
{
    Q_OBJECT

private slots:
    void tintedIconKeepsLogicalSize_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("100-percent") << qreal(1.0);
        QTest::newRow("125-percent") << qreal(1.25);
        QTest::newRow("150-percent") << qreal(1.5);
        QTest::newRow("200-percent") << qreal(2.0);
        QTest::newRow("250-percent") << qreal(2.5);
        QTest::newRow("300-percent") << qreal(3.0);
    }

    void tintedIconKeepsLogicalSize()
    {
        QFETCH(qreal, dpr);
        QPixmap source(256, 256);
        source.fill(Qt::white);
        const QColor tint("#3daeff");
        const QPixmap icon = ui::tintedIcon(QIcon(source), QSize(18, 18), dpr, tint);
        QVERIFY(!icon.isNull());
        QCOMPARE(icon.devicePixelRatio(), dpr);
        const int pixels = int(std::ceil(18 * dpr));
        QCOMPARE(icon.size(), QSize(pixels, pixels));
        QCOMPARE(icon.toImage().pixelColor(pixels / 2, pixels / 2), tint);

        // Paint exactly as the toolbar does: there must be no ink below/right
        // of its button, even when the display uses a fractional scale.
        QImage canvas(QSize(40 * dpr, 40 * dpr), QImage::Format_ARGB32_Premultiplied);
        canvas.setDevicePixelRatio(dpr);
        canvas.fill(Qt::transparent);
        {
            QPainter painter(&canvas);
            painter.drawPixmap(QRectF(6, 6, 18, 18), icon, QRectF(icon.rect()));
        }
        for (int y = int(30 * dpr); y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x)
                QCOMPARE(qAlpha(canvas.pixel(x, y)), 0);
        for (int x = int(30 * dpr); x < canvas.width(); ++x)
            for (int y = 0; y < canvas.height(); ++y)
                QCOMPARE(qAlpha(canvas.pixel(x, y)), 0);
    }

    void smallThemeIconStaysCentered()
    {
        QPixmap source(8, 4);
        source.fill(Qt::white);
        const QColor tint(Qt::red);
        const QImage icon = ui::tintedIcon(QIcon(source), QSize(18, 18), 2, tint).toImage();
        QCOMPARE(icon.size(), QSize(36, 36));
        QCOMPARE(icon.pixelColor(18, 18), tint);
        QCOMPARE(qAlpha(icon.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(icon.pixel(35, 35)), 0);
    }
};

QTEST_MAIN(TestTheme)
#include "tst_theme.moc"
