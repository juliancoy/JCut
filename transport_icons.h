#pragma once

#include <QIcon>
#include <QSize>

namespace editor {

enum class TransportIconGlyph {
    Play,
    Pause,
    StepBack,
    StepForward,
    ToStart,
    ToEnd,
    Volume,
    VolumeMuted,
};

QIcon transportIcon(TransportIconGlyph glyph, const QSize& size = QSize(18, 18));
QIcon playPauseTransportIcon(bool playing, const QSize& size = QSize(18, 18));
QIcon volumeTransportIcon(bool muted, const QSize& size = QSize(18, 18));
void validateTransportIconResources();

} // namespace editor
