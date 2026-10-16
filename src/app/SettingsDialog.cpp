#include "app/SettingsDialog.h"

#include "backend/CaptureBackend.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
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

/// 一页 = 标题 + 可选说明 + 表单区（按导航切换）
QWidget *makePage(const QString &title, const QString &subtitle, QWidget *parent,
                  QFormLayout **formOut)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::spec::cardPadding + 4, ui::spec::cardPadding,
                               ui::spec::cardPadding, ui::spec::cardPadding);
    layout->setSpacing(ui::spec::gap + 2);

    auto *heading = new QLabel(title, page);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    if (!subtitle.isEmpty())
        layout->addWidget(hintLabel(subtitle, page));

    auto *form = new QFormLayout();
    form->setContentsMargins(0, ui::spec::gap, 0, 0);
    form->setHorizontalSpacing(ui::spec::cardPadding);
    form->setVerticalSpacing(ui::spec::gap + 2);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form);
    layout->addStretch(1);
    if (formOut)
        *formOut = form;
    return page;
}

} // namespace

SettingsDialog::SettingsDialog(Settings *settings, CaptureBackend *backend, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_backend(backend)
{
    setWindowTitle(QStringLiteral("SeaShot 设置"));
    setMinimumSize(780, 620);
    buildUi();
    loadFromSettings();
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::spec::cardPadding + 4, ui::spec::cardPadding + 4,
                               ui::spec::cardPadding + 4, ui::spec::cardPadding + 4);
    layout->setSpacing(ui::spec::cardPadding);

    // ── 头部：应用图标 + 标题 + 一句说明 ─────────────────────
    auto *header = new QHBoxLayout();
    header->setSpacing(ui::spec::cardPadding);
    auto *logo = new QLabel(this);
    const QPixmap logoPixmap = ui::appIconPixmap(40);
    if (!logoPixmap.isNull())
        logo->setPixmap(logoPixmap);
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

    // ── 左侧导航 + 右侧页面 ─────────────────────────────────
    auto *nav = new QListWidget(this);
    nav->setObjectName(QStringLiteral("nav"));
    nav->setFixedWidth(190);
    nav->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
    nav->setFrameShape(QFrame::NoFrame);
    nav->setUniformItemSizes(true);

    auto *pages = new QStackedWidget(this);

    // 页 1：截图后
    QFormLayout *afterForm = nullptr;
    QWidget *afterPage = makePage(QStringLiteral("截图后"),
                                  QStringLiteral("截完图之后 SeaShot 自动做什么（这三项互不冲突）"),
                                  pages, &afterForm);
    m_copyBox = new QCheckBox(QStringLiteral("自动复制到剪贴板"), afterPage);
    m_copyBox->setObjectName(QStringLiteral("copyAfterCapture"));
    afterForm->addRow(QString(), m_copyBox);
    m_openEditorBox = new QCheckBox(QStringLiteral("直接打开编辑器（不再弹结果浮层）"), afterPage);
    m_openEditorBox->setObjectName(QStringLiteral("openEditorAfterCapture"));
    afterForm->addRow(QString(), m_openEditorBox);
    m_autoSaveBox = new QCheckBox(QStringLiteral("自动保存一份到默认目录"), afterPage);
    m_autoSaveBox->setObjectName(QStringLiteral("autoSaveAfterCapture"));
    afterForm->addRow(QString(), m_autoSaveBox);
    pages->addWidget(afterPage);

    // 页 2：保存
    QFormLayout *saveForm = nullptr;
    QWidget *savePage = makePage(QStringLiteral("保存"),
                                 QStringLiteral("默认落盘位置与命名规则；「另存为」始终会弹对话框让你选"), pages,
                                 &saveForm);
    auto *dirRow = new QWidget(savePage);
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    dirLayout->setSpacing(ui::spec::gap);
    m_dirEdit = new QLineEdit(dirRow);
    m_dirEdit->setObjectName(QStringLiteral("saveDir"));
    m_dirEdit->setClearButtonEnabled(true);
    dirLayout->addWidget(m_dirEdit, 1);
    auto *browseButton = new QToolButton(dirRow);
    browseButton->setObjectName(QStringLiteral("browseSaveDir"));
    browseButton->setIcon(ui::icon({QStringLiteral("folder-open"), QStringLiteral("document-open")}));
    browseButton->setIconSize(QSize(ui::spec::iconSize, ui::spec::iconSize));
    browseButton->setToolTip(QStringLiteral("选择保存目录"));
    dirLayout->addWidget(browseButton);
    saveForm->addRow(QStringLiteral("默认保存目录"), dirRow);

    m_templateEdit = new QLineEdit(savePage);
    m_templateEdit->setObjectName(QStringLiteral("filenameTemplate"));
    m_templateEdit->setClearButtonEnabled(true);
    saveForm->addRow(QStringLiteral("文件名模板"), m_templateEdit);
    saveForm->addRow(QString(), hintLabel(QStringLiteral("%1 = 时间戳（yyyyMMdd_HHmmss）　%d = 日期　"
                                                         "%t = 时间；同名自动加序号，不覆盖"),
                                          savePage));

    m_keepShadowBox = new QCheckBox(QStringLiteral("窗口截图保留阴影/装饰边距"), savePage);
    m_keepShadowBox->setObjectName(QStringLiteral("keepShadowMargin"));
    saveForm->addRow(QString(), m_keepShadowBox);
    pages->addWidget(savePage);

    // 页 3：界面
    QFormLayout *uiForm = nullptr;
    QWidget *uiPage = makePage(QStringLiteral("界面"),
                               QStringLiteral("结果浮层按原尺寸显示、贴在截图框左下角，可以按住拖动"), pages,
                               &uiForm);
    m_toastSpin = new QSpinBox(uiPage);
    m_toastSpin->setObjectName(QStringLiteral("toastAutoHideMs"));
    m_toastSpin->setRange(0, 60000);
    m_toastSpin->setSingleStep(500);
    m_toastSpin->setSuffix(QStringLiteral(" ms"));
    m_toastSpin->setSpecialValueText(QStringLiteral("不自动消失"));
    uiForm->addRow(QStringLiteral("结果浮层自动消失"), m_toastSpin);
    m_trayBox = new QCheckBox(QStringLiteral("常驻托盘"), uiPage);
    m_trayBox->setObjectName(QStringLiteral("trayPersistent"));
    uiForm->addRow(QString(), m_trayBox);
    pages->addWidget(uiPage);

    // 页 4：全局快捷键
    QFormLayout *keyForm = nullptr;
    QWidget *keyPage = makePage(QStringLiteral("全局快捷键"),
                                QStringLiteral("录制中按同一个键 = 停止录制"), pages, &keyForm);
    auto *keyRow = new QWidget(keyPage);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(ui::spec::gap);
    m_hotkeyEdit = new QKeySequenceEdit(keyRow);
    m_hotkeyEdit->setObjectName(QStringLiteral("hotkey"));
    m_hotkeyEdit->setMaximumSequenceLength(1);
    keyLayout->addWidget(m_hotkeyEdit, 1);
    auto *clearButton = new QPushButton(QStringLiteral("清除"), keyRow);
    clearButton->setObjectName(QStringLiteral("clearHotkey"));
    clearButton->setIcon(ui::icon({QStringLiteral("edit-clear"), QStringLiteral("edit-clear-history")}));
    keyLayout->addWidget(clearButton);
    keyForm->addRow(QStringLiteral("截取屏幕"), keyRow);

    m_hotkeyStatus = new QLabel(keyPage);
    m_hotkeyStatus->setObjectName(QStringLiteral("hotkeyStatus"));
    m_hotkeyStatus->setWordWrap(true);
    keyForm->addRow(QString(), m_hotkeyStatus);

    auto *kdeButton = new QPushButton(QStringLiteral("打开 KDE 快捷键设置…"), keyPage);
    kdeButton->setObjectName(QStringLiteral("openKdeKeys"));
    kdeButton->setIcon(ui::iconSettings());
    kdeButton->setToolTip(QStringLiteral("在「系统设置 → 快捷键」里给 SeaShot 的 capture 动作绑定/改键"));
    auto *kdeRow = new QHBoxLayout();
    kdeRow->addWidget(kdeButton);
    kdeRow->addStretch(1);
    keyForm->addRow(QString(), kdeRow);
    pages->addWidget(keyPage);

    // 页 5：GIF 录制
    QFormLayout *recordForm = nullptr;
    QWidget *recordPage = makePage(QStringLiteral("GIF 录制"),
                                   QStringLiteral("只录选定区域；不设时长与体积上限"), pages, &recordForm);
    m_fpsSpin = new QSpinBox(recordPage);
    m_fpsSpin->setObjectName(QStringLiteral("gifFps"));
    m_fpsSpin->setRange(1, 60);
    m_fpsSpin->setSuffix(QStringLiteral(" fps"));
    recordForm->addRow(QStringLiteral("帧率"), m_fpsSpin);
    recordForm->addRow(QString(), hintLabel(QStringLiteral("托盘菜单「录制 GIF…」或快捷键开始；"
                                                           "录制中按快捷键 / 点指示器上的「停止」结束"),
                                            recordPage));
    pages->addWidget(recordPage);

    // 导航项与页面一一对应
    const struct {
        QString label;
        QIcon icon;
    } entries[] = {
        {QStringLiteral("截图后"), ui::iconCapture()},
        {QStringLiteral("保存"), ui::iconSave()},
        {QStringLiteral("界面"), ui::iconSettings()},
        {QStringLiteral("全局快捷键"),
         ui::icon({QStringLiteral("input-keyboard"), QStringLiteral("preferences-desktop-keyboard")})},
        {QStringLiteral("GIF 录制"), ui::iconRecord()},
    };
    for (const auto &entry : entries) {
        auto *item = new QListWidgetItem(entry.icon, entry.label, nav);
        item->setSizeHint(QSize(0, ui::spec::buttonHeight + ui::spec::gap * 2));
    }
    nav->setCurrentRow(0);
    connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    auto *content = new QHBoxLayout();
    content->setSpacing(ui::spec::cardPadding);
    content->addWidget(nav);
    content->addWidget(pages, 1);
    layout->addLayout(content, 1);

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
