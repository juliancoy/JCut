#include "speaker_section_core.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace jcut {
namespace {

using json = nlohmann::json;

std::string stringValue(
    const json& object,
    const char* key,
    std::string fallback = {})
{
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    return found != object.end() && found->is_string()
        ? found->get<std::string>()
        : fallback;
}

std::string wordText(const json& word)
{
    std::string text = stringValue(word, "text");
    if (text.empty()) text = stringValue(word, "word");
    return text;
}

std::string effectiveSpeaker(
    const json& word,
    const std::string& segmentSpeaker)
{
    const std::string speaker = stringValue(word, "speaker");
    return speaker.empty() ? segmentSpeaker : speaker;
}

double finiteNumber(const json& object, const char* key, double fallback)
{
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return fallback;
    try {
        const double value = found->get<double>();
        return std::isfinite(value) ? value : fallback;
    } catch (const json::exception&) {
        return fallback;
    }
}

std::int64_t integerValue(
    const json& object,
    const char* key,
    std::int64_t fallback)
{
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    if (found == object.end() ||
        !(found->is_number_integer() || found->is_number_unsigned())) {
        return fallback;
    }
    try {
        return found->get<std::int64_t>();
    } catch (const json::exception&) {
        return fallback;
    }
}

bool booleanValue(
    const json& object,
    const char* key,
    bool fallback)
{
    if (!object.is_object()) return fallback;
    const auto found = object.find(key);
    return found != object.end() && found->is_boolean()
        ? found->get<bool>()
        : fallback;
}

json objectValue(const json& object, const char* key)
{
    if (!object.is_object()) return json::object();
    const auto found = object.find(key);
    return found != object.end() && found->is_object()
        ? *found
        : json::object();
}

std::string displayLabel(
    const json& profiles,
    const std::string& speakerId)
{
    if (profiles.is_object()) {
        const auto profile = profiles.find(speakerId);
        if (profile != profiles.end() && profile->is_object()) {
            const std::string name = stringValue(*profile, "name");
            if (!name.empty()) return name;
        }
    }
    return speakerId;
}

std::string sectionKey(
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame)
{
    return speakerId + "|" + std::to_string(startFrame) + "|" +
        std::to_string(endFrame);
}

std::optional<json> matchingSection(
    const json& transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame)
{
    const json speakerFlow =
        objectValue(transcriptRoot, "speaker_flow");
    const json clips = objectValue(speakerFlow, "clips");
    const json clip = objectValue(clips, clipId.c_str());
    const json resolved =
        objectValue(clip, "resolved_current");
    json sections =
        resolved.value("section_track_map", json::array());
    if (!sections.is_array() || sections.empty()) {
        sections = clip.value(
            "section_track_map", json::array());
    }
    if (!sections.is_array()) return std::nullopt;
    const std::string key =
        sectionKey(speakerId, startFrame, endFrame);
    for (const json& section : sections) {
        if (!section.is_object()) continue;
        if (stringValue(section, "section_key") == key ||
            (stringValue(section, "speaker_id") == speakerId &&
             integerValue(section, "start_frame", -1) ==
                 startFrame &&
             integerValue(section, "end_frame", -1) ==
                 endFrame)) {
            return section;
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<SpeakerSectionCore> projectSpeakerSectionsCore(
    const json& transcriptRoot,
    int minimumWords,
    double framesPerSecond,
    std::size_t maximumSnippetWords)
{
    std::vector<SpeakerSectionCore> rows;
    if (!transcriptRoot.is_object()) return rows;
    minimumWords = std::clamp(minimumWords, 0, 1000);
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
        framesPerSecond = 30.0;
    }
    const json profiles =
        transcriptRoot.value("speaker_profiles", json::object());
    const json segments =
        transcriptRoot.value("segments", json::array());
    if (!segments.is_array()) return rows;

    SpeakerSectionCore current;
    auto flush = [&]() {
        if (!current.speakerId.empty() &&
            current.wordCount >= static_cast<std::size_t>(minimumWords)) {
            rows.push_back(current);
        }
        current = SpeakerSectionCore{};
    };
    auto addEntry = [&](const std::string& speakerId,
                        const std::string& text,
                        double startSeconds,
                        double endSeconds) {
        if (speakerId.empty()) {
            flush();
            return;
        }
        const std::int64_t startFrame = startSeconds >= 0.0
            ? std::max<std::int64_t>(
                0,
                static_cast<std::int64_t>(
                    std::floor(startSeconds * framesPerSecond)))
            : -1;
        const std::int64_t endFrame = endSeconds >= 0.0
            ? std::max<std::int64_t>(
                0,
                static_cast<std::int64_t>(
                    std::ceil(endSeconds * framesPerSecond)))
            : startFrame;
        if (current.speakerId != speakerId) {
            flush();
            current.speakerId = speakerId;
            current.displayLabel =
                displayLabel(profiles, speakerId);
            current.startFrame = startFrame;
            current.endFrame = endFrame;
        } else if (endFrame >= 0) {
            current.endFrame = std::max(current.endFrame, endFrame);
        }
        if (current.startFrame < 0 && startFrame >= 0) {
            current.startFrame = startFrame;
        }
        ++current.wordCount;
        if (!text.empty() &&
            current.snippetWords.size() < maximumSnippetWords) {
            current.snippetWords.push_back(text);
        }
    };

    for (const json& segment : segments) {
        if (!segment.is_object()) continue;
        const std::string segmentSpeaker =
            stringValue(segment, "speaker");
        const json words = segment.value("words", json::array());
        if (words.is_array() && !words.empty()) {
            for (const json& word : words) {
                if (!word.is_object() ||
                    booleanValue(word, "skipped", false)) {
                    continue;
                }
                const double start =
                    finiteNumber(word, "start", -1.0);
                addEntry(
                    effectiveSpeaker(word, segmentSpeaker),
                    wordText(word),
                    start,
                    finiteNumber(word, "end", start));
            }
            continue;
        }
        const double start =
            finiteNumber(segment, "start", -1.0);
        addEntry(
            segmentSpeaker,
            stringValue(segment, "text"),
            start,
            finiteNumber(segment, "end", start));
    }
    flush();
    return rows;
}

bool setSpeakerSectionSkippedCore(
    json* transcriptRoot,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    bool skipped,
    double framesPerSecond,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!transcriptRoot || !transcriptRoot->is_object() ||
        speakerId.empty() || startFrame < 0 || endFrame < startFrame ||
        !std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
        if (errorOut) *errorOut =
            "A valid transcript, speaker, frame range, and frame rate are required.";
        return false;
    }
    auto segmentsIt = transcriptRoot->find("segments");
    if (segmentsIt == transcriptRoot->end() ||
        !segmentsIt->is_array()) {
        if (errorOut) *errorOut =
            "Transcript segments are unavailable.";
        return false;
    }
    bool changed = false;
    for (json& segment : *segmentsIt) {
        if (!segment.is_object()) continue;
        const std::string segmentSpeaker =
            stringValue(segment, "speaker");
        auto wordsIt = segment.find("words");
        if (wordsIt == segment.end() || !wordsIt->is_array()) {
            continue;
        }
        for (json& word : *wordsIt) {
            if (!word.is_object() ||
                effectiveSpeaker(word, segmentSpeaker) != speakerId ||
                wordText(word).empty()) {
                continue;
            }
            const double startSeconds =
                finiteNumber(word, "start", -1.0);
            if (startSeconds < 0.0) continue;
            const std::int64_t wordFrame =
                std::max<std::int64_t>(
                    0,
                    static_cast<std::int64_t>(
                        std::floor(startSeconds * framesPerSecond)));
            if (wordFrame < startFrame || wordFrame > endFrame ||
                booleanValue(word, "skipped", false) == skipped) {
                continue;
            }
            word["skipped"] = skipped;
            changed = true;
        }
    }
    return changed;
}

SpeakerSectionOptionsCore speakerSectionOptionsCore(
    const json& transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame)
{
    SpeakerSectionOptionsCore result;
    const std::optional<json> section = matchingSection(
        transcriptRoot,
        clipId,
        speakerId,
        startFrame,
        endFrame);
    if (!section) return result;
    result.stored = true;
    result.rotationDegrees = std::clamp(
        finiteNumber(*section, "rotation", 0.0),
        -180.0,
        180.0);
    const json options =
        objectValue(*section, "section_options");
    const json grading =
        objectValue(options, "grading");
    const json mask =
        objectValue(options, "mask");
    result.gradingEnabled =
        booleanValue(grading, "enabled", false);
    result.gradingBrightness = std::clamp(
        finiteNumber(grading, "brightness", 0.0),
        -1.0,
        1.0);
    result.gradingContrast = std::clamp(
        finiteNumber(grading, "contrast", 1.0),
        0.0,
        4.0);
    result.gradingSaturation = std::clamp(
        finiteNumber(grading, "saturation", 1.0),
        0.0,
        4.0);
    result.maskEnabled =
        booleanValue(mask, "enabled", false);
    result.maskOpacity = std::clamp(
        finiteNumber(mask, "opacity", 1.0), 0.0, 1.0);
    result.maskFeather = std::clamp(
        finiteNumber(mask, "feather", 0.0), 0.0, 200.0);
    result.maskBlur = std::clamp(
        finiteNumber(mask, "blur", 0.0), 0.0, 200.0);
    result.maskDilate = std::clamp(
        finiteNumber(mask, "dilate", 0.0), 0.0, 200.0);
    result.maskInvert =
        booleanValue(mask, "invert", false);
    return result;
}

bool setSpeakerSectionOptionsCore(
    json* transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    std::size_t wordCount,
    const SpeakerSectionOptionsCore& input,
    const std::string& updatedAtUtc,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!transcriptRoot || !transcriptRoot->is_object() ||
        clipId.empty() || speakerId.empty() || startFrame < 0 ||
        endFrame < startFrame) {
        if (errorOut) *errorOut =
            "A valid transcript, clip, speaker, and section range are required.";
        return false;
    }
    SpeakerSectionOptionsCore options = input;
    options.rotationDegrees =
        std::clamp(options.rotationDegrees, -180.0, 180.0);
    options.gradingBrightness =
        std::clamp(options.gradingBrightness, -1.0, 1.0);
    options.gradingContrast =
        std::clamp(options.gradingContrast, 0.0, 4.0);
    options.gradingSaturation =
        std::clamp(options.gradingSaturation, 0.0, 4.0);
    options.maskOpacity =
        std::clamp(options.maskOpacity, 0.0, 1.0);
    options.maskFeather =
        std::clamp(options.maskFeather, 0.0, 200.0);
    options.maskBlur =
        std::clamp(options.maskBlur, 0.0, 200.0);
    options.maskDilate =
        std::clamp(options.maskDilate, 0.0, 200.0);

    json& speakerFlow = (*transcriptRoot)["speaker_flow"];
    if (!speakerFlow.is_object()) speakerFlow = json::object();
    speakerFlow["schema_version"] = "1.0";
    json& clips = speakerFlow["clips"];
    if (!clips.is_object()) clips = json::object();
    json& clip = clips[clipId];
    if (!clip.is_object()) clip = json::object();
    clip["clip_id"] = clipId;
    json& resolved = clip["resolved_current"];
    if (!resolved.is_object()) resolved = json::object();
    json sections =
        resolved.value("section_track_map", json::array());
    if (!sections.is_array()) sections = json::array();

    const std::string key =
        sectionKey(speakerId, startFrame, endFrame);
    json next = json::array();
    bool found = false;
    for (json section : sections) {
        if (!section.is_object()) {
            next.push_back(std::move(section));
            continue;
        }
        if (stringValue(section, "section_key") != key &&
            !(stringValue(section, "speaker_id") == speakerId &&
              integerValue(section, "start_frame", -1) ==
                  startFrame &&
              integerValue(section, "end_frame", -1) ==
                  endFrame)) {
            next.push_back(std::move(section));
            continue;
        }
        found = true;
        section["section_key"] = key;
        section["speaker_id"] = speakerId;
        section["start_frame"] = startFrame;
        section["end_frame"] = endFrame;
        section["word_count"] = wordCount;
        section["rotation"] = options.rotationDegrees;
        json tracks =
            section.value("tracks", json::array());
        if (tracks.is_array()) {
            for (json& track : tracks) {
                if (track.is_object()) {
                    track["rotation"] =
                        options.rotationDegrees;
                }
            }
            section["tracks"] = std::move(tracks);
        }
        section["section_options"] = {
            {"schema_version", "1.0"},
            {"scope", "speaker_section"},
            {"speaker_id", speakerId},
            {"start_frame", startFrame},
            {"end_frame", endFrame},
            {"grading", {
                {"enabled", options.gradingEnabled},
                {"keyframe_mode", "section_start_end"},
                {"brightness", options.gradingBrightness},
                {"contrast", options.gradingContrast},
                {"saturation", options.gradingSaturation},
            }},
            {"mask", {
                {"enabled", options.maskEnabled},
                {"opacity", options.maskOpacity},
                {"feather", options.maskFeather},
                {"blur", options.maskBlur},
                {"dilate", options.maskDilate},
                {"invert", options.maskInvert},
            }},
        };
        if (!updatedAtUtc.empty()) {
            section["updated_at_utc"] = updatedAtUtc;
        }
        next.push_back(std::move(section));
    }
    if (!found) {
        json section = {
            {"section_key", key},
            {"speaker_id", speakerId},
            {"start_frame", startFrame},
            {"end_frame", endFrame},
            {"word_count", wordCount},
            {"resolution_source", "contiguous_section_options"},
            {"rotation", options.rotationDegrees},
            {"tracks", json::array()},
            {"section_options", {
                {"schema_version", "1.0"},
                {"scope", "speaker_section"},
                {"speaker_id", speakerId},
                {"start_frame", startFrame},
                {"end_frame", endFrame},
                {"grading", {
                    {"enabled", options.gradingEnabled},
                    {"keyframe_mode", "section_start_end"},
                    {"brightness", options.gradingBrightness},
                    {"contrast", options.gradingContrast},
                    {"saturation", options.gradingSaturation},
                }},
                {"mask", {
                    {"enabled", options.maskEnabled},
                    {"opacity", options.maskOpacity},
                    {"feather", options.maskFeather},
                    {"blur", options.maskBlur},
                    {"dilate", options.maskDilate},
                    {"invert", options.maskInvert},
                }},
            }},
        };
        if (!updatedAtUtc.empty()) {
            section["updated_at_utc"] = updatedAtUtc;
        }
        next.push_back(std::move(section));
    }
    const json before =
        resolved.value("section_track_map", json::array());
    if (next == before) return false;
    resolved["section_track_map"] = std::move(next);
    if (!updatedAtUtc.empty()) {
        resolved["updated_at_utc"] = updatedAtUtc;
        clip["updated_at_utc"] = updatedAtUtc;
    }
    return true;
}

bool setSpeakerSectionTrackAssignmentsCore(
    json* transcriptRoot,
    const std::string& clipId,
    const std::string& speakerId,
    std::int64_t startFrame,
    std::int64_t endFrame,
    std::size_t wordCount,
    const std::vector<TranscriptTrackAssignmentAnchor>& inputAnchors,
    bool replaceExisting,
    const std::string& resolutionSource,
    const std::string& updatedAtUtc,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!transcriptRoot || !transcriptRoot->is_object() ||
        clipId.empty() || speakerId.empty() || startFrame < 0 ||
        endFrame < startFrame) {
        if (errorOut) {
            *errorOut =
                "A valid transcript, clip, speaker, and section range are required.";
        }
        return false;
    }

