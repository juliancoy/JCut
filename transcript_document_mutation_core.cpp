#include "transcript_document_mutation_core.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

namespace jcut {
namespace {

using json = nlohmann::json;

json* wordAt(json* root, const TranscriptWordRef& reference, std::string* errorOut)
{
    if (!root || !root->is_object() || reference.segmentIndex < 0 ||
        reference.wordIndex < 0) {
        if (errorOut) *errorOut = "Invalid transcript word reference.";
        return nullptr;
    }
    auto segmentsIt = root->find("segments");
    if (segmentsIt == root->end() || !segmentsIt->is_array() ||
        reference.segmentIndex >= static_cast<int>(segmentsIt->size())) {
        if (errorOut) *errorOut = "Transcript segment no longer exists.";
        return nullptr;
    }
    json& segment = (*segmentsIt)[static_cast<std::size_t>(reference.segmentIndex)];
    auto wordsIt = segment.find("words");
    if (!segment.is_object() || wordsIt == segment.end() || !wordsIt->is_array() ||
        reference.wordIndex >= static_cast<int>(wordsIt->size())) {
        if (errorOut) *errorOut = "Transcript word no longer exists.";
        return nullptr;
    }
    json& word = (*wordsIt)[static_cast<std::size_t>(reference.wordIndex)];
    if (!word.is_object()) {
        if (errorOut) *errorOut = "Transcript word is not an object.";
        return nullptr;
    }
    return &word;
}

void addEditTag(json& word, const char* tag)
{
    json& edits = word["transcript_edits"];
    if (!edits.is_array()) edits = json::array();
    if (std::find(edits.begin(), edits.end(), tag) == edits.end()) edits.push_back(tag);
}

struct LocatedWord {
    TranscriptWordRef reference;
    json* word = nullptr;
    json* segment = nullptr;
    int fallbackOrder = 0;
};

std::vector<LocatedWord> locatedWords(json* root)
{
    std::vector<LocatedWord> result;
    if (!root || !root->is_object()) return result;
    auto segmentsIt = root->find("segments");
    if (segmentsIt == root->end() || !segmentsIt->is_array()) return result;
    int fallbackOrder = 0;
    for (std::size_t segmentIndex = 0; segmentIndex < segmentsIt->size(); ++segmentIndex) {
        json& segment = (*segmentsIt)[segmentIndex];
        if (!segment.is_object()) continue;
        auto wordsIt = segment.find("words");
        if (wordsIt == segment.end() || !wordsIt->is_array()) continue;
        for (std::size_t wordIndex = 0; wordIndex < wordsIt->size(); ++wordIndex) {
            json& word = (*wordsIt)[wordIndex];
            if (!word.is_object()) continue;
            LocatedWord located;
            located.reference.segmentIndex = static_cast<int>(segmentIndex);
            located.reference.wordIndex = static_cast<int>(wordIndex);
            located.reference.originalSegmentIndex =
                word.value("original_segment_index", static_cast<int>(segmentIndex));
            located.reference.originalWordIndex =
                word.value("original_word_index", static_cast<int>(wordIndex));
            located.reference.renderOrder = word.value("render_order", fallbackOrder);
            located.word = &word;
            located.segment = &segment;
            located.fallbackOrder = fallbackOrder++;
            result.push_back(located);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        const int lhsOrder = lhs.reference.renderOrder >= 0
            ? lhs.reference.renderOrder : lhs.fallbackOrder;
        const int rhsOrder = rhs.reference.renderOrder >= 0
            ? rhs.reference.renderOrder : rhs.fallbackOrder;
        return lhsOrder < rhsOrder;
    });
    return result;
}

bool sameAddress(const TranscriptWordRef& lhs, const TranscriptWordRef& rhs)
{
    return lhs.segmentIndex == rhs.segmentIndex && lhs.wordIndex == rhs.wordIndex;
}

std::vector<LocatedWord>::iterator findLocated(
    std::vector<LocatedWord>* words,
    const TranscriptWordRef& reference)
{
    return std::find_if(words->begin(), words->end(), [&](const LocatedWord& located) {
        return sameAddress(located.reference, reference);
    });
}

} // namespace

bool patchTranscriptWord(json* root,
                         const TranscriptWordRef& reference,
                         const TranscriptWordPatch& patch,
                         std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    json* word = wordAt(root, reference, errorOut);
    if (!word) return false;
    const double oldStart = word->value("start", 0.0);
    const double oldEnd = std::max(oldStart, word->value("end", oldStart));
    double start = patch.startSeconds.value_or(oldStart);
    double end = patch.endSeconds.value_or(oldEnd);
    if (!std::isfinite(start) || !std::isfinite(end)) {
        if (errorOut) *errorOut = "Transcript timing must be finite.";
        return false;
    }
    start = std::max(0.0, start);
    if (patch.startSeconds && !patch.endSeconds) start = std::min(start, oldEnd);
    end = std::max(start, end);
    bool changed = false;
    if (patch.startSeconds && start != oldStart) {
        (*word)["start"] = start;
        addEditTag(*word, "timing");
        changed = true;
    }
    if (patch.endSeconds && end != oldEnd) {
        (*word)["end"] = end;
        addEditTag(*word, "timing");
        changed = true;
    }
    if (patch.text) {
        const char* key = word->contains("word") ? "word" : "text";
        if (word->value(key, std::string{}) != *patch.text) {
            (*word)[key] = *patch.text;
            addEditTag(*word, "text");
            changed = true;
        }
    }
    if (patch.skipped && word->value("skipped", false) != *patch.skipped) {
        if (*patch.skipped) (*word)["skipped"] = true;
        else word->erase("skipped");
        addEditTag(*word, "skip");
        changed = true;
    }
    return changed;
}

bool deleteTranscriptWord(json* root,
                          const TranscriptWordRef& reference,
                          std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!wordAt(root, reference, errorOut)) return false;
    json& words = (*root)["segments"][static_cast<std::size_t>(reference.segmentIndex)]["words"];
    words.erase(words.begin() + reference.wordIndex);
    (*root)["transcript_deleted_edits"] = root->value("transcript_deleted_edits", 0) + 1;
    return true;
}

