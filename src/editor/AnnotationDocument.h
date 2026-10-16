// 标注文档 + 撤销/重做。
//
// 用「整份快照」做撤销栈：标注数量很小（几十个），快照实现简单、行为可预测，
// 也不会出现"部分回滚"这类难查的 bug。新操作会清空重做栈（标准语义）。
#pragma once

#include "editor/Annotation.h"

#include <QVector>

namespace seashot {

class AnnotationDocument
{
public:
    static constexpr int kMaxUndoDepth = 200;

    void add(const Annotation &annotation);
    void update(int index, const Annotation &annotation);
    void removeAt(int index);
    void clear();
    void reset(const QVector<Annotation> &annotations); ///< 打开新图时清空历史

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    const QVector<Annotation> &items() const { return m_items; }
    int count() const { return m_items.size(); }
    const Annotation &at(int index) const { return m_items.at(index); }

    /// 每次内容变化 +1；界面用它判断是否需要重绘
    int revision() const { return m_revision; }

private:
    void pushUndoState();

    QVector<Annotation> m_items;
    QVector<QVector<Annotation>> m_undoStack;
    QVector<QVector<Annotation>> m_redoStack;
    int m_revision = 0;
};

} // namespace seashot
