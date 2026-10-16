// Phase 2 编辑器：标注模型 / 撤销栈 / 渲染 / 画布交互
#include "editor/AnnotationCanvas.h"
#include "editor/AnnotationDocument.h"
#include "editor/AnnotationRenderer.h"

#include <QLineEdit>
#include <QWheelEvent>
#include <QtTest>

using namespace seashot;

namespace {
Annotation makeRect(const QPointF &a, const QPointF &b, int width = 4)
{
    Annotation annotation;
    annotation.kind = ToolKind::Rectangle;
    annotation.points = {a, b};
    annotation.width = width;
    return annotation;
}
} // namespace

class TestAnnotation : public QObject
{
    Q_OBJECT

private slots:
    // ---------- 模型 ----------
    void boundsFollowsPointsAndWidth()
    {
        const Annotation rect = makeRect(QPointF(100, 80), QPointF(20, 30), 4);
        // 归一化：(20,30)-(100,80)，再外扩线宽
        QCOMPARE(rect.bounds(), QRectF(16, 26, 88, 58));
    }

    void hitTestFindsStrokeButNotHole()
    {
        const Annotation rect = makeRect(QPointF(0, 0), QPointF(100, 100), 4);
        QVERIFY(rect.hitTest(QPointF(50, 0)));   // 上边
        QVERIFY(rect.hitTest(QPointF(0, 50)));   // 左边
        QVERIFY(!rect.hitTest(QPointF(50, 50))); // 内部空白
        QVERIFY(!rect.hitTest(QPointF(300, 300)));
    }

    void hitTestWorksForEllipseArrowPenText()
    {
        Annotation ellipse;
        ellipse.kind = ToolKind::Ellipse;
        ellipse.points = {QPointF(0, 0), QPointF(100, 60)};
        ellipse.width = 3;
        QVERIFY(ellipse.hitTest(QPointF(50, 0)));    // 椭圆上顶点
        QVERIFY(!ellipse.hitTest(QPointF(50, 30)));  // 圆心附近是空心的

        Annotation arrow;
        arrow.kind = ToolKind::Arrow;
        arrow.points = {QPointF(0, 0), QPointF(100, 0)};
        arrow.width = 4;
        QVERIFY(arrow.hitTest(QPointF(50, 1)));
        QVERIFY(!arrow.hitTest(QPointF(50, 40)));

        Annotation pen;
        pen.kind = ToolKind::Pen;
        pen.points = {QPointF(0, 0), QPointF(50, 50), QPointF(100, 0)};
        pen.width = 2;
        QVERIFY(pen.hitTest(QPointF(25, 25)));
        QVERIFY(!pen.hitTest(QPointF(50, 0)));

        Annotation text;
        text.kind = ToolKind::Text;
        text.text = QStringLiteral("hello");
        text.width = 6;
        text.points = {QPointF(10, 10)};
        QVERIFY(text.hitTest(QPointF(20, 20)));
        QVERIFY(!text.hitTest(QPointF(20, 400)));
    }

    void translateMovesAllPoints()
    {
        Annotation pen;
        pen.kind = ToolKind::Pen;
        pen.points = {QPointF(0, 0), QPointF(10, 10)};
        pen.translate(QPointF(5, -3));
        QCOMPARE(pen.points.at(0), QPointF(5, -3));
        QCOMPARE(pen.points.at(1), QPointF(15, 7));
    }

    void handlesOnlyForShapeTools()
    {
        QCOMPARE(makeRect(QPointF(0, 0), QPointF(10, 10)).handles().size(), 2);
        Annotation pen;
        pen.kind = ToolKind::Pen;
        pen.points = {QPointF(0, 0), QPointF(1, 1)};
        QVERIFY(pen.handles().isEmpty());

        Annotation rect = makeRect(QPointF(0, 0), QPointF(10, 10));
        rect.moveHandle(1, QPointF(30, 40));
        QCOMPARE(rect.points.at(1), QPointF(30, 40));
    }

