#include "app/SettingsDialog.h"

#include "backend/CaptureBackend.h"
#include "util/Log.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

namespace seashot {

SettingsDialog::SettingsDialog(Settings *settings, CaptureBackend *backend, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_backend(backend)
{
    setWindowTitle(QStringLiteral("SeaShot 设置"));
    setMinimumWidth(560);
    buildUi();
    loadFromSettings();
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    // ── 截图后 ────────────────────────────────────────────────
    auto *afterGroup = new QGroupBox(QStringLiteral("截图后"), this);
    auto *afterLayout = new QVBoxLayout(afterGroup);

    m_copyBox = new QCheckBox(QStringLiteral("自动复制到剪贴板"), afterGroup);
    m_copyBox->setObjectName(QStringLiteral("copyAfterCapture"));
    m_copyBox->setToolTip(QStringLiteral("关闭后截图只弹浮层、不写剪贴板"));
    afterLayout->addWidget(m_copyBox);

    m_openEditorBox = new QCheckBox(QStringLiteral("直接打开编辑器（不再弹结果浮层）"), afterGroup);
    m_openEditorBox->setObjectName(QStringLiteral("openEditorAfterCapture"));
    afterLayout->addWidget(m_openEditorBox);

    m_autoSaveBox = new QCheckBox(QStringLiteral("自动保存一份到默认目录"), afterGroup);
    m_autoSaveBox->setObjectName(QStringLiteral("autoSaveAfterCapture"));
    afterLayout->addWidget(m_autoSaveBox);

    layout->addWidget(afterGroup);

    // ── 保存 ──────────────────────────────────────────────────
    auto *saveGroup = new QGroupBox(QStringLiteral("保存"), this);
    auto *saveForm = new QFormLayout(saveGroup);

    auto *dirRow = new QWidget(saveGroup);
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    m_dirEdit = new QLineEdit(dirRow);
    m_dirEdit->setObjectName(QStringLiteral("saveDir"));
    dirLayout->addWidget(m_dirEdit, 1);
    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), dirRow);
    browseButton->setObjectName(QStringLiteral("browseSaveDir"));
    dirLayout->addWidget(browseButton);
    saveForm->addRow(QStringLiteral("默认保存目录"), dirRow);

    m_templateEdit = new QLineEdit(saveGroup);
    m_templateEdit->setObjectName(QStringLiteral("filenameTemplate"));
    m_templateEdit->setToolTip(QStringLiteral("可用 %1 = 时间戳（yyyyMMdd_HHmmss），%d = 日期，%t = 时间；同名自动加序号"));
    saveForm->addRow(QStringLiteral("文件名模板"), m_templateEdit);
    saveForm->addRow(QString(), new QLabel(QStringLiteral("提示：%1 = 时间戳，%d = 日期，%t = 时间；同名自动加序号"), saveGroup));

    m_keepShadowBox = new QCheckBox(QStringLiteral("窗口截图保留阴影/装饰边距"), saveGroup);
    m_keepShadowBox->setObjectName(QStringLiteral("keepShadowMargin"));
    saveForm->addRow(QString(), m_keepShadowBox);

    layout->addWidget(saveGroup);

    // ── 界面 ──────────────────────────────────────────────────
    auto *uiGroup = new QGroupBox(QStringLiteral("界面"), this);
    auto *uiForm = new QFormLayout(uiGroup);

    m_toastSpin = new QSpinBox(uiGroup);
    m_toastSpin->setObjectName(QStringLiteral("toastAutoHideMs"));
    m_toastSpin->setRange(0, 60000);
    m_toastSpin->setSingleStep(500);
    m_toastSpin->setSuffix(QStringLiteral(" ms"));
    m_toastSpin->setSpecialValueText(QStringLiteral("不自动消失"));
    uiForm->addRow(QStringLiteral("结果浮层自动消失"), m_toastSpin);

    m_trayBox = new QCheckBox(QStringLiteral("常驻托盘"), uiGroup);
    m_trayBox->setObjectName(QStringLiteral("trayPersistent"));
    uiForm->addRow(QString(), m_trayBox);

    layout->addWidget(uiGroup);

