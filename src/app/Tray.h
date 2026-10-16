// 托盘（KStatusNotifierItem）：截图 / 退出
#pragma once

#include <QObject>
#include <QString>

class KStatusNotifierItem;
class QAction;
class QMenu;

namespace seashot {

class Tray : public QObject
{
    Q_OBJECT

public:
    explicit Tray(QObject *parent = nullptr);
    ~Tray() override;

    void setHotkeyLabel(const QString &label);
    bool available() const { return m_available; }

    /// 托盘菜单项文字（测试与验收用）
    QStringList menuActionTexts() const;

    /// 录制中：把第一项从「录制 GIF…」切成「停止录制」
    void setRecording(bool recording);

signals:
    void captureRequested();
    void recordToggleRequested();
    void settingsRequested();
    void shortcutSettingsRequested();
    void quitRequested();

private:
    KStatusNotifierItem *m_item = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_recordAction = nullptr;
    bool m_available = false;
};

} // namespace seashot
