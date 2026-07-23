#pragma once

#include "transcript_document_mutation_core.h"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut {

struct SpeakerSectionCore {
    std::string speakerId;
    std::string displayLabel;
    std::int64_t startFrame = -1;
    std::int64_t endFrame = -1;
    std::size_t wordCount = 0;
    std::vector<std::string> snippetWords;
};

struct SpeakerSectionOptionsCore {
    double rotationDegrees = 0.0;
    bool gradingEnabled = false;
    double gradingBrightness = 0.0;
    double gradingContrast = 1.0;
    double gradingSaturation = 1.0;
    bool maskEnabled = false;
    double maskOpacity = 1.0;
    double maskFeather = 0.0;
    double maskBlur = 0.0;
    double maskDilate = 0.0;
    bool maskInvert = false;
    bool stored = false;
};

// Projects the contiguous speaker rows used by the Qt Speakers/Sections view.
// Skipped words do not contribute to a row, and a speaker change closes the
// current row. Unknown transcript fields remain untouched.
std::vector<SpeakerSectionCore> projectSpeakerSectionsCore(
    const nlohmann::json& transcriptRoot,
    int minimumWords = 10,
    double framesPerSecond = 30.0,
    std::size_t maximumSnippetWords = 14);

// Applies Qt's inclusive speaker/range skip policy to word-backed sections.
// A successful no-op returns false without an error.
bool setSpeakerSectionSkippedCore(
    nlohmann::json* transcriptRoot,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    bool skipped,
    double framesPerSecond = 30.0,
    std::string* errorOut = nullptr);

SpeakerSectionOptionsCore speakerSectionOptionsCore(
    const nlohmann::json& transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame);

// Upserts the same speaker_flow/resolved_current/section_track_map row used by
// Qt. Existing track entries and unknown section fields are retained, and a
// rotation change is mirrored into every assigned track entry.
bool setSpeakerSectionOptionsCore(
    nlohmann::json* transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    std::size_t wordCount,
    const SpeakerSectionOptionsCore& options,
    const std::string& updatedAtUtc = {},
    std::string* errorOut = nullptr);

// Adds or replaces the continuity-track anchors attached to one contiguous
// section. Existing rows, track-entry extensions, and section options survive
// the mutation. An empty replacement clears the section's track assignment
// without deleting the section row.
bool setSpeakerSectionTrackAssignmentsCore(
    nlohmann::json* transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    std::size_t wordCount,
    const std::vector<TranscriptTrackAssignmentAnchor>& anchors,
    bool replaceExisting,
    const std::string& resolutionSource =
        "contiguous_section_picker",
    const std::string& updatedAtUtc = {},
    std::string* errorOut = nullptr);

} // namespace jcut