std::optional<TranscriptWordRef> insertTranscriptWord(
    json* root,
    const TranscriptWordRef& anchor,
    bool above,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::vector<LocatedWord> ordered = locatedWords(root);
    auto anchorIt = findLocated(&ordered, anchor);
    if (anchorIt == ordered.end()) {
        if (errorOut) *errorOut = "Transcript anchor word no longer exists.";
        return std::nullopt;
    }
    const std::size_t anchorOrder =
        static_cast<std::size_t>(std::distance(ordered.begin(), anchorIt));
    const std::size_t insertOrder = anchorOrder + (above ? 0U : 1U);
    const double anchorStart = anchorIt->word->value("start", 0.0);
    const double anchorEnd = std::max(anchorStart, anchorIt->word->value("end", anchorStart));
    const double previousEnd = anchorOrder > 0
        ? ordered[anchorOrder - 1].word->value("end", 0.0)
        : 0.0;
    const double nextStart = anchorOrder + 1 < ordered.size()
        ? ordered[anchorOrder + 1].word->value("start", anchorEnd + 1.0)
        : anchorEnd + 1.0;

    double start = 0.0;
    double end = 0.0;
    if (above) {
        start = (previousEnd + anchorStart) / 2.0;
        end = std::min(start + 0.1, anchorStart - 0.01);
        if (end <= start) {
            start = anchorStart - 0.2;
            end = anchorStart - 0.05;
        }
    } else {
        start = (anchorEnd + nextStart) / 2.0;
        end = std::min(start + 0.1, nextStart - 0.01);
        if (end <= start || start < anchorEnd) {
            start = anchorEnd + 0.05;
            end = anchorEnd + 0.2;
        }
    }
    start = std::max(0.0, start);
    end = std::max(start + 0.01, end);

    for (std::size_t index = 0; index < ordered.size(); ++index) {
        (*ordered[index].word)["render_order"] =
            static_cast<int>(index + (index >= insertOrder ? 1U : 0U));
    }
    json inserted = {
        {"word", "[new]"},
        {"start", start},
        {"end", end},
        {"render_order", static_cast<int>(insertOrder)},
        {"original_segment_index", -1},
        {"original_word_index", -1},
        {"transcript_edits", json::array({"inserted"})},
    };
    std::string speaker = anchorIt->word->value("speaker", std::string{});
    if (speaker.empty() && anchorIt->segment) {
        speaker = anchorIt->segment->value("speaker", std::string{});
    }
    if (!speaker.empty()) inserted["speaker"] = std::move(speaker);

    json& words = (*root)["segments"][static_cast<std::size_t>(anchor.segmentIndex)]["words"];
    const int physicalIndex = std::clamp(
        anchor.wordIndex + (above ? 0 : 1), 0, static_cast<int>(words.size()));
    words.insert(words.begin() + physicalIndex, std::move(inserted));
    TranscriptWordRef result;
    result.segmentIndex = anchor.segmentIndex;
    result.wordIndex = physicalIndex;
    result.originalSegmentIndex = -1;
    result.originalWordIndex = -1;
    result.renderOrder = static_cast<int>(insertOrder);
    return result;
}

