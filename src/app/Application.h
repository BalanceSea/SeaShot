// 应用编排：单实例、托盘、快捷键、选择模式、截图、剪贴板、结果浮层
#pragma once

#include "backend/BackendFactory.h"
#include "core/Types.h"
#include "editor/Annotation.h"
#include "overlay/Overlay.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <memory>

class QLocalServer;
class QLockFile;
class QWidget;

namespace seashot {

class Settings;
class Overlay;
class Toast;
class SaveExportDialog;
class Tray;
class CaptureBackend;
class GifRecorder;
class RecordingIndicator;
class PinWindow;
class ScrollCapturer;
class GlobalShortcutsPortal;

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
    QString pinPath; ///< --pin PATH：把一张图贴（钉）到屏幕上（Phase C）
    bool scrollCapture = false; ///< --scroll-capture：滚动截屏（Phase D）
    int scrollMs = 0;           ///< --scroll-ms N：滚动截屏最多跑 N 毫秒（0 = 用默认值）
    QString ocrPath;            ///< --ocr PATH：对一张图做文字识别（Phase E）
    QString ocrLangs;           ///< --ocr-langs "chi_sim+eng"：指定语言（默认自动）
    /// Phase F：全局快捷键走 xdg-desktop-portal GlobalShortcuts（应用自己申请、用户授权）
    bool hotkeyPortalStatus = false; ///< --hotkey-portal-status：报告门户可用性与当前绑定
    bool hotkeyPortalRequest = false;///< --hotkey-portal-request：发起注册（第一次会弹授权对话框）
    QString hotkeyPortalTrigger;     ///< --hotkey-portal-trigger：期望的按键（空 = 让用户在对话框里选）
    int hotkeyPortalMs = 180000;     ///< --hotkey-portal-ms：等用户应答的上限
    int hotkeyPortalListenMs = 0;    ///< --hotkey-portal-listen-ms：授权后继续监听按键多久（0=不等）
    QString hotkeyPortalId;          ///< --hotkey-portal-id：快捷键 id（默认 screenshot）
    bool hotkeyPortalConfigure = false; ///< --hotkey-portal-configure：授权后打开系统的改键对话框
    QString pinDemoOut; ///< --pin-demo PREFIX：贴图的自动化验收（Phase C）
    QString inplaceDemoOut; ///< --inplace-demo PATH：就地标注（Phase A1）的自动化验收
    QString benchCapturePath; ///< --bench-capture PATH：截全屏存盘并打印帧时间戳（给基准测试当观测者）
    bool probePortal = false; ///< --probe-portal-screencast：验证 portal 授权持久化
    QString portalTokenFile;  ///< 存/取 ScreenCast restore token 的文件
    bool probePortalNoStart = false; ///< 只走到 SelectSources（不弹授权对话框）
    int probePipewireFrames = 0;     ///< >0 时：从 portal 的 PipeWire fd 消费这么多帧（接帧验证）
    QString probePipewireOut;        ///< 接帧落盘目录
    int probePipewireLib = 0;        ///< >0 时：用 libpipewire 直连取这么多帧（与录制器同一套代码）
};

struct RecordingRequest {
    bool hasRect = false;
    QRectF rect;
    QString saveTo;
    int seconds = 0;
    bool quitWhenFinished = false;
};

class Application : public QObject
{
    Q_OBJECT

public:
    explicit Application(const Options &options, QObject *parent = nullptr);
    Application(const Options &options, std::unique_ptr<CaptureBackend> backend, QObject *parent);
    ~Application() override;

    /// 返回进程退出码
    int run();

private:
    friend class TestApplication;
    /// 打开编辑器（截图结果浮层上的「编辑」按钮、以及 --edit 都走这里）
    void openEditor(const QImage &image, const QString &sourcePath);
    /// 录制流程（Phase 4）
    void startRecordFlow();
    bool startRecordingRequest(const RecordingRequest &request);
    bool beginRecording(const SelectionResult &selection);
    void stopRecording();
    void onRecordingFinished(const QString &gifPath, int captured, int dropped, int failed,
                             qint64 elapsedMs, const QString &error);
    QString defaultGifPath() const;
    /// 打开配置界面（托盘菜单与 --settings 共用）
    void openSettings();
    /// 打开「保存 / 导出」统一窗口（截图另存为 + 录制导出都在这里）
    void openSaveExport(bool recordingPage);
    /// 配置变化后重新应用（浮层超时/托盘/阴影/快捷键）
    void applySettings();