    // ── 快捷键 ────────────────────────────────────────────────
    auto *keyGroup = new QGroupBox(QStringLiteral("全局快捷键"), this);
    auto *keyLayout = new QVBoxLayout(keyGroup);

    auto *keyRow = new QHBoxLayout();
    m_hotkeyEdit = new QKeySequenceEdit(keyGroup);
    m_hotkeyEdit->setObjectName(QStringLiteral("hotkey"));
    m_hotkeyEdit->setMaximumSequenceLength(1);
    keyRow->addWidget(new QLabel(QStringLiteral("截取屏幕："), keyGroup));
    keyRow->addWidget(m_hotkeyEdit, 1);
    auto *clearButton = new QPushButton(QStringLiteral("清除"), keyGroup);
    clearButton->setObjectName(QStringLiteral("clearHotkey"));
    keyRow->addWidget(clearButton);
    keyLayout->addLayout(keyRow);

    m_hotkeyStatus = new QLabel(keyGroup);
    m_hotkeyStatus->setObjectName(QStringLiteral("hotkeyStatus"));
    m_hotkeyStatus->setWordWrap(true);
    keyLayout->addWidget(m_hotkeyStatus);

    auto *kdeButton = new QPushButton(QStringLiteral("打开 KDE 快捷键设置…"), keyGroup);
    kdeButton->setObjectName(QStringLiteral("openKdeKeys"));
    kdeButton->setToolTip(QStringLiteral("在「系统设置 → 快捷键」里给 SeaShot 的 capture 动作绑定/改键"));
    keyLayout->addWidget(kdeButton);

    layout->addWidget(keyGroup);

    // ── 录制（Phase 4 生效） ──────────────────────────────────
    auto *recordGroup = new QGroupBox(QStringLiteral("GIF 录制（Phase 4 生效）"), this);
    auto *recordForm = new QFormLayout(recordGroup);
    m_fpsSpin = new QSpinBox(recordGroup);
    m_fpsSpin->setObjectName(QStringLiteral("gifFps"));
    m_fpsSpin->setRange(1, 60);
    m_fpsSpin->setSuffix(QStringLiteral(" fps"));
    recordForm->addRow(QStringLiteral("帧率"), m_fpsSpin);
    layout->addWidget(recordGroup);

    // ── 底部 ──────────────────────────────────────────────────
    m_configPathLabel = new QLabel(this);
    m_configPathLabel->setObjectName(QStringLiteral("configPath"));
    m_configPathLabel->setWordWrap(true);
    layout->addWidget(m_configPathLabel);

    auto *buttons = new QDialogButtonBox(this);
    auto *resetButton = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    resetButton->setObjectName(QStringLiteral("resetDefaults"));
    auto *closeButton = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::AcceptRole);
    closeButton->setObjectName(QStringLiteral("close"));
    layout->addWidget(buttons);

    // ── 连线：改动即时生效 ────────────────────────────────────
    const auto writeback = [this]() { persist(); };
    connect(m_copyBox, &QCheckBox::toggled, this, writeback);
    connect(m_openEditorBox, &QCheckBox::toggled, this, writeback);
    connect(m_autoSaveBox, &QCheckBox::toggled, this, writeback);
    connect(m_keepShadowBox, &QCheckBox::toggled, this, writeback);
    connect(m_trayBox, &QCheckBox::toggled, this, writeback);
    connect(m_toastSpin, &QSpinBox::valueChanged, this, writeback);
    connect(m_fpsSpin, &QSpinBox::valueChanged, this, writeback);
    connect(m_dirEdit, &QLineEdit::textChanged, this, writeback);
    connect(m_templateEdit, &QLineEdit::textChanged, this, writeback);
    connect(browseButton, &QPushButton::clicked, this, &SettingsDialog::chooseSaveDir);
    connect(m_hotkeyEdit, &QKeySequenceEdit::editingFinished, this, [this]() {
        if (m_loading)
            return;
        m_settings->setHotkey(m_hotkeyEdit->keySequence().toString(QKeySequence::NativeText));
        emit hotkeyChanged(m_settings->hotkey());
        updateHotkeyStatus();
    });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        m_hotkeyEdit->clear();
        m_settings->setHotkey(QString());
        emit hotkeyChanged(QString());
        updateHotkeyStatus();
    });
    connect(kdeButton, &QPushButton::clicked, this, [this]() {
        const QString program = QStandardPaths::findExecutable(QStringLiteral("systemsettings"));
        if (program.isEmpty()) {
            m_hotkeyStatus->setText(QStringLiteral("找不到 systemsettings，请手动打开：系统设置 → 快捷键"));
            return;
        }
        QStringList args{QStringLiteral("kcm_keys")};
        if (!QProcess::startDetached(program, args))
            m_hotkeyStatus->setText(QStringLiteral("启动 systemsettings 失败"));
    });
    connect(resetButton, &QPushButton::clicked, this, &SettingsDialog::resetToDefaults);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void SettingsDialog::loadFromSettings()
{
    m_loading = true;
    m_copyBox->setChecked(m_settings->copyAfterCapture());
    m_openEditorBox->setChecked(m_settings->openEditorAfterCapture());
    m_autoSaveBox->setChecked(m_settings->autoSaveAfterCapture());
    m_keepShadowBox->setChecked(m_settings->keepShadowMargin());
    m_trayBox->setChecked(m_settings->trayPersistent());
    m_dirEdit->setText(m_settings->saveDir());
    m_templateEdit->setText(m_settings->filenameTemplate());
    m_toastSpin->setValue(m_settings->toastAutoHideMs());
    m_fpsSpin->setValue(m_settings->gifFps());
    m_hotkeyEdit->setKeySequence(QKeySequence(m_settings->hotkey()));
    m_configPathLabel->setText(QStringLiteral("配置文件：%1（改动立即生效并落盘）")
                                   .arg(m_settings->configFilePath()));
    m_loading = false;
    updateHotkeyStatus();
}