    std::vector<TranscriptTrackAssignmentAnchor> anchors;
    anchors.reserve(inputAnchors.size());
    for (TranscriptTrackAssignmentAnchor anchor : inputAnchors) {
        if (anchor.trackId < 0 ||
            std::find_if(
                anchors.begin(),
                anchors.end(),
                [&](const auto& existing) {
                    return existing.trackId == anchor.trackId;
                }) != anchors.end()) {
            continue;
        }
        anchor.sourceFrame = std::max<std::int64_t>(
            0, anchor.sourceFrame);
        anchor.x = std::clamp(anchor.x, 0.0, 1.0);
        anchor.y = std::clamp(anchor.y, 0.0, 1.0);
        anchor.box = std::clamp(anchor.box, 0.01, 1.0);
        if (anchor.streamId.empty() ||
            anchor.streamId == "raw_detection") {
            anchor.streamId = "T" + std::to_string(anchor.trackId);
        }
        anchors.push_back(std::move(anchor));
    }
    if (!replaceExisting && anchors.empty()) {
        if (errorOut) *errorOut = "At least one continuity track is required.";
        return false;
    }

    const json originalRoot = *transcriptRoot;
    json& speakerFlow = (*transcriptRoot)["speaker_flow"];
    if (!speakerFlow.is_object()) speakerFlow = json::object();
    speakerFlow["schema_version"] = "1.0";
    json& clips = speakerFlow["clips"];
    if (!clips.is_object()) clips = json::object();
    json& clip = clips[clipId];
    if (!clip.is_object()) clip = json::object();
    clip["clip_id"] = clipId;
    json& resolved = clip["resolved_current"];
    if (!resolved.is_object()) resolved = json::object();
    json sections =
        resolved.value("section_track_map", json::array());
    if (!sections.is_array()) sections = json::array();

