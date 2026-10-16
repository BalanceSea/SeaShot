// 配置持久化（~/.config/seashot/seashot.conf），改动即时生效
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

namespace seashot {

class Settings : public QObject
{
    Q_OBJECT

public:
    explicit Settings(QObject *parent = nullptr);

    bool copyAfterCapture() const;
    void setCopyAfterCapture(bool value);

    QString saveDir() const;
    void setSaveDir(const QString &value);

    QString filenameTemplate() const;
    void setFilenameTemplate(const QString &value);

    bool autoSaveAfterCapture() const;
    void setAutoSaveAfterCapture(bool value);

    /// 截图后直接打开编辑器（浮层不再弹）—— Phase 2 后新增
    bool openEditorAfterCapture() const;
    void setOpenEditorAfterCapture(bool value);

    bool keepShadowMargin() const;
    void setKeepShadowMargin(bool value);

    /// 全局快捷键（空 = 不绑定；PLAN 已定默认不绑定）
    QString hotkey() const;
    void setHotkey(const QString &value);

    /// 录制中间格式：
    ///  - "lossless"（默认）：无损 FFV1，之后转 GIF/MP4 都只损失一次，但临时文件很大
    ///    （实测全屏 1920x1080 ≈554 MB/分钟）
    ///  - "mp4"：直接录 H.264 MP4，临时文件小得多；转 GIF 会多一次有损
    /// 未识别的值一律按 "lossless" 处理。
    QString recordFormat() const;
    void setRecordFormat(const QString &value);
    bool recordLossless() const;

    int gifFps() const;
    void setGifFps(int value);

    bool trayPersistent() const;
    void setTrayPersistent(bool value);

    /// 结果浮层的兼容超时（0 = 不自动消失，默认）
    int toastAutoHideMs() const;
    void setToastAutoHideMs(int value);

    QString configFilePath() const;

    /// 恢复默认（删掉所有键，下次读取就是默认值）
    void clearAll();

    /// 按模板生成默认保存路径（应用 saveDir + filenameTemplate；不覆盖同名文件）
    QString defaultSavePath(const QString &suffix = QStringLiteral("png")) const;
    /// 保存图像到指定路径（PNG），失败写 *error
    static bool saveImage(const QString &path, const QImage &image, QString *error);

signals:
    void changed();

private:
    QString uniquePath(const QString &directory, const QString &baseName, const QString &suffix) const;
};

} // namespace seashot
