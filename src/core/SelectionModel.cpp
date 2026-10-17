#include "core/SelectionModel.h"

#include <QtMath>

namespace seashot {

WindowInfo pickWindowAt(const QVector<WindowInfo> &windows, const QPointF &logicalPoint)
{
    WindowInfo best;
    for (const WindowInfo &w : windows) {
        if (!w.isValid() || !w.normal || !w.visible || w.minimized)
            continue;
        if (w.captureOverlay || w.excludedFromCapture)
            continue;
        if (!w.frameGeometry.contains(logicalPoint))
            continue;
        // 命中多个时取栈序更高的；栈序相同/未知(-1) 时按列表顺序取靠后者（视为更靠上）。
        const bool better = !best.isValid() || w.stackIndex >= best.stackIndex;
        if (better)
            best = w;
    }
    return best;
}

QRectF normalizeDragRect(const QPointF &a, const QPointF &b)
{
    const double x1 = qMin(a.x(), b.x());
    const double y1 = qMin(a.y(), b.y());
    const double x2 = qMax(a.x(), b.x());
    const double y2 = qMax(a.y(), b.y());
    return QRectF(QPointF(x1, y1), QPointF(x2, y2));
}

bool dragExceedsClickThreshold(const QPointF &a, const QPointF &b, double threshold)
{
    return qAbs(a.x() - b.x()) > threshold || qAbs(a.y() - b.y()) > threshold;
}

} // namespace seashot
