// 配置界面（Phase 3）
//
// 约定：**改动即时生效**（每次控件变化就写回 QSettings 并发 Settings::changed），
// 不需要“保存”按钮；「恢复默认」= 清空配置。
// 快捷键用 QKeySequenceEdit 直接写进 KGlobalAccel（同时落盘到 settings），
// 也提供按钮跳到系统设置里的 KDE 快捷键页做更细的控制。
#pragma once

#include "app/Settings.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QKeySequenceEdit;
class QSpinBox;
class QComboBox;

namespace seashot {

class CaptureBackend;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(Settings *settings, CaptureBackend *backend, QWidget *parent = nullptr);

signals:
    /// 用户在界面里改了快捷键（Application 负责真正注册）
    void hotkeyChanged(const QString &sequence);
    /// Phase F：用户点了「向系统申请注册」——由 Application 发起 portal 请求（会弹授权对话框）
    void hotkeyPortalRequested();
    /// Phase F：用户点了「更改快捷键（系统对话框）」——由 Application 打开系统的配置对话框
    void hotkeyPortalConfigureRequested();

private:
    void buildUi();
    void loadFromSettings();
    void persist();          ///< 把控件值写回 Settings（即时生效）
    void chooseSaveDir();
    void resetToDefaults();
    void updateHotkeyStatus();
    /// Phase F：把 portal 的可用性/绑定状态写进状态标签（由 Application 回填）
public:
    void setPortalStatus(const QString &text);

    QString m_portalStatusText; ///< Phase F：portal 状态（由 Application 回填）
    Settings *m_settings = nullptr;
    CaptureBackend *m_backend = nullptr;

    QCheckBox *m_copyBox = nullptr;
    QCheckBox *m_openEditorBox = nullptr;
    QCheckBox *m_autoSaveBox = nullptr;
    QCheckBox *m_keepShadowBox = nullptr;
    QCheckBox *m_trayBox = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QLineEdit *m_templateEdit = nullptr;
    QSpinBox *m_toastSpin = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    QComboBox *m_recordFormatBox = nullptr;
    QKeySequenceEdit *m_hotkeyEdit = nullptr;
    QLabel *m_hotkeyStatus = nullptr;
    QLabel *m_configPathLabel = nullptr;
    bool m_loading = false; ///< 避免载入时触发 writeback
};

} // namespace seashot
