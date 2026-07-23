#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jcut {

enum class TranscriptMiningField {
    SpeakerName,
    SpeakerOrganization,
    WordSpeaker,
};

struct TranscriptMiningProposal {
    TranscriptMiningField field = TranscriptMiningField::SpeakerName;
    std::string targetId;
    int segmentIndex = -1;
    int wordIndex = -1;
    std::string currentValue;
    std::string proposedValue;
    double confidence = 0.0;
    std::string rationale;
};

std::vector<TranscriptMiningProposal> mineTranscriptSpeakerNames(
    const nlohmann::json& transcriptRoot);
std::vector<TranscriptMiningProposal> mineTranscriptOrganizations(
    const nlohmann::json& transcriptRoot);
std::vector<TranscriptMiningProposal> mineSpuriousSpeakerAssignments(
    const nlohmann::json& transcriptRoot);
bool applyTranscriptMiningProposals(
    nlohmann::json* transcriptRoot,
    const std::vector<TranscriptMiningProposal>& proposals,
    std::string* errorOut = nullptr);
nlohmann::json buildCloudSpeakerMiningPayload(
    const nlohmann::json& transcriptRoot);
std::vector<TranscriptMiningProposal> parseCloudSpeakerMiningResponse(
    const nlohmann::json& transcriptRoot,
    const nlohmann::json& responseRoot,
    std::string* errorOut = nullptr);

} // namespace jcut
