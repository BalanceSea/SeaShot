// SeaShot 入口：命令行解析 + 应用元数据
#include "app/Application.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTextStream>

#include <LayerShellQt/Shell>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("seashot"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("seashot"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("seashot.local"));
    // app_id → KWin 侧 resourceClass；自排除与 --selftest 的 Exec 比对都依赖它
    QGuiApplication::setDesktopFileName(QStringLiteral("seashot"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("SeaShot"));
    QGuiApplication::setWindowIcon(seashot::ui::appIcon());

    // layer-shell 必须在创建窗口前启用（Phase 0 已实测可用）
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        LayerShellQt::Shell::useLayerShell();

    // 默认把 info 级日志打开：SeaShot 的诊断（[kwin]/[shot2]/[toast]/[app]）依赖它
    QLoggingCategory::setFilterRules(QStringLiteral("*.info=true\n*.debug=false"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("SeaShot —— KDE Plasma 6 (Wayland/X11) 截图工具"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption selftestOption(QStringLiteral("selftest"),
                                      QStringLiteral("逐项检查截图能力并打印结论"));
    QCommandLineOption listOption(QStringLiteral("list-windows"),
                                  QStringLiteral("打印当前窗口列表（含几何与栈序）"));
    QCommandLineOption captureOption(QStringLiteral("capture"),
                                     QStringLiteral("开始一次交互式截图（已有实例时转发给它）"));
    QCommandLineOption screenOption(QStringLiteral("capture-screen"),
                                    QStringLiteral("免交互：截取整块虚拟桌面并退出"));
    QCommandLineOption rectOption(QStringLiteral("rect"),
                                  QStringLiteral("免交互：截取矩形 x,y,w,h"), QStringLiteral("x,y,w,h"));
    QCommandLineOption windowOption(QStringLiteral("window"),
                                    QStringLiteral("免交互：按 handle 截取窗口"), QStringLiteral("handle"));
    QCommandLineOption saveOption(QStringLiteral("save"),
                                  QStringLiteral("把结果保存到该路径"), QStringLiteral("path"));
    QCommandLineOption noCopyOption(QStringLiteral("no-copy"),
                                    QStringLiteral("本次不写入剪贴板（覆盖设置）"));
    QCommandLineOption overlayHoldOption(
        QStringLiteral("overlay-hold"),
        QStringLiteral("显示选择覆盖层若干毫秒后退出（验收/调试用）"), QStringLiteral("ms"));
    QCommandLineOption backendOption(QStringLiteral("backend"),
                                     QStringLiteral("后端：auto|wayland|x11"), QStringLiteral("name"),
                                     QStringLiteral("auto"));
    QCommandLineOption editOption(QStringLiteral("edit"),
                                  QStringLiteral("用编辑器打开一张 PNG"), QStringLiteral("path"));
    QCommandLineOption annotateInOption(QStringLiteral("annotate-in"),
                                        QStringLiteral("（诊断/验收）标注演示的输入图"),
                                        QStringLiteral("path"));
    QCommandLineOption annotateOutOption(QStringLiteral("annotate-out"),
                                         QStringLiteral("（诊断/验收）标注演示的输出图"),
                                         QStringLiteral("path"));
    QCommandLineOption openEditorOption(
        QStringLiteral("open-editor"),
        QStringLiteral("本次截图后直接打开编辑器（覆盖配置里的 capture/openEditorAfterCapture）"));
    QCommandLineOption settingsOption(QStringLiteral("settings"),
                                      QStringLiteral("打开配置界面"));
    QCommandLineOption setHotkeyOption(QStringLiteral("set-hotkey"),
                                       QStringLiteral("写入并注册全局快捷键（none = 解绑）"),
                                       QStringLiteral("keys"));
    QCommandLineOption hotkeyStatusOption(QStringLiteral("hotkey-status"),
                                          QStringLiteral("打印当前实际生效的快捷键"));
    QCommandLineOption printConfigOption(QStringLiteral("print-config"),
                                         QStringLiteral("打印生效中的配置"));
    QCommandLineOption recordOption(
        QStringLiteral("record"),
        QStringLiteral("录制 GIF：交互选区域（配合 --rect 可直接用矩形，配合 --seconds 自动停）"));
    QCommandLineOption recordStopOption(QStringLiteral("record-stop"),
                                        QStringLiteral("让常驻实例停止正在进行的录制"));
    QCommandLineOption secondsOption(QStringLiteral("seconds"),
                                     QStringLiteral("录制 N 秒后自动停止（0 = 手动停）"),
                                     QStringLiteral("N"), QStringLiteral("0"));
    QCommandLineOption dragBenchOption(
        QStringLiteral("toast-drag-bench"),
        QStringLiteral("（诊断）程序化拖动结果浮层 MS 毫秒，量出目标位置与实见位置的偏差"),
        QStringLiteral("ms"));
    QCommandLineOption probePortalOption(
        QStringLiteral("probe-portal-screencast"),
        QStringLiteral("（诊断）建立 portal ScreenCast 会话，验证授权持久化（首次弹窗、之后静默）"));
    QCommandLineOption probePortalNoStartOption(
        QStringLiteral("probe-portal-no-start"),
        QStringLiteral("（诊断）portal 链路自检：只到 SelectSources，不弹授权对话框"));
    QCommandLineOption probePipewireFramesOption(
        QStringLiteral("probe-pipewire-frames"),
        QStringLiteral("（诊断）接帧验证：从 portal 的 PipeWire fd 消费 N 帧并落盘（需 gst-launch-1.0）"),
        QStringLiteral("count"), QStringLiteral("0"));
    QCommandLineOption probePipewireOutOption(
        QStringLiteral("probe-pipewire-out"),
        QStringLiteral("（诊断）接帧落盘目录"),
        QStringLiteral("dir"), QStringLiteral("/tmp/seashot-pipewire"));
    QCommandLineOption probePipewireLibOption(
        QStringLiteral("probe-pipewire-lib"),
        QStringLiteral("（诊断）用 libpipewire 直连取 N 帧（与录制器同一套代码），落盘并报帧率"),
        QStringLiteral("count"), QStringLiteral("0"));
    QCommandLineOption portalTokenOption(
        QStringLiteral("portal-token-file"),
        QStringLiteral("（诊断）存/取 ScreenCast restore token 的文件"),
        QStringLiteral("path"),
        QDir::homePath() + QStringLiteral("/.cache/seashot/screencast.token"));
    QCommandLineOption benchCaptureOption(
        QStringLiteral("bench-capture"),
        QStringLiteral("（诊断）抓一帧全屏存盘并打印帧时间戳，给拖动基准当观测者"),
        QStringLiteral("path"));
    QCommandLineOption ocrOption(QStringLiteral("ocr"),
                                 QStringLiteral("（Phase E）对一张图做文字识别，结果打到 stdout"),
                                 QStringLiteral("path"));
    QCommandLineOption ocrLangsOption(
        QStringLiteral("ocr-langs"),
        QStringLiteral("OCR 语言（默认自动：装了中英就用 chi_sim+eng）"),
        QStringLiteral("langs"), QStringLiteral(""));
    QCommandLineOption scrollCaptureOption(
        QStringLiteral("scroll-capture"),
        QStringLiteral("滚动截屏（Phase D）：自己滚页面，SeaShot 定时抓帧拼成一张长图；"
                       "配合 --rect 可脚本化，配合 --scroll-ms 限制最长运行时间"),
        QStringLiteral(""));
    QCommandLineOption scrollMsOption(
        QStringLiteral("scroll-ms"),
        QStringLiteral("滚动截屏最多跑 N 毫秒（默认 20000）"),
        QStringLiteral("ms"), QStringLiteral("20000"));
    QCommandLineOption pinOption(QStringLiteral("pin"),
                                 QStringLiteral("把一张 PNG 贴（钉）到屏幕上（Phase C）"),
                                 QStringLiteral("path"));
    QCommandLineOption pinDemoOption(
        QStringLiteral("pin-demo"),
        QStringLiteral("（验收）贴图：程序化验证拖动/缩放/右键菜单/多张并存/关闭，证据写入 <prefix>-*.png"),
        QStringLiteral("prefix"));
    QCommandLineOption inplaceDemoOption(
        QStringLiteral("inplace-demo"),
        QStringLiteral("（验收）就地标注：合成一次拖框 → 校验 1:1 与手柄选区，证据写入 <prefix>-overlay.png / <prefix>-frozen.png"),
        QStringLiteral("prefix"));

    parser.addOption(selftestOption);
    parser.addOption(listOption);
    parser.addOption(captureOption);
    parser.addOption(screenOption);
    parser.addOption(rectOption);
    parser.addOption(windowOption);
    parser.addOption(saveOption);
    parser.addOption(noCopyOption);
    parser.addOption(overlayHoldOption);
    parser.addOption(backendOption);
    parser.addOption(editOption);
    parser.addOption(annotateInOption);
    parser.addOption(annotateOutOption);
    parser.addOption(openEditorOption);
    parser.addOption(settingsOption);
    parser.addOption(setHotkeyOption);
    parser.addOption(hotkeyStatusOption);
    parser.addOption(printConfigOption);
    parser.addOption(recordOption);
    parser.addOption(recordStopOption);
    parser.addOption(secondsOption);
    parser.addOption(dragBenchOption);
    parser.addOption(probePortalOption);
    parser.addOption(probePortalNoStartOption);
    parser.addOption(probePipewireFramesOption);
    parser.addOption(probePipewireOutOption);
    parser.addOption(probePipewireLibOption);
    parser.addOption(portalTokenOption);
    parser.addOption(benchCaptureOption);
    parser.addOption(ocrOption);
    parser.addOption(ocrLangsOption);
    parser.addOption(scrollCaptureOption);
    parser.addOption(scrollMsOption);
    parser.addOption(pinOption);
    parser.addOption(pinDemoOption);
    parser.addOption(inplaceDemoOption);
    parser.process(app);

    seashot::Options options;
    options.selftest = parser.isSet(selftestOption);
    options.listWindows = parser.isSet(listOption);
    options.capture = parser.isSet(captureOption);
    options.captureScreen = parser.isSet(screenOption);
    options.saveTo = parser.value(saveOption);
    options.noCopy = parser.isSet(noCopyOption);
    options.windowHandle = parser.value(windowOption);
    options.backend = parser.value(backendOption);
    options.overlayHoldMs = parser.value(overlayHoldOption).toInt();
    options.editPath = parser.value(editOption);
    options.annotateIn = parser.value(annotateInOption);
    options.annotateOut = parser.value(annotateOutOption);
    options.openEditor = parser.isSet(openEditorOption);
    options.settings = parser.isSet(settingsOption);
    options.setHotkey = parser.value(setHotkeyOption);
    options.hotkeyStatus = parser.isSet(hotkeyStatusOption);
    options.printConfig = parser.isSet(printConfigOption);
    options.record = parser.isSet(recordOption);
    options.recordStop = parser.isSet(recordStopOption);
    options.seconds = parser.value(secondsOption).toInt();
    options.toastDragBenchMs = parser.value(dragBenchOption).toInt();
    options.benchCapturePath = parser.value(benchCaptureOption);
    options.inplaceDemoOut = parser.value(inplaceDemoOption);
    options.ocrPath = parser.value(ocrOption);
    options.ocrLangs = parser.value(ocrLangsOption);
    options.scrollCapture = parser.isSet(scrollCaptureOption);
    options.scrollMs = parser.value(scrollMsOption).toInt();
    options.pinPath = parser.value(pinOption);
    options.pinDemoOut = parser.value(pinDemoOption);
    options.probePortal = parser.isSet(probePortalOption);
    options.portalTokenFile = parser.value(portalTokenOption);
    options.probePortalNoStart = parser.isSet(probePortalNoStartOption);
    options.probePipewireFrames = parser.value(probePipewireFramesOption).toInt();
    options.probePipewireOut = parser.value(probePipewireOutOption);
    options.probePipewireLib = parser.value(probePipewireLibOption).toInt();

    if (parser.isSet(rectOption)) {
        const QStringList parts = parser.value(rectOption).split(QLatin1Char(','));
        if (parts.size() == 4) {
            options.rect = QRectF(parts.at(0).toDouble(), parts.at(1).toDouble(),
                                  parts.at(2).toDouble(), parts.at(3).toDouble());
            options.hasRect = options.rect.width() > 0 && options.rect.height() > 0;
        }
        if (!options.hasRect) {
            QTextStream(stderr) << "--rect 需要形如 100,200,300,400 的参数\n";
            return 2;
        }
    }

    seashot::Application application(options);
    return application.run();
}
