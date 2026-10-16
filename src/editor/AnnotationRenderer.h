// 把标注渲染成图片（导出/复制的唯一出口）
//
// ⚠️ 模糊/马赛克需要读源图像素，Annotation::draw() 拿不到，
//    所以这里提供 drawRegionEffect() 作为**唯一实现**，
//    渲染（导出）与画布预览都调它 —— 否则就会出现「导出的和看到的不一样」。
#pragma once

#include "editor/Annotation.h"

#include <QImage>
#include <QPainter>
#include <QVector>

namespace seashot {

/// 在源图副本上绘制标注。**导出尺寸 == 源图尺寸**（验收判据之一）。
/// 源图带 alpha 时保留 alpha。
QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations);

/// 区域特效（模糊/马赛克）的落地实现：按 annotation 的框取源图区域，处理成一块可贴回的图。
/// region 会被裁到源图内；宽高不足 1 像素或框外时返回空图。
/// strength 由 annotation.width 推导（模糊半径 / 马赛克块边长）。
QImage regionEffectPatch(const QImage &source, const QRectF &region, ToolKind kind, int width);

/// 画布与渲染器共用：把某个区域特效标注贴上去。
/// painter 必须已经处于**图像坐标系**（已按缩放/平移变换）。非区域特效时什么都不做。
void drawRegionEffect(QPainter &painter, const QImage &source, const Annotation &annotation);

} // namespace seashot
