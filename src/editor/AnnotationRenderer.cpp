#include "editor/AnnotationRenderer.h"

#include <QPainter>

#include <algorithm>

namespace seashot {

namespace {

/// 一维盒式模糊（水平方向），就地结果写进 dst
void blurRow(const QImage &src, QImage &dst, int y, int radius)
{
    const int w = src.width();
    int a = 0, r = 0, g = 0, b = 0;
    // 初始窗口：以 x=0 为中心，越界用边缘像素补
    for (int dx = -radius; dx <= radius; ++dx) {
        const QRgb p = src.pixel(qBound(0, dx, w - 1), y);
        a += qAlpha(p);
        r += qRed(p);
        g += qGreen(p);
        b += qBlue(p);
    }
    const int n = radius * 2 + 1;
    for (int x = 0; x < w; ++x) {
        dst.setPixel(x, y, qRgba(r / n, g / n, b / n, a / n));
        // 滑动窗口：移出左边，移入右边
        const QRgb out = src.pixel(qBound(0, x - radius, w - 1), y);
        const QRgb in = src.pixel(qBound(0, x + radius + 1, w - 1), y);
        a += qAlpha(in) - qAlpha(out);
        r += qRed(in) - qRed(out);
        g += qGreen(in) - qGreen(out);
        b += qBlue(in) - qBlue(out);
    }
}

/// 一维盒式模糊（垂直方向）
void blurColumn(const QImage &src, QImage &dst, int x, int radius)
{
    const int h = src.height();
    int a = 0, r = 0, g = 0, b = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        const QRgb p = src.pixel(x, qBound(0, dy, h - 1));
        a += qAlpha(p);
        r += qRed(p);
        g += qGreen(p);
        b += qBlue(p);
    }
    const int n = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        dst.setPixel(x, y, qRgba(r / n, g / n, b / n, a / n));
        const QRgb out = src.pixel(x, qBound(0, y - radius, h - 1));
        const QRgb in = src.pixel(x, qBound(0, y + radius + 1, h - 1));
        a += qAlpha(in) - qAlpha(out);
        r += qRed(in) - qRed(out);
        g += qGreen(in) - qGreen(out);
        b += qBlue(in) - qBlue(out);
    }
}

/// 可分离盒式模糊跑两遍 ≈ 高斯。大区域先降到 1/2 再处理，保证交互时不卡。
QImage boxBlur(QImage image, int radius)
{
    if (radius < 1 || image.isNull())
        return image;
    image = image.convertToFormat(QImage::Format_ARGB32);

    const bool downscale = qint64(image.width()) * image.height() > 400 * 400;
    const int scale = downscale ? 2 : 1;
    if (downscale)
        image = image.scaled(qMax(1, image.width() / 2), qMax(1, image.height() / 2),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const int r = qMax(1, radius / scale);
    for (int pass = 0; pass < 2; ++pass) {
        QImage tmp = image;
        for (int y = 0; y < image.height(); ++y)
            blurRow(image, tmp, y, r);
        QImage tmp2 = tmp;
        for (int x = 0; x < tmp.width(); ++x)
            blurColumn(tmp, tmp2, x, r);
        image = tmp2;
    }

    if (downscale)
        image = image.scaled(qMax(1, image.width() * 2), qMax(1, image.height() * 2),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace

QImage regionEffectPatch(const QImage &source, const QRectF &region, ToolKind kind, int width)
{
    if (source.isNull() || !isRegionEffect(kind))
        return {};
    const QRect area = region.toAlignedRect().intersected(source.rect());
    if (area.width() < 1 || area.height() < 1)
        return {};

    QImage patch = source.convertToFormat(QImage::Format_ARGB32).copy(area);
    const int strength = qBound(1, width, 64);
    if (kind == ToolKind::Mosaic) {
        // 马赛克：缩到 1/块边长再最近邻放大回来
        const int block = qBound(2, strength, 64);
        const QSize small(qMax(1, area.width() / block), qMax(1, area.height() / block));
        patch = patch.scaled(small, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                    .scaled(area.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    } else {
        patch = boxBlur(patch, qMax(2, strength));
    }
    return patch;
}

void drawRegionEffect(QPainter &painter, const QImage &source, const Annotation &annotation,
                      double annotationScale)
{
    if (!isRegionEffect(annotation.kind))
        return;
    const QRectF region = annotation.bounds();
    const QRectF pixels(region.x() * annotationScale, region.y() * annotationScale,
                        region.width() * annotationScale, region.height() * annotationScale);
    const QImage patch = regionEffectPatch(source, pixels, annotation.kind,
                                           qMax(1, qRound(annotation.width * annotationScale)));
    if (patch.isNull())
        return;
    painter.drawImage(region.toAlignedRect(), patch);
}

QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations,
                         double annotationScale)
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
    painter.scale(annotationScale, annotationScale);
    for (const Annotation &annotation : annotations) {
        if (isRegionEffect(annotation.kind)) {
            // 区域特效覆盖在「当前已画的内容」之上：所以后画的模糊会遮住先画的箭头，
            // 与画布预览的顺序语义一致（两边都走这个函数）。
            drawRegionEffect(painter, target, annotation, annotationScale);
        } else {
            annotation.draw(painter);
        }
    }
    painter.end();
    return target;
}

} // namespace seashot
