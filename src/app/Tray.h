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

signals:
    void captureRequested();
    void quitRequested();

private:
    KStatusNotifierItem *m_item = nullptr;
    QMenu *m_menu = nullptr;
    bool m_available = false;
};

} // namespace seashot
