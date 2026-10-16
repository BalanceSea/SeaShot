// 应用编排：单实例、托盘、快捷键、选择模式、截图、剪贴板、结果浮层
#pragma once

#include "backend/BackendFactory.h"
#include "core/Types.h"

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QString>

#include <memory>

class QLocalServer;

namespace seashot {

class Settings;
class Overlay;
class Toast;
class Tray;
class CaptureBackend;

struct Options {
    QString backend = QStringLiteral("auto"); ///< auto|wayland|x11
    bool selftest = false;
    bool listWindows = false;
    bool capture = false; ///< 交互式截图（已有实例时转发给它）
    bool captureScreen = false; ///< 免交互：整块虚拟桌面
    QRectF rect; ///< 免交互：--rect x,y,w,h
    bool hasRect = false;
    QString windowHandle; ///< 免交互：--window <handle>
    QString saveTo; ///< --save PATH
    bool noCopy = false; ///< 不写剪贴板（覆盖设置里的开关）
    int overlayHoldMs = 0; ///< --overlay-hold MS：显示覆盖层若干毫秒后退出（验收用）
    QString editPath; ///< --edit PATH：直接用编辑器打开一张图
    QString annotateIn; ///< --annotate-demo 的输入图
    QString annotateOut; ///< --annotate-demo 的输出图
    bool openEditor = false; ///< --open-editor：本次截图后直接打开编辑器（覆盖设置）
};

class Application : public QObject
{
    Q_OBJECT

public:
    explicit Application(const Options &options, QObject *parent = nullptr);
    ~Application() override;

    /// 返回进程退出码
    int run();

private:
    /// 打开编辑器（截图结果浮层上的「编辑」按钮、以及 --edit 都走这里）
    void openEditor(const QImage &image, const QString &sourcePath);
    void startResident();
    void startCapture();
    void handleSelection(const SelectionResult &result);
    void performCapture();
    void finishCapture(const CaptureImage &image);
    void saveAs();
    void copyLast();
    int runSelfTest();
    int runListWindows();
    int runNonInteractiveCapture();
    int showOverlayAndHold();
    int runAnnotateDemo();
    QRect virtualGeometry() const;
    bool isOneShot() const;

    static QString socketPath();
    static bool sendToExistingInstance(const QString &command);
    bool startServer();

    Options m_options;
    std::unique_ptr<Settings> m_settings;
    BackendChoice m_choice;
    std::unique_ptr<CaptureBackend> m_backend;
    std::unique_ptr<Overlay> m_overlay;
    std::unique_ptr<Toast> m_toast;
    std::unique_ptr<Tray> m_tray;
    QLocalServer *m_server = nullptr;
    QString m_backendError;
    QString m_backendReason;
    bool m_selfExclusion = false;
    bool m_shouldQuitAfterCapture = false;
    QImage m_lastImage;
    QString m_lastSavedPath;
    QRect m_lastAnchorRect; ///< 上一次截图的源矩形（全局坐标），结果浮层贴它左下角
    bool m_keepAliveForToast = false; ///< 一次性 CLI 下：等浮层消失再退出进程
    bool m_editorOpened = false; ///< 开过编辑器（一次性 CLI 下要等它关掉再退）
    SelectionResult m_pendingSelection;
};

} // namespace seashot
