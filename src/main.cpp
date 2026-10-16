// SeaShot 入口：命令行解析 + 应用元数据
#include "app/Application.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
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

    // layer-shell 必须在创建窗口前启用（Phase 0 已实测可用）
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        LayerShellQt::Shell::useLayerShell();

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
