#pragma once

#include <QString>

namespace seashot {

/// Open the KDE shortcuts page as a normal desktop application.
bool openKdeShortcutSettings(QString *error = nullptr);

} // namespace seashot