void SettingsDialog::persist()
{
    if (m_loading)
        return;
    m_settings->setCopyAfterCapture(m_copyBox->isChecked());
    m_settings->setOpenEditorAfterCapture(m_openEditorBox->isChecked());
    m_settings->setAutoSaveAfterCapture(m_autoSaveBox->isChecked());
    m_settings->setKeepShadowMargin(m_keepShadowBox->isChecked());
    m_settings->setTrayPersistent(m_trayBox->isChecked());
    m_settings->setSaveDir(m_dirEdit->text().trimmed());
    m_settings->setFilenameTemplate(m_templateEdit->text());
    m_settings->setToastAutoHideMs(m_toastSpin->value());
    m_settings->setGifFps(m_fpsSpin->value());
    SS_INFO() << "[settings] 已更新（即时生效）";
    QTextStream(stdout) << "SETTINGS_UPDATED copy=" << (m_copyBox->isChecked() ? 1 : 0)
                        << " openEditor=" << (m_openEditorBox->isChecked() ? 1 : 0)
                        << " autoSave=" << (m_autoSaveBox->isChecked() ? 1 : 0)
                        << " toastAutoHideMs=" << m_toastSpin->value()
                        << " fps=" << m_fpsSpin->value() << "\n";
}

void SettingsDialog::chooseSaveDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择默认保存目录"), m_dirEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_dirEdit->setText(dir);
}

void SettingsDialog::resetToDefaults()
{
    m_settings->clearAll();
    // clearAll 会清掉所有键 → 重新载入界面，让控件回到默认值
    m_hotkeyEdit->clear();
    loadFromSettings();
    emit hotkeyChanged(QString());
    QTextStream(stdout) << "SETTINGS_RESET\n";
}

void SettingsDialog::updateHotkeyStatus()
{
    if (!m_hotkeyStatus)
        return;
    const QString configured = m_settings->hotkey();
    QString actual = QStringLiteral("(未知)");
    if (m_backend) {
        // 让后端把当前 KGlobalAccel 里的实际绑定回读出来
        QString error;
        m_backend->setHotkey(QKeySequence(configured), &error);
        actual = error.isEmpty() ? QStringLiteral("已注册") : error;
    }
    m_hotkeyStatus->setText(
        QStringLiteral("当前：%1（保存在配置里的值）。注册状态：%2\n"
                       "提示：KDE 的系统设置里也能给 SeaShot 的 capture 动作单独改键。")
            .arg(configured.isEmpty() ? QStringLiteral("未绑定") : configured, actual));
}

} // namespace seashot