    void validityRules()
    {
        QVERIFY(!makeRect(QPointF(5, 5), QPointF(5, 5)).isValid());       // 太小
        QVERIFY(makeRect(QPointF(0, 0), QPointF(20, 0)).isValid());       // 细线也算
        Annotation empty;
        empty.kind = ToolKind::Text;
        QVERIFY(!empty.isValid());                                        // 没文字
        empty.text = QStringLiteral("x");
        empty.points = {QPointF(1, 1)};
        QVERIFY(empty.isValid());
    }

    // ---------- 撤销栈 ----------
    void undoRedoRoundTrip()
    {
        AnnotationDocument document;
        QCOMPARE(document.count(), 0);
        QVERIFY(!document.canUndo());
        document.add(makeRect(QPointF(0, 0), QPointF(10, 10)));
        document.add(makeRect(QPointF(20, 20), QPointF(30, 30)));
        QCOMPARE(document.count(), 2);
        QVERIFY(document.canUndo());

        QVERIFY(document.undo());
        QCOMPARE(document.count(), 1);
        QVERIFY(document.undo());
        QCOMPARE(document.count(), 0);
        QVERIFY(!document.canUndo());

        QVERIFY(document.redo());
        QCOMPARE(document.count(), 1);
        QVERIFY(document.redo());
        QCOMPARE(document.count(), 2);
        QVERIFY(!document.canRedo());
    }

    void newActionClearsRedoStack()
    {
        AnnotationDocument document;
        document.add(makeRect(QPointF(0, 0), QPointF(10, 10)));
        document.undo();
        QVERIFY(document.canRedo());
        document.add(makeRect(QPointF(5, 5), QPointF(25, 25)));
        QVERIFY(!document.canRedo());
        QCOMPARE(document.count(), 1);
    }

    void updateAndRemoveAreUndoable()
    {
        AnnotationDocument document;
        const Annotation original = makeRect(QPointF(0, 0), QPointF(10, 10));
        document.add(original);
        Annotation moved = original;
        moved.translate(QPointF(50, 50));
        document.update(0, moved);
        QCOMPARE(document.at(0).points.at(0), QPointF(50, 50));
        document.undo();
        QCOMPARE(document.at(0).points.at(0), QPointF(0, 0));

        document.removeAt(0);
        QCOMPARE(document.count(), 0);
        document.undo();
        QCOMPARE(document.count(), 1);

        document.clear();
        QCOMPARE(document.count(), 0);
        document.undo();
        QCOMPARE(document.count(), 1);
    }

    void undoStackIsCapped()
    {
        AnnotationDocument document;
        for (int i = 0; i < AnnotationDocument::kMaxUndoDepth + 20; ++i)
            document.add(makeRect(QPointF(i, i), QPointF(i + 5, i + 5)));
        int undoCount = 0;
        while (document.undo())
            ++undoCount;
        QCOMPARE(undoCount, AnnotationDocument::kMaxUndoDepth);
    }

    // ---------- 渲染 ----------
    void renderKeepsSourceSizeAndAlpha()
    {
        QImage source(120, 90, QImage::Format_ARGB32);
        source.fill(QColor(10, 20, 30, 255));
        const QImage rendered = renderAnnotations(source, {makeRect(QPointF(10, 10), QPointF(60, 60))});
        QCOMPARE(rendered.size(), source.size());
        QCOMPARE(rendered.pixel(110, 80), source.pixel(110, 80)); // 远处像素不动
        QVERIFY(rendered.pixel(10, 10) != source.pixel(10, 10));  // 描边处被画上
    }

