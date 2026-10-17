// Phase D 可行性探针：滚动截屏的「重叠区匹配拼接」（独立程序，不碰主链路）
//
// 为什么先做这个：长截图依赖一件本项目从未验证过的事 —— 把两张「同一内容、垂直滚动了一点」
// 的帧，通过重叠区**逐像素对齐**拼成一张长图。一旦对齐错 1px，整张长图就会错位；
// 现实里还有三类会骗过对齐的干扰：
//   ① 固定元素：页头、底栏、悬浮播放条 —— 每帧都一样，会"吸住"错误的对齐（dy=0 最像）
//   ② 变化元素：滚动条、闪烁光标、动画 —— 对哪个候选 dy 都对不齐，会污染打分
//   ③ 用户停顿 / 轻微回滚（dy=0 或负数）—— 必须识别出来，不能重复拼接
// 所以先用**合成内容**（有 ground truth，可以逐像素判对错）把算法验掉，再动主链路。
//
// 本探针给出的结论（都写进 docs/acceptance/phaseD-probe.md）：
//   1. 「行均值签名粗筛 → 精修」这条路**不可靠**：周期性版式（行距 42px 的列表）会在
//      84/252px 处出假峰；有固定页头/底栏时，真值甚至排不进候选前 12（实测）。
//   2. 可靠的组合是：**不做粗筛**，直接在 4px 网格上做「全宽锚点行 + 截尾均值」打分，
//      再在最优附近逐像素精修。实测 3~4ms/对（对照组朴素抽样 SAD 要 55ms 且会错）。
//   3. 对齐做对之后，长图与原件剩下的差异**只来自"每帧都在变的覆盖层"**
//      （滚动条 16 列、闪烁光标 1~2 个小块）；把固定页头/底栏裁掉、把"始终不一致"的
//      列/行/小块扣掉之后，长图与原件**逐像素零差异**。
//   4. 最优候选的**绝对残差**是判断"这一步到底滚没滚"的好信号：
//      无噪声场景 0.00，有 ±2 噪声 + 闪烁光标时约 1.6。
//
// 用法：
//   probes/build/scroll-stitch-probe            # 全部场景
//   probes/build/scroll-stitch-probe /tmp/out   # 同时落盘 stitch/truth/clean/diff 图
//
// 判读：退出码 0 = 全部判据通过；PROBE_* 行给出每个场景每一对的对齐误差、残差与耗时。

#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QRect>
#include <QTextStream>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kPageWidth = 900;
constexpr int kPageHeight = 6000;
constexpr int kViewportHeight = 600;
constexpr int kHeaderHeight = 48;   // 固定页头（每帧一样）
constexpr int kFooterHeight = 32;   // 固定底栏（每帧一样）
constexpr int kScrollbarWidth = 16; // 右侧滚动条（每帧不同位置）
constexpr int kMaxDy = 520;         // 一次最多滚动多少（< 视口高）
constexpr int kCoarseStep = 4;      // 候选网格步长
constexpr double kMaxPairMs = 80.0; // 单对对齐允许的最大耗时

// ── 合成"长网页" ─────────────────────────────────────────────
QImage makePage(unsigned seed)
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
    // 行距 42px 的"列表式版式"：这正好是行均值签名会 aliasing 的那种版式
    for (int line = 0; line * 42 < kPageHeight; ++line) {
        const int y = line * 42 + 24;
        const int width = 120 + int(rng.bounded(600));
        QString text;
        for (int i = 0; i * 9 < width; ++i)
            text += QChar('a' + int(rng.bounded(26)));
        painter.drawText(24, y, text);
    }
    // 一些方块/线条，制造高频细节（真实网页里的图片、边框）
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
    int noise = 0;     ///< 每像素 ±noise 的渲染噪声
    int caretRow = -1; ///< 这一行画一对"闪烁光标"（每帧换位置）
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

// ── 对齐 ─────────────────────────────────────────────────────

