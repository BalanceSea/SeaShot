#include "geometry/ScreenMapper.h"

#include <QtMath>

namespace seashot {

namespace {
double effectiveScale(const OutputInfo &out)
{
    return (out.scale > 0.0 && !qFuzzyIsNull(out.scale)) ? out.scale : 1.0;
}
} // namespace

int ScreenMapper::roundCoord(double v)
{
    return static_cast<int>(std::lround(v));
}

void ScreenMapper::setOutputs(const QVector<OutputInfo> &outputs)
{
    m_outputs = outputs;
}

const OutputInfo *ScreenMapper::outputAt(const QPointF &logicalPoint) const
{
    for (const OutputInfo &o : m_outputs) {
        if (o.logicalGeometry.contains(logicalPoint.toPoint()))
            return &o;
    }
    return m_outputs.isEmpty() ? nullptr : &m_outputs.first();
}

const OutputInfo *ScreenMapper::outputByName(const QString &name) const
{
    for (const OutputInfo &o : m_outputs) {
        if (o.name == name)
            return &o;
    }
    return nullptr;
}

QRect ScreenMapper::toPhysical(const QRectF &logical, const OutputInfo &out) const
{
    const double s = effectiveScale(out);
    const QRectF rel(logical.x() - out.logicalGeometry.x(),
                     logical.y() - out.logicalGeometry.y(),
                     logical.width(), logical.height());
    return QRect(roundCoord(rel.x() * s), roundCoord(rel.y() * s),
                 qMax(1, roundCoord(rel.width() * s)), qMax(1, roundCoord(rel.height() * s)));
}

QRectF ScreenMapper::toLogical(const QRect &physical, const OutputInfo &out) const
{
    const double s = effectiveScale(out);
    return QRectF(out.logicalGeometry.x() + physical.x() / s,
                  out.logicalGeometry.y() + physical.y() / s,
                  physical.width() / s, physical.height() / s);
}

QSizeF ScreenMapper::toLogicalSize(const QSize &physical, const OutputInfo &out) const
{
    const double s = effectiveScale(out);
    return QSizeF(physical.width() / s, physical.height() / s);
}

} // namespace seashot
