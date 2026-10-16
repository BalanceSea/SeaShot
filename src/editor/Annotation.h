// 标注数据模型（纯 C++，不依赖界面，便于单元测试）
//
// 坐标系约定：所有点都存**图像坐标**（像素），与缩放无关 —— 这样导出时不用换算，
// 画布只负责把图像坐标映射到控件坐标。
#pragma once

#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace seashot {

enum class ToolKind {
    Select,
    Arrow,
    Rectangle,
    Ellipse,
    Pen,
    Text,
    Highlight, ///< 记号笔：半透明粗笔迹（Multiply 混合，压住底字仍可读）
    Blur,      ///< 模糊：拖一个矩形，对区域做高斯近似模糊
    Mosaic,    ///< 马赛克：拖一个矩形，区域降采样再放大
    Number,    ///< 序号：单击放置一个带数字的圆徽标（依次递增）
};

/// 需要「读取源图像素」的区域特效。
/// ⚠️ 这类标注在 Annotation::draw() 里画不出来（拿不到像素），
///    必须由 renderAnnotations() / AnnotationCanvas 用
///    AnnotationRenderer::drawRegionEffect() 统一处理 —— 预览与导出共用它，
///    否则就会出现「导出的和看到的不一样」。
inline bool isRegionEffect(ToolKind kind)
{
    return kind == ToolKind::Blur || kind == ToolKind::Mosaic;
}

QString toolName(ToolKind kind);

struct Annotation {
    ToolKind kind = ToolKind::Rectangle;
    /// Rectangle/Ellipse/Arrow：两个角点；Pen：折线点；Text：一个锚点（左上角）
    QVector<QPointF> points;
    QString text;
    QColor color = QColor(255, 64, 64);
    /// 形状：线宽（像素）；文字：字号档位（实际字号 = max(10, width*4)）
    int width = 4;

    /// 序号徽标半径（图像像素）。由 width 推导，所以「粗细/字号」对它同样有效。
    double numberRadius() const { return qMax(9.0, width * 2.5); }

    bool isValid() const;

    /// 包围盒（图像坐标）。文字按字体度量计算。
    QRectF bounds() const;

    /// 命中测试：形状按描边距离、文字按包围盒（tolerance 为额外的像素容差）
    bool hitTest(const QPointF &point, double tolerance = 4.0) const;

    void translate(const QPointF &delta);

    /// 选中后的手柄（Rectangle/Ellipse/Arrow 返回两个角点，其他为空）
    QVector<QPointF> handles() const;
    void moveHandle(int index, const QPointF &position);

    /// 在图像坐标下绘制（painter 已按缩放/平移变换过）
    void draw(QPainter &painter) const;

    /// 文字用的字体
    QFont textFont() const;

private:
    QRectF normalizedRect() const;
};

} // namespace seashot