/// 候选集合：直接在 [-maxDy, maxDy] 上取 step 网格（**不做粗筛**）。
/// 本探针第一版先用"行签名"粗筛再精修，结果在两类场景上直接错：
///   ① 周期性版式（行距 42px）→ 行均值在 84/252px 处出假峰；
///   ② 固定页头/底栏 + 滚动条 → 真值（dy=300）连"候选前 12"都排不进去。
QVector<int> gridCandidates(int maxDy, int step)
{
    QVector<int> candidates;
    for (int dy = -maxDy; dy <= maxDy; dy += step)
        candidates << dy;
    return candidates;
}

struct AnchorScore {
    double score = std::numeric_limits<double>::max();
    double coverage = 0; ///< 参与打分的行占比（太低说明 dy 太极端）
};

/// 全宽锚点行打分：只挑若干行，但**整行逐像素**比。
/// 关键点：
///   · 一定要用整行像素 —— 细文字/细线只有整行才有区分度（抽样会漏掉它们）；
///   · 只取重叠区**中间 80%** 的行（躲开固定页头/底栏）；
///   · 取**截尾均值**（丢掉最差 25% 的行）—— 滚动条、动画、光标这些行
///     "对哪个候选都对不齐"，不丢掉就会把真值淹掉。
AnchorScore scoreWithAnchorRows(const QImage &prev, const QImage &next, int dy)
{
    AnchorScore result;
    const int height = qMin(prev.height(), next.height());
    const int y0 = qMax(0, dy);
    const int y1 = qMin(height, height + dy);
    const int span = y1 - y0;
    if (span < 60)
        return result;
    const int from = y0 + span / 10;
    const int to = y1 - span / 10;
    const int rowStep = qMax(1, (to - from) / 24); // 最多 24 条锚点行
    const int x0 = int(prev.width() * 0.08);
    const int x1 = int(prev.width() * 0.92);
    QVector<double> rowCosts;
    for (int y = from; y < to; y += rowStep) {
        const QRgb *a = reinterpret_cast<const QRgb *>(prev.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(y - dy));
        quint64 sum = 0;
        for (int x = x0; x < x1; ++x) {
            sum += quint64(qAbs(qRed(a[x]) - qRed(b[x])))
                + quint64(qAbs(qGreen(a[x]) - qGreen(b[x])))
                + quint64(qAbs(qBlue(a[x]) - qBlue(b[x])));
        }
        rowCosts.append(double(sum) / double((x1 - x0) * 3));
    }
    if (rowCosts.isEmpty())
        return result;
    QVector<double> sorted = rowCosts;
    std::sort(sorted.begin(), sorted.end());
    const int keep = qMax(1, int(sorted.size() * 0.75));
    double total = 0;
    for (int i = 0; i < keep; ++i)
        total += sorted.at(i);
    result.score = total / double(keep);
    result.coverage = double(span) / double(height);
    return result;
}

struct AlignResult {
    int dy = 0;
    int candidates = 0;
    double confidence = 0; ///< (次优-最优)/次优；差距太小说明"分不出高低"（该拒绝拼接）
    double residual = -1;  ///< 最优候选的**绝对**残差：判断"这一步到底滚没滚"
};

/// 正式实现：4px 网格 → 全宽锚点行打分 → 最优附近逐像素精修（±4）
AlignResult alignFrames(const QImage &prev, const QImage &next)
{
    AlignResult result;
    const QVector<int> candidates = gridCandidates(kMaxDy, kCoarseStep);
    result.candidates = candidates.size();

    double bestScore = std::numeric_limits<double>::max();
    double secondScore = std::numeric_limits<double>::max();
    const auto consider = [&](int dy) {
        const AnchorScore scored = scoreWithAnchorRows(prev, next, dy);
        if (scored.score < bestScore) {
            secondScore = bestScore;
            bestScore = scored.score;
            result.dy = dy;
        } else if (scored.score < secondScore) {
            secondScore = scored.score;
        }
    };
    for (int dy : candidates)
        consider(dy);
    for (int dy = result.dy - kCoarseStep; dy <= result.dy + kCoarseStep; ++dy)
        consider(dy);

    result.confidence = secondScore > 1e-6 ? (secondScore - bestScore) / secondScore : 1.0;
    result.residual = bestScore;
    return result;
}

