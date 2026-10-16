#include "app/Tray.h"

#include "util/Log.h"

#include <QAction>
#include <QMenu>

#include <KStatusNotifierItem>

namespace seashot {

Tray::Tray(QObject *parent)
    : QObject(parent)
{
    m_item = new KStatusNotifierItem(this);
    m_item->setCategory(KStatusNotifierItem::ApplicationStatus);
    m_item->setStatus(KStatusNotifierItem::Active);
    m_item->setIconByName(QStringLiteral("camera-photo"));
    m_item->setTitle(QStringLiteral("SeaShot"));
    m_item->setToolTip(QStringLiteral("camera-photo"), QStringLiteral("SeaShot"),
                       QStringLiteral("截图工具"));

    m_menu = new QMenu();
    QAction *captureAction = m_menu->addAction(QStringLiteral("截图"));
    connect(captureAction, &QAction::triggered, this, &Tray::captureRequested);
    m_recordAction = m_menu->addAction(QStringLiteral("录制 GIF…"));
    connect(m_recordAction, &QAction::triggered, this, &Tray::recordToggleRequested);
    m_menu->addSeparator();
    QAction *settingsAction = m_menu->addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, &Tray::settingsRequested);
    QAction *keysAction = m_menu->addAction(QStringLiteral("KDE 快捷键设置…"));
    connect(keysAction, &QAction::triggered, this, &Tray::shortcutSettingsRequested);
    m_menu->addSeparator();
    QAction *quitAction = m_menu->addAction(QStringLiteral("退出 SeaShot"));
    connect(quitAction, &QAction::triggered, this, &Tray::quitRequested);
    m_item->setContextMenu(m_menu);

    connect(m_item, &KStatusNotifierItem::activateRequested, this,
            [this](bool, const QPoint &) { emit captureRequested(); });

    m_available = true;
    SS_INFO() << "[tray] KStatusNotifierItem 已创建";
}

Tray::~Tray()
{
    // 不手动 delete m_menu：KF6 的 KStatusNotifierItem 会接管 contextMenu 的所有权，
    // 手动再删一次就是 double free（实测在 ~Tray 里直接崩到 0x0）。
    // 这里只把 QObject 父子关系留给 Qt 处理。
    m_menu = nullptr;
}

void Tray::setHotkeyLabel(const QString &label){
    if (!m_item)
        return;
    m_item->setToolTip(QStringLiteral("camera-photo"), QStringLiteral("SeaShot"),
                       label.isEmpty() ? QStringLiteral("快捷键：未绑定（可在系统设置里绑定）")
                                       : QStringLiteral("快捷键：%1").arg(label));
}

void Tray::setRecording(bool recording)
{
    if (!m_recordAction)
        return;
    m_recordAction->setText(recording ? QStringLiteral("停止录制") : QStringLiteral("录制 GIF…"));
}

QStringList Tray::menuActionTexts() const
{
    QStringList texts;
    if (!m_menu)
        return texts;
    for (QAction *action : m_menu->actions()) {
        if (action->isSeparator())
            continue;
        texts << action->text();
    }
    return texts;
}

} // namespace seashot
