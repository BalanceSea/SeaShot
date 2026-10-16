#include "editor/AnnotationDocument.h"

namespace seashot {

void AnnotationDocument::pushUndoState()
{
    m_undoStack.append(m_items);
    if (m_undoStack.size() > kMaxUndoDepth)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    ++m_revision;
}

void AnnotationDocument::add(const Annotation &annotation)
{
    pushUndoState();
    m_items.append(annotation);
}

void AnnotationDocument::update(int index, const Annotation &annotation)
{
    if (index < 0 || index >= m_items.size())
        return;
    pushUndoState();
    m_items[index] = annotation;
}

void AnnotationDocument::removeAt(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    pushUndoState();
    m_items.removeAt(index);
}

void AnnotationDocument::clear()
{
    if (m_items.isEmpty())
        return;
    pushUndoState();
    m_items.clear();
}

void AnnotationDocument::reset(const QVector<Annotation> &annotations)
{
    m_items = annotations;
    m_undoStack.clear();
    m_redoStack.clear();
    ++m_revision;
}

bool AnnotationDocument::undo()
{
    if (m_undoStack.isEmpty())
        return false;
    m_redoStack.append(m_items);
    m_items = m_undoStack.takeLast();
    ++m_revision;
    return true;
}

bool AnnotationDocument::redo()
{
    if (m_redoStack.isEmpty())
        return false;
    m_undoStack.append(m_items);
    m_items = m_redoStack.takeLast();
    ++m_revision;
    return true;
}

} // namespace seashot
