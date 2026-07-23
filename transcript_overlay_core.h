#pragma once

#include "transcript_document_core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jcut {

struct TranscriptOverlayLineCore {
    std::vector<std::string> words;
    int activeWord = -1;
};

struct TranscriptOverlayLayoutCore {
    std::vector<TranscriptOverlayLineCore> lines;
    std::string speakerId;
    std::string speakerLabel;
    std::string speakerTitle;
    std::string speakerPrimaryColor;
    std::string speakerSecondaryColor;
    std::string speakerAccentColor;
    double speakerLocationX = 0.5;
    double speakerLocationY = 0.85;
    bool speakerLocationValid = false;
    double speakerTrackingX = 0.5;
    double speakerTrackingY = 0.5;
    double speakerTrackingBoxSize = -1.0;
    double speakerTrackingRotationDegrees = 0.0;
    bool speakerTrackingValid = false;
    bool speakerTrackingCenterRotationFallback = false;
    bool truncatedTop = false;
    bool truncatedBottom = false;
};

struct TranscriptOverlayLayoutOptions {
    TranscriptTiming timing;
    int maxCharsPerLine = 28;
    int maxLines = 2;
    bool autoScroll = false;
};

// Qt-free counterpart of editor_shared_transcript_overlay.cpp. It consumes
// the shared read-only transcript projection so preview/export frontends use
// the same persisted word ordering, timing, speaker labels, and skip policy.
TranscriptOverlayLayoutCore transcriptOverlayLayoutForRows(
    const std::vector<TranscriptRow>& rows,
    std::int64_t sourceFrame,
    const TranscriptOverlayLayoutOptions& options = {});

} // namespace jcut
