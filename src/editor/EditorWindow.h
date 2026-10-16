// 编辑器窗口：工具栏（工具/颜色/粗细/撤销重做/复制/保存/另存为）+ 画布
#pragma once

#include "editor/Annotation.h"
#include "editor/AnnotationCanvas.h"

#include <QImage>
#include <QWidget>

class QAction;
class QLabel;
class QToolBar;
class QScrollArea;
class QSpinBox;
class QToolButton;

namespace seashot {

class EditorWindow : public QWidget
{
    Q_OBJECT

public:
    explicit EditorWindow(const QImage &image, const QString &sourcePath = QString(),
                          QWidget *parent = nullptr);

    QString savedPath() const { return m_savedPath; }
    bool hasUnsavedChanges() const { return m_dirty; }
    AnnotationCanvas *canvas() const { return m_canvas; }

    /// 立即保存到「另存为」默认路径（自动化/测试入口），成功返回路径
    QString saveToDefaultPath();

signals:
    void savedTo(const QString &path);
    void copiedToClipboard();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void refreshActions();
    void chooseColor();
    void save();
    void saveAs();
    void copyToClipboard();
    void setTool(ToolKind tool);
    void updateTitle();
    void updateZoomUi();

    AnnotationCanvas *m_canvas = nullptr;
    QHash<int, QToolButton *> m_toolButtons;
    QToolButton *m_colorButton = nullptr;
    QSpinBox *m_widthBox = nullptr;
    QScrollArea *m_scroll = nullptr;
    QAction *m_fitAction = nullptr;
    QLabel *m_zoomLabel = nullptr;
    /// 顶栏（用于按工具栏首选宽度决定窗口宽度，避免工具被折进溢出菜单）
    QToolBar *m_toolbar = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QLabel *m_status = nullptr;
    QString m_sourcePath;
    QString m_savedPath;
    bool m_dirty = false;
};

} // namespace seashot
