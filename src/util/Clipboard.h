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
    /// 把纯文本放进剪贴板（OCR 结果用）。走 wl-copy，失败回退 QClipboard。
    static bool copyText(const QString &text, QString *error = nullptr);
    /// 把整个文件写进剪贴板（用于 GIF：动图必须整体投递，不能只给一帧）。
    /// ⚠️ wl-copy 一次只能挂一种 MIME，所以这里只能选一个最合适的：
    ///    GIF 用 image/gif（聊天/浏览器可当动图粘贴）。要"粘成文件"需另选 text/uri-list。
    static bool copyFile(const QString &path, const QString &mimeType,
                         QString *error = nullptr);
};

} // namespace seashot
