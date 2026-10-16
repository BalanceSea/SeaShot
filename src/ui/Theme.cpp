#include "ui/Theme.h"

namespace seashot::ui {

QIcon icon(const QStringList &names)
{
    for (const QString &name : names) {
        const QIcon candidate = QIcon::fromTheme(name);
        if (!candidate.isNull() && !candidate.availableSizes().isEmpty())
            return candidate;
        if (!candidate.isNull() && candidate.name().isEmpty() == false)
            return candidate;
    }
    return {};
}

QIcon icon(const QString &name)
{
    return icon(QStringList{name});
}

CardColors cardColors(const QPalette &palette)
{
    CardColors colors;
    colors.card = palette.color(QPalette::Window);
    colors.cardText = palette.color(QPalette::WindowText);
    colors.bar = palette.color(QPalette::Window);
    colors.bar.setAlpha(235);
    colors.button = palette.color(QPalette::Button);
    colors.buttonText = palette.color(QPalette::ButtonText);
    colors.primary = palette.color(QPalette::Highlight);
    colors.primaryText = palette.color(QPalette::HighlightedText);
    colors.border = palette.color(QPalette::Mid);
    colors.frameOuter = QColor(0, 0, 0);
    colors.frameOuter.setAlpha(spec::frameOuterAlpha);
    colors.frameInner = palette.color(QPalette::Highlight);
    colors.frameInner.setAlpha(spec::frameInnerAlpha);
    return colors;
}

QPixmap appIconPixmap(int size)
{
    static QPixmap source;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        source.load(QStringLiteral(":/seashot/resources/icons/seashot.png"));
    }
    if (source.isNull()) {
        const QIcon fallback = iconCapture();
        return fallback.isNull() ? QPixmap() : fallback.pixmap(size, size);
    }
    return source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QIcon appIcon()
{
    const QPixmap pixmap = appIconPixmap(256);
    if (pixmap.isNull())
        return iconCapture();
    return QIcon(pixmap);
}

QIcon iconEdit()
{
    return icon({QStringLiteral("document-edit"), QStringLiteral("edit-entry"),
                 QStringLiteral("edit")});
}

QIcon iconSaveAs()
{
    return icon({QStringLiteral("document-save-as"), QStringLiteral("document-save")});
}

QIcon iconCopy()
{
    return icon({QStringLiteral("edit-copy"), QStringLiteral("edit-duplicate")});
}

QIcon iconClose()
{
    return icon({QStringLiteral("window-close"), QStringLiteral("dialog-close")});
}

QIcon iconRecord()
{
    return icon({QStringLiteral("media-record"), QStringLiteral("camera-video")});
}

QIcon iconStop()
{
    return icon({QStringLiteral("media-playback-stop"), QStringLiteral("process-stop")});
}

QIcon iconCapture()
{
    return icon({QStringLiteral("camera-photo"), QStringLiteral("camera")});
}

QIcon iconSettings()
{
    return icon({QStringLiteral("settings-configure"), QStringLiteral("preferences-system")});
}

QIcon iconUndo()
{
    return icon({QStringLiteral("edit-undo"), QStringLiteral("edit-undo-history")});
}

QIcon iconRedo()
{
    return icon({QStringLiteral("edit-redo"), QStringLiteral("edit-redo-history")});
}

QIcon iconDelete()
{
    return icon({QStringLiteral("edit-delete"), QStringLiteral("edit-delete-remove")});
}

QIcon iconSave()
{
    return icon({QStringLiteral("document-save")});
}

QIcon iconRefresh()
{
    return icon({QStringLiteral("view-refresh")});
}

QIcon iconToolSelect()
{
    return icon({QStringLiteral("transform-move"), QStringLiteral("edit-select")});
}

QIcon iconToolArrow()
{
    return icon({QStringLiteral("draw-arrow"), QStringLiteral("draw-line")});
}

QIcon iconToolRectangle()
{
    return icon({QStringLiteral("draw-rectangle"), QStringLiteral("draw-highlight")});
}

QIcon iconToolEllipse()
{
    return icon({QStringLiteral("draw-circle"), QStringLiteral("draw-ellipse")});
}

QIcon iconToolPen()
{
    return icon({QStringLiteral("draw-freehand"), QStringLiteral("draw-line")});
}

QIcon iconToolHighlight()
{
    // 本机 Breeze 有 draw-highlight（已实测）；后面两个是其他主题的回退
    return icon({QStringLiteral("draw-highlight"), QStringLiteral("draw-marker"),
                 QStringLiteral("marker")});
}

QIcon iconToolBlur()
{
    return icon({QStringLiteral("blur"), QStringLiteral("edit-blur"),
                 QStringLiteral("image-filter")});
}

QIcon iconToolMosaic()
{
    return icon({QStringLiteral("pixelate"), QStringLiteral("edit-pixelize"),
                 QStringLiteral("mosaic")});
}

QIcon iconToolNumber()
{
    return icon({QStringLiteral("draw-number"), QStringLiteral("format-number"),
                 QStringLiteral("draw-text")});
}

QIcon iconToolText()
{
    return icon({QStringLiteral("draw-text"), QStringLiteral("insert-text")});
}

} // namespace seashot::ui