    void applyHotkey(const QString &sequence);
    void startResident();
    /// 开始一次交互式截图（热键/托盘/--capture 都走这里）
    void startCapture();
    void handleSelection(const SelectionResult &result);
    /// Phase A1 就地标注：抓冻结屏幕 → 让覆盖层进入 Annotating（不打开新窗口）
    void enterAnnotationPhase();
    /// 选区阶段收尾：隐藏覆盖层并（按需等一帧后）开始截图
    void finishSelectionPhase();
    void performCapture();
    void finishCapture(const CaptureImage &image);
    void saveAs();
    /// 保存到设置里的默认目录（按文件名模板命名，不弹对话框）
    void saveToDefault();
    /// 录制导出格式
    enum class RecordingFormat { Gif, Mp4 };
    void exportRecording(RecordingFormat format);
    void exportRecordingGif();
    void exportRecordingMp4();
    void copyRecordingGif();
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
    /// 接帧验证：把 portal 给的 PipeWire fd 交给 gst-launch-1.0 取 frameCount 帧并落盘。
    /// 返回 0 = 真的出帧了。不改现有录制路径，纯诊断。
    int runPipeWireFrameProbe(int fd, int frameCount, const QString &outDir, uint nodeId);
    /// 用 libpipewire 直连取帧（与录制器同一套代码的独立验证入口）
    int runPipeWireLibProbe(int fd, uint nodeId, int frameCount, const QString &outDir);
    /// 尝试用 PipeWire 帧源开始录制（需要已有 restore token）。
    /// 失败时把原因写进 error，调用方回退到轮询路径。
    bool startPipeWireRecording(const QRectF &region, int fps, QString *error);
    void stopPipeWireRecording();
    /// ISSUE-001 诊断：程序化拖拽浮层 + 截屏定位，量出「目标位置 vs 实见位置」的偏差
    int runToastDragBench(int durationMs);
    /// Phase A1 验收：程序化合成的鼠标拖拽 → 覆盖层就地标注 → 落盘证据 +
    /// 「覆盖层画面 vs 冻结图」的逐像素 1:1 比对（选区内应完全相同）
    int runInplaceDemo(const QString &outPrefix);
    /// Phase C：贴（钉）一张图到屏幕上。anchorGlobal 是贴图左上角的目标位置。
    void pinImage(const QImage &image, const QPoint &anchorGlobal);
    /// Phase C：`--pin PATH`
    int runPinFile(const QString &path);
    /// Phase C 验收：程序化验证拖动/缩放/右键菜单/多张并存/关闭无残留
    int runPinDemo(const QString &outPrefix);
    /// Phase D：开始滚动截屏（选区已定）。用户自己滚页面，我们定时抓帧拼接。
    void startScrollCapture(const SelectionResult &selection);
    void onScrollFinished(const QImage &image, const QString &report);
    void onScrollFailed(const QString &error);
    /// Phase D：`--scroll-capture`（脚本式：给定矩形，跑到自己停/到时长上限就收工）
    int runScrollCaptureCli();
    /// Phase E：对一张图做文字识别。interactive=true 时把结果弹成面板（否则只打印）
    void startOcr(const QImage &image, bool interactive);
    /// Phase E：`--ocr PATH`
    int runOcrCli(const QString &path);
    /// Phase F：`--hotkey-portal-status`（静默查可用性与当前绑定）
    int runHotkeyPortalStatus();
    /// Phase F：`--hotkey-portal-request`（发起注册；第一次会弹系统授权对话框）
    int runHotkeyPortalRequest();
    /// Phase F：把 portal 授权结果记进配置，并接上"按下就截图"
    void applyPortalShortcut(const QString &trigger);
    /// Phase F：懒加载 portal 对象并**只接一次**信号（回调都在这里）
    class GlobalShortcutsPortal *ensureShortcutPortal();
    /// Phase F：常驻启动时静默检查"是否已授权"，是则监听 Activated
    void setupShortcutPortal();
    /// Phase F：**首次运行**自动发起一次注册（于是系统会弹授权对话框，由用户决定）。
    /// 已经授权过 / 已经用别的方式设过键 / 问过一次 → 都不再问。
    void maybeAskForHotkeyOnFirstRun();
    /// Phase F：界面/托盘发起"让 SeaShot 申请注册"（会弹系统授权对话框）
    void requestHotkeyPortal();
    /// Phase F：打开系统的快捷键配置对话框
    void configureHotkeyPortal();

signals:
    /// Phase F：一次 portal 申请流程结束（授权成功 / 被拒绝 / 超时）
    void portalFlowFinished(bool ok, const QString &trigger);

private:
    std::unique_ptr<GlobalShortcutsPortal> m_shortcutPortal;
    bool m_portalRelistPending = false; ///< bind 应答没带按键 → 再 ListShortcuts 一次确认
    bool m_firstRunAskPending = false;  ///< 首次运行：等 ListShortcuts 结果再决定要不要发起
    /// 门户那条路已经在用：此时**不再注册 KGlobalAccel**（否则系统设置里会出现两条
    /// SeaShot 快捷键、还可能抢同一个键）。用户显式 --set-hotkey 时会把它置回 false。
    bool m_portalHotkeyActive = false;
    /// 门户状态是否已经问过。没问清之前**先不注册 KGlobalAccel** ——
    /// 否则每次启动都会有一小段时间两条快捷键同时绑着（系统设置里能看到抖动），
    /// 而且会把用户已经授权好的那条覆盖成同一个键。
    bool m_portalStateKnown = false;
    /// 本次会话里是否已经把这条快捷键 bind 过。
    /// ⚠️ 门户只对"在**本会话**里 BindShortcuts 过"的快捷键派发 Activated ——
    ///    只 CreateSession + ListShortcuts 是收不到按键事件的（实测：按了没反应）。
    bool m_portalBoundInSession = false;
    QRect virtualGeometry() const;
    bool isOneShot() const;

