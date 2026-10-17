// 全局快捷键：走 xdg-desktop-portal 的 **GlobalShortcuts** 接口（Phase F）
//
// 为什么用它：这是 Wayland 下"应用自己注册全局快捷键"的标准做法，而且**授权由用户决定** ——
// 应用发起 BindShortcuts 时，系统会弹一个授权对话框（列出请求的快捷键、可以改键），
// 用户点允许/拒绝。授权结果由桌面记住（按 app id），之后同 id 的请求不再弹窗。
//
// 与 KGlobalAccel 的关系（两条路并存，二选一，避免同一个键触发两次）：
//   · **portal**：应用自己申请 + 用户授权（本文件）；授权后由桌面的快捷键服务分发 Activated。
//   · KGlobalAccel：KDE 原生接口，无需授权，但用户通常要自己去系统设置里绑键。
//
// 底下这套"请求/应答"的坑与 ScreenCast 一模一样（见 record/PortalScreencast.cpp 的注释）：
//   · Response 的路径必须按规范拼：/org/freedesktop/portal/desktop/request/<sender>/<token>，
//     其中 sender 要去掉开头的 ':'、把 '.' 换成 '_'；
//   · token 只允许 [A-Za-z0-9_]；
//   · 必须先订阅 Response 再发方法调用（应答可能比方法返回还早）；
//   · 身份（app id）要显式 Register，而且 portal 的关联是**按 D-Bus 连接**的 ——
//     所以用一条私有连接，这样 KDE 才能把我们和 `seashot.desktop` 认成同一个应用（否则不记住授权）。
#pragma once

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <functional>



namespace seashot {

// BindShortcuts 的第二个参数是 a(sa{sv})。这里**用一个带流运算符的小结构体**表示
// "一个快捷键 = (id, 属性字典)" —— 与 ScreenCast 那边收 a(ua{sv}) 的做法一致。
//
// ⚠️ 别图省事直接用 QList<QPair<QString, QVariantMap>>：那样写**能编译、调用也能发出**，
//    但 Qt 在发送时会把这一项**静默丢掉**（参数从 4 个变 3 个），portal 只会回一句
//    "类型不匹配 (osa{sv}) vs (oa(sa{sv})sa{sv})"。实测踩过，非常难查。
//    结构体 + 显式运算符就没这问题（元素/数组两级的签名都能算出来）。
struct PortalShortcutEntry {
    QString id;
    QVariantMap properties;
};
QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcutEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcutEntry &entry);


class GlobalShortcutsPortal : public QObject
{
    Q_OBJECT

public:
    struct Shortcut {
        QString id;               ///< 稳定 id（我们的就是 "capture"）
        QString description;      ///< 给用户看的说明（对话框里显示）
        QString trigger;          ///< 实际生效的按键（portal 应答里回填）
        QString preferredTrigger; ///< 期望的按键（可为空 = 让用户在对话框里自己定）
    };

    struct Result {
        bool ok = false;
        bool denied = false; ///< 用户拒绝/取消（与"出错"区分开）
        QVector<Shortcut> shortcuts;
        QString error;
        qint64 latencyMs = -1; ///< 从发起到用户应答的耗时（能看出有没有弹窗）
        QString describe() const;
    };

    explicit GlobalShortcutsPortal(QObject *parent = nullptr);

    /// 接口在不在（顺带拿 version）
    static bool available(uint *version = nullptr, QString *error = nullptr);

    /// 把 Qt 的按键串转成 portal 要的 XDG 格式（**修饰符要大写**）。
    /// ⚠️ 实测：传 "Ctrl+Alt+S" 会被 KDE 判成 `Unknown modifier "Ctrl"` 直接丢掉，
    ///    结果"授权了但没有键"；必须 "CTRL+ALT+S"。
    static QString toXdgTrigger(const QString &qtSequence);

    /// 我们要注册的快捷键 id。
    /// ⚠️ 为什么特意换一个名字（不是 KGlobalAccel 那条路用的 "capture"）：
    ///    KDE 6 里 KGlobalAccel **与 portal 共用同一份快捷键存储**（kglobalshortcutsrc）。
    ///    "capture" 这个 id 早就被 KGlobalAccel 那条路建过条目了，portal 因此认为
    ///    "这个应用的这条快捷键已知" —— **静默放行、不弹授权对话框**（实测）。
    ///    换成一个没被 KGlobalAccel 用过的 id，系统才会把它当新请求，弹出授权对话框，
    ///    由用户决定允不允许 —— 这正是"让用户自己决定"的那一步。
    static QString shortcutId();

    /// 请求注册（**第一次会弹授权对话框**，用户可允许/拒绝/改键）。
    /// 同 app id 已经授权过时，桌面会静默恢复，不再弹窗。
    void requestBind(const QVector<Shortcut> &shortcuts, int timeoutMs = 180000);

    /// 打开系统的"快捷键配置"对话框，让用户改键
    void configure(const QString &parentWindow = QStringLiteral(""));
    /// 由调用方决定何时"接着往下走"（例如：bind 没拿到按键 → 自动打开配置对话框让用户选键）
    bool sessionReady() const { return !m_sessionPath.isEmpty(); }

    /// 查询当前绑定（不弹窗）
    void listShortcuts(int timeoutMs = 20000);

    /// 结束会话（进程退出/不再需要时调用）
    void closeSession();

    QString sessionPath() const { return m_sessionPath; }

signals:
    void activated(const QString &shortcutId);
    /// 用户在系统对话框里改了绑定（ConfigureShortcuts 之后会来）
    void shortcutsChanged(const GlobalShortcutsPortal::Result &result);
    void bound(const GlobalShortcutsPortal::Result &result);
    void listed(const GlobalShortcutsPortal::Result &result);
    void denied(const QString &reason);
    void failed(const QString &error);

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);
    void onShortcutsChanged(const QDBusObjectPath &session, const QVariantMap &shortcuts);
    void onActivated(const QDBusObjectPath &session, const QString &shortcutId,
                     qulonglong timestamp, const QVariantMap &options);

private:
    void createSession(const std::function<void(bool)> &then);
    void callWithResponse(const QString &method, const QVariantList &leadingArgs,
                          const QVariantMap &options,
                          const std::function<void(uint, const QVariantMap &)> &callback,
                          int timeoutMs);
    void connectActivationSignals();
    static QString newToken(const QString &prefix);
    static QString requestPathFor(const QString &token);
    static QVector<Shortcut> parseShortcuts(const QVariant &value);
    void finishResult(Result result);

    QDBusConnection m_bus;
    QString m_sessionPath;
    std::function<void(uint, const QVariantMap &)> m_responseHandler;
    QVector<Shortcut> m_requested;
    QString m_pendingMethod;
    quint64 m_generation = 0; ///< 每次调用 +1：让过期的超时定时器认得出自己已经没用
};

} // namespace seashot

Q_DECLARE_METATYPE(seashot::PortalShortcutEntry)
