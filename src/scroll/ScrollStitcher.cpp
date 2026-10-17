#include "scroll/ScrollStitcher.h"

#include "util/Log.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seashot {

namespace {
constexpr int kCoarseStep = 4;      ///< 候选网格步长（探针实测 4px 足够）
constexpr int kRefineRadius = 4;    ///< 最优候选附近逐像素精修半径
constexpr int kAnchorRows = 24;     ///< 每次打分最多用多少条锚点行
constexpr double kTrimRatio = 0.75; ///< 截尾均值：只保留最一致的 75% 行
// "固定行/稳定列"判定的像素容差：要放得下渲染噪声（实测 ±2/帧 → 两帧差可达 4），
// 但真正的滚动内容每秒都在变，远大于这个容差，不会误判
constexpr double kBandTolerance = 6;
// 一行里允许有这么多比例的像素对不上，仍然算"固定行"。
// 为什么不能要求"逐像素全等"：滚动条的滑块就压在顶部几行上，它每帧都在动 ——
// 若要求全等，「固定页头」最多只能认到 4 行（实测就是这么翻车的）
constexpr double kBandMismatchRatio = 0.01;
constexpr double kColumnUnstable = 0.10; ///< 列不一致率超过它就可能是滚动条
constexpr double kColumnBaseline = 4.0;  ///< 同时还要比"内容列的中位数"高这么多倍
} // namespace

// ── 对齐 ─────────────────────────────────────────────────────
ScrollAlign ScrollStitcher::align(const QImage &prev, const QImage &next, int maxDy)
{
    ScrollAlign result;
    if (prev.isNull() || next.isNull() || prev.width() != next.width())
        return result;

    const int height = qMin(prev.height(), next.height());
    const int search = qBound(8, maxDy, qMax(8, height - 40));
    // 候选：直接在 [-search, search] 上取网格（**不做粗筛**，见头文件里的结论）
    QVector<int> candidates;
    for (int dy = -search; dy <= search; dy += kCoarseStep)
        candidates << dy;
    result.candidates = candidates.size();

    // 打分：全宽锚点行（中间 80% 的行、整行逐像素、截尾均值）
    const auto score = [&](int dy, double *coverageOut) {
        const int y0 = qMax(0, dy);
        const int y1 = qMin(height, height + dy);
        const int span = y1 - y0;
        if (span < 60)
            return std::numeric_limits<double>::max();
        const int from = y0 + span / 10;
        const int to = y1 - span / 10;
        const int rowStep = qMax(1, (to - from) / kAnchorRows);
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
            return std::numeric_limits<double>::max();
        std::sort(rowCosts.begin(), rowCosts.end());
        const int keep = qMax(1, int(rowCosts.size() * kTrimRatio));
        double total = 0;
        for (int i = 0; i < keep; ++i)
            total += rowCosts.at(i);
        if (coverageOut)
            *coverageOut = double(span) / double(height);
        return total / double(keep);
    };

    double bestScore = std::numeric_limits<double>::max();
    double secondScore = std::numeric_limits<double>::max();
    for (int dy : candidates) {
        const double value = score(dy, nullptr);
        if (value < bestScore) {
            secondScore = bestScore;
            bestScore = value;
            result.dy = dy;
        } else if (value < secondScore) {
            secondScore = value;
        }
    }
    for (int dy = result.dy - kRefineRadius; dy <= result.dy + kRefineRadius; ++dy) {
        const double value = score(dy, nullptr);
        if (value < bestScore) {
            secondScore = bestScore;
            bestScore = value;
            result.dy = dy;
        } else if (value < secondScore) {
            secondScore = value;
        }
    }
    result.residual = bestScore;
    result.confidence = secondScore > 1e-6 ? (secondScore - bestScore) / secondScore : 1.0;
    return result;
}

