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

    /// 选区被调整（拖手柄改尺寸/移位置）时用：把当前标注**以及撤销/重做栈里的每一个快照**
    /// 一起平移。标注本身锚定的是**屏幕内容**，选区原点一变就得反向平移；
    /// 快照不跟着平移的话，之后一撤销标注就会“跳到另一个坐标系”里。
    /// 这不是一次可撤销的编辑（只是换坐标帧），所以不压栈、只动 revision。
    void translateAll(const QPointF &delta);

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
