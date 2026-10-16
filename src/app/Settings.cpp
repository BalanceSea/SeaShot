#include "app/Settings.h"

#include "util/Log.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace seashot {

namespace {
constexpr const char *kCopyAfterCapture = "capture/copyAfterCapture";
constexpr const char *kSaveDir = "capture/saveDir";
constexpr const char *kFilenameTemplate = "capture/filenameTemplate";
constexpr const char *kAutoSave = "capture/autoSave";
constexpr const char *kOpenEditor = "capture/openEditorAfterCapture";
constexpr const char *kKeepShadow = "capture/keepShadowMargin";
constexpr const char *kHotkey = "shortcut/hotkey";
constexpr const char *kGifFps = "record/fps";
constexpr const char *kTrayPersistent = "ui/trayPersistent";
constexpr const char *kToastAutoHideMs = "ui/toastAutoHideMs";
} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    // 组织/应用名在 main() 里设置为 seashot，配置文件即 ~/.config/seashot/seashot.conf
}

QString Settings::configFilePath() const
{
    return QSettings().fileName();
}

bool Settings::copyAfterCapture() const
{
    return QSettings().value(QLatin1String(kCopyAfterCapture), true).toBool();
}

void Settings::setCopyAfterCapture(bool value)
{
    QSettings().setValue(QLatin1String(kCopyAfterCapture), value);
    emit changed();
}

QString Settings::saveDir() const
{
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        + QStringLiteral("/Screenshots");
    return QSettings().value(QLatin1String(kSaveDir), fallback).toString();
}

void Settings::setSaveDir(const QString &value)
{
    QSettings().setValue(QLatin1String(kSaveDir), value);
    emit changed();
}

QString Settings::filenameTemplate() const
{
    return QSettings().value(QLatin1String(kFilenameTemplate), QStringLiteral("seashot_%1"))
        .toString();
}

void Settings::setFilenameTemplate(const QString &value)
{
    QSettings().setValue(QLatin1String(kFilenameTemplate), value);
    emit changed();
}

bool Settings::autoSaveAfterCapture() const
{
    return QSettings().value(QLatin1String(kAutoSave), false).toBool();
}

void Settings::setAutoSaveAfterCapture(bool value)
{
    QSettings().setValue(QLatin1String(kAutoSave), value);
    emit changed();
}

bool Settings::keepShadowMargin() const{
    return QSettings().value(QLatin1String(kKeepShadow), true).toBool();
}

void Settings::setKeepShadowMargin(bool value)
{
    QSettings().setValue(QLatin1String(kKeepShadow), value);
    emit changed();
}

bool Settings::openEditorAfterCapture() const
{
    return QSettings().value(QLatin1String(kOpenEditor), false).toBool();
}

void Settings::setOpenEditorAfterCapture(bool value)
{
    QSettings().setValue(QLatin1String(kOpenEditor), value);
    emit changed();
}

QString Settings::hotkey() const
{
    return QSettings().value(QLatin1String(kHotkey), QString()).toString();
}

void Settings::setHotkey(const QString &value)
{
    QSettings().setValue(QLatin1String(kHotkey), value);
    emit changed();
}

int Settings::gifFps() const
{
    return QSettings().value(QLatin1String(kGifFps), 15).toInt();
}

void Settings::setGifFps(int value)
{
    QSettings().setValue(QLatin1String(kGifFps), value);
    emit changed();
}

bool Settings::trayPersistent() const
{
    return QSettings().value(QLatin1String(kTrayPersistent), true).toBool();
}

void Settings::setTrayPersistent(bool value)
{
    QSettings().setValue(QLatin1String(kTrayPersistent), value);
    emit changed();
}

int Settings::toastAutoHideMs() const
{
    return QSettings().value(QLatin1String(kToastAutoHideMs), 0).toInt();
}

void Settings::setToastAutoHideMs(int value)
{
    QSettings().setValue(QLatin1String(kToastAutoHideMs), qMax(0, value));
    emit changed();
}

QString Settings::uniquePath(const QString &directory, const QString &baseName,
                             const QString &suffix) const
{
    QDir dir(directory);
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));

    QString candidate = dir.filePath(QStringLiteral("%1.%2").arg(baseName, suffix));
    if (!QFileInfo::exists(candidate))
        return candidate;
    for (int i = 1; i < 10000; ++i) {
        candidate = dir.filePath(QStringLiteral("%1_%2.%3").arg(baseName).arg(i).arg(suffix));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return dir.filePath(QStringLiteral("%1_%2.%3")
                            .arg(baseName)
                            .arg(QDateTime::currentMSecsSinceEpoch())
                            .arg(suffix));
}

QString Settings::defaultSavePath(const QString &suffix) const
{
    QString base = filenameTemplate();
    base.replace(QStringLiteral("%1"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    base.replace(QStringLiteral("%d"), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
    base.replace(QStringLiteral("%t"), QDateTime::currentDateTime().toString(QStringLiteral("HHmmss")));
    return uniquePath(saveDir(), base, suffix);
}

bool Settings::saveImage(const QString &path, const QImage &image, QString *error)
{
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("空图像");
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!image.save(path, "PNG")) {
        if (error)
            *error = QStringLiteral("写入失败: %1").arg(path);
        return false;
    }
    SS_INFO() << "[settings] 已保存" << path;
    return true;
}

} // namespace seashot
