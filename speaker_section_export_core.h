#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace jcut {

struct SpeakerSectionExportCore {
    std::string speakerId;
    std::string speakerDisplayName;
    std::int64_t sourceStartFrame = -1;
    std::int64_t sourceEndFrame = -1;
    std::size_t wordCount = 0;
    int sectionOrdinal = 0;
    std::vector<int> trackIds;
    std::string snippet;
};

inline std::vector<int> normalizedSpeakerSectionTrackIds(
    const std::vector<int>& trackIds)
{
    std::vector<int> result;
    for (const int trackId : trackIds) {
        if (trackId < 0 ||
            std::find(
                result.begin(), result.end(), trackId) !=
                result.end()) {
            continue;
        }
        result.push_back(trackId);
    }
    return result;
}

inline std::vector<SpeakerSectionExportCore>
coalescedSpeakerSectionExports(
    const std::vector<SpeakerSectionExportCore>& sections)
{
    std::vector<SpeakerSectionExportCore> result;
    for (SpeakerSectionExportCore section : sections) {
        if (section.speakerId.empty() ||
            section.sourceStartFrame < 0 ||
            section.sourceEndFrame <
                section.sourceStartFrame) {
            continue;
        }
        section.trackIds =
            normalizedSpeakerSectionTrackIds(
                section.trackIds);
        if (!result.empty() &&
            result.back().speakerId ==
                section.speakerId) {
            auto& previous = result.back();
            previous.sourceEndFrame = std::max(
                previous.sourceEndFrame,
                section.sourceEndFrame);
            previous.wordCount += section.wordCount;
            if (!section.snippet.empty()) {
                if (!previous.snippet.empty()) {
                    previous.snippet += ' ';
                }
                previous.snippet += section.snippet;
            }
            previous.trackIds.insert(
                previous.trackIds.end(),
                section.trackIds.begin(),
                section.trackIds.end());
            previous.trackIds =
                normalizedSpeakerSectionTrackIds(
                    previous.trackIds);
        } else {
            result.push_back(std::move(section));
        }
    }
    return result;
}

inline std::string speakerSectionExportTitle(
    const SpeakerSectionExportCore& section)
{
    std::string speaker =
        section.speakerDisplayName.empty()
        ? section.speakerId
        : section.speakerDisplayName;
    if (speaker.empty()) speaker = "Speaker";
    const std::vector<int> tracks =
        normalizedSpeakerSectionTrackIds(
            section.trackIds);
    if (tracks.empty()) {
        return speaker + " no track";
    }
    std::string result = speaker +
        (tracks.size() == 1 ? " track " : " tracks ");
    for (std::size_t index = 0;
         index < tracks.size();
         ++index) {
        if (index > 0) result += '-';
        result += std::to_string(tracks[index]);
    }
    return result;
}

inline std::string sanitizedSpeakerSectionExportBase(
    const SpeakerSectionExportCore& section)
{
    std::string value =
        speakerSectionExportTitle(section);
    std::string filtered;
    bool previousSpace = false;
    for (const char character : value) {
        const unsigned char byte =
            static_cast<unsigned char>(character);
        if (character == '\\' || character == '/' ||
            character == ':' || character == '*' ||
            character == '?' || character == '"' ||
            character == '<' || character == '>' ||
            character == '|') {
            continue;
        }
        if (std::isspace(byte)) {
            if (!filtered.empty() && !previousSpace) {
                filtered += '_';
            }
            previousSpace = true;
            continue;
        }
        previousSpace = false;
        if ((byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') ||
            character == '.' ||
            character == '_' ||
            character == '-') {
            if (character != '_' ||
                filtered.empty() ||
                filtered.back() != '_') {
                filtered += character;
            }
        }
    }
    value = std::move(filtered);
    while (!value.empty() &&
           (value.front() == '.' ||
            value.front() == '_' ||
            value.front() == '-')) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           (value.back() == '.' ||
            value.back() == '_' ||
            value.back() == '-')) {
        value.pop_back();
    }
    if (value.empty()) {
        value = "speaker_section_" +
            std::to_string(
                std::max(1, section.sectionOrdinal));
    }
    if (value.size() > 80) value.resize(80);
    return value;
}

inline std::string speakerSectionExportSpeedSuffix(
    double speed)
{
    const double normalized =
        std::isfinite(speed) && speed > 0.001
        ? speed
        : 1.0;
    char buffer[32] = {};
    std::snprintf(
        buffer, sizeof(buffer), "%.3f", normalized);
    std::string value = buffer;
    while (value.find('.') != std::string::npos &&
           !value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return "_" + value + "x";
}

} // namespace jcut
