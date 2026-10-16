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

    int gifFps() const;
    void setGifFps(int value);

    bool trayPersistent() const;
    void setTrayPersistent(bool value);

    QString configFilePath() const;

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