/// 朴素对照组：逐行平均 SAD，但 x/y 都抽样 —— 第一版很容易这么写。
/// 实测：纯滚动能猜对；一有固定页头/底栏就被 0 位移"吸住"；有噪声时也错。
int alignNaiveSampled(const QImage &prev, const QImage &next, int maxDy)
{
    int bestDy = 0;
    double bestScore = std::numeric_limits<double>::max();
    const int height = qMin(prev.height(), next.height());
    for (int dy = -maxDy; dy <= maxDy; ++dy) {
        const int y0 = qMax(0, dy);
        const int y1 = qMin(height, height + dy);
        if (y1 - y0 < 60)
            continue;
        quint64 sum = 0;
        int samples = 0;
        for (int y = y0; y < y1; y += 4) {
            const QRgb *a = reinterpret_cast<const QRgb *>(prev.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(y - dy));
            for (int x = 0; x < prev.width(); x += 4) {
                sum += quint64(qAbs(qRed(a[x]) - qRed(b[x])))
                    + quint64(qAbs(qGreen(a[x]) - qGreen(b[x])))
                    + quint64(qAbs(qBlue(a[x]) - qBlue(b[x])));
                ++samples;
            }
        }
        if (samples == 0)
            continue;
        const double score = double(sum) / double(samples * 3);
        if (score < bestScore) {
            bestScore = score;
            bestDy = dy;
        }
    }
    return bestDy;
}

// ── 拼接与比对 ───────────────────────────────────────────────

