// org.kde.KWin.ScreenShot2 的封装。
//
// Phase 0 实测得到的两条硬约束（必须遵守，否则挂起或白等）：
//  1) 像素由 KWin 写进我们传入的管道，响应在写完后才回；**必须异步调用 + 边读管道边等响应**，
//     阻塞式 call() 再读管道会在屏幕级图像上互等死锁。
//  2) **不能等 EOF**：KWin 不关闭它那份写端副本，等 EOF 会白等到超时。
//     用响应里的 stride*height 算精确字节数，读满即停。
#pragma once

#include "core/Types.h"

#include <QObject>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace seashot {

class ScreenShot2 : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(const CaptureImage &)>;

    explicit ScreenShot2(QObject *parent = nullptr);
    ~ScreenShot2() override;

    /// KWin 的截图服务是否在会话总线上
    bool serviceAvailable() const;

    /// org.kde.KWin.ScreenShot2 的 Version 属性（取不到返回 -1）
    int version() const;

    /// 按窗口 handle（KWin internalId，形如 {uuid}）截取。
    /// 注意：无选项时只返回**客户区**；要「所见即所得」需要 options 里带 include-decoration=true。
    void captureWindow(const QString &handle, const QVariantMap &options, Callback callback);

    /// 按逻辑坐标矩形截取（Phase 0 实测：入参是逻辑坐标，返回物理像素 + scale）
    void captureRect(const QRect &logicalRect, const QVariantMap &options, Callback callback);

    /// 按输出名截取整屏
    void captureScreen(const QString &outputName, const QVariantMap &options, Callback callback);

    bool lastNoAuthorized() const { return m_lastNoAuthorized; }

private:
    struct Request;

    void start(const QString &method, const QVariantList &args, const QVariantMap &options,
               Callback callback);
    void maybeFinish(const std::shared_ptr<Request> &request, bool force = false);
    void finish(const std::shared_ptr<Request> &request, const QString &error);

    bool m_lastNoAuthorized = false;
};

} // namespace seashot
