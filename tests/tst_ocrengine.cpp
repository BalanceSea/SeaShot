// OcrEngine（Phase E）单元测试
//
// 注意：这是**外部依赖**（tesseract + 语言包）。没装就 QSKIP，不判失败 ——
// 这样在别的机器上跑 ctest 也不会红，但本机装了就能真正验到识别结果。
#include "ocr/OcrEngine.h"

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QtTest>

using namespace seashot;

namespace {

/// 画一张"白底黑字"的图：OCR 最理想的输入，用来判"识别对不对"
QImage renderText(const QString &text, int pointsize = 64, const QString &family = QString())
{
    QImage image(1400, 200, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QFont font;
    if (!family.isEmpty())
        font.setFamily(family);
    font.setPixelSize(pointsize);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(QRect(30, 0, 1340, 200), Qt::AlignLeft | Qt::AlignVCenter, text);
    painter.end();
    return image;
}

/// 去掉空白与常见标点：中文 OCR 会在字之间插空格，比较前先规整
QString normalize(const QString &text)
{
    QString out;
    for (const QChar &c : text)
        if (!c.isSpace() && !QStringLiteral("，。！？、,.!?;:").contains(c))
            out += c;
    return out;
}

} // namespace

class TestOcrEngine : public QObject
{
    Q_OBJECT

private slots:
    void reportsAvailability()
    {
        QStringList languages;
        QString error;
        const bool ok = OcrEngine::available(&languages, &error);
        if (!ok)
            QSKIP("本机没有 tesseract，跳过（装了才能验识别结果）");
        QVERIFY2(!languages.isEmpty(), qPrintable(error));
        qInfo() << "OCR 语言包:" << languages.join(QLatin1Char(','));
        QVERIFY(languages.contains(QLatin1String("eng"))); // 英文包是基本要求
    }

    void recognizesEnglishText()
    {
        QStringList languages;
        if (!OcrEngine::available(&languages, nullptr) || !languages.contains(QLatin1String("eng")))
            QSKIP("没有 tesseract/eng，跳过");
        const QImage image = renderText(QStringLiteral("Hello World OCR test 12345"));
        const OcrResult result = OcrEngine::recognize(image, {QStringLiteral("eng")});
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY2(normalize(result.text).contains(QLatin1String("HelloWorld")),
                 qPrintable(result.text));
        QVERIFY2(result.text.contains(QLatin1String("12345")), qPrintable(result.text));
        QVERIFY(result.elapsedMs >= 0);
        QCOMPARE(result.lines.size(), 1);
    }

    void recognizesChineseWhenAvailable()
    {
        QStringList languages;
        if (!OcrEngine::available(&languages, nullptr) || !languages.contains(QLatin1String("chi_sim")))
            QSKIP("没有 chi_sim 语言包，跳过");
        const QImage image = renderText(QStringLiteral("你好世界这是识别测试"), 72);
        const OcrResult result = OcrEngine::recognize(image, {QStringLiteral("chi_sim")});
        QVERIFY2(result.ok, qPrintable(result.error));
        const QString got = normalize(result.text);
        // 允许个别字错，但至少要对上一大半（合成图通常全对）
        int hits = 0;
        for (const QChar &c : QStringLiteral("你好世界这是识别测试"))
            if (got.contains(c))
                ++hits;
        QVERIFY2(hits >= 7, qPrintable(QStringLiteral("识别=%1 命中=%2/10").arg(got).arg(hits)));
    }

    void emptyImageFailsWithMessage()
    {
        const OcrResult result = OcrEngine::recognize(QImage(), {QStringLiteral("eng")});
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("空图像")));
    }

    void englishOnlyStillWorksWhenChineseMissing()
    {
        // 语言解析：显式给 eng 时不应因为缺 chi_sim 而失败
        QStringList languages;
        if (!OcrEngine::available(&languages, nullptr) || !languages.contains(QLatin1String("eng")))
            QSKIP("没有 tesseract/eng，跳过");
        const QImage image = renderText(QStringLiteral("SeaShot"), 72);
        const OcrResult result = OcrEngine::recognize(image, {QStringLiteral("eng")});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.languages, QStringLiteral("eng"));
    }
};

QTEST_MAIN(TestOcrEngine)
#include "tst_ocrengine.moc"
