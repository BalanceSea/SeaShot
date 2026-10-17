// ScrollStitcher（滚动截屏的重叠区匹配拼接）单元测试
//
// 用例直接搬自 `probes/scroll-stitch-probe.cpp` 的四个场景（那个探针已经用合成内容 +
// ground truth 验过算法，见 `docs/acceptance/phaseD-probe.md`）：
//   S1 纯滚动 / S2 固定页头+底栏+滚动条 / S3 停顿与回滚 / S4 渲染噪声+闪烁光标
// 探针里是"跑完比像素"，这里改成"喂给产品的 ScrollStitcher，断言它的判决与产出"。
#include "scroll/ScrollStitcher.h"

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QtTest>

using namespace seashot;

namespace {

constexpr int kPageWidth = 900;
constexpr int kPageHeight = 6000;
constexpr int kViewportHeight = 600;
constexpr int kHeaderHeight = 48;
constexpr int kFooterHeight = 32;
constexpr int kScrollbarWidth = 16;

/// 合成"长网页"：行距 42px 的列表式版式（正是行均值签名会 aliasing 的那种）
QImage makePage(unsigned seed = 20261017u)
{
    QImage page(kPageWidth, kPageHeight, QImage::Format_RGB32);
    QPainter painter(&page);
    QLinearGradient gradient(0, 0, 0, kPageHeight);
    gradient.setColorAt(0.0, QColor(248, 249, 252));
    gradient.setColorAt(1.0, QColor(232, 236, 244));
    painter.fillRect(page.rect(), gradient);
    painter.setPen(QColor(30, 34, 44));
    QFont font = painter.font();
    font.setPixelSize(15);
    painter.setFont(font);
    QRandomGenerator rng(seed);
    for (int line = 0; line * 42 < kPageHeight; ++line) {
        const int y = line * 42 + 24;
        const int width = 120 + int(rng.bounded(600));
        QString text;
        for (int i = 0; i * 9 < width; ++i)
            text += QChar('a' + int(rng.bounded(26)));
        painter.drawText(24, y, text);
    }
    for (int i = 0; i < 120; ++i) {
        const int x = int(rng.bounded(kPageWidth - 60));
        const int y = int(rng.bounded(kPageHeight - 40));
        painter.fillRect(x, y, 20 + int(rng.bounded(120)), 8 + int(rng.bounded(30)),
                         QColor(int(rng.bounded(200)), int(rng.bounded(200)),
                                int(rng.bounded(200))));
    }
    return page;
}

struct FrameOptions {
    bool fixedHeader = false;
    bool fixedFooter = false;
    bool scrollbar = false;
    int noise = 0;
    int caretRow = -1;
};

QImage frameAt(const QImage &page, int offset, const FrameOptions &options, unsigned seed)
{
    QImage frame =
        page.copy(0, qBound(0, offset, kPageHeight - kViewportHeight), kPageWidth, kViewportHeight)
            .convertToFormat(QImage::Format_RGB32);
    QPainter painter(&frame);
    if (options.fixedHeader)
        painter.fillRect(0, 0, kPageWidth, kHeaderHeight, QColor(64, 96, 160));
    if (options.fixedFooter)
        painter.fillRect(0, kViewportHeight - kFooterHeight, kPageWidth, kFooterHeight,
                         QColor(40, 44, 52));
    if (options.scrollbar) {
        const int barHeight = 80;
        const int travel = kViewportHeight - barHeight - 8;
        const int y = 4 + int(double(offset) / double(kPageHeight - kViewportHeight) * travel);
        painter.fillRect(kPageWidth - kScrollbarWidth, 0, kScrollbarWidth, kViewportHeight,
                         QColor(226, 229, 235));
        painter.fillRect(kPageWidth - kScrollbarWidth, y, kScrollbarWidth, barHeight,
                         QColor(120, 126, 138));
    }
    if (options.caretRow >= 0) {
        painter.fillRect(kPageWidth / 2 - 40, options.caretRow, 3, 18, QColor(20, 20, 20));
        painter.fillRect(kPageWidth / 2 + 40, options.caretRow, 3, 18, QColor(20, 20, 20));
    }
    painter.end();
    if (options.noise > 0) {
        QRandomGenerator rng(seed);
        for (int y = 0; y < frame.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(frame.scanLine(y));
            for (int x = 0; x < frame.width(); ++x) {
                const int n = int(rng.bounded(2 * options.noise + 1)) - options.noise;
                line[x] = qRgb(qBound(0, qRed(line[x]) + n, 255),
                               qBound(0, qGreen(line[x]) + n, 255),
                               qBound(0, qBlue(line[x]) + n, 255));
            }
        }
    }
    return frame;
}

/// 逐像素比（容忍 tolerance），返回超容差的像素数
int countMismatch(const QImage &a, const QImage &b, int tolerance)
{
    int mismatched = 0;
    for (int y = 0; y < a.height() && y < b.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width() && x < b.width(); ++x) {
            const int d = qMax(qMax(qAbs(qRed(pa[x]) - qRed(pb[x])), qAbs(qGreen(pa[x]) - qGreen(pb[x]))),
                               qAbs(qBlue(pa[x]) - qBlue(pb[x])));
            if (d > tolerance)
                ++mismatched;
        }
    }
    return mismatched;
}

} // namespace

