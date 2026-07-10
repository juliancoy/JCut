#pragma once

#include <QString>

#include <cstdint>

struct ExportRangeSegment {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
};

enum class RenderSyncAction {
    DuplicateFrame,
    SkipFrame,
};

enum class PlaybackClockSource {
    Auto,
    Audio,
    Timeline,
};

enum class PlaybackAudioWarpMode {
    Disabled,
    Varispeed,
    TimeStretch,
    RubberBand,
    RubberBandPassThroughFrequency,
};

struct RenderSyncMarker {
    QString clipId;
    int64_t frame = 0;
    RenderSyncAction action = RenderSyncAction::DuplicateFrame;
    int count = 1;
};
