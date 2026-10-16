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
    delete m_menu;
}

void Tray::setHotkeyLabel(const QString &label)
{
    if (!m_item)
        return;
    m_item->setToolTip(QStringLiteral("camera-photo"), QStringLiteral("SeaShot"),
                       label.isEmpty() ? QStringLiteral("快捷键：未绑定（可在系统设置里绑定）")
                                       : QStringLiteral("快捷键：%1").arg(label));
}

} // namespace seashot