class TestScrollStitcher : public QObject
{
    Q_OBJECT

private slots:
    // ---------- 对齐 ----------
    void alignRecoversExactOffsets()
    {
        const QImage page = makePage();
        // 纯滚动：260 / 520 / 780 / 910
        const QVector<int> offsets{0, 260, 520, 780, 910};
        for (int i = 1; i < offsets.size(); ++i) {
            const QImage prev = frameAt(page, offsets.at(i - 1), FrameOptions(), 1u);
            const QImage next = frameAt(page, offsets.at(i), FrameOptions(), 2u);
            const ScrollAlign aligned = ScrollStitcher::align(prev, next, 520);
            QCOMPARE(aligned.dy, offsets.at(i) - offsets.at(i - 1));
            QVERIFY(aligned.residual < 0.01);
        }
    }

    void alignSurvivesFixedUiAndNoise()
    {
        const QImage page = makePage();
        FrameOptions options;
        options.fixedHeader = true;
        options.fixedFooter = true;
        options.scrollbar = true;
        options.noise = 2;
        options.caretRow = 300;
        // 固定页头/底栏 + 滚动条 + ±2 噪声（这是朴素抽样 SAD 会错的那一档）
        for (int i = 1; i <= 3; ++i) {
            const QImage prev = frameAt(page, (i - 1) * 280, options, 100u + unsigned(i));
            const QImage next = frameAt(page, i * 280, options, 200u + unsigned(i));
            const ScrollAlign aligned = ScrollStitcher::align(prev, next, 520);
            QCOMPARE(aligned.dy, 280);
            QVERIFY2(aligned.residual < 6.0, qPrintable(QString::number(aligned.residual)));
        }
    }