// ── 累积 ─────────────────────────────────────────────────────
void ScrollStitcher::start(const QImage &firstFrame)
{
    m_frames.clear();
    m_offsets.clear();
    m_previousOffset = 0;
    m_totalDy = 0;
    m_rejected = 0;
    m_noMovement = 0;
    m_scrolledBack = 0;
    m_previous = firstFrame.convertToFormat(QImage::Format_RGB32);
    if (m_previous.isNull())
        return;
    // 单步位移不能超过帧高（否则重叠区太小、也对不上）
    m_config.maxDy = qMin(m_config.maxDy, qMax(24, m_previous.height() - 40));
    m_frames.append(m_previous);
    m_offsets.append(0);
    SS_INFO() << "[scroll] 开始拼接：首帧" << m_previous.size() << "maxDy=" << m_config.maxDy;
}

ScrollStitcher::AbsorbResult ScrollStitcher::absorb(const QImage &frame)
{
    AbsorbResult result;
    if (!started()) {
        start(frame);
        result.verdict = Verdict::Absorbed;
        return result;
    }
    const QImage next = frame.convertToFormat(QImage::Format_RGB32);
    if (next.isNull() || next.width() != m_previous.width())
        return result;

    const ScrollAlign aligned = align(m_previous, next, m_config.maxDy);
    result.dy = aligned.dy;
    result.residual = aligned.residual;

    if (aligned.residual > m_config.rejectResidual) {
        // 对不上就不并入（平滑滚动的中间帧、动画帧都是这样）；也不更新 m_previous，
        // 这样下一帧仍然跟"最后一张可信的帧"比 —— 否则误差会一路累积。
        ++m_rejected;
        result.verdict = Verdict::Rejected;
        SS_INFO() << "[scroll] 拒绝一帧：残差" << aligned.residual << "dy=" << aligned.dy;
        return result;
    }

    if (aligned.dy < 0) {
        ++m_scrolledBack;
        result.verdict = Verdict::ScrolledBack;
        // 回滚的帧**不并入结果**，但它改变了"当前在长图哪个位置"——
        // 下一帧的位移是相对它算的，所以基准必须跟着退，否则之后每一帧都会错位这一段
        m_previousOffset += aligned.dy;
        m_previous = next;
        return result;
    }

    if (aligned.dy < m_config.minStep) {
        ++m_noMovement;
        result.verdict = Verdict::NoMovement;
        m_previous = next;
        return result;
    }

    const int covered = m_previousOffset + m_previous.height();
    const int newCovered = m_previousOffset + aligned.dy + next.height();
    if (newCovered > m_config.maxHeight) {
        result.verdict = Verdict::TooTall;
        return result;
    }

    result.verdict = Verdict::Absorbed;
    result.absorbedRows = qMax(0, newCovered - covered);
    m_offsets.append(m_previousOffset + aligned.dy);
    m_previousOffset = m_offsets.last();
    m_frames.append(next);
    m_totalDy += aligned.dy;
    m_previous = next;
    SS_INFO() << "[scroll] 并入一帧：dy=" << aligned.dy << "残差" << aligned.residual
              << "累计" << m_totalDy << "帧数" << m_frames.size();
    return result;
}

int ScrollStitcher::height() const
{
    if (m_frames.isEmpty())
        return 0;
    return m_offsets.last() + m_frames.last().height();
}

// ── 固定行 / 滚动条列检测 ────────────────────────────────────
int ScrollStitcher::detectFixedTopRows() const
{
    if (m_frames.size() < 2)
        return 0;
    const QImage &reference = m_frames.first();
    const int limit = qMin(reference.height() / 3, reference.height());
    const int allowed = int(reference.width() * kBandMismatchRatio);
    // 滚动条那几列每帧都在变，必须排除掉 —— 否则它一列 16px 就把「固定行」判死
    // （实测：不排除的话，48px 的页头最多只认到 4 行）
    // 只看中间 80% 的列：滚动条贴在边上，这样就不必先知道"哪几列是滚动条"
    // （否则条带检测与列检测会互相依赖 —— 实测这个循环会把中位数抬到 26.7%，
    //   于是阈值永远够不着，一列都裁不掉）
    const int xFrom = int(reference.width() * 0.1);
    const int xTo = int(reference.width() * 0.9);
    int fixed = 0;
    for (int y = 0; y < limit; ++y) {
        bool fixedRow = true;
        for (int i = 1; i < m_frames.size() && fixedRow; ++i) {
            const QImage &frame = m_frames.at(i);
            const QRgb *a = reinterpret_cast<const QRgb *>(reference.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
            int bad = 0;
            for (int x = xFrom; x < xTo; ++x) {
                const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                                   qAbs(qBlue(a[x]) - qBlue(b[x])));
                if (d > kBandTolerance && ++bad > allowed) {
                    fixedRow = false;
                    break;
                }
            }
        }
        if (!fixedRow)
            break; // 固定条带必须从顶部连续
        ++fixed;
    }
    return fixed;
}

