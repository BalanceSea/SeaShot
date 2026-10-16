// 应用编排：单实例、托盘、快捷键、选择模式、截图、剪贴板、结果浮层
#pragma once

#include "backend/BackendFactory.h"
#include "core/Types.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>

#include <memory>

class QLocalServer;
class QWidget;

namespace seashot {

class Settings;
class Overlay;
class Toast;
class Tray;
class CaptureBackend;
class GifRecorder;
class RecordingIndicator;

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
    bool settings = false; ///< --settings：打开配置界面
    QString setHotkey; ///< --set-hotkey <seq>：写入并注册快捷键后退出
    bool hotkeyStatus = false; ///< --hotkey-status：打印当前绑定的快捷键
    bool printConfig = false; ///< --print-config：打印生效中的配置
    bool record = false; ///< --record：进入录制流程（交互选区域，或配合 --rect 直接用矩形）
    bool recordStop = false; ///< --record-stop：让常驻实例停止录制
    int seconds = 0; ///< --seconds N：录制 N 秒后自动停（0 = 手动停）
    int toastDragBenchMs = 0; ///< --toast-drag-bench MS：诊断拖动延迟（ISSUE-001）
    QString benchCapturePath; ///< --bench-capture PATH：截全屏存盘并打印帧时间戳（给基准测试当观测者）
    bool probePortal = false; ///< --probe-portal-screencast：验证 portal 授权持久化
    QString portalTokenFile;  ///< 存/取 ScreenCast restore token 的文件
    bool probePortalNoStart = false; ///< 只走到 SelectSources（不弹授权对话框）
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
    /// 录制流程（Phase 4）
    void startRecordFlow();
    void beginRecording(const SelectionResult &selection);
    void stopRecording();
    void onRecordingFinished(const QString &gifPath, int captured, int dropped, int failed,
                             qint64 elapsedMs, const QString &error);
    QString defaultGifPath() const;
    /// 打开配置界面（托盘菜单与 --settings 共用）
    void openSettings();
    /// 配置变化后重新应用（浮层超时/托盘/阴影/快捷键）
    void applySettings();

    void applyHotkey(const QString &sequence);
    void startResident();
    /// 开始一次交互式截图（热键/托盘/--capture 都走这里）
    void startCapture();
    void handleSelection(const SelectionResult &result);
    void performCapture();
    void finishCapture(const CaptureImage &image);
    void saveAs();
    /// 保存到设置里的默认目录（按文件名模板命名，不弹对话框）
    void saveToDefault();
    void copyLast();
    int runSelfTest();
    int runListWindows();
    int runNonInteractiveCapture();
    int showOverlayAndHold();
    int runAnnotateDemo();
    int runPrintConfig();
    /// ISSUE-001 诊断：子进程观测版（同进程拍不到自己的 layer-shell 表面）
    int runBenchCapture(const QString &path);
    /// 验证 xdg-desktop-portal ScreenCast 的授权持久化（首次弹窗、之后静默）
    int runPortalScreencastProbe(const QString &tokenFile);
    /// ISSUE-001 诊断：程序化拖拽浮层 + 截屏定位，量出「目标位置 vs 实见位置」的偏差
    int runToastDragBench(int durationMs);
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
    std::unique_ptr<GifRecorder> m_recorder;
    std::unique_ptr<RecordingIndicator> m_recordingIndicator;
    bool m_recordIntent = false; ///< 正在选择录制区域
    bool m_keepAliveForRecording = false;
    QPointer<QWidget> m_settingsDialog;
    QString m_appliedHotkey;
    bool m_hotkeyApplied = false;
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
