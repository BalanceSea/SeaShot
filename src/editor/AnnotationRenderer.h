// 把标注渲染成图片（导出/复制的唯一出口）
#pragma once

#include "editor/Annotation.h"

#include <QImage>
#include <QVector>

namespace seashot {

/// 在源图副本上绘制标注。**导出尺寸 == 源图尺寸**（验收判据之一）。
/// 源图带 alpha 时保留 alpha。
QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations);

} // namespace seashot
