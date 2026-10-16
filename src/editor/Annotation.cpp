#include "editor/Annotation.h"

#include <QFontMetricsF>
#include <QtMath>

namespace seashot {

namespace {

double pointSegmentDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double lengthSquared = ab.x() * ab.x() + ab.y() * ab.y();
    if (lengthSquared <= 0.0)
        return QLineF(point, a).length();
    double t = ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y()) / lengthSquared;
    t = qBound(0.0, t, 1.0);
    const QPointF projection(a.x() + t * ab.x(), a.y() + t * ab.y());
    return QLineF(point, projection).length();
}

double polylineDistance(const QPointF &point, const QVector<QPointF> &points)
{
    if (points.isEmpty())
        return std::numeric_limits<double>::max();
    if (points.size() == 1)
        return QLineF(point, points.first()).length();
    double best = std::numeric_limits<double>::max();
    for (int i = 1; i < points.size(); ++i)
        best = qMin(best, pointSegmentDistance(point, points.at(i - 1), points.at(i)));
    return best;
}

/// 椭圆描边距离的近似：用归一化半径偏差乘短半轴
double ellipseDistance(const QPointF &point, const QRectF &rect)
{
    const double a = rect.width() / 2.0;
    const double b = rect.height() / 2.0;
    if (a <= 0.0 || b <= 0.0)
        return std::numeric_limits<double>::max();
    const QPointF center = rect.center();
    const double nx = (point.x() - center.x()) / a;
    const double ny = (point.y() - center.y()) / b;
    const double r = std::sqrt(nx * nx + ny * ny);
    return qAbs(r - 1.0) * qMin(a, b);
}

double rectOutlineDistance(const QPointF &point, const QRectF &rect)
{
    const QVector<QPointF> corners{rect.topLeft(), rect.topRight(), rect.bottomRight(),
                                   rect.bottomLeft(), rect.topLeft()};
    return polylineDistance(point, corners);
}

} // namespace

QString toolName(ToolKind kind)
{
    switch (kind) {
    case ToolKind::Select: return QStringLiteral("选择");
    case ToolKind::Arrow: return QStringLiteral("箭头");
    case ToolKind::Rectangle: return QStringLiteral("矩形");
    case ToolKind::Ellipse: return QStringLiteral("椭圆");
    case ToolKind::Pen: return QStringLiteral("画笔");
    case ToolKind::Text: return QStringLiteral("文字");
    }
    return QStringLiteral("?");
}

QFont Annotation::textFont() const
{
    QFont font;
    font.setPixelSize(qMax(10, width * 4));
    return font;
}

bool Annotation::isValid() const
{
    if (kind == ToolKind::Text)
        return !text.trimmed().isEmpty() && !points.isEmpty();
    if (points.size() < 2)
        return false;
    if (kind == ToolKind::Pen)
        return points.size() >= 2;
    const QRectF rect = normalizedRect();
    return rect.width() >= 2.0 || rect.height() >= 2.0;
}

QRectF Annotation::normalizedRect() const
{
    if (points.size() < 2)
        return {};
    const QPointF a = points.first();
    const QPointF b = points.last();
    return QRectF(QPointF(qMin(a.x(), b.x()), qMin(a.y(), b.y())),
                  QPointF(qMax(a.x(), b.x()), qMax(a.y(), b.y())));
}

QRectF Annotation::bounds() const
{
    if (kind == ToolKind::Text) {
        if (points.isEmpty())
            return {};
        // 必须与 draw() 一致：文字画在 points.first() + (0, ascent) 的基线上
        const QFontMetricsF metrics(textFont());
        return metrics.boundingRect(text).translated(points.first()
                                                     + QPointF(0, metrics.ascent()));
    }
    if (kind == ToolKind::Pen) {
        QRectF rect;
        for (const QPointF &p : points)
            rect = rect.isNull() ? QRectF(p, QSizeF(0, 0)) : rect.united(QRectF(p, QSizeF(0, 0)));
        return rect.adjusted(-width, -width, width, width);
    }
    return normalizedRect().adjusted(-width, -width, width, width);
}

bool Annotation::hitTest(const QPointF &point, double tolerance) const
{
    const double slack = tolerance + width / 2.0;
    switch (kind) {
    case ToolKind::Text:
        return bounds().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
    case ToolKind::Pen:
        return polylineDistance(point, points) <= slack;
    case ToolKind::Arrow:
        return polylineDistance(point, points) <= slack;
    case ToolKind::Rectangle:
        return rectOutlineDistance(point, normalizedRect()) <= slack;
    case ToolKind::Ellipse:
        return ellipseDistance(point, normalizedRect()) <= slack;
    case ToolKind::Select:
        break;
    }
    return false;
}

void Annotation::translate(const QPointF &delta)
{
    for (QPointF &point : points)
        point += delta;
}

QVector<QPointF> Annotation::handles() const
{
    if (kind == ToolKind::Rectangle || kind == ToolKind::Ellipse || kind == ToolKind::Arrow)
        return points;
    return {};
}

void Annotation::moveHandle(int index, const QPointF &position)
{
    if (index >= 0 && index < points.size())
        points[index] = position;
}

void Annotation::draw(QPainter &painter) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case ToolKind::Rectangle:
        painter.drawRect(normalizedRect());
        break;
    case ToolKind::Ellipse:
        painter.drawEllipse(normalizedRect());
        break;
    case ToolKind::Pen:
        if (points.size() >= 2)
            painter.drawPolyline(points.constData(), points.size());
        break;
    case ToolKind::Arrow: {
        if (points.size() < 2)
            break;
        const QPointF start = points.first();
        const QPointF end = points.last();
        painter.drawLine(start, end);
        const double angle = std::atan2(end.y() - start.y(), end.x() - start.x());
        const double headLength = qMax(12.0, width * 4.0);
        const double spread = M_PI / 7.0; // ~25°
        const QPointF left(end.x() - headLength * std::cos(angle - spread),
                           end.y() - headLength * std::sin(angle - spread));
        const QPointF right(end.x() - headLength * std::cos(angle + spread),
                            end.y() - headLength * std::sin(angle + spread));
        painter.drawLine(end, left);
        painter.drawLine(end, right);
        break;
    }
    case ToolKind::Text:
        if (!points.isEmpty()) {
            painter.setFont(textFont());
            const QFontMetricsF metrics(textFont());
            painter.drawText(points.first() + QPointF(0, metrics.ascent()), text);
        }
        break;
    case ToolKind::Select:
        break;
    }
    painter.restore();
}

} // namespace seashot
