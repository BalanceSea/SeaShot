// 托盘（KStatusNotifierItem）：截图 / 退出
#pragma once

#include <QObject>
#include <QString>

class KStatusNotifierItem;
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

signals:
    void captureRequested();
    void settingsRequested();
    void shortcutSettingsRequested();
    void quitRequested();

private:
    KStatusNotifierItem *m_item = nullptr;
    QMenu *m_menu = nullptr;
    bool m_available = false;
};

} // namespace seashot
