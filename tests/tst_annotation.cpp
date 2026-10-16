// Phase 2 编辑器：标注模型 / 撤销栈 / 渲染 / 画布交互
#include "editor/AnnotationCanvas.h"
#include "editor/AnnotationDocument.h"
#include "editor/AnnotationRenderer.h"

#include <QLineEdit>
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
};

QTEST_MAIN(TestAnnotation)
#include "tst_annotation.moc"
