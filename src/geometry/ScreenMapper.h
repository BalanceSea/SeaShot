// 逻辑坐标 ↔ 物理像素 的**唯一**转换点。
//
// 背景（Phase 0 实测）：
//  - KWin 给的 frameGeometry 可能是小数（实测 x=536.582963768418）。
//  - org.kde.KWin.ScreenShot2 的 CaptureArea 收逻辑坐标，返回的像素是物理像素，
//    并在返回里给出 scale。
//  - 本机实测 scale=1、单输出；scale≠1 与多输出的路径由单元测试覆盖，
//    真机验证待用户临时改缩放（见 PLAN.md「风险与回退」）。
#pragma once

#include "core/Types.h"

namespace seashot {

class ScreenMapper
{
public:
    void setOutputs(const QVector<OutputInfo> &outputs);
    QVector<OutputInfo> outputs() const { return m_outputs; }

    /// 逻辑点所在的输出；找不到时返回第一个输出（单输出场景即所求）。
    const OutputInfo *outputAt(const QPointF &logicalPoint) const;
    const OutputInfo *outputByName(const QString &name) const;

    /// 逻辑矩形 → 该输出的物理像素矩形（**相对该输出左上角**）。
    QRect toPhysical(const QRectF &logical, const OutputInfo &out) const;

    /// 物理像素矩形 → 逻辑矩形（相对虚拟桌面原点）。
    QRectF toLogical(const QRect &physical, const OutputInfo &out) const;

    /// 物理像素尺寸 → 逻辑尺寸。
    QSizeF toLogicalSize(const QSize &physical, const OutputInfo &out) const;

    /// 统一的取整策略：四舍五入到最近整数（负数同样按数轴取整）。
    static int roundCoord(double v);

private:
    QVector<OutputInfo> m_outputs;
};

} // namespace seashot
