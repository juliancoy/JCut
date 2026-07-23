#pragma once

#include "transcript_document_core.h"

#include <optional>
#include <string>
#include <vector>

namespace jcut {

struct TranscriptWordPatch {
    std::optional<double> startSeconds;
    std::optional<double> endSeconds;
    std::optional<std::string> text;
    std::optional<bool> skipped;
};

struct TranscriptSpeakerProfilePatch {
    std::optional<std::string> name;
    std::optional<std::string> organization;
    std::optional<double> x;
    std::optional<double> y;
};

struct TranscriptTrackAssignmentAnchor {
    int trackId = -1;
    std::string streamId;
    std::int64_t sourceFrame = 0;
    double x = 0.5;
    double y = 0.5;
    double box = 0.2;
};

// Mutates the same WhisperX word object addressed by a projected row. Unknown
// JSON fields are preserved. A successful no-op returns false without an error.
bool patchTranscriptWord(nlohmann::json* root,
                         const TranscriptWordRef& reference,
                         const TranscriptWordPatch& patch,
                         std::string* errorOut = nullptr);
bool deleteTranscriptWord(nlohmann::json* root,
                          const TranscriptWordRef& reference,
                          std::string* errorOut = nullptr);
std::optional<TranscriptWordRef> insertTranscriptWord(
    nlohmann::json* root,
    const TranscriptWordRef& anchor,
    bool above,
    std::string* errorOut = nullptr);
bool expandTranscriptWordTiming(nlohmann::json* root,
                                const TranscriptWordRef& reference,
                                std::string* errorOut = nullptr);
bool restoreTranscriptWord(nlohmann::json* root,
                           const TranscriptWordRef& reference,
                           const nlohmann::json& originalRoot,
                           std::string* errorOut = nullptr);
bool moveTranscriptWordRenderOrder(nlohmann::json* root,
                                   const TranscriptWordRef& reference,
                                   int direction,
                                   std::string* errorOut = nullptr);
bool patchTranscriptSpeakerProfile(
    nlohmann::json* root,
    const std::string& speakerId,
    const TranscriptSpeakerProfilePatch& patch,
    std::string* errorOut = nullptr);
bool setTranscriptSpeakerTrackAssignments(
    nlohmann::json* root,
    const std::string& clipId,
    const std::string& speakerId,
    const std::vector<TranscriptTrackAssignmentAnchor>& anchors,
    bool replaceExistingForSpeaker,
    const std::string& updatedAtUtc,
    std::string* errorOut = nullptr);
bool saveTranscriptDocumentAtomic(const std::string& path,
                                  const nlohmann::json& root,
                                  std::string* errorOut = nullptr);

} // namespace jcut
