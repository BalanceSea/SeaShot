// 编辑器的画布：显示截图 + 叠加标注 + 处理鼠标/键盘。
//
// 坐标：标注一律用**图像坐标**，画布只做 图像坐标 ↔ 控件坐标 的映射（等比缩放居中）。
// 鼠标事件都经过 handleMouseXxx()，自动化测试可以直接调这三个入口驱动交互。
#pragma once

#include "editor/AnnotationDocument.h"

#include <QColor>
#include <QWidget>

class QLineEdit;

namespace seashot {

class AnnotationCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit AnnotationCanvas(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    QImage image() const { return m_image; }
    /// 源图 + 当前标注（导出/复制用）
    QImage renderedImage() const;

    void setTool(ToolKind tool);
    ToolKind tool() const { return m_tool; }
    void setColor(const QColor &color);
    QColor color() const { return m_color; }
    void setStrokeWidth(int width);
    int strokeWidth() const { return m_width; }

    AnnotationDocument &document() { return m_document; }
    int selectedIndex() const { return m_selected; }
    void selectAnnotation(int index);
    void deleteSelected();

    /// 交互入口（鼠标事件与测试共用）
    void handleMousePress(const QPointF &widgetPos, Qt::MouseButton button);
    void handleMouseMove(const QPointF &widgetPos);
    void handleMouseRelease(const QPointF &widgetPos, Qt::MouseButton button);
    void handleDoubleClick(const QPointF &widgetPos);

    /// 测试/诊断用：把图像坐标换算成控件坐标
    QPointF imageToWidgetForTest(const QPointF &imagePos) const { return imageToWidget(imagePos); }
    double zoomForTest() const { return zoom(); }

signals:
    void documentChanged();
    void selectionChanged(int index);
    void statusMessage(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    double zoom() const;
    QRectF imageDisplayRect() const;
    QPointF widgetToImage(const QPointF &widgetPos) const;
    QPointF imageToWidget(const QPointF &imagePos) const;
    int hitAnnotation(const QPointF &imagePos) const;
    int hitHandle(int index, const QPointF &imagePos) const;
    void beginTextEditing(const QPointF &imagePos, int editingIndex);
    void commitTextEditing();
    void cancelTextEditing();
    void drawSelectionOverlay(QPainter &painter, const Annotation &annotation) const;

    QImage m_image;
    AnnotationDocument m_document;
    ToolKind m_tool = ToolKind::Rectangle;
    QColor m_color{255, 64, 64};
    int m_width = 4;
    int m_selected = -1;

    Annotation m_draft; ///< 正在绘制/拖动的临时标注（不直接改文档）
    bool m_drawing = false;
    bool m_dragging = false;
    int m_handleIndex = -1;
    QPointF m_pressImagePos;
    Annotation m_dragOriginal;

    QLineEdit *m_textEditor = nullptr;
    /// 不依赖控件可见性（父窗口没显示/最小化时依然成立）
    bool m_textEditing = false;
    int m_textEditingIndex = -1;
    QPointF m_textAnchor;
};

} // namespace seashot