int ScrollStitcher::detectFixedBottomRows() const
{
    if (m_frames.size() < 2)
        return 0;
    const QImage &reference = m_frames.first();
    const int height = reference.height();
    const int limit = qMin(height / 3, height);
    const int allowed = int(reference.width() * kBandMismatchRatio);
    // 只看中间 80% 的列：滚动条贴在边上，这样就不必先知道"哪几列是滚动条"
    // （否则条带检测与列检测会互相依赖 —— 实测这个循环会把中位数抬到 26.7%，
    //   于是阈值永远够不着，一列都裁不掉）
    const int xFrom = int(reference.width() * 0.1);
    const int xTo = int(reference.width() * 0.9);
    int fixed = 0;
    for (int k = 1; k <= limit; ++k) {
        const int y = height - k;
        bool fixedRow = true;
        for (int i = 1; i < m_frames.size() && fixedRow; ++i) {
            const QImage &frame = m_frames.at(i);
            const QRgb *a = reinterpret_cast<const QRgb *>(reference.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
            int bad = 0;
            for (int x = xFrom; x < xTo; ++x) {
                const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                                   qAbs(qBlue(a[x]) - qBlue(b[x])));
                if (d > kBandTolerance && ++bad > allowed) {
                    fixedRow = false;
                    break;
                }
            }
        }
        if (!fixedRow)
            break;
        ++fixed;
    }
    return fixed;
}

QVector<double> ScrollStitcher::columnMismatchRates(const QImage &prev, const QImage &next,
                                                    int dy) const
{
    QVector<double> rates(prev.width(), 0.0);
    const int height = qMin(prev.height(), next.height());
    // 固定页头/底栏在每帧的同一位置、内容却不同 —— 它们会让**所有列**得到同样的不一致率，
    // 把中位数抬起来、阈值就永远够不着。所以统计前先把这些行排除掉。
    const int bandTop = detectFixedTopRows();
    const int bandBottom = detectFixedBottomRows();
    const int y0 = qMax(bandTop, qMax(0, dy) + bandTop);
    const int y1 = qMin(height - bandBottom, height + dy - bandBottom);
    if (y1 - y0 < 40)
        return rates;
    QVector<int> bad(prev.width(), 0);
    int rows = 0;
    for (int y = y0; y < y1; y += 4) {
        const QRgb *a = reinterpret_cast<const QRgb *>(prev.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(next.constScanLine(y - dy));
        for (int x = 0; x < prev.width(); ++x) {
            const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                               qAbs(qBlue(a[x]) - qBlue(b[x])));
            if (d > kBandTolerance)
                ++bad[x];
        }
        ++rows;
    }
    if (rows == 0)
        return rates;
    for (int x = 0; x < prev.width(); ++x)
        rates[x] = double(bad.at(x)) / double(rows);
    return rates;
}