    // ---------- 累积 / 判决 ----------
    void absorbRecognisesPauseBackAndTall()
    {
        const QImage page = makePage();
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, FrameOptions(), 1u));
        QCOMPARE(stitcher.absorb(frameAt(page, 240, FrameOptions(), 2u)).verdict,
                 ScrollStitcher::Verdict::Absorbed);
        QCOMPARE(stitcher.absorb(frameAt(page, 240, FrameOptions(), 3u)).verdict,
                 ScrollStitcher::Verdict::NoMovement); // 用户停顿
        QCOMPARE(stitcher.absorb(frameAt(page, 228, FrameOptions(), 4u)).verdict,
                 ScrollStitcher::Verdict::ScrolledBack); // 轻微回滚
        QCOMPARE(stitcher.absorb(frameAt(page, 700, FrameOptions(), 5u)).verdict,
                 ScrollStitcher::Verdict::Absorbed);
        // 回滚之后的位置必须是对的：长图应当覆盖到 700 + 视口高
        QCOMPARE(stitcher.height(), 700 + kViewportHeight);
        QCOMPARE(stitcher.scrolledBackCount(), 1);
        QCOMPARE(stitcher.noMovementCount(), 1);
    }

    void absorbRejectsFrameWithHugeResidual()
    {
        const QImage page = makePage();
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, FrameOptions(), 1u));
        // 完全无关的一帧（模拟平滑滚动中间帧/动画帧）：残差会很大 → 拒绝
        QImage garbage(kPageWidth, kViewportHeight, QImage::Format_RGB32);
        garbage.fill(QColor(10, 200, 30));
        QCOMPARE(stitcher.absorb(garbage).verdict, ScrollStitcher::Verdict::Rejected);
        QCOMPARE(stitcher.rejectedCount(), 1);
        // 被拒绝的帧不影响后续：下一张正常帧仍然能正确并入
        QCOMPARE(stitcher.absorb(frameAt(page, 300, FrameOptions(), 2u)).verdict,
                 ScrollStitcher::Verdict::Absorbed);
        QCOMPARE(stitcher.totalDy(), 300);
    }

    // ---------- 产出：固定条带 / 滚动条列 / 内容一致 ----------
    void imageCropsFixedBandsAndScrollbarThenMatchesPage()
    {
        const QImage page = makePage();
        FrameOptions options;
        options.fixedHeader = true;
        options.fixedFooter = true;
        options.scrollbar = true;
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, options, 1u));
        for (int i = 1; i <= 4; ++i)
            stitcher.absorb(frameAt(page, i * 300, options, 100u + unsigned(i)));

        QCOMPARE(stitcher.fixedTopRows(), kHeaderHeight);
        QCOMPARE(stitcher.fixedBottomRows(), kFooterHeight);
        QCOMPARE(stitcher.edgeCropColumns().second, kScrollbarWidth);
        QCOMPARE(stitcher.edgeCropColumns().first, 0);

        const QImage stitched = stitcher.image();
        // 裁掉固定页头之后：长图第 r 行 == 原件第 r+48 行；宽度少了滚动条那 16 列
        QCOMPARE(stitched.width(), kPageWidth - kScrollbarWidth);
        QCOMPARE(stitched.height(), 1200 + kViewportHeight - kHeaderHeight - kFooterHeight);
        const QImage expected = page.copy(0, kHeaderHeight, kPageWidth - kScrollbarWidth,
                                         stitched.height());
        QCOMPARE(countMismatch(stitched, expected, 0), 0); // 逐像素一致
    }

    void imageKeepsContentWhenNoFixedUi()
    {
        const QImage page = makePage();
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, FrameOptions(), 1u));
        for (int i = 1; i <= 3; ++i)
            stitcher.absorb(frameAt(page, i * 260, FrameOptions(), 100u + unsigned(i)));
        QCOMPARE(stitcher.fixedTopRows(), 0);
        QCOMPARE(stitcher.edgeCropColumns().first, 0);
        QCOMPARE(stitcher.edgeCropColumns().second, 0);
        const QImage stitched = stitcher.image();
        QCOMPARE(stitched.width(), kPageWidth);
        QCOMPARE(stitched.height(), 780 + kViewportHeight);
        const QImage expected = page.copy(0, 0, kPageWidth, stitched.height());
        QCOMPARE(countMismatch(stitched, expected, 0), 0);
    }

    void imageToleratesNoiseButNoMisalignment()
    {
        const QImage page = makePage();
        FrameOptions options;
        options.fixedHeader = true;
        options.fixedFooter = true;
        options.scrollbar = true;
        options.noise = 2;
        options.caretRow = 200;
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, options, 1u));
        for (int i = 1; i <= 3; ++i)
            stitcher.absorb(frameAt(page, i * 280, options, 100u + unsigned(i)));

        const QImage stitched = stitcher.image();
        const QImage expected = page.copy(0, kHeaderHeight, kPageWidth - kScrollbarWidth,
                                         stitched.height());
        // 噪声 ±2、容差 6：除了闪烁光标那几条（每帧都有、位置还变），应当一致
        const int mismatched = countMismatch(stitched, expected, 6);
        const int caretArea = 4 * 2 * 3 * 18; // 4 帧 × 2 个标记 × 3x18
        QVERIFY2(mismatched <= caretArea * 2,
                 qPrintable(QStringLiteral("mismatched=%1 caretArea=%2").arg(mismatched).arg(caretArea)));
    }

    void heightLimitStopsGrowth()
    {
        const QImage page = makePage();
        ScrollStitcher::Config config;
        config.maxHeight = 700; // 故意设很小
        ScrollStitcher stitcher(config);
        stitcher.start(frameAt(page, 0, FrameOptions(), 1u));
        stitcher.absorb(frameAt(page, 100, FrameOptions(), 2u));
        const ScrollStitcher::AbsorbResult result = stitcher.absorb(frameAt(page, 400, FrameOptions(), 3u));
        QCOMPARE(result.verdict, ScrollStitcher::Verdict::TooTall);
    }

    void reportMentionsBandsAndCrops()
    {
        const QImage page = makePage();
        FrameOptions options;
        options.fixedHeader = true;
        options.fixedFooter = true;
        options.scrollbar = true;
        ScrollStitcher stitcher;
        stitcher.start(frameAt(page, 0, options, 1u));
        for (int i = 1; i <= 2; ++i)
            stitcher.absorb(frameAt(page, i * 300, options, 100u + unsigned(i)));
        const QString report = stitcher.report();
        QVERIFY(report.contains(QStringLiteral("固定页头=48")));
        QVERIFY(report.contains(QStringLiteral("固定底栏=32")));
        QVERIFY(report.contains(QStringLiteral("左右裁列=0/16")));
    }
};

QTEST_MAIN(TestScrollStitcher)
#include "tst_scrollstitcher.moc"
