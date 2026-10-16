// KWin 助手脚本的宿主：
//  - 把内嵌的 seashot-kwin.js 释放到缓存目录，用 org.kde.kwin.Scripting 加载/卸载
//  - 注册会话 D-Bus 服务 org.seashot.SeaShot:/KWinBridge，接收窗口快照与读取命令队列
//  - 向脚本下发「把我们自己的窗口排除出截图」等命令
#pragma once

#include "core/Types.h"

#include <QObject>
#include <QStringList>

#include <memory>

namespace seashot {

class KWinBridge : public QObject
{
    Q_OBJECT
    // KWin 脚本用 callDBus 调的就是这个名字，必须与 resources/seashot-kwin.js 里的 IFACE 一致
    Q_CLASSINFO("D-Bus Interface", "org.seashot.KWinBridge")

public:
    explicit KWinBridge(QObject *parent = nullptr);
    ~KWinBridge() override;

    /// 注册 D-Bus 服务 → 释放脚本 → loadScript → run → 等待首个快照
    bool start(QString *error);
    void stop();
    bool isRunning() const { return m_running; }

    QVector<WindowInfo> windows() const { return m_windows; }
    QString activeWindowId() const;

    /// 把 appId 含 "seashot" 的窗口设成排除出截图（Phase 0 实测有效的自排除手段）
    bool requestExcludeOwnWindows(bool value, QString *error);

    /// 是否有窗口被标记为排除出截图（验收用）
    bool ownWindowsExcluded() const;

    QString diagnostics() const;

public slots:
    /// KWin 脚本 → 本进程：窗口快照 JSON
    void Publish(const QString &json);
    /// 本进程 → KWin 脚本：取一条待执行命令（空字符串 = 没有命令）
    QString TakeCommand();

signals:
    void windowsChanged();

private:
    bool writeScriptToCache(QString *path, QString *error) const;
    void enqueue(const QString &command);
    /// 清理上次崩溃/被 kill 留下的 KWin 助手脚本
    void sweepStaleScripts();

    /// 每个进程用自己的 D-Bus 服务名（否则第二个 SeaShot 实例无法启动 —— Phase 1 验收时踩到过）
    static QString serviceName();

    QStringList m_commands;
    QVector<WindowInfo> m_windows;
    bool m_running = false;
    bool m_serviceRegistered = false;
    QString m_pluginName;
    qint64 m_lastSnapshotMs = 0;
    int m_publishedCount = 0;
};

} // namespace seashot
