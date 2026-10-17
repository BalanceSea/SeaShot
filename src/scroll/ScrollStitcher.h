// 滚动截屏的重叠区匹配拼接（Phase D 的核心，纯计算、不碰界面）
//
// 算法来自 `probes/scroll-stitch-probe.cpp` 的实测结论（见 `docs/acceptance/phaseD-probe.md`）：
//   候选：不做粗筛，直接在 [-maxDy, maxDy] 上取 4px 网格
//         （行均值签名粗筛**不可靠**：周期版式出假峰，固定页头/底栏时真值进不了候选）
//   打分：全宽锚点行 —— 只取重叠区中间 80% 的行、**整行逐像素**、取**截尾均值**丢最差 25%
//         （抽样会漏掉细文字；不丢最差行就会被滚动条/动画/光标淹掉）
//   精修：最优候选 ±4 逐像素比
//   下游信号：最优候选的**绝对残差** —— 用它拒掉"没对上"的帧（平滑滚动的中间帧、动画帧）
//
// 这里只做三件事：
//   ① align()                  两张帧的位移对齐
//   ② absorb()                 把一帧并进结果（含残差拒帧、dy=0 识别）
//   ③ image()/report()         产出长图（自动裁掉"跨帧固定"的页头/底栏与边缘滚动条列）
#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

namespace seashot {

/// 两张帧对齐的结果
struct ScrollAlign {
    int dy = 0;            ///< >0 = 内容向上滚了 dy 像素（新帧比旧帧"往下"多出内容）
    double residual = -1;  ///< 最优候选的绝对残差（0 = 完美；越大越不可信）
    int candidates = 0;
    double confidence = 0; ///< (次优-最优)/次优（本机实测噪声场景会很低，只作诊断）
};

class ScrollStitcher
{
public:
    struct Config {
        int maxDy = 520;             ///< 单步最大位移（超过就搜不到了）；start() 会按帧高收紧
        double rejectResidual = 6.0; ///< 残差超过它 → 这一帧不可信，拒绝（不并入）
        int minStep = 2;             ///< 位移小于它视作"没滚动"（不重复拼接）
        int maxHeight = 20000;       ///< 长图高度上限（内存：900×20000 ≈ 72MB）
        bool cropFixedBands = true;  ///< 自动裁掉跨帧固定的页头/底栏
        bool cropEdgeScrollbar = true; ///< 自动裁掉贴边的"每帧都在变"的滚动条列
        int maxEdgeCrop = 24;        ///< 每侧最多裁几列（防止把内容裁掉）
    };

    enum class Verdict {
        Absorbed,   ///< 吃下了新内容
        NoMovement, ///< 没滚动（dy≈0）
        Rejected,   ///< 残差太大，这一帧不可信（跳过）
        ScrolledBack, ///< 往回滚了（dy<0）—— 不并入，交给上层决定
        TooTall,    ///< 到高度上限了
    };

    struct AbsorbResult {
        Verdict verdict = Verdict::NoMovement;
        int dy = 0;
        int absorbedRows = 0;
        double residual = -1;
    };

    ScrollStitcher() = default;
    explicit ScrollStitcher(const Config &config) : m_config(config) {}
    void setConfig(const Config &config) { m_config = config; }
    const Config &config() const { return m_config; }

    /// 放入第一帧（确定宽度；此时清空之前的状态）
    void start(const QImage &firstFrame);
    bool started() const { return !m_frames.isEmpty(); }

    /// 并入一帧（按上面的判决）
    AbsorbResult absorb(const QImage &frame);

    /// 拼好的长图（每帧裁掉固定页头/底栏与边缘列之后按累计位移拼）
    QImage image() const;
    /// 当前长图高度（未减掉固定页头）
    int height() const;
    /// 已经吃下的总位移（= 内容实际滚过多少行）
    int totalDy() const { return m_totalDy; }
    /// 检测结果与统计（诊断/验收用）
    int fixedTopRows() const { return detectFixedTopRows(); }
    int fixedBottomRows() const { return detectFixedBottomRows(); }
    QPair<int, int> edgeCropColumns() const { return detectEdgeColumns(); }
    int frameCount() const { return m_frames.size(); }
    int rejectedCount() const { return m_rejected; }
    int noMovementCount() const { return m_noMovement; }
    int scrolledBackCount() const { return m_scrolledBack; }
    QString report() const;

    /// 对齐两张帧（静态方法，便于单测）
    static ScrollAlign align(const QImage &prev, const QImage &next, int maxDy);

private:
    int detectFixedTopRows() const;
    int detectFixedBottomRows() const;
    QPair<int, int> detectEdgeColumns() const;
    /// 两张帧在重叠行上"逐列不一致率"
    QVector<double> columnMismatchRates(const QImage &prev, const QImage &next, int dy) const;

    Config m_config;
    QVector<QImage> m_frames; ///< 已接受的帧（按顺序）
    QVector<int> m_offsets;   ///< 每帧在长图里的起始行
    QImage m_previous;        ///< 上一张"可信"的帧（被拒绝的帧不更新它）
    /// m_previous 在长图里的起始行（**含回滚**：回滚的帧不并入结果，但它的位置是下一帧的基准）
    int m_previousOffset = 0;
    int m_totalDy = 0;
    int m_rejected = 0;
    int m_noMovement = 0;
    int m_scrolledBack = 0;
};

} // namespace seashot
