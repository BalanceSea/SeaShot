// xdg-desktop-portal ScreenCast 会话（第一阶段：会话建立 + 授权持久化验证）
//
// 目的与现状（先说清楚代价）：
//  - Wayland 下客户端拿不到屏幕内容，除非用户批准过一次 —— 没有任何客户端能绕过；
//  - 但**不是每次都要**：CreateSession 里带 persist_mode=2（PersistUntilRevoked）时，
//    KDE 的授权对话框会出现「记住/允许恢复」项，portal 会回一个 restore_token；
//    之后用这个 token 建会话就**不再弹窗**，且 token 落盘（~/.local/share/flatpak/db/screencast）
//    —— 本机已确认 OBS 就有这样的记录；
//  - 这就意味着走这条路，**首次必须点一次**。当前基于 ScreenShot2 轮询的实现一次都不弹。
//
// 本文件目前只做"建会话 + 拿 token + 可选 OpenPipeWireRemote"，供 --probe-portal-screencast
// 验证授权持久化；帧管线（消费 PipeWire 流）是第二步，确认静默后再接。
#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <functional>

namespace seashot {

class PortalScreencast : public QObject
{
    Q_OBJECT

public:
    struct StreamInfo {
        uint nodeId = 0;
        QVariantMap properties;
    };

    struct Result {
        bool ok = false;
        QString sessionPath;
        QString restoreToken; ///< 下次 CreateSession 带上它就能免弹窗
        QVector<StreamInfo> streams;
        bool streamsPresent = false; ///< Start 结果里是否有 streams（不手工迭代 a(ua{sv})）
        qint64 startLatencyMs = -1; ///< 从 Start() 到用户应答的耗时（判断有没有弹窗）
        bool usedRestoreToken = false;
        QString error;

        QString describe() const;
    };

    using Callback = std::function<void(const Result &)>;

    explicit PortalScreencast(QObject *parent = nullptr);

    /// 建立一次 ScreenCast 会话。restoreToken 为空 = 首次（会弹授权对话框）。
    /// persistMode: 0=不持久化, 1=本次运行有效, 2=持久直到撤销
    void start(const QString &restoreToken, uint persistMode, Callback callback,
               bool stopBeforeStart = false);

    /// 拿到 PipeWire 远程连接的 fd（帧管线要用；失败返回 -1）
    int openPipeWireRemote(QString *error = nullptr);

    /// 结束会话（释放流）
    void closeSession();
    QString sessionPath() const { return m_sessionPath; }

    /// 本次进程自报的 app id（= desktop 文件名去后缀）。
    /// ⚠️ 不注册的话，portal 眼里的调用者是个"无名进程"，KDE 会因此**拒绝持久化**：
    ///    既不给 restore_token，也不写 ~/.local/share/flatpak/db/screencast
    ///    （本机实测：OBS 有记录，seashot 没有 —— 差别就在这里）。
    static QString hostAppId();

    /// 向 org.freedesktop.host.portal.Registry 注册 app id（必须在其它 portal 调用之前）。
    static bool registerHostAppId(const QString &appId, QString *error = nullptr);

    /// ScreenCast portal 的接口版本（探测/自检用）
    static int portalVersion();

public slots:
    /// portal 的 Request.Response 应答（内部信号路由，不对外使用）
    void onPortalResponse(uint response, const QVariantMap &results);

private:
    void callWithResponse(const QString &interface, const QString &method,
                          const QVariantList &leadingArgs, const QVariantMap &options,
                          const std::function<void(uint, const QVariantMap &)> &callback);
    void fail(const QString &message);
    static QString requestPathFor(const QString &token);
    static QString newToken(const QString &prefix);

    QString m_sessionPath;
    std::function<void(uint, const QVariantMap &)> m_responseHandler;
    QString m_sender;
    Callback m_callback;
    bool m_finished = false;
};

} // namespace seashot