    void renderDrawsEveryToolKind()
    {
        QImage source(200, 120, QImage::Format_ARGB32);
        source.fill(Qt::black);

        Annotation arrow;
        arrow.kind = ToolKind::Arrow;
        arrow.color = Qt::red;
        arrow.width = 4;
        arrow.points = {QPointF(10, 10), QPointF(110, 10)};

        Annotation ellipse;
        ellipse.kind = ToolKind::Ellipse;
        ellipse.color = Qt::green;
        ellipse.width = 4;
        ellipse.points = {QPointF(10, 40), QPointF(110, 100)};

        Annotation pen;
        pen.kind = ToolKind::Pen;
        pen.color = Qt::yellow;
        pen.width = 4;
        pen.points = {QPointF(120, 10), QPointF(140, 60), QPointF(190, 90)};

        Annotation text;
        text.kind = ToolKind::Text;
        text.color = Qt::white;
        text.width = 6;
        text.text = QStringLiteral("SeaShot");
        text.points = {QPointF(10, 20)};

        const QImage rendered = renderAnnotations(source, {arrow, ellipse, pen, text});
        QCOMPARE(rendered.size(), source.size());
        QVERIFY(rendered.pixel(60, 10) != source.pixel(60, 10)); // 箭头杆
        QVERIFY(rendered.pixel(60, 40) != source.pixel(60, 40)); // 椭圆上顶点
        QVERIFY(rendered.pixel(140, 60) != source.pixel(140, 60)); // 画笔拐点
        // 文字：在其包围盒里至少有一个非黑像素
        bool textDrawn = false;
        const QRectF box = text.bounds();
        for (int y = qMax(0, int(box.top())); y < qMin(rendered.height(), int(box.bottom())); ++y) {
            for (int x = qMax(0, int(box.left())); x < qMin(rendered.width(), int(box.right())); ++x) {
                if (rendered.pixel(x, y) != source.pixel(x, y)) {
                    textDrawn = true;
                    break;
                }
            }
            if (textDrawn)
                break;
        }
        QVERIFY(textDrawn);
    }

    void renderWithNoAnnotationsOnlyConvertsFormat()
    {
        QImage source(30, 30, QImage::Format_RGB32);
        source.fill(Qt::blue);
        const QImage rendered = renderAnnotations(source, {});
        QCOMPARE(rendered.size(), source.size());
        QCOMPARE(rendered.pixel(15, 15), QColor(Qt::blue).rgb());
    }

    // ---------- 画布交互（直接调交互入口，不依赖真实鼠标） ----------
    void canvasDrawsAndKeepsImageCoordinates()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        QImage image(200, 200, QImage::Format_ARGB32);
        image.fill(Qt::black);
        canvas.setImage(image);
        canvas.setStrokeWidth(4);
        canvas.setTool(ToolKind::Rectangle);
        canvas.selectAnnotation(-1);

        const QPointF from = canvas.imageToWidgetForTest(QPointF(20, 30));
        const QPointF to = canvas.imageToWidgetForTest(QPointF(120, 130));
        canvas.handleMousePress(from, Qt::LeftButton);
        canvas.handleMouseMove(to);
        canvas.handleMouseRelease(to, Qt::LeftButton);

