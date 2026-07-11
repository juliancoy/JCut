#pragma once

#include <QColor>
#include <QPushButton>

inline void applyOverlayColorButtonStyle(QPushButton* button,
                                         const QColor& requested,
                                         bool showValue = false)
{
    if (!button) return;
    const QColor color = requested.isValid() ? requested : QColor(Qt::white);
    const QColor opaque(color.red(), color.green(), color.blue());
    if (showValue) button->setText(opaque.name(QColor::HexRgb));
    button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; "
                       "border: 1px solid #2e3b4a; border-radius: 4px; padding: 3px 8px; }")
            .arg(opaque.name(QColor::HexRgb),
                 opaque.lightness() > 128 ? QStringLiteral("#000000")
                                          : QStringLiteral("#ffffff")));
}