bool expandTranscriptWordTiming(json* root,
                                const TranscriptWordRef& reference,
                                std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    std::vector<LocatedWord> ordered = locatedWords(root);
    auto target = findLocated(&ordered, reference);
    if (target == ordered.end()) {
        if (errorOut) *errorOut = "Transcript word no longer exists.";
        return false;
    }
    const std::size_t index =
        static_cast<std::size_t>(std::distance(ordered.begin(), target));
    const double oldStart = target->word->value("start", 0.0);
    const double oldEnd = std::max(oldStart, target->word->value("end", oldStart));
    const double start = index > 0
        ? ordered[index - 1].word->value("end", oldStart) : oldStart;
    const double end = index + 1 < ordered.size()
        ? ordered[index + 1].word->value("start", oldEnd) : oldEnd;
    if (start == oldStart && end == oldEnd) return false;
    (*target->word)["start"] = std::max(0.0, std::min(start, end));
    (*target->word)["end"] = std::max((*target->word)["start"].get<double>(), end);
    addEditTag(*target->word, "timing");
    return true;
}

bool restoreTranscriptWord(json* root,
                           const TranscriptWordRef& reference,
                           const json& originalRoot,
                           std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    json* current = wordAt(root, reference, errorOut);
    if (!current) return false;
    const int originalSegment = current->value(
        "original_segment_index", reference.originalSegmentIndex);
    const int originalWord = current->value(
        "original_word_index", reference.originalWordIndex);
    TranscriptWordRef originalReference;
    originalReference.segmentIndex = originalSegment;
    originalReference.wordIndex = originalWord;
    json originalCopy = originalRoot;
    json* original = wordAt(&originalCopy, originalReference, errorOut);
    if (!original) return false;
    const json& originalSegmentObject =
        originalCopy["segments"][static_cast<std::size_t>(originalSegment)];
    const std::string text = original->value(
        "word", original->value("text", std::string{}));
    const double start = original->value("start", 0.0);
    const double end = std::max(start, original->value("end", start));
    std::string speaker = original->value("speaker", std::string{});
    if (speaker.empty()) speaker = originalSegmentObject.value("speaker", std::string{});
    const bool skipped = original->value("skipped", false);
    const char* textKey = current->contains("word") ? "word" : "text";
    const bool changed = current->value(textKey, std::string{}) != text ||
        current->value("start", 0.0) != start ||
        current->value("end", start) != end ||
        current->value("speaker", std::string{}) != speaker ||
        current->value("skipped", false) != skipped ||
        current->contains("transcript_edits");
    if (!changed) return false;
    (*current)[textKey] = text;
    (*current)["start"] = start;
    (*current)["end"] = end;
    if (speaker.empty()) current->erase("speaker");
    else (*current)["speaker"] = speaker;
    if (skipped) (*current)["skipped"] = true;
    else current->erase("skipped");
    current->erase("transcript_edits");
    return true;
}

