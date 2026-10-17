// SeaShot 公共数据结构
#pragma once

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

namespace seashot {

/// 本进程自己的 app_id：KWin 侧用它识别我们的窗口并做「排除出截图」。
inline constexpr const char *kOwnAppId = "seashot";

/// 输出（显示器）信息。logicalGeometry 是合成器逻辑坐标（可能是小数经过取整的结果）。
struct OutputInfo {
    QString name;
    QRect logicalGeometry;
    double scale = 1.0;
};

/// 窗口信息。frameGeometry 含标题栏（KWin 实测：frame 比 client 高一个标题栏），
/// 坐标是逻辑坐标，**可能是小数**（Phase 0 实测 x=536.5829…）。
struct WindowInfo {
    QString id; ///< KWin internalId（形如 {uuid}）或 X11 window id（十六进制字符串）
    int pid = 0;
    QString appId;
    QString title;
    QRectF frameGeometry;
    QRectF clientGeometry;
    int stackIndex = -1; ///< 越大越靠上；-1 表示未知
    bool active = false;
    bool minimized = false;
    bool normal = true;
    bool visible = true;
    bool excludedFromCapture = false; ///< KWin 侧「排除出截图」标志（自检/验收用）
    bool captureOverlay = false; ///< 本应用的选框、贴图、提示层或录制指示器

    bool isValid() const { return !id.isEmpty() && !frameGeometry.isEmpty(); }
};

enum class SelectionMode { Window, Rect };

/// 用户完成的一次选区。
struct SelectionResult {
    SelectionMode mode = SelectionMode::Rect;
    WindowInfo window;     ///< mode == Window 时有效
    QRectF logicalRect;    ///< 逻辑坐标；Window 模式时等于 window.frameGeometry
    QString outputName;
};

/// 一次截取的产物。
struct CaptureImage {
    QImage image;
    double scale = 1.0;
    QString error;

    bool ok() const { return error.isEmpty() && !image.isNull(); }
    bool noAuthorized() const { return error.contains(QLatin1String("NoAuthorized")); }
};

} // namespace seashot