QPair<int, int> ScrollStitcher::detectEdgeColumns() const
{
    QPair<int, int> crop(0, 0); // left, right
    if (m_frames.size() < 2)
        return crop;
    // 只用相邻帧的"重叠区不一致率"来判：内容列在对齐之后应当几乎一致，
    // 滚动条那一列（滑块在动）会明显更差。再要求它**贴着边**且不超过 maxEdgeCrop。
    QVector<double> worst(m_frames.first().width(), 0.0);
    const bool debug = qEnvironmentVariableIsSet("SEASHOT_SCROLL_DEBUG");
    for (int i = 1; i < m_frames.size(); ++i) {
        const int dy = m_offsets.at(i) - m_offsets.at(i - 1);
        if (dy <= 0)
            continue;
        const QVector<double> rates = columnMismatchRates(m_frames.at(i - 1), m_frames.at(i), dy);
        if (debug)
            qWarning("[scroll] pair %d dy=%d rates: x0=%.3f x449=%.3f x880=%.3f x895=%.3f", i, dy,
                     rates.value(0), rates.value(449), rates.value(880), rates.value(895));
        for (int x = 0; x < rates.size() && x < worst.size(); ++x)
            worst[x] = qMax(worst.at(x), rates.at(x));
    }
    QVector<double> sorted = worst;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.isEmpty() ? 0.0 : sorted.at(sorted.size() / 2);
    const double threshold = qMax(kColumnUnstable, kColumnBaseline * median);
    const int width = worst.size();
    const int maxCrop = qMax(0, m_config.maxEdgeCrop);
    for (int k = 1; k <= qMin(maxCrop, width); ++k) {
        if (worst.at(k - 1) >= threshold)
            crop.first = k;
        else
            break;
    }
    for (int k = 1; k <= qMin(maxCrop, width); ++k) {
        if (worst.at(width - k) >= threshold)
            crop.second = k;
        else
            break;
    }
    if (qEnvironmentVariableIsSet("SEASHOT_SCROLL_DEBUG")) {
        double maxRate = 0;
        int maxX = -1;
        for (int x = 0; x < worst.size(); ++x) {
            if (worst.at(x) > maxRate) {
                maxRate = worst.at(x);
                maxX = x;
            }
        }
        qWarning("[scroll] 列不一致率：max=%.3f @x=%d median=%.3f threshold=%.3f crop=%d/%d",
                 maxRate, maxX, median, threshold, crop.first, crop.second);
    }
    return crop;
}

// ── 产出 ─────────────────────────────────────────────────────
QImage ScrollStitcher::image() const
{
    if (m_frames.isEmpty())
        return QImage();
    const int top = m_config.cropFixedBands ? detectFixedTopRows() : 0;
    const int bottom = m_config.cropFixedBands ? detectFixedBottomRows() : 0;
    const QPair<int, int> crop = m_config.cropEdgeScrollbar ? detectEdgeColumns() : QPair<int, int>(0, 0);

    // 每帧先裁掉固定页头/底栏（固定 UI 在每帧都有，不裁就会每隔一屏重复一次）
    QVector<QImage> cropped;
    QVector<int> offsets;
    for (int i = 0; i < m_frames.size(); ++i) {
        const QImage &frame = m_frames.at(i);
        const int contentHeight = frame.height() - top - bottom;
        if (contentHeight <= 0)
            continue;
        const int left = crop.first;
        const int right = crop.second;
        const int contentWidth = frame.width() - left - right;
        if (contentWidth <= 0)
            continue;
        cropped.append(frame.copy(left, top + 0, contentWidth, contentHeight));
        offsets.append(m_offsets.at(i));
    }
    if (cropped.isEmpty())
        return QImage();

    int height = 0;
    for (int i = 0; i < cropped.size(); ++i)
        height = qMax(height, offsets.at(i) + cropped.at(i).height());
    QImage result(cropped.first().width(), qMax(1, height), QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    for (int i = 0; i < cropped.size(); ++i)
        painter.drawImage(0, offsets.at(i), cropped.at(i));
    painter.end();
    return result;
}

QString ScrollStitcher::report() const
{
    const QPair<int, int> crop = edgeCropColumns();
    return QStringLiteral("帧数=%1 累计位移=%2 原始高=%3 固定页头=%4 固定底栏=%5 左右裁列=%6/%7 "
                          "拒绝帧=%8 未滚动=%9 回滚=%10")
        .arg(m_frames.size())
        .arg(m_totalDy)
        .arg(height())
        .arg(detectFixedTopRows())
        .arg(detectFixedBottomRows())
        .arg(crop.first)
        .arg(crop.second)
        .arg(m_rejected)
        .arg(m_noMovement)
        .arg(m_scrolledBack);
}

} // namespace seashot
