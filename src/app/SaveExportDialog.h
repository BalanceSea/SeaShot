// 「保存 / 导出」窗口（统一界面）：截图与录制的交付都在这里
//
// 为什么要有它（2026-10-17 用户要求）：
//  - 截图后的**结果浮层去掉了**（截图完只自动复制到剪贴板，不弹任何窗）；
//  - "另存为" 与 "保存录制（导出 GIF/MP4/复制 GIF）" 原来分散在两个不同的地方
//    （托盘/浮层按钮），现在收进**同一个窗口**的两页里；
//  - 录制结束时**自动打开**这个窗口的"录制"页 —— 格式由你在这里选（这就是原来的
//    "延迟导出 + 格式选择"流程，只是从浮层换成了正经窗口）。
#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

class QLabel;
class QPushButton;
class QTabWidget;

namespace seashot {

class SaveExportDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Page { Screenshot, Recording };

    explicit SaveExportDialog(QWidget *parent = nullptr);

    /// 截图页的数据（image 为空 = 没有可用的截图）
    void setScreenshot(const QImage &image, const QString &savedPath);
    /// 录制页的数据
    void setRecording(int frames, qint64 elapsedMs, const QString &note, bool available);

    void showPage(Page page);

signals:
    void copyScreenshotRequested();
    void saveScreenshotRequested();
    void saveScreenshotAsRequested();
    void editScreenshotRequested();
    void exportGifRequested();
    void exportMp4Requested();
    void copyGifRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void applyAutoClick();

    QTabWidget *m_tabs = nullptr;
    QLabel *m_shotPreview = nullptr;
    QLabel *m_shotInfo = nullptr;
    QLabel *m_recInfo = nullptr;
    QPushButton *m_shotCopy = nullptr;
    QPushButton *m_shotSave = nullptr;
    QPushButton *m_shotSaveAs = nullptr;
    QPushButton *m_shotEdit = nullptr;
    QPushButton *m_gif = nullptr;
    QPushButton *m_mp4 = nullptr;
    QPushButton *m_copyGif = nullptr;
    bool m_autoClickDone = false;
};

} // namespace seashot
