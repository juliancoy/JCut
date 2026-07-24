#pragma once

#include "editor_document_core.h"
#include "transcript_document_core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jcut {

enum class SpeakerTitleFlyInStyleCore {
    SlideFromLeft = 0,
    SlideFromRight = 1,
    RiseFromBottom = 2,
    DropFromTop = 3,
    WrapAroundSpeaker = 4,
};

struct SpeakerTitleFlyInSettingsCore {
    SpeakerTitleFlyInStyleCore style =
        SpeakerTitleFlyInStyleCore::SlideFromLeft;
    std::int64_t titleDurationFrames = 90;
    std::int64_t titleStartDelayFrames = 11;
    bool showAtSectionEnd = false;
    std::int64_t cadenceFrames = 0;
    std::int64_t flyInFrames = 11;
    std::int64_t flyOutFrames = 14;
    double wrapRadius = 1.05;
    double wrapDepth = 0.70;
    double wrapStartAngleDegrees = -110.0;
    double wrapEndAngleDegrees = 110.0;
    double wrapPitchDegrees = 8.0;
    double wrapRollDegrees = 0.0;
    double rotationStartXDegrees = 0.0;
    double rotationStartYDegrees = 0.0;
    double rotationStartZDegrees = 0.0;
    double titleFontSize = 48.0;
    bool titleAutoFitToOutput = true;
    double titleBoxWidth = 720.0;
    bool titleBackgroundEnabled = true;
    bool showSpeakerName = true;
    bool showSpeakerOrganization = true;
    std::string titleTextMaterialStyle = "solid";
    std::string titleBorderMaterialStyle = "solid";
    std::string titleTextPatternImagePath;
    std::string titleBorderPatternImagePath;
    double titlePatternScale = 1.0;
    bool titleExtrude3D = false;
    std::string titleExtrudeMode = "eroded_solid";
    double titleExtrudeDepth = 0.16;
    double titleBevelScale = 0.7;
};

bool applySpeakerTitleFlyInCore(
    EditorClip* clip,
    const SpeakerTitleFlyInSettingsCore& settings);

std::vector<EditorClip> makeSpeakerTitleClipsCore(
    const EditorClip& sourceClip,
    const TranscriptDocumentCore& transcript,
    int targetTrackId,
    const SpeakerTitleFlyInSettingsCore& settings = {});

} // namespace jcut
