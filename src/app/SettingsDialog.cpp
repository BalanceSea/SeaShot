#include "app/SettingsDialog.h"

#include "backend/CaptureBackend.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

namespace seashot {

namespace {

/// 说明性小字：比正文小一号、用次要文字色（跟随主题）
QLabel *hintLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.92);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    return label;
}

QGroupBox *makeGroup(const QString &title, QWidget *parent, QFormLayout **formOut = nullptr)
{
    auto *group = new QGroupBox(title, parent);
    auto *form = new QFormLayout(group);
    form->setContentsMargins(ui::spec::cardPadding, ui::spec::cardPadding, ui::spec::cardPadding,
                             ui::spec::cardPadding);
    form->setHorizontalSpacing(ui::spec::cardPadding);
    form->setVerticalSpacing(ui::spec::gap + 2);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    if (formOut)
        *formOut = form;
    return group;
}

QToolButton *iconOnlyButton(const QIcon &icon, const QString &text, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(icon);
    button->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
    button->setToolTip(text);
    button->setAutoRaise(false);
    return button;
}

} // namespace

SettingsDialog::SettingsDialog(Settings *settings, CaptureBackend *backend, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_backend(backend)
{
    setWindowTitle(QStringLiteral("SeaShot 设置"));
    setMinimumWidth(620);
    buildUi();
    loadFromSettings();
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::spec::cardPadding + 4, ui::spec::cardPadding + 4,
                               ui::spec::cardPadding + 4, ui::spec::cardPadding + 4);
    layout->setSpacing(ui::spec::cardPadding);

    // ── 头部：图标 + 标题 + 一句说明 ─────────────────────────
    auto *header = new QHBoxLayout();
    header->setSpacing(ui::spec::cardPadding);
    auto *logo = new QLabel(this);
    const QIcon appIcon = ui::iconCapture();
    if (!appIcon.isNull())
        logo->setPixmap(appIcon.pixmap(32, 32));
    header->addWidget(logo, 0, Qt::AlignTop);
    auto *titleBox = new QVBoxLayout();
    titleBox->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("SeaShot"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    title->setFont(titleFont);
    titleBox->addWidget(title);
    titleBox->addWidget(hintLabel(QStringLiteral("截图、编辑与 GIF 录制 —— 改动立即生效并写入配置文件"), this));
    header->addLayout(titleBox, 1);
    layout->addLayout(header);

    // ── 截图后 ──────────────────────────────────────────────
    QFormLayout *afterForm = nullptr;
    auto *afterGroup = makeGroup(QStringLiteral("截图后"), this, &afterForm);
    m_copyBox = new QCheckBox(QStringLiteral("自动复制到剪贴板"), afterGroup);
    m_copyBox->setObjectName(QStringLiteral("copyAfterCapture"));
    afterForm->addRow(QString(), m_copyBox);
    m_openEditorBox = new QCheckBox(QStringLiteral("直接打开编辑器（不再弹结果浮层）"), afterGroup);
    m_openEditorBox->setObjectName(QStringLiteral("openEditorAfterCapture"));
    afterForm->addRow(QString(), m_openEditorBox);
    m_autoSaveBox = new QCheckBox(QStringLiteral("自动保存一份到默认目录"), afterGroup);
    m_autoSaveBox->setObjectName(QStringLiteral("autoSaveAfterCapture"));
    afterForm->addRow(QString(), m_autoSaveBox);
    layout->addWidget(afterGroup);

    // ── 保存 ────────────────────────────────────────────────
    QFormLayout *saveForm = nullptr;
    auto *saveGroup = makeGroup(QStringLiteral("保存"), this, &saveForm);
    auto *dirRow = new QWidget(saveGroup);
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    dirLayout->setSpacing(ui::spec::gap);
    m_dirEdit = new QLineEdit(dirRow);
    m_dirEdit->setObjectName(QStringLiteral("saveDir"));
    m_dirEdit->setClearButtonEnabled(true);
    dirLayout->addWidget(m_dirEdit, 1);
    auto *browseButton = iconOnlyButton(
        ui::icon({QStringLiteral("folder-open"), QStringLiteral("document-open")}),
        QStringLiteral("选择保存目录"), dirRow);
    browseButton->setObjectName(QStringLiteral("browseSaveDir"));
    dirLayout->addWidget(browseButton);
    saveForm->addRow(QStringLiteral("默认保存目录"), dirRow);

    m_templateEdit = new QLineEdit(saveGroup);
    m_templateEdit->setObjectName(QStringLiteral("filenameTemplate"));
    m_templateEdit->setClearButtonEnabled(true);
    saveForm->addRow(QStringLiteral("文件名模板"), m_templateEdit);
    saveForm->addRow(QString(),
                     hintLabel(QStringLiteral("%1 = 时间戳（yyyyMMdd_HHmmss）　%d = 日期　%t = 时间；"
                                              "同名自动加序号，不覆盖"),
                                saveGroup));

    m_keepShadowBox = new QCheckBox(QStringLiteral("窗口截图保留阴影/装饰边距"), saveGroup);
    m_keepShadowBox->setObjectName(QStringLiteral("keepShadowMargin"));
    saveForm->addRow(QString(), m_keepShadowBox);
    layout->addWidget(saveGroup);

    // ── 界面 ────────────────────────────────────────────────
    QFormLayout *uiForm = nullptr;
    auto *uiGroup = makeGroup(QStringLiteral("界面"), this, &uiForm);
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

    // ── 全局快捷键 ──────────────────────────────────────────
    auto *keyGroup = new QGroupBox(QStringLiteral("全局快捷键"), this);
    auto *keyLayout = new QVBoxLayout(keyGroup);
    keyLayout->setContentsMargins(ui::spec::cardPadding, ui::spec::cardPadding, ui::spec::cardPadding,
                                  ui::spec::cardPadding);
    keyLayout->setSpacing(ui::spec::gap + 2);
    auto *keyRow = new QHBoxLayout();
    keyRow->setSpacing(ui::spec::gap);
    keyRow->addWidget(new QLabel(QStringLiteral("截取屏幕："), keyGroup));
    m_hotkeyEdit = new QKeySequenceEdit(keyGroup);
    m_hotkeyEdit->setObjectName(QStringLiteral("hotkey"));
    m_hotkeyEdit->setMaximumSequenceLength(1);
    keyRow->addWidget(m_hotkeyEdit, 1);
    auto *clearButton = new QPushButton(QStringLiteral("清除"), keyGroup);
    clearButton->setObjectName(QStringLiteral("clearHotkey"));
    clearButton->setIcon(ui::icon({QStringLiteral("edit-clear"), QStringLiteral("edit-clear-history")}));
    keyRow->addWidget(clearButton);
    keyLayout->addLayout(keyRow);

    m_hotkeyStatus = new QLabel(keyGroup);
    m_hotkeyStatus->setObjectName(QStringLiteral("hotkeyStatus"));
    m_hotkeyStatus->setWordWrap(true);
    keyLayout->addWidget(m_hotkeyStatus);

    auto *kdeButton = new QPushButton(QStringLiteral("打开 KDE 快捷键设置…"), keyGroup);
    kdeButton->setObjectName(QStringLiteral("openKdeKeys"));
    kdeButton->setIcon(ui::iconSettings());
    kdeButton->setToolTip(QStringLiteral("在「系统设置 → 快捷键」里给 SeaShot 的 capture 动作绑定/改键"));
    auto *kdeRow = new QHBoxLayout();
    kdeRow->addWidget(kdeButton);
    kdeRow->addStretch(1);
    keyLayout->addLayout(kdeRow);
    layout->addWidget(keyGroup);

    // ── 录制（Phase 4 已生效） ───────────────────────────────
    QFormLayout *recordForm = nullptr;
    auto *recordGroup = makeGroup(QStringLiteral("GIF 录制"), this, &recordForm);
    m_fpsSpin = new QSpinBox(recordGroup);
    m_fpsSpin->setObjectName(QStringLiteral("gifFps"));
    m_fpsSpin->setRange(1, 60);
    m_fpsSpin->setSuffix(QStringLiteral(" fps"));
    recordForm->addRow(QStringLiteral("帧率"), m_fpsSpin);
    recordForm->addRow(QString(),
                       hintLabel(QStringLiteral("只录选定区域；不设时长与体积上限；录制中按快捷键即停止"),
                                 recordGroup));
    layout->addWidget(recordGroup);

    layout->addStretch(1);

    // ── 底部：配置文件路径 + 右对齐按钮 ─────────────────────
    auto *footer = new QHBoxLayout();
    footer->setSpacing(ui::spec::gap);
    m_configPathLabel = new QLabel(this);
    m_configPathLabel->setObjectName(QStringLiteral("configPath"));
    m_configPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont smallFont = m_configPathLabel->font();
    smallFont.setPointSizeF(smallFont.pointSizeF() * 0.92);
    m_configPathLabel->setFont(smallFont);
    QPalette pathPalette = m_configPathLabel->palette();
    pathPalette.setColor(QPalette::WindowText, pathPalette.color(QPalette::PlaceholderText));
    m_configPathLabel->setPalette(pathPalette);
    footer->addWidget(m_configPathLabel, 1);

    auto *buttons = new QDialogButtonBox(this);
    auto *resetButton = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    resetButton->setObjectName(QStringLiteral("resetDefaults"));
    resetButton->setIcon(ui::iconUndo());
    auto *closeButton = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::AcceptRole);
    closeButton->setObjectName(QStringLiteral("close"));
    closeButton->setIcon(ui::iconClose());
    closeButton->setDefault(true);
    footer->addWidget(buttons);
    layout->addLayout(footer);

    // ── 连线：改动即时生效 ──────────────────────────────────
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
    connect(browseButton, &QToolButton::clicked, this, &SettingsDialog::chooseSaveDir);
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
        if (!QProcess::startDetached(program, {QStringLiteral("kcm_keys")}))
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
    m_configPathLabel->setText(QStringLiteral("配置文件 %1").arg(m_settings->configFilePath()));
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
        QString error;
        m_backend->setHotkey(QKeySequence(configured), &error);
        actual = error.isEmpty() ? QStringLiteral("已注册") : error;
    }
    m_hotkeyStatus->setText(
        QStringLiteral("当前：%1（配置里的值）· 注册状态：%2\n"
                       "KDE 的系统设置里也能给 SeaShot 的 capture 动作单独改键。")
            .arg(configured.isEmpty() ? QStringLiteral("未绑定") : configured, actual));
}

} // namespace seashot