bool moveTranscriptWordRenderOrder(json* root,
                                   const TranscriptWordRef& reference,
                                   int direction,
                                   std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (direction == 0) return false;
    std::vector<LocatedWord> ordered = locatedWords(root);
    auto target = findLocated(&ordered, reference);
    if (target == ordered.end()) {
        if (errorOut) *errorOut = "Transcript word no longer exists.";
        return false;
    }
    const int index = static_cast<int>(std::distance(ordered.begin(), target));
    const int other = index + (direction < 0 ? -1 : 1);
    if (other < 0 || other >= static_cast<int>(ordered.size())) return false;
    std::swap(ordered[static_cast<std::size_t>(index)],
              ordered[static_cast<std::size_t>(other)]);
    for (std::size_t order = 0; order < ordered.size(); ++order) {
        (*ordered[order].word)["render_order"] = static_cast<int>(order);
    }
    return true;
}

bool patchTranscriptSpeakerProfile(
    json* root,
    const std::string& speakerId,
    const TranscriptSpeakerProfilePatch& patch,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!root || !root->is_object() || speakerId.empty()) {
        if (errorOut) *errorOut = "Speaker identity is empty.";
        return false;
    }
    json& profiles = (*root)["speaker_profiles"];
    if (!profiles.is_object()) profiles = json::object();
    json& profile = profiles[speakerId];
    if (!profile.is_object()) profile = json::object();
    bool changed = false;
    const auto patchText = [&](const char* key, const std::optional<std::string>& value) {
        if (!value) return;
        if (profile.value(key, std::string{}) == *value) return;
        if (value->empty()) profile.erase(key);
        else profile[key] = *value;
        changed = true;
    };
    patchText("name", patch.name);
    patchText("organization", patch.organization);
    if (patch.x || patch.y) {
        json& location = profile["location"];
        if (!location.is_object()) location = json::object();
        const auto patchCoordinate = [&](const char* key, const std::optional<double>& value) {
            if (!value) return;
            if (!std::isfinite(*value)) {
                if (errorOut) *errorOut = "Speaker location must be finite.";
                return;
            }
            const double bounded = std::clamp(*value, 0.0, 1.0);
            if (!location.contains(key) || !location[key].is_number() ||
                location[key].get<double>() != bounded) {
                location[key] = bounded;
                changed = true;
            }
        };
        patchCoordinate("x", patch.x);
        if (errorOut && !errorOut->empty()) return false;
        patchCoordinate("y", patch.y);
        if (errorOut && !errorOut->empty()) return false;
    }
    return changed;
}