    static QString socketPath();
    static bool sendToExistingInstance(const QString &command);
    bool startServer();
    void handleInstanceCommand(const QString &command);

    Options m_options;
    std::unique_ptr<Settings> m_settings;
    BackendChoice m_choice;
    std::unique_ptr<CaptureBackend> m_backend;
    std::unique_ptr<Overlay> m_overlay;
    /// 结果浮层（Toast）：**产品路径已退役**（截图不弹窗），只剩 ISSUE-001 的拖动基准在用
    std::unique_ptr<Toast> m_toast;
    /// 「保存 / 导出」统一窗口（懒创建）
    std::unique_ptr<SaveExportDialog> m_saveExport;
    std::unique_ptr<Tray> m_tray;
    std::unique_ptr<GifRecorder> m_recorder;
    std::unique_ptr<RecordingIndicator> m_recordingIndicator;
    bool m_recordIntent = false; ///< 正在选择录制区域
    bool m_recordStarting = false; ///< 首帧/门户等待期间禁止重入
    bool m_capturePreparing = false;
    quint64 m_captureGeneration = 0; ///< Ignore frames from cancelled capture requests.
    bool m_keepAliveForRecording = false;
    QPointer<QWidget> m_settingsDialog;
    QString m_appliedHotkey;
    bool m_hotkeyApplied = false;
    QLocalServer *m_server = nullptr;
    std::unique_ptr<QLockFile> m_instanceLock;
    RecordingRequest m_recordingRequest;
    QTimer m_recordStopTimer;
    QString m_backendError;
    QString m_backendReason;
    bool m_selfExclusion = false;
    bool m_shouldQuitAfterCapture = false;
    QImage m_lastImage;
    QString m_lastSavedPath;
    QRect m_lastAnchorRect; ///< 上一次截图的源矩形（全局坐标），结果浮层贴它左下角
    /// PipeWire 录制期间的资源：帧源 + 会话 + fd（顺序销毁很关键）
    std::shared_ptr<class PipeWireFrameSource> m_pipeWireSource;
    std::unique_ptr<class PortalScreencast> m_recordingPortal;
    int m_recordingPipeWireFd = -1;
    bool m_keepAliveForToast = false; ///< 一次性 CLI 下：等浮层消失再退出进程
    bool m_editorOpened = false; ///< 开过编辑器（一次性 CLI 下要等它关掉再退）
    /// 「完成」时上层要做的交付动作（Phase B/C：Enter / Ctrl+C / Ctrl+S / 贴图）
    Overlay::ConfirmAction m_pendingAction = Overlay::ConfirmAction::Plain;
    std::unique_ptr<ScrollCapturer> m_scrollCapturer;
    QPointer<QWidget> m_ocrWindow; ///< OCR 结果面板（一次性 CLI 下要等它关掉再退）
    /// 屏幕上的贴图（Phase C）
    QVector<QPointer<PinWindow>> m_pins;
    bool m_quitWhenPinsClosed = false;
    SelectionResult m_pendingSelection;
};

} // namespace seashot
