#include "app/SaveExportDialog.h"

#include "ui/Theme.h"

#include <QFileInfo>
#include <QLabel>
#include <QHBoxLayout>
#include <QImage>
#include <QPushButton>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QShowEvent>

namespace seashot {

SaveExportDialog::SaveExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("SeaShot · 保存 / 导出"));
    setWindowIcon(ui::appIcon());
    resize(640, 460);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::spec::cardPadding, ui::spec::cardPadding,
                              ui::spec::cardPadding, ui::spec::cardPadding);
    layout->setSpacing(ui::spec::gap);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    // ── 第 1 页：截图 ─────────────────────────────────────
    auto *shotPage = new QWidget(m_tabs);
    auto *shotLayout = new QVBoxLayout(shotPage);
    shotLayout->setSpacing(ui::spec::gap);
    m_shotPreview = new QLabel(shotPage);
    m_shotPreview->setAlignment(Qt::AlignCenter);
    m_shotPreview->setMinimumHeight(220);
    m_shotPreview->setFrameShape(QFrame::StyledPanel);
    shotLayout->addWidget(m_shotPreview, 1);
    m_shotInfo = new QLabel(shotPage);
    m_shotInfo->setWordWrap(true);
    m_shotInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shotLayout->addWidget(m_shotInfo);

    auto *shotButtons = new QHBoxLayout();
    shotButtons->setSpacing(ui::spec::gap);
    m_shotCopy = new QPushButton(QStringLiteral("复制"), shotPage);
    m_shotCopy->setIcon(ui::iconCopy());
    m_shotSave = new QPushButton(QStringLiteral("保存到默认目录"), shotPage);
    m_shotSave->setIcon(ui::iconSave());
    m_shotSaveAs = new QPushButton(QStringLiteral("另存为…"), shotPage);
    m_shotSaveAs->setIcon(ui::iconSaveAs());
    m_shotEdit = new QPushButton(QStringLiteral("在编辑器里打开…"), shotPage);
    m_shotEdit->setIcon(ui::iconEdit());
    shotButtons->addWidget(m_shotCopy);
    shotButtons->addWidget(m_shotSave);
    shotButtons->addWidget(m_shotSaveAs);
    shotButtons->addWidget(m_shotEdit);
    shotButtons->addStretch(1);
    shotLayout->addLayout(shotButtons);
    m_tabs->addTab(shotPage, QStringLiteral("截图"));

    // ── 第 2 页：录制 ─────────────────────────────────────
    auto *recPage = new QWidget(m_tabs);
    auto *recLayout = new QVBoxLayout(recPage);
    recLayout->setSpacing(ui::spec::gap);
    m_recInfo = new QLabel(recPage);
    m_recInfo->setWordWrap(true);
    m_recInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recLayout->addWidget(m_recInfo);
    recLayout->addStretch(1);

    auto *recButtons = new QHBoxLayout();
    recButtons->setSpacing(ui::spec::gap);
    m_gif = new QPushButton(QStringLiteral("导出 GIF…"), recPage);
    m_mp4 = new QPushButton(QStringLiteral("导出 MP4…"), recPage);
    m_copyGif = new QPushButton(QStringLiteral("复制 GIF"), recPage);
    m_copyGif->setIcon(ui::iconCopy());
    recButtons->addWidget(m_gif);
    recButtons->addWidget(m_mp4);
    recButtons->addWidget(m_copyGif);
    recButtons->addStretch(1);
    recLayout->addLayout(recButtons);
    m_tabs->addTab(recPage, QStringLiteral("录制"));

    auto *closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setIcon(ui::iconClose());
    closeRow->addWidget(close);
    layout->addLayout(closeRow);

    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_shotCopy, &QPushButton::clicked, this, &SaveExportDialog::copyScreenshotRequested);
    connect(m_shotSave, &QPushButton::clicked, this, &SaveExportDialog::saveScreenshotRequested);
    connect(m_shotSaveAs, &QPushButton::clicked, this,
            &SaveExportDialog::saveScreenshotAsRequested);
    connect(m_shotEdit, &QPushButton::clicked, this, &SaveExportDialog::editScreenshotRequested);
    connect(m_gif, &QPushButton::clicked, this, &SaveExportDialog::exportGifRequested);
    connect(m_mp4, &QPushButton::clicked, this, &SaveExportDialog::exportMp4Requested);
    connect(m_copyGif, &QPushButton::clicked, this, &SaveExportDialog::copyGifRequested);

    setScreenshot(QImage(), QString());
    setRecording(0, 0, QString(), false);
}

