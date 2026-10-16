#include "editor/AnnotationCanvas.h"

#include "editor/AnnotationRenderer.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>

namespace seashot {

namespace {
constexpr double kHandleRadius = 5.0; ///< 手柄命中半径（控件像素）
constexpr double kHitTolerance = 5.0; ///< 标注命中容差（图像像素）
} // namespace

AnnotationCanvas::AnnotationCanvas(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setCursor(Qt::CrossCursor);
}

void AnnotationCanvas::setImage(const QImage &image)
{
    m_image = image;
    m_selected = -1;
    m_draft = Annotation();
    m_document.reset({});
    update();
}

void AnnotationCanvas::setTool(ToolKind tool)
{
    m_tool = tool;
    m_draft = Annotation();
    m_drawing = false;
    setCursor(tool == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    if (tool != ToolKind::Text)
        cancelTextEditing();
    update();
}

void AnnotationCanvas::setColor(const QColor &color)
{
    m_color = color;
}

void AnnotationCanvas::setStrokeWidth(int width)
{
    m_width = qBound(1, width, 48);
}

QImage AnnotationCanvas::renderedImage() const
{
    return renderAnnotations(m_image, m_document.items());
}

double AnnotationCanvas::zoom() const
{
    if (m_image.isNull())
        return 1.0;
    const double sx = double(width()) / double(m_image.width());
    const double sy = double(height()) / double(m_image.height());
    return qBound(0.02, qMin(sx, sy), 1.0); // 不放大，避免糊
}

QRectF AnnotationCanvas::imageDisplayRect() const
{
    if (m_image.isNull())
        return {};
    const double z = zoom();
    const QSizeF scaled(m_image.width() * z, m_image.height() * z);
    return QRectF(QPointF((width() - scaled.width()) / 2.0, (height() - scaled.height()) / 2.0),
                  scaled);
}

QPointF AnnotationCanvas::widgetToImage(const QPointF &widgetPos) const
{
    const QRectF display = imageDisplayRect();
    const double z = zoom();
    if (z <= 0.0)
        return widgetPos;
    return QPointF((widgetPos.x() - display.x()) / z, (widgetPos.y() - display.y()) / z);
}

QPointF AnnotationCanvas::imageToWidget(const QPointF &imagePos) const
{
    const QRectF display = imageDisplayRect();
    const double z = zoom();
    return QPointF(display.x() + imagePos.x() * z, display.y() + imagePos.y() * z);
}

void AnnotationCanvas::selectAnnotation(int index)
{
    if (index < -1 || index >= m_document.count())
        index = -1;
    if (index == m_selected)
        return;
    m_selected = index;
    emit selectionChanged(m_selected);
    update();
}

int AnnotationCanvas::hitAnnotation(const QPointF &imagePos) const
{
    // 后画的在上层 → 从后往前找
    for (int i = m_document.count() - 1; i >= 0; --i) {
        if (m_document.at(i).hitTest(imagePos, kHitTolerance))
            return i;
    }
    return -1;
}

int AnnotationCanvas::hitHandle(int index, const QPointF &imagePos) const
{
    if (index < 0 || index >= m_document.count())
        return -1;
    const QVector<QPointF> handles = m_document.at(index).handles();
    const double tolerance = kHandleRadius / qMax(0.05, zoom());
    for (int i = 0; i < handles.size(); ++i) {
        if (QLineF(handles.at(i), imagePos).length() <= tolerance)
            return i;
    }
    return -1;
}

void AnnotationCanvas::handleMousePress(const QPointF &widgetPos, Qt::MouseButton button)
{
    if (m_image.isNull())
        return;
    const QPointF imagePos = widgetToImage(widgetPos);
    m_pressImagePos = imagePos;

    if (button == Qt::RightButton) {
        // 右键：取消正在画的东西 / 取消选中
        m_drawing = false;
        m_dragging = false;
        m_draft = Annotation();
        selectAnnotation(-1);
        return;
    }
    if (button != Qt::LeftButton)
        return;

    if (m_tool == ToolKind::Select) {
        m_handleIndex = hitHandle(m_selected, imagePos);
        if (m_handleIndex >= 0) {
            m_dragging = true;
            m_dragOriginal = m_document.at(m_selected);
            m_draft = m_dragOriginal;
            return;
        }
        const int index = hitAnnotation(imagePos);
        selectAnnotation(index);
        if (index >= 0) {
            m_dragging = true;
            m_handleIndex = -1;
            m_dragOriginal = m_document.at(index);
            m_draft = m_dragOriginal;
        }
        return;
    }

    if (m_tool == ToolKind::Text) {
        beginTextEditing(imagePos, -1);
        return;
    }

    m_draft = Annotation();
    m_draft.kind = m_tool;
    m_draft.color = m_color;
    m_draft.width = m_width;
    m_draft.points = {imagePos, imagePos};
    m_drawing = true;
    update();
}

void AnnotationCanvas::handleMouseMove(const QPointF &widgetPos)
{
    if (m_image.isNull())
        return;
    const QPointF imagePos = widgetToImage(widgetPos);

    if (m_drawing) {
        if (m_draft.kind == ToolKind::Pen) {
            const QPointF last = m_draft.points.last();
            if (QLineF(last, imagePos).length() >= 1.5 / qMax(0.05, zoom()))
                m_draft.points.append(imagePos);
        } else if (m_draft.points.size() >= 2) {
            m_draft.points[1] = imagePos;
        }
        update();
        return;
    }

    if (m_dragging) {
        if (m_handleIndex >= 0) {
            m_draft = m_dragOriginal;
            m_draft.moveHandle(m_handleIndex, imagePos);
        } else {
            m_draft = m_dragOriginal;
            m_draft.translate(imagePos - m_pressImagePos);
        }
        update();
    }
}

void AnnotationCanvas::handleMouseRelease(const QPointF &widgetPos, Qt::MouseButton button)
{
    if (button != Qt::LeftButton)
        return;
    const QPointF imagePos = widgetToImage(widgetPos);

    if (m_drawing) {
        if (m_draft.kind == ToolKind::Pen)
            m_draft.points.append(imagePos);
        else if (m_draft.points.size() >= 2)
            m_draft.points[1] = imagePos;
        m_drawing = false;
        if (m_draft.isValid()) {
            m_document.add(m_draft);
            selectAnnotation(m_document.count() - 1);
            emit documentChanged();
        }
        m_draft = Annotation();
        update();
        return;
    }

    if (m_dragging) {
        m_dragging = false;
        if (m_selected >= 0 && m_draft.isValid()) {
            m_document.update(m_selected, m_draft);
            emit documentChanged();
        }
        m_draft = Annotation();
        m_handleIndex = -1;
        update();
    }
}

void AnnotationCanvas::handleDoubleClick(const QPointF &widgetPos)
{
    if (m_tool != ToolKind::Select)
        return;
    const int index = hitAnnotation(widgetToImage(widgetPos));
    if (index >= 0 && m_document.at(index).kind == ToolKind::Text) {
        selectAnnotation(index);
        beginTextEditing(m_document.at(index).points.value(0), index);
    }
}

void AnnotationCanvas::beginTextEditing(const QPointF &imagePos, int editingIndex)
{
    m_textEditing = true;
    m_textAnchor = imagePos;
    m_textEditingIndex = editingIndex;
    if (!m_textEditor) {
        m_textEditor = new QLineEdit(this);
        m_textEditor->setStyleSheet(QStringLiteral(
            "QLineEdit{background:rgba(20,20,24,220);color:white;border:1px solid #3daeff;"
            "padding:2px 4px;}"));
        m_textEditor->installEventFilter(this);
        connect(m_textEditor, &QLineEdit::returnPressed, this, &AnnotationCanvas::commitTextEditing);
        connect(m_textEditor, &QLineEdit::editingFinished, this, [this]() {
            // 失焦也算提交（Esc 会先走 eventFilter 取消）
            if (m_textEditing)
                commitTextEditing();
        });
    }
    if (editingIndex >= 0)
        m_textEditor->setText(m_document.at(editingIndex).text);
    else
        m_textEditor->clear();

    const QPointF widgetPos = imageToWidget(imagePos);
    const int fontSize = qMax(10, m_width * 4);
    m_textEditor->setFont(QFont(QString(), -1, QFont::Normal));
    m_textEditor->setFixedHeight(qMax(24, int(fontSize * zoom()) + 12));
    m_textEditor->resize(qMax(160, int(fontSize * zoom() * 6)), m_textEditor->height());
    m_textEditor->move(int(widgetPos.x()), int(widgetPos.y()));
    m_textEditor->show();
    m_textEditor->setFocus();
    emit statusMessage(QStringLiteral("输入文字后回车确认，Esc 取消"));
}

void AnnotationCanvas::commitTextEditing()
{
    if (!m_textEditor)
        return;
    const QString text = m_textEditor->text();
    m_textEditor->hide();
    m_textEditing = false;
    const int editingIndex = m_textEditingIndex;
    m_textEditingIndex = -1;
    if (text.trimmed().isEmpty()) {
        update();
        return;
    }

    Annotation annotation;
    annotation.kind = ToolKind::Text;
    annotation.points = {m_textAnchor};
    annotation.text = text;
    annotation.color = m_color;
    annotation.width = m_width;
    if (editingIndex >= 0 && editingIndex < m_document.count()) {
        annotation.points = {m_document.at(editingIndex).points.value(0)};
        m_document.update(editingIndex, annotation);
    } else {
        m_document.add(annotation);
        selectAnnotation(m_document.count() - 1);
    }
    emit documentChanged();
    update();
}

void AnnotationCanvas::cancelTextEditing()
{
    if (!m_textEditing)
        return;
    m_textEditor->hide();
    m_textEditing = false;
    m_textEditingIndex = -1;
    update();
}

void AnnotationCanvas::deleteSelected()
{
    if (m_selected < 0)
        return;
    m_document.removeAt(m_selected);
    selectAnnotation(-1);
    emit documentChanged();
    update();
}

void AnnotationCanvas::drawSelectionOverlay(QPainter &painter, const Annotation &annotation) const
{
    const double z = zoom();
    QPen pen(QColor(61, 174, 255), 1.0 / qMax(0.05, z), Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(annotation.bounds());

    painter.setPen(QPen(QColor(61, 174, 255), 1.5 / qMax(0.05, z)));
    painter.setBrush(QColor(255, 255, 255));
    const double r = kHandleRadius / qMax(0.05, z);
    for (const QPointF &handle : annotation.handles())
        painter.drawEllipse(handle, r, r);
}

void AnnotationCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(32, 34, 38));

    if (m_image.isNull()) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("没有图像"));
        return;
    }

    const QRectF display = imageDisplayRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(display, m_image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    painter.save();
    painter.translate(display.topLeft());
    painter.scale(zoom(), zoom());
    if (m_selected < 0 && m_draft.isValid())
        m_draft.draw(painter);
    for (int i = 0; i < m_document.count(); ++i) {
        if (i == m_selected && m_draft.isValid())
            m_draft.draw(painter); // 拖动中的预览
        else
            m_document.at(i).draw(painter);
    }
    if (m_selected >= 0 && m_document.count() > m_selected) {
        const Annotation &current = m_draft.isValid() ? m_draft : m_document.at(m_selected);
        drawSelectionOverlay(painter, current);
    }
    painter.restore();

    // 图像边框
    painter.setPen(QPen(QColor(255, 255, 255, 60), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(display);
}

void AnnotationCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    handleMousePress(event->position(), event->button());
    event->accept();
}

void AnnotationCanvas::mouseMoveEvent(QMouseEvent *event)
{
    handleMouseMove(event->position());
    event->accept();
}

void AnnotationCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    handleMouseRelease(event->position(), event->button());
    event->accept();
}

void AnnotationCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    handleDoubleClick(event->position());
    event->accept();
}

void AnnotationCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_textEditing)
            cancelTextEditing();
        else {
            m_drawing = false;
            m_dragging = false;
            m_draft = Annotation();
            selectAnnotation(-1);
        }
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AnnotationCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_textEditing)
        cancelTextEditing();
}

bool AnnotationCanvas::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_textEditor && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            cancelTextEditing();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace seashot