        QCOMPARE(canvas.document().count(), 1);
        QCOMPARE(canvas.document().at(0).kind, ToolKind::Rectangle);
        QCOMPARE(canvas.document().at(0).points.at(0), QPointF(20, 30));
        // 释放点换算回来应落在 (120,130) 附近（允许 1px 取整误差）
        QVERIFY(qAbs(canvas.document().at(0).points.at(1).x() - 120) <= 1.0);
        QVERIFY(qAbs(canvas.document().at(0).points.at(1).y() - 130) <= 1.0);
    }

    void canvasPenCollectsPolyline()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        canvas.setTool(ToolKind::Pen);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(10, 10)), Qt::LeftButton);
        for (int i = 1; i <= 10; ++i)
            canvas.handleMouseMove(canvas.imageToWidgetForTest(QPointF(10 + i * 8, 10 + i * 4)));
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(90, 50)), Qt::LeftButton);
        QCOMPARE(canvas.document().count(), 1);
        QCOMPARE(canvas.document().at(0).kind, ToolKind::Pen);
        QVERIFY(canvas.document().at(0).points.size() >= 5);
    }

    void canvasSelectMovesAndResizes()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        canvas.setTool(ToolKind::Rectangle);
        const QPointF a = canvas.imageToWidgetForTest(QPointF(40, 40));
        const QPointF b = canvas.imageToWidgetForTest(QPointF(120, 120));
        canvas.handleMousePress(a, Qt::LeftButton);
        canvas.handleMouseRelease(b, Qt::LeftButton);
        QCOMPARE(canvas.document().count(), 1);

        // 选择工具：点上边缘选中 → 拖动整体移动
        canvas.setTool(ToolKind::Select);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(80, 40)), Qt::LeftButton);
        QCOMPARE(canvas.selectedIndex(), 0);
        const QPointF delta(20, 10);
        canvas.handleMouseMove(canvas.imageToWidgetForTest(QPointF(80, 40) + delta));
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(80, 40) + delta), Qt::LeftButton);
        QCOMPARE(canvas.document().at(0).points.at(0), QPointF(60, 50));

        // 拖角点改大小
        const QPointF handle = canvas.imageToWidgetForTest(QPointF(140, 130));
        canvas.handleMousePress(handle, Qt::LeftButton);
        canvas.handleMouseMove(canvas.imageToWidgetForTest(QPointF(180, 170)));
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(180, 170)), Qt::LeftButton);
        QCOMPARE(canvas.document().at(0).points.at(1), QPointF(180, 170));

        // 删除 + 撤销
        canvas.deleteSelected();
        QCOMPARE(canvas.document().count(), 0);
        QVERIFY(canvas.document().undo());
        QCOMPARE(canvas.document().count(), 1);
    }

    void canvasTextEditingCommitsAndEdits()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.show(); // offscreen 平台下也走真实的显示路径（子控件可见性判断更真实）
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        canvas.setTool(ToolKind::Text);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(30, 50)), Qt::LeftButton);

        auto *editor = canvas.findChild<QLineEdit *>();
        QVERIFY(editor);
        QVERIFY(editor->isVisible());
        editor->setText(QStringLiteral("标注文字"));
        QMetaObject::invokeMethod(editor, "returnPressed");

        QCOMPARE(canvas.document().count(), 1);
        QCOMPARE(canvas.document().at(0).kind, ToolKind::Text);
        QCOMPARE(canvas.document().at(0).text, QStringLiteral("标注文字"));
        QCOMPARE(canvas.document().at(0).points.at(0), QPointF(30, 50));

        // 双击（选择工具）改文字 → 走 update，可撤销
        canvas.setTool(ToolKind::Select);
        canvas.handleDoubleClick(canvas.imageToWidgetForTest(QPointF(35, 55)));
        QVERIFY(editor->isVisible());
        editor->setText(QStringLiteral("改过的文字"));
        QMetaObject::invokeMethod(editor, "returnPressed");
        QCOMPARE(canvas.document().count(), 1);
        QCOMPARE(canvas.document().at(0).text, QStringLiteral("改过的文字"));
        QVERIFY(canvas.document().undo());
        QCOMPARE(canvas.document().at(0).text, QStringLiteral("标注文字"));
    }

    void canvasZoomScalesCoordinateMapping()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        QVERIFY(canvas.fitToWindow()); // 新图默认适应窗口

        canvas.setZoom(2.0);
        QVERIFY(!canvas.fitToWindow());
        QCOMPARE(canvas.zoom(), 2.0);
        const QPointF origin = canvas.imageToWidgetForTest(QPointF(0, 0));
        const QPointF ten = canvas.imageToWidgetForTest(QPointF(10, 10));
        QCOMPARE(ten.x() - origin.x(), 20.0); // 1 图像像素 = 2 控件像素
        QCOMPARE(ten.y() - origin.y(), 20.0);

        // 缩放后**标注坐标仍是图像坐标**（导出与命中不受缩放影响）
        canvas.setTool(ToolKind::Rectangle);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(20, 30)), Qt::LeftButton);
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(120, 130)), Qt::LeftButton);
        QCOMPARE(canvas.document().count(), 1);
        QCOMPARE(canvas.document().at(0).points.at(0), QPointF(20, 30));
        QVERIFY(qAbs(canvas.document().at(0).points.at(1).x() - 120) <= 1.0);

        canvas.zoomTo100();
        QCOMPARE(canvas.zoom(), 1.0);
        canvas.zoomToFit();
        QVERIFY(canvas.fitToWindow());
    }

    void canvasWheelZoomNeedsControl()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        const double before = canvas.zoom();

        QWheelEvent plain(QPointF(100, 100), QPointF(100, 100), QPoint(0, 0), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&canvas, &plain);
        QCOMPARE(canvas.zoom(), before); // 不按 Ctrl 不缩放（交给滚动区）

        QWheelEvent withCtrl(QPointF(100, 100), QPointF(100, 100), QPoint(0, 0), QPoint(0, 120),
                             Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&canvas, &withCtrl);
        QVERIFY(canvas.zoom() > before);
        QVERIFY(!canvas.fitToWindow());
    }

    void canvasMiddleDragRequestsPan()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.setImage(QImage(200, 200, QImage::Format_ARGB32));
        QSignalSpy panSpy(&canvas, &AnnotationCanvas::panRequested);

        canvas.handleMousePress(QPointF(100, 100), Qt::MiddleButton);
        canvas.handleMouseMove(QPointF(130, 90));
        canvas.handleMouseRelease(QPointF(130, 90), Qt::MiddleButton);

        QCOMPARE(panSpy.count(), 1);
        const QPoint delta = panSpy.first().at(0).toPoint();
        QCOMPARE(delta, QPoint(30, -10));
        QCOMPARE(canvas.document().count(), 0); // 平移不会画上标注
    }

    void canvasRenderedImageHasAnnotations()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.show();
        QImage image(200, 200, QImage::Format_ARGB32);
        image.fill(Qt::black);
        canvas.setImage(image);
        canvas.setColor(QColor(255, 0, 0));
        canvas.setStrokeWidth(6);
        canvas.setTool(ToolKind::Arrow);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(20, 20)), Qt::LeftButton);
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(150, 150)), Qt::LeftButton);

        const QImage rendered = canvas.renderedImage();
        QCOMPARE(rendered.size(), image.size()); // 导出尺寸 == 源图尺寸
        QVERIFY(rendered.pixel(85, 85) != image.pixel(85, 85));
    }

    /// 新增四种工具（高亮/模糊/马赛克/序号）的模型与渲染行为
    void newToolsModel()
    {
        // 高亮：与画笔同为折线，但更粗、且不是描边状
        Annotation highlight;
        highlight.kind = ToolKind::Highlight;
        highlight.width = 6;
        highlight.points = {QPointF(0, 50), QPointF(200, 50)};
        QVERIFY(highlight.isValid());
        QVERIFY(highlight.hitTest(QPointF(100, 50)));   // 笔迹上
        QVERIFY(highlight.hitTest(QPointF(100, 56)));   // 容差按 width*1.5 放宽
        QVERIFY(!highlight.hitTest(QPointF(100, 200)));

        // 序号：单击即有效（只有一个点），按中心圆形命中；不再能改手柄
        Annotation number;
        number.kind = ToolKind::Number;
        number.text = QStringLiteral("1");
        number.width = 4;
        number.points = {QPointF(100, 100)};
        QVERIFY(number.isValid());
        QVERIFY(number.hitTest(QPointF(100, 100)));
        QVERIFY(number.hitTest(QPointF(100 + number.numberRadius() - 1, 100)));
        QVERIFY(!number.hitTest(QPointF(100 + number.numberRadius() + 20, 100)));
        QVERIFY(number.handles().isEmpty());
        QVERIFY(number.bounds().contains(QPointF(100, 100)));

        // 模糊/马赛克：区域语义，可拖动、可命中内部（不是描边），且不向外扩
        Annotation blur;
        blur.kind = ToolKind::Blur;
        blur.width = 8;
        blur.points = {QPointF(20, 20), QPointF(120, 90)};
        QVERIFY(blur.isValid());
        QVERIFY(blur.hitTest(QPointF(70, 55)));  // 内部也算命中
        QVERIFY(!blur.hitTest(QPointF(300, 300)));
        QCOMPARE(blur.handles().size(), 2);      // 能拖角点改大小
        QCOMPARE(blur.bounds(), QRectF(20, 20, 100, 70));
        blur.moveHandle(1, QPointF(200, 150));
        QCOMPARE(blur.bounds(), QRectF(20, 20, 180, 130));
    }

    /// 区域特效真的改像素，而且预览与导出走同一实现（所见即所得的前提）
    void regionEffectChangesPixels()
    {
        QImage source(160, 120, QImage::Format_ARGB32);
        // 画黑白相间的细条纹：模糊/马赛克后条纹一定会被抹平
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, (x % 2 == 0) ? Qt::white : Qt::black);

        Annotation blur;
        blur.kind = ToolKind::Blur;
        blur.width = 10;
        blur.points = {QPointF(40, 30), QPointF(120, 90)};

        const QImage rendered = renderAnnotations(source, {blur});
        QCOMPARE(rendered.size(), source.size());   // 尺寸不变
        QCOMPARE(rendered.pixel(10, 10), source.pixel(10, 10)); // 框外不动
        // 框内条纹被抹平 → 不再是纯黑或纯白
        const QColor inside = QColor::fromRgba(rendered.pixel(80, 60));
        QVERIFY(inside.red() > 20 && inside.red() < 235);

        // 马赛克：同一块内多个像素变成同一个值（降采样再放大）
        Annotation mosaic = blur;
        mosaic.kind = ToolKind::Mosaic;
        mosaic.width = 12;
        const QImage mos = renderAnnotations(source, {mosaic});
        QCOMPARE(mos.pixel(60, 50), mos.pixel(62, 50));
        QCOMPARE(mos.size(), source.size());

        // 空框 / 框外：不崩、不改像素
        Annotation outside;
        outside.kind = ToolKind::Blur;
        outside.points = {QPointF(500, 500), QPointF(600, 600)};
        QCOMPARE(renderAnnotations(source, {outside}).pixel(5, 5), source.pixel(5, 5));
        QVERIFY(regionEffectPatch(source, QRectF(0, 0, 0, 0), ToolKind::Blur, 5).isNull());
    }

    /// 序号工具在画布上单击即放置，且编号递增
    void numberToolPlacesIncrementingBadges()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.show();
        QImage image(200, 200, QImage::Format_ARGB32);
        image.fill(Qt::white);
        canvas.setImage(image);
        canvas.setColor(QColor(255, 0, 0));
        canvas.setStrokeWidth(6);
        canvas.setTool(ToolKind::Number);

        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(40, 40)), Qt::LeftButton);
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(40, 40)), Qt::LeftButton);
        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(120, 80)), Qt::LeftButton);
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(120, 80)), Qt::LeftButton);

        QCOMPARE(canvas.document().count(), 2);
        QCOMPARE(canvas.document().at(0).kind, ToolKind::Number);
        QCOMPARE(canvas.document().at(0).text, QStringLiteral("1"));
        QCOMPARE(canvas.document().at(1).text, QStringLiteral("2"));
        // 画在图上：徽标圆心附近（避开居中的白色数字）应该是红色填充
        const double r = canvas.document().at(0).numberRadius();
        const QColor badgePixel =
            QColor::fromRgba(canvas.renderedImage().pixel(40, int(40 - r * 0.75)));
        QVERIFY(badgePixel.red() > 200 && badgePixel.green() < 80);
        // 画布外的像素不应被改
        QCOMPARE(canvas.renderedImage().pixel(199, 199), image.pixel(199, 199));
    }

    /// 区域特效在画布上拖出来，并且导出的尺寸/像素与预览一致
    void regionEffectViaCanvasDrag()
    {
        AnnotationCanvas canvas;
        canvas.resize(400, 400);
        canvas.show();
        QImage image(160, 120, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, (x % 2 == 0) ? Qt::white : Qt::black);
        canvas.setImage(image);
        canvas.setStrokeWidth(10);
        canvas.setTool(ToolKind::Mosaic);

        canvas.handleMousePress(canvas.imageToWidgetForTest(QPointF(30, 30)), Qt::LeftButton);
        canvas.handleMouseMove(canvas.imageToWidgetForTest(QPointF(130, 90)));
        canvas.handleMouseRelease(canvas.imageToWidgetForTest(QPointF(130, 90)), Qt::LeftButton);

        QCOMPARE(canvas.document().count(), 1);
        QVERIFY(isRegionEffect(canvas.document().at(0).kind));
        QVERIFY(canvas.documentHasRegionEffect());
        const QImage rendered = canvas.renderedImage();
        QCOMPARE(rendered.size(), image.size());
        QCOMPARE(rendered.pixel(80, 60), rendered.pixel(82, 60)); // 马赛克块内同色
        QCOMPARE(rendered.pixel(5, 5), image.pixel(5, 5));        // 框外不变
        // 撤销后应回到原图（区域特效也要能被撤销）
        QVERIFY(canvas.document().undo());
        QCOMPARE(canvas.document().count(), 0);
    }
};

QTEST_MAIN(TestAnnotation)
#include "tst_annotation.moc"
