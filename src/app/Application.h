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
    SelectionResult m_pendingSelection;
};

} // namespace seashot
