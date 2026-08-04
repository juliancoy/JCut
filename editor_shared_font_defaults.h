#pragma once

#include <QString>

#ifdef __APPLE__
inline const QString kDefaultFontFamily = QStringLiteral("Helvetica Neue");
#else
inline const QString kDefaultFontFamily = QStringLiteral("DejaVu Sans");
#endif