bool setTranscriptSpeakerTrackAssignments(
    json* root,
    const std::string& clipId,
    const std::string& speakerId,
    const std::vector<TranscriptTrackAssignmentAnchor>& anchors,
    bool replaceExistingForSpeaker,
    const std::string& updatedAtUtc,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!root || !root->is_object() || clipId.empty() || speakerId.empty()) {
        if (errorOut) *errorOut = "Clip and speaker identities are required.";
        return false;
    }
    std::vector<TranscriptTrackAssignmentAnchor> normalizedAnchors;
    for (TranscriptTrackAssignmentAnchor anchor : anchors) {
        if (anchor.trackId >= 0 &&
            std::find_if(
                normalizedAnchors.begin(), normalizedAnchors.end(),
                [&](const auto& candidate) {
                    return candidate.trackId == anchor.trackId;
                }) ==
                normalizedAnchors.end()) {
            anchor.sourceFrame = std::max<std::int64_t>(0, anchor.sourceFrame);
            anchor.x = std::clamp(anchor.x, 0.0, 1.0);
            anchor.y = std::clamp(anchor.y, 0.0, 1.0);
            anchor.box = std::clamp(anchor.box, 0.01, 1.0);
            normalizedAnchors.push_back(std::move(anchor));
        }
    }
    const auto anchorForTrack = [&](int trackId) {
        return std::find_if(
            normalizedAnchors.begin(), normalizedAnchors.end(),
            [&](const auto& anchor) { return anchor.trackId == trackId; });
    };
    json& speakerFlow = (*root)["speaker_flow"];
    if (!speakerFlow.is_object()) speakerFlow = json::object();
    speakerFlow["schema_version"] = "1.0";
    json& clips = speakerFlow["clips"];
    if (!clips.is_object()) clips = json::object();
    json& clip = clips[clipId];
    if (!clip.is_object()) clip = json::object();
    clip["clip_id"] = clipId;
    if (!updatedAtUtc.empty()) clip["updated_at_utc"] = updatedAtUtc;
    json& resolved = clip["resolved_current"];
    if (!resolved.is_object()) resolved = json::object();
    if (!updatedAtUtc.empty()) resolved["updated_at_utc"] = updatedAtUtc;
    json current = resolved.value("track_identity_map", json::array());
    if (!current.is_array()) current = json::array();
    const json before = current;
    json next = json::array();
    std::vector<int> applied;
    for (json row : current) {
        if (!row.is_object()) continue;
        const int trackId = row.value("track_id", -1);
        const auto selectedAnchor = anchorForTrack(trackId);
        const bool selected = selectedAnchor != normalizedAnchors.end();
        const bool owned =
            row.value("identity_id", std::string{}) == speakerId;
        if (selected) {
            row["identity_id"] = speakerId;
            row["stream_id"] = selectedAnchor->streamId;
            row["anchor_source_frame"] = selectedAnchor->sourceFrame;
            row["anchor_x"] = selectedAnchor->x;
            row["anchor_y"] = selectedAnchor->y;
            row["anchor_box_size"] = selectedAnchor->box;
            row["resolution_source"] = "speaker_track_picker";
            if (!updatedAtUtc.empty()) row["updated_at_utc"] = updatedAtUtc;
            next.push_back(std::move(row));
            applied.push_back(trackId);
        } else if (!(replaceExistingForSpeaker && owned)) {
            next.push_back(std::move(row));
        }
    }
    for (const auto& anchor : normalizedAnchors) {
        if (std::find(applied.begin(), applied.end(), anchor.trackId) != applied.end()) {
            continue;
        }
        json row = {
            {"track_id", anchor.trackId},
            {"identity_id", speakerId},
            {"stream_id", anchor.streamId},
            {"anchor_source_frame", anchor.sourceFrame},
            {"anchor_x", anchor.x},
            {"anchor_y", anchor.y},
            {"anchor_box_size", anchor.box},
            {"resolution_source", "speaker_track_picker"},
        };
        if (!updatedAtUtc.empty()) row["updated_at_utc"] = updatedAtUtc;
        next.push_back(std::move(row));
    }
    if (next == before) return false;
    resolved["track_identity_map"] = std::move(next);
    return true;
}

bool saveTranscriptDocumentAtomic(const std::string& path,
                                  const json& root,
                                  std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    namespace fs = std::filesystem;
    const fs::path destination(path);
    if (destination.empty()) {
        if (errorOut) *errorOut = "Transcript path is empty.";
        return false;
    }
    const fs::path temporary = destination.string() + ".jcut.tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !(stream << root.dump(2) << '\n') || !stream.flush()) {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            if (errorOut) *errorOut = "Could not write temporary transcript file.";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    if (ec) {
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        if (errorOut) *errorOut = "Could not replace transcript: " + ec.message();
        return false;
    }
    return true;
}

} // namespace jcut