void SaveExportDialog::setScreenshot(const QImage &image, const QString &savedPath)
{
    const bool has = !image.isNull();
    m_shotCopy->setEnabled(has);
    m_shotSave->setEnabled(has);
    m_shotSaveAs->setEnabled(has);
    m_shotEdit->setEnabled(has);
    if (!has) {
        m_shotPreview->setPixmap(QPixmap());
        m_shotPreview->setText(QStringLiteral("还没有截图（按快捷键截一张）"));
        m_shotInfo->clear();
        return;
    }
    const int maxSide = 480;
    QImage preview = image;
    if (preview.width() > maxSide || preview.height() > maxSide)
        preview = preview.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_shotPreview->setPixmap(QPixmap::fromImage(preview));
    m_shotInfo->setText(
        QStringLiteral("尺寸：%1 × %2 像素%3")
            .arg(image.width())
            .arg(image.height())
            .arg(savedPath.isEmpty()
                     ? QString()
                     : QStringLiteral("　已保存：%1").arg(QFileInfo(savedPath).fileName())));
}

void SaveExportDialog::setRecording(int frames, qint64 elapsedMs, const QString &note,
                                   bool available)
{
    m_gif->setEnabled(available);
    m_mp4->setEnabled(available);
    m_copyGif->setEnabled(available);
    if (!available) {
        m_recInfo->setText(QStringLiteral("没有待导出的录制。\n"
                                          "（录制结束后会自动打开这一页，让你选导出格式）"));
        return;
    }
    m_recInfo->setText(QStringLiteral("上一次录制：%1 帧 · 约 %2 秒\n%3\n\n"
                                      "中间文件已保留，导出成哪种格式由你在这里决定；"
                                      "导出后中间文件会自动删除。")
                           .arg(frames)
                           .arg(elapsedMs / 1000.0, 0, 'f', 1)
                           .arg(note));
}

void SaveExportDialog::showPage(Page page)
{
    m_tabs->setCurrentIndex(page == Page::Recording ? 1 : 0);
}

void SaveExportDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (event->spontaneous())
        return;
    applyAutoClick();
}

void SaveExportDialog::applyAutoClick()
{
    // 诊断/证据：SEASHOT_EXPORT_DIALOG_GRAB=/tmp/x.png → 把窗口抓成图后退出（与编辑器的同类开关一致）
    const QString grabPath = qEnvironmentVariable("SEASHOT_EXPORT_DIALOG_GRAB");
    if (!grabPath.isEmpty() && !m_autoClickDone) {
        m_autoClickDone = true;
        QTimer::singleShot(500, this, [this, grabPath]() {
            const bool ok = grab().save(grabPath);
            QTextStream(stdout) << "EXPORT_DIALOG_GRAB " << grabPath
                                << " ok=" << (ok ? 1 : 0) << " size=" << width() << "x" << height()
                                << "\n";
            QTextStream(stdout).flush();
            QCoreApplication::quit();
        });
        return;
    }
    // 验收用：SEASHOT_EXPORT_AUTOCLICK=gif|mp4|copygif|copy|saveas|edit
    // 走的就是按钮 clicked 的同一条路径（与真实点击一致）。
    if (m_autoClickDone)
        return;
    const QString name = qEnvironmentVariable("SEASHOT_EXPORT_AUTOCLICK").toLower();
    if (name.isEmpty())
        return;
    m_autoClickDone = true;
    QPushButton *target = nullptr;
    if (name == QLatin1String("gif"))
        target = m_gif;
    else if (name == QLatin1String("mp4"))
        target = m_mp4;
    else if (name == QLatin1String("copygif"))
        target = m_copyGif;
    else if (name == QLatin1String("copy"))
        target = m_shotCopy;
    else if (name == QLatin1String("saveas"))
        target = m_shotSaveAs;
    else if (name == QLatin1String("edit"))
        target = m_shotEdit;
    if (!target || !target->isEnabled())
        return;
    QTextStream(stdout) << "EXPORT_DIALOG_AUTOCLICK " << name << "\n";
    QTextStream(stdout).flush();
    QTimer::singleShot(400, target, &QPushButton::click);
}

} // namespace seashot
