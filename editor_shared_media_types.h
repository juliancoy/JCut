#pragma once

#include "editor_timeline_types.h"
#include "timeline_fps.h"

#include <QSize>
#include <QString>

#include <cstdint>

struct MediaProbeResult {
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    bool hasVideo = false;
    bool hasAlpha = false;
    int64_t durationFrames = 120;
    QString codecName;
    QSize frameSize;
    double fps = static_cast<double>(kTimelineFps);
};
