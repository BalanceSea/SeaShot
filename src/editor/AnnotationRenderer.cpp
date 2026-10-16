#include "editor/AnnotationRenderer.h"

#include <QPainter>

namespace seashot {

QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations)
{
    if (source.isNull())
        return source;

    // 统一转成 ARGB32：既能保留 alpha，也避免在不同格式上画抗锯齿线条出怪色
    QImage target = source.convertToFormat(QImage::Format_ARGB32);
    if (annotations.isEmpty())
        return target;

    QPainter painter(&target);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const Annotation &annotation : annotations)
        annotation.draw(painter);
    painter.end();
    return target;
}

} // namespace seashot