    const std::string key =
        sectionKey(speakerId, startFrame, endFrame);
    json nextSections = json::array();
    bool found = false;
    for (json section : sections) {
        if (!section.is_object() ||
            (stringValue(section, "section_key") != key &&
             !(stringValue(section, "speaker_id") == speakerId &&
               integerValue(section, "start_frame", -1) ==
                   startFrame &&
               integerValue(section, "end_frame", -1) ==
                   endFrame))) {
            nextSections.push_back(std::move(section));
            continue;
        }
        found = true;
        section["section_key"] = key;
        section["speaker_id"] = speakerId;
        section["start_frame"] = startFrame;
        section["end_frame"] = endFrame;
        section["word_count"] = wordCount;
        section["resolution_source"] = resolutionSource.empty()
            ? "contiguous_section_picker"
            : resolutionSource;
        const double rotation = std::clamp(
            finiteNumber(section, "rotation", 0.0),
            -180.0,
            180.0);

        json currentTracks =
            section.value("tracks", json::array());
        if (!currentTracks.is_array()) currentTracks = json::array();
        if (currentTracks.empty()) {
            const int legacyTrackId = static_cast<int>(
                integerValue(section, "track_id", -1));
            if (legacyTrackId >= 0) {
                currentTracks.push_back({
                    {"track_id", legacyTrackId},
                    {"stream_id", stringValue(
                        section,
                        "stream_id",
                        "T" + std::to_string(legacyTrackId))},
                    {"source_frame", integerValue(
                        section, "source_frame", 0)},
                    {"x", finiteNumber(section, "x", 0.5)},
                    {"y", finiteNumber(section, "y", 0.5)},
                    {"box", finiteNumber(section, "box", 0.2)},
                    {"rotation", rotation},
                });
            }
        }

        json nextTracks = json::array();
        if (!replaceExisting) {
            nextTracks = currentTracks;
        }
        for (const auto& anchor : anchors) {
            json entry = json::object();
            for (const json& current : currentTracks) {
                if (current.is_object() &&
                    integerValue(current, "track_id", -1) ==
                        anchor.trackId) {
                    entry = current;
                    break;
                }
            }
            entry["track_id"] = anchor.trackId;
            entry["stream_id"] = anchor.streamId;
            entry["title"] =
                "Contiguous section assignment anchor T" +
                std::to_string(anchor.trackId);
            entry["source_frame"] = anchor.sourceFrame;
            entry["x"] = anchor.x;
            entry["y"] = anchor.y;
            entry["box"] = anchor.box;
            entry["rotation"] = rotation;
            bool replaced = false;
            for (json& current : nextTracks) {
                if (current.is_object() &&
                    integerValue(current, "track_id", -1) ==
                        anchor.trackId) {
                    current = entry;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) nextTracks.push_back(std::move(entry));
        }
        section["tracks"] = nextTracks;
        if (nextTracks.empty()) {
            for (const char* field : {
                     "track_id", "stream_id", "source_frame",
                     "x", "y", "box"}) {
                section.erase(field);
            }
        } else {
            const json& primary = nextTracks.front();
            section["track_id"] =
                integerValue(primary, "track_id", -1);
            section["stream_id"] =
                stringValue(primary, "stream_id");
            section["source_frame"] =
                integerValue(primary, "source_frame", 0);
            section["x"] = finiteNumber(primary, "x", 0.5);
            section["y"] = finiteNumber(primary, "y", 0.5);
            section["box"] = finiteNumber(primary, "box", 0.2);
        }
        section["rotation"] = rotation;
        if (!updatedAtUtc.empty()) {
            section["updated_at_utc"] = updatedAtUtc;
        }
        nextSections.push_back(std::move(section));
    }

    if (!found) {
        if (anchors.empty()) {
            *transcriptRoot = originalRoot;
            return false;
        }
        json section = {
            {"section_key", key},
            {"speaker_id", speakerId},
            {"start_frame", startFrame},
            {"end_frame", endFrame},
            {"word_count", wordCount},
            {"resolution_source", resolutionSource.empty()
                ? "contiguous_section_picker"
                : resolutionSource},
            {"rotation", 0.0},
            {"tracks", json::array()},
        };
        if (!updatedAtUtc.empty()) {
            section["updated_at_utc"] = updatedAtUtc;
        }
        nextSections.push_back(std::move(section));
        resolved["section_track_map"] = std::move(nextSections);
        return setSpeakerSectionTrackAssignmentsCore(
            transcriptRoot,
            clipId,
            speakerId,
            startFrame,
            endFrame,
            wordCount,
            anchors,
            true,
            resolutionSource,
            updatedAtUtc,
            errorOut);
    }

    const json before =
        resolved.value("section_track_map", json::array());
    if (nextSections == before) {
        *transcriptRoot = originalRoot;
        return false;
    }
    resolved["section_track_map"] = std::move(nextSections);
    if (!updatedAtUtc.empty()) {
        resolved["updated_at_utc"] = updatedAtUtc;
        clip["updated_at_utc"] = updatedAtUtc;
    }
    return true;
}

} // namespace jcut