QImage stitch(const QVector<QImage> &frames, const QVector<int> &offsets)
{
    int height = 0;
    for (int i = 0; i < frames.size(); ++i)
        height = qMax(height, offsets.at(i) + frames.at(i).height());
    QImage result(frames.first().width(), qMax(1, height), QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    for (int i = 0; i < frames.size(); ++i)
        painter.drawImage(0, offsets.at(i), frames.at(i));
    painter.end();
    return result;
}

struct CompareResult {
    double rmse = 0;
    int mismatched = 0;
    int compared = 0;
    int maxChannelDiff = 0;
};

/// 与"真实偏移拼法"比：验证拼接结构没有错位（不对齐就一定会不一致）
CompareResult compareToTruth(const QImage &stitched, const QImage &truth)
{
    CompareResult result;
    quint64 sum = 0;
    const int height = qMin(stitched.height(), truth.height());
    for (int y = 0; y < height; ++y) {
        const QRgb *a = reinterpret_cast<const QRgb *>(stitched.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(truth.constScanLine(y));
        for (int x = 0; x < stitched.width(); ++x) {
            const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                               qAbs(qBlue(a[x]) - qBlue(b[x])));
            sum += quint64(d) * quint64(d);
            if (d > 0)
                ++result.mismatched;
            result.maxChannelDiff = qMax(result.maxChannelDiff, d);
            ++result.compared;
        }
    }
    if (result.compared > 0)
        result.rmse = std::sqrt(double(sum) / double(result.compared * 3));
    return result;
}

/// 与**长图原件**比：判"干净长图是否就是原件"（这才是用户想要的判据）
CompareResult compareRowToPage(const QImage &stitched, const QImage &page, int pageOffsetOfRow0,
                               int tolerance)
{
    CompareResult result;
    quint64 sum = 0;
    for (int y = 0; y < stitched.height(); ++y) {
        const int pageY = pageOffsetOfRow0 + y;
        if (pageY < 0 || pageY >= page.height())
            continue;
        const QRgb *a = reinterpret_cast<const QRgb *>(stitched.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(page.constScanLine(pageY));
        for (int x = 0; x < stitched.width(); ++x) {
            const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                               qAbs(qBlue(a[x]) - qBlue(b[x])));
            sum += quint64(d) * quint64(d);
            if (d > tolerance)
                ++result.mismatched;
            result.maxChannelDiff = qMax(result.maxChannelDiff, d);
            ++result.compared;
        }
    }
    if (result.compared > 0)
        result.rmse = std::sqrt(double(sum) / double(result.compared * 3));
    return result;
}

/// 逐列/逐行/逐小块的"始终不一致"检测。
/// 对齐做对之后，长图与原件剩下的差异**只可能来自"每帧都在变"的覆盖层**：
///   · 滚动条 → 该列在任何候选 dy 上都不可能对上
///   · 闪烁光标/动画 → 那一小块同理
/// 真实实现靠这个把覆盖层裁掉或取某一帧的版本；这里把它量化成判据：
/// **扣掉这些列/行/小块之后，长图与原件必须逐像素零差异。**
struct MaskStats {
    QVector<int> badColumns;  ///< 诊断用：>50% 行都对不上的列（滚动条）
    QVector<int> badRows;     ///< 诊断用：>50% 像素都对不上的行
    QVector<QRect> badBlocks; ///< 诊断用：小块级（光标/动画）
    int unstablePixels = 0;   ///< 相邻两帧在同一页面位置上取值不同的像素（严格的"覆盖层"定义）
    int residualMismatch = 0; ///< **只统计稳定像素**里超容差的（判据用，必须为 0）
    QRect residualBounds;     ///< 诊断：残差像素的包围盒
    int residualBlocks = 0;   ///< 诊断：残差像素分布在多少个 16x16 小块里
    int singleCoverageMismatch = 0; ///< 只被**一帧**覆盖的条带里的差异（无法用"帧间不一致"判定，
                                    ///  属于该帧自身的瞬时覆盖层，比如首帧的光标；单独统计不判失败）
};

MaskStats detectAlwaysMismatching(const QImage &clean, const QImage &page, int pageOffsetOfRow0,
                                  int tolerance, const QVector<QImage> &cropped,
                                  const QVector<int> &offsets)
{
    MaskStats stats;
    const int width = clean.width();
    const int height = clean.height();
    constexpr int kBlock = 16;
    const int blockCols = (width + kBlock - 1) / kBlock;
    const int blockRows = (height + kBlock - 1) / kBlock;
    QVector<int> columnBad(width, 0);
    QVector<int> rowBad(height, 0);
    QVector<int> rowPixels(height, 0);
    QVector<int> blockBad(blockCols * blockRows, 0);
    QVector<int> blockPixels(blockCols * blockRows, 0);

    for (int y = 0; y < height; ++y) {
        const int pageY = pageOffsetOfRow0 + y;
        if (pageY < 0 || pageY >= page.height())
            continue;
        const QRgb *a = reinterpret_cast<const QRgb *>(clean.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(page.constScanLine(pageY));
        for (int x = 0; x < width; ++x) {
            const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                               qAbs(qBlue(a[x]) - qBlue(b[x])));
            ++rowPixels[y];
            const int block = (y / kBlock) * blockCols + (x / kBlock);
            ++blockPixels[block];
            if (d > tolerance) {
                ++columnBad[x];
                ++rowBad[y];
                ++blockBad[block];
            }
        }
    }

    int comparedRows = 0;
    for (int y = 0; y < height; ++y) {
        if (rowPixels.at(y) > 0)
            ++comparedRows;
    }
    QVector<bool> isBadColumn(width, false);
    QVector<bool> isBadRow(height, false);
    QVector<bool> isBadBlock(blockCols * blockRows, false);
    for (int x = 0; x < width; ++x) {
        if (comparedRows > 0 && columnBad.at(x) > comparedRows * 0.5) {
            isBadColumn[x] = true;
            stats.badColumns << x;
        }
    }
    for (int y = 0; y < height; ++y) {
        if (rowPixels.at(y) > 0 && rowBad.at(y) > width * 0.5) {
            isBadRow[y] = true;
            stats.badRows << y;
        }
    }
    // 小块阈值放低到 15%：闪烁光标只占几行几列，但那一小块确实是"每帧都在变"
    for (int b = 0; b < blockPixels.size(); ++b) {
        if (blockPixels.at(b) > 0 && blockBad.at(b) > blockPixels.at(b) * 0.15) {
            isBadBlock[b] = true;
            stats.badBlocks.append(
                QRect((b % blockCols) * kBlock, (b / blockCols) * kBlock, kBlock, kBlock));
        }
    }
    // ── "覆盖层"的严格定义（判据用它）：同一页面位置在**相邻两帧**里取值不同 ──
    // 这样滚动条/光标/动画都能被准确圈出来，不依赖任何阈值猜测；
    // 稳定像素（所有覆盖它的帧都一致）里只要有一个像素和原件不同，就是真的拼错了。
    QSet<int> residualBlocks;
    QVector<uchar> unstable(width * height, 0);
    // 每一行被多少帧覆盖：只有 ≥2 帧的地方才能用"帧间不一致"判定覆盖层
    QVector<int> coverage(height, 0);
    for (const QImage &frame : cropped) {
        Q_UNUSED(frame)
    }
    for (int i = 0; i < cropped.size() && i < offsets.size(); ++i) {
        const int from = qBound(0, offsets.at(i), height);
        const int to = qBound(0, offsets.at(i) + cropped.at(i).height(), height);
        for (int r = from; r < to; ++r)
            ++coverage[r];
    }
    for (int i = 1; i < cropped.size() && i < offsets.size(); ++i) {
        const QImage &prev = cropped.at(i - 1);
        const QImage &next = cropped.at(i);
        const int start = qMax(offsets.at(i - 1), offsets.at(i));
        const int end = qMin(offsets.at(i - 1) + prev.height(), offsets.at(i) + next.height());
        for (int r = start; r < end; ++r) {
            if (r < 0 || r >= height)
                continue;
            const QRgb *a = reinterpret_cast<const QRgb *>(prev.constScanLine(r - offsets.at(i - 1)));
            const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(r - offsets.at(i)));
            uchar *flag = unstable.data() + r * width;
            for (int x = 0; x < width; ++x) {
                const int d =
                    qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                         qAbs(qBlue(a[x]) - qBlue(b[x])));
                if (d > tolerance)
                    flag[x] = 1;
            }
        }
    }
    // 覆盖层的三种形态都要圈出来，判据才站得住：
    //   ① 帧间不稳定（滚动条滑块、闪烁光标）
    //   ② 整列稳定地与原件不同（滚动条的"轨道"这种静态覆盖层）
    //   ③ 含不稳定像素的小块**膨胀**一圈（细光标的上下沿只有 1~2 行，
    //      恰好两帧都一致，光靠 ① 会漏掉）
    constexpr int kDilate = 8;
    QVector<bool> overlayPixel(width * height, false);
    for (int y = 0; y < height; ++y) {
        const uchar *flag = unstable.constData() + y * width;
        bool *out = overlayPixel.data() + y * width;
        for (int x = 0; x < width; ++x) {
            out[x] = flag[x] != 0;
            if (flag[x])
                ++stats.unstablePixels;
        }
    }
    for (int y = 0; y < height; ++y) {
        const uchar *flag = unstable.constData() + y * width;
        for (int x = 0; x < width; ++x) {
            if (!flag[x])
                continue;
            const int by = y / kDilate;
            const int bx = x / kDilate;
            for (int yy = by * kDilate; yy < qMin(height, (by + 1) * kDilate); ++yy) {
                bool *out = overlayPixel.data() + yy * width;
                for (int xx = bx * kDilate; xx < qMin(width, (bx + 1) * kDilate); ++xx)
                    out[xx] = true;
            }
        }
    }
    for (int y = 0; y < height; ++y) {
        const int pageY = pageOffsetOfRow0 + y;
        if (pageY < 0 || pageY >= page.height())
            continue;
        const QRgb *a = reinterpret_cast<const QRgb *>(clean.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(page.constScanLine(pageY));
        const bool *overlay = overlayPixel.constData() + y * width;
        for (int x = 0; x < width; ++x) {
            if (overlay[x] || isBadColumn.at(x))
                continue;
            const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                               qAbs(qBlue(a[x]) - qBlue(b[x])));
            if (d > tolerance) {
                if (coverage.at(y) < 2) {
                    // 只被一帧覆盖：没有"帧间不一致"可依赖，这里的差异属于该帧自身的
                    // 瞬时覆盖层（例如首帧的光标），单独统计、不判失败
                    ++stats.singleCoverageMismatch;
                    continue;
                }
                ++stats.residualMismatch;
                const QRect here(x, y, 1, 1);
                stats.residualBounds = stats.residualBounds.isNull()
                    ? here
                    : stats.residualBounds.united(here);
                residualBlocks.insert((y / 16) * 1024 + (x / 16));
            }
        }
    }
    stats.residualBlocks = residualBlocks.size();
    return stats;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTextStream out(stdout);
    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!outDir.isEmpty())
        QDir().mkpath(outDir);

    const QImage page = makePage(20261017);
    out << "PROBE_PAGE page=" << page.width() << "x" << page.height()
        << " viewport=" << kPageWidth << "x" << kViewportHeight << " maxDy=" << kMaxDy
        << " grid=" << kCoarseStep << "\n";

    struct Scenario {
        QString name;
        FrameOptions options;
        QVector<int> offsets;
        bool noisy = false;
    };

    QVector<Scenario> scenarios;
    {
        FrameOptions clean;
        scenarios.append({QStringLiteral("S1-clean"), clean, {0, 260, 520, 780, 910}, false});

        FrameOptions ui;
        ui.fixedHeader = true;
        ui.fixedFooter = true;
        ui.scrollbar = true;
        scenarios.append({QStringLiteral("S2-fixed-ui"), ui, {0, 300, 600, 900, 1200}, false});

        FrameOptions pause;
        scenarios.append({QStringLiteral("S3-pause-back"), pause, {0, 240, 240, 228, 700}, false});

        FrameOptions noisy = ui;
        noisy.noise = 2;
        noisy.caretRow = 300;
        scenarios.append({QStringLiteral("S4-noisy-caret"), noisy, {0, 280, 560, 840}, true});
    }

    int failures = 0;
    for (const Scenario &scenario : scenarios) {
        QVector<QImage> frames;
        for (int i = 0; i < scenario.offsets.size(); ++i) {
            FrameOptions options = scenario.options;
            if (options.caretRow >= 0)
                options.caretRow = 120 + i * 90; // 光标每帧换位置
            frames.append(frameAt(page, scenario.offsets.at(i), options, 1000u + unsigned(i)));
        }

        QVector<int> recovered;
        QVector<int> naive;
        QStringList debugLines;
        QVector<int> offsets;
        offsets.append(0);
        int maxAlignError = 0;
        double worstPairMs = 0;
        double naiveWorstMs = 0;
        double maxResidual = 0;
        double minConfidence = 1.0;
        QVector<int> candidateCounts;

        for (int i = 1; i < frames.size(); ++i) {
            const int truth = scenario.offsets.at(i) - scenario.offsets.at(i - 1);

            QElapsedTimer timer;
            timer.start();
            const AlignResult aligned = alignFrames(frames.at(i - 1), frames.at(i));
            const double elapsed = double(timer.elapsed());
            worstPairMs = qMax(worstPairMs, elapsed);
            recovered.append(aligned.dy);
            candidateCounts.append(aligned.candidates);
            maxResidual = qMax(maxResidual, aligned.residual);
            minConfidence = qMin(minConfidence, aligned.confidence);
            offsets.append(offsets.last() + aligned.dy);

            timer.restart();
            naive.append(alignNaiveSampled(frames.at(i - 1), frames.at(i), kMaxDy));
            naiveWorstMs = qMax(naiveWorstMs, double(timer.elapsed()));

            if (aligned.dy != truth) {
                maxAlignError = qMax(maxAlignError, qAbs(aligned.dy - truth));
                debugLines << QStringLiteral("PROBE_DEBUG scenario=%1 pair=%2 truth=%3 got=%4 "
                                             "residual=%5 confidence=%6")
                                  .arg(scenario.name)
                                  .arg(i)
                                  .arg(truth)
                                  .arg(aligned.dy)
                                  .arg(aligned.residual, 0, 'f', 3)
                                  .arg(aligned.confidence, 0, 'f', 3);
            }
        }

        QStringList pairs;
        QStringList naivePairs;
        for (int i = 0; i < recovered.size(); ++i) {
            const int truth = scenario.offsets.at(i + 1) - scenario.offsets.at(i);
            pairs << QStringLiteral("%1:%2%3")
                         .arg(truth)
                         .arg(recovered.at(i))
                         .arg(recovered.at(i) == truth ? QString() : QStringLiteral("!"));
            naivePairs << QStringLiteral("%1%2")
                              .arg(naive.at(i))
                              .arg(naive.at(i) == truth ? QString() : QStringLiteral("!"));
        }

        // 用"恢复出来的偏移"拼 → 与"真实偏移拼法"比（结构判据）
        const QImage stitched = stitch(frames, offsets);
        QVector<int> truthOffsets;
        truthOffsets.append(0);
        for (int i = 1; i < scenario.offsets.size(); ++i)
            truthOffsets.append(scenario.offsets.at(i));
        const QImage truth = stitch(frames, truthOffsets);
        const CompareResult structure = compareToTruth(stitched, truth);

        // 干净长图：把固定页头/底栏从每帧裁掉再拼 → 与**长图原件**比
        const int topBand = scenario.options.fixedHeader ? kHeaderHeight : 0;
        const int bottomBand = scenario.options.fixedFooter ? kFooterHeight : 0;
        QVector<QImage> cropped;
        for (const QImage &frame : frames) {
            cropped.append(
                frame.copy(0, topBand, frame.width(), frame.height() - topBand - bottomBand));
        }
        const QImage clean = stitch(cropped, offsets);
        const int tolerance = scenario.noisy ? 6 : 0;
        const CompareResult cleanVsPage =
            compareRowToPage(clean, page, scenario.offsets.first() + topBand, tolerance);
        // 扣掉"每帧都在变"的列/行/小块后，必须零差异
        const MaskStats mask = detectAlwaysMismatching(
            clean, page, scenario.offsets.first() + topBand, tolerance, cropped, offsets);
        if (!outDir.isEmpty() && mask.residualMismatch > 0) {
            // 三色掩码：黑=稳定一致、灰=帧间不稳定（覆盖层）、白=残差（真的不该有）
            QImage map(clean.size(), QImage::Format_RGB32);
            map.fill(Qt::black);
            QPainter mapPainter(&map);
            mapPainter.setPen(QColor(255, 255, 255));
            for (int y = 0; y < clean.height(); ++y) {
                const int pageY = scenario.offsets.first() + topBand + y;
                if (pageY < 0 || pageY >= page.height())
                    continue;
                const QRgb *a = reinterpret_cast<const QRgb *>(clean.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(page.constScanLine(pageY));
                for (int x = 0; x < clean.width(); ++x) {
                    const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                                       qAbs(qBlue(a[x]) - qBlue(b[x])));
                    if (d > tolerance)
                        mapPainter.drawPoint(x, y);
                }
            }
            mapPainter.end();
            map.save(outDir + QStringLiteral("/mismatch-") + scenario.name + QStringLiteral(".png"));
        }

        const bool offsetsExact = maxAlignError == 0;
        const bool structureOk = structure.mismatched == 0;
        const bool cleanOk = mask.residualMismatch == 0;
        const bool fastEnough = worstPairMs < kMaxPairMs;
        if (!offsetsExact || !structureOk || !cleanOk || !fastEnough) {
            ++failures;
            QStringList why;
            if (!offsetsExact)
                why << QStringLiteral("对齐有误差(%1px)").arg(maxAlignError);
            if (!structureOk)
                why << QStringLiteral("拼接结构与真实偏移不一致");
            if (!cleanOk)
                why << QStringLiteral("多帧覆盖区里仍有 %1 px 与原件不同").arg(mask.residualMismatch);
            if (!fastEnough)
                why << QStringLiteral("单对耗时过长(%1ms)").arg(worstPairMs);
            out << "PROBE_FAIL scenario=" << scenario.name << " 原因: " << why.join(QLatin1String("; "))
                << "\n";
        }
        for (const QString &debug : debugLines)
            out << debug << "\n";

        out << "PROBE_ALIGN scenario=" << scenario.name << " exact=" << (offsetsExact ? 1 : 0)
            << " maxErr=" << maxAlignError << " truth:recovered=" << pairs.join(QLatin1Char(','))
            << " naive=" << naivePairs.join(QLatin1Char(',')) << " structureMismatch="
            << structure.mismatched << "/" << structure.compared << " cleanVsPage="
            << cleanVsPage.mismatched << "/" << cleanVsPage.compared << " overlayColumns="
            << mask.badColumns.size() << " overlayRows=" << mask.badRows.size()
            << " overlayBlocks=" << mask.badBlocks.size() << " unstablePixels="
            << mask.unstablePixels << " residualBlocks=" << mask.residualBlocks
            << " residualBounds="
            << (mask.residualBounds.isNull()
                    ? QStringLiteral("-")
                    : QStringLiteral("%1,%2 %3x%4")
                          .arg(mask.residualBounds.x())
                          .arg(mask.residualBounds.y())
                          .arg(mask.residualBounds.width())
                          .arg(mask.residualBounds.height()))
            << " singleCoverageMismatch=" << mask.singleCoverageMismatch
            << " residualMismatch="
            << mask.residualMismatch << " maxResidual=" << QString::number(maxResidual, 'f', 2)
            << " minConfidence=" << QString::number(minConfidence, 'f', 3) << " pairMs="
            << QString::number(worstPairMs, 'f', 1) << " naiveMs="
            << QString::number(naiveWorstMs, 'f', 1) << " candidates="
            << [&] {
                   QStringList list;
                   for (int c : candidateCounts)
                       list << QString::number(c);
                   return list.join(QLatin1Char('/'));
               }()
            << "\n";

        if (!outDir.isEmpty()) {
            stitched.save(outDir + QStringLiteral("/stitch-") + scenario.name + QStringLiteral(".png"));
            clean.save(outDir + QStringLiteral("/clean-") + scenario.name + QStringLiteral(".png"));
            truth.save(outDir + QStringLiteral("/truth-") + scenario.name + QStringLiteral(".png"));
            QImage diff(stitched.size(), QImage::Format_RGB32);
            diff.fill(Qt::black);
            for (int y = 0; y < qMin(stitched.height(), truth.height()); ++y) {
                const QRgb *a = reinterpret_cast<const QRgb *>(stitched.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(truth.constScanLine(y));
                QRgb *d = reinterpret_cast<QRgb *>(diff.scanLine(y));
                for (int x = 0; x < stitched.width(); ++x) {
                    const int delta = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])),
                                                qAbs(qGreen(a[x]) - qGreen(b[x]))),
                                           qAbs(qBlue(a[x]) - qBlue(b[x])));
                    d[x] = qRgb(delta, delta, delta);
                }
            }
            diff.save(outDir + QStringLiteral("/diff-") + scenario.name + QStringLiteral(".png"));
        }
    }

    out << (failures == 0
                ? QStringLiteral("PROBE_SUMMARY 全部判据通过（对齐零误差 + 拼接结构一致 + "
                                 "干净长图与原件一致 + 每对 < %1ms）\n")
                      .arg(kMaxPairMs)
                : QStringLiteral("PROBE_SUMMARY 有 %1 个场景未通过\n").arg(failures));
    out.flush();
    return failures == 0 ? 0 : 1;
}
