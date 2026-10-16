// 剪贴板：优先走 wl-copy（Phase 0 实测：图像原样往返，且 SeaShot 退出后仍可粘贴）
#pragma once

#include <QImage>
#include <QString>

namespace seashot {

class Clipboard
{
public:
    static bool wlCopyAvailable();

    /// 把图像放进系统剪贴板。成功返回 true；
    /// 失败时 *error 写入原因（wl-clipboard 不可用时会回退到 QClipboard）。
    static bool copyImage(const QImage &image, QString *error = nullptr);
};

} // namespace seashot
