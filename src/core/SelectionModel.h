// 选区的纯逻辑（不依赖任何后端，便于单元测试）
#pragma once

#include "core/Types.h"

namespace seashot {

/// 悬停命中：返回光标下最上层的可选窗口。
/// - 只考虑 normal && visible && !minimized 的窗口
/// - 排除截图辅助层及被合成器明确排除的窗口；本应用设置/编辑器可以选择
/// - stackIndex 越大越靠上；相同或未知(-1)时按列表顺序取靠后者（视为更靠上）
/// 未命中返回 isValid()==false 的 WindowInfo。
WindowInfo pickWindowAt(const QVector<WindowInfo> &windows, const QPointF &logicalPoint);

/// 拖拽矩形归一化（两个角点 → 左上+宽高），并保证宽高非负。
QRectF normalizeDragRect(const QPointF &a, const QPointF &b);

/// 拖拽是否够大、算得上「区域选择」而不是「单击」。
bool dragExceedsClickThreshold(const QPointF &a, const QPointF &b, double threshold = 4.0);

} // namespace seashot
