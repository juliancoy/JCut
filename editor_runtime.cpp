#include "editor_runtime.h"

#include "editor_document_core_json.h"
#include "editor_grading_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr float kMinPlaybackSpeed = 0.25f;
constexpr float kMaxPlaybackSpeed = 2.0f;
constexpr float kMinPreviewZoom = 0.5f;
constexpr float kMaxPreviewZoom = 3.0f;
constexpr double kDefaultTimelineFps = 30.0;
constexpr std::int64_t kEditorAudioSampleRate = 48000;
constexpr std::int64_t kEditorSamplesPerFrame =
    kEditorAudioSampleRate / 30;

double normalizedScale(double value)
{
    const double clamped = std::clamp(value, -100.0, 100.0);
    if (std::abs(clamped) >= 0.01) {
        return clamped;
    }
    return clamped < 0.0 ? -0.01 : 0.01;
}

bool mediaKindHasVisuals(const std::string& mediaKind)
{
    return mediaKind == "video" || mediaKind == "image" ||
        mediaKind == "title" || mediaKind == "graphics";
}

bool editorClipHasVisuals(const jcut::EditorClip& clip)
{
    if (mediaKindHasVisuals(clip.mediaKind)) {
        return true;
    }
    if (clip.mediaKind == "audio") {
        return false;
    }
    // Legacy documents can omit mediaKind while still carrying the visual
    // enablement used by the neutral render bridge. Treat unknown/future kinds
    // as visual when that persisted signal says the clip has a video layer.
    return clip.videoEnabled;
}

std::string trimmed(std::string value)
{
    const auto notSpace = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

template <typename Predicate>
void selectSingle(std::vector<jcut::EditorTrack>* items, Predicate predicate)
{
    for (jcut::EditorTrack& item : *items) {
        item.selected = predicate(item);
    }
}

template <typename Predicate>
void selectSingle(std::vector<jcut::EditorClip>* items, Predicate predicate)
{
    for (jcut::EditorClip& item : *items) {
        item.selected = predicate(item);
    }
}

jcut::EditorClip* findClip(std::vector<jcut::EditorClip>* clips, int clipId)
{
    for (jcut::EditorClip& clip : *clips) {
        if (clip.id == clipId) {
            return &clip;
        }
    }
    return nullptr;
}

jcut::EditorTrack* findTrack(std::vector<jcut::EditorTrack>* tracks, int trackId)
{
    for (jcut::EditorTrack& track : *tracks) {
        if (track.id == trackId) {
            return &track;
        }
    }
    return nullptr;
}

std::size_t trackIndexForId(const std::vector<jcut::EditorTrack>& tracks,
                            int trackId)
{
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].id == trackId) {
            return index;
        }
    }
    return tracks.size();
}

void selectDeterministicClip(const std::vector<jcut::EditorTrack>& tracks,
                             std::vector<jcut::EditorClip>* clips)
{
    if (!clips) {
        return;
    }
    for (jcut::EditorClip& clip : *clips) {
        clip.selected = false;
    }
    if (clips->empty()) {
        return;
    }
    const auto nextSelection = std::min_element(
        clips->begin(), clips->end(),
        [&](const jcut::EditorClip& left, const jcut::EditorClip& right) {
            const std::size_t leftTrack = trackIndexForId(tracks, left.trackId);
            const std::size_t rightTrack =
                trackIndexForId(tracks, right.trackId);
            if (leftTrack != rightTrack) {
                return leftTrack < rightTrack;
            }
            if (left.startFrame != right.startFrame) {
                return left.startFrame < right.startFrame;
            }
            return left.id < right.id;
        });
    nextSelection->selected = true;
}

using ClipPersistentIdSet = std::unordered_set<std::string>;

ClipPersistentIdSet clipOwnershipClosure(
    const jcut::EditorDocumentCore& document,
    const ClipPersistentIdSet& seedIds,
    bool includeAncestors);

struct SelectedFrameShift {
    std::int64_t delta = 0;
    ClipPersistentIdSet persistentIds;
};

bool prepareSelectedFrameShift(const jcut::EditorDocumentCore& document,
                               std::int64_t requestedDelta,
                               SelectedFrameShift* shift,
                               std::string* error)
{
    if (!shift) {
        return false;
    }
    shift->delta = 0;
    shift->persistentIds.clear();

    ClipPersistentIdSet selectedIds;
    selectedIds.reserve(document.clips.size());
    for (const jcut::EditorClip& clip : document.clips) {
        if (!clip.selected) {
            continue;
        }
        const std::string clipId =
            jcut::trimmedEditorClipId(clip.persistentId);
        if (!clipId.empty()) {
            selectedIds.insert(clipId);
        }
    }
    if (selectedIds.empty()) {
        if (error) {
            *error = "no clips selected";
        }
        return false;
    }

    // A matte can participate only through its selected parent. Its persisted
    // lock is a relationship invariant, not a reason to block a parent edit.
    for (const jcut::EditorClip& clip : document.clips) {
        if (!clip.selected) {
            continue;
        }
        if (jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte") {
            const std::string parentId =
                jcut::trimmedEditorClipId(clip.linkedSourceClipId);
            if (parentId.empty() || selectedIds.find(parentId) == selectedIds.end()) {
                if (error) {
                    *error = "mask matte must be moved with its source";
                }
                return false;
            }
        } else if (clip.locked) {
            if (error) {
                *error = "locked clips cannot be moved";
            }
            return false;
        }
    }

    shift->persistentIds =
        clipOwnershipClosure(document, selectedIds, false);

    int minimumStartFrame = std::numeric_limits<int>::max();
    for (const jcut::EditorClip& clip : document.clips) {
        if (shift->persistentIds.find(
                jcut::trimmedEditorClipId(clip.persistentId)) ==
            shift->persistentIds.end()) {
            continue;
        }
        minimumStartFrame = std::min(minimumStartFrame, clip.startFrame);
    }
    shift->delta = std::max<std::int64_t>(
        requestedDelta, -static_cast<std::int64_t>(minimumStartFrame));
    for (const jcut::EditorClip& clip : document.clips) {
        if (shift->persistentIds.find(
                jcut::trimmedEditorClipId(clip.persistentId)) ==
            shift->persistentIds.end()) {
            continue;
        }
        const std::int64_t nextStart =
            static_cast<std::int64_t>(clip.startFrame) + shift->delta;
        if (nextStart < 0 || nextStart > std::numeric_limits<int>::max()) {
            if (error) {
                *error = "selected clip frame move is out of range";
            }
            return false;
        }
    }
    for (const jcut::EditorRenderSyncMarker& marker :
         document.renderSyncMarkers) {
        if (shift->persistentIds.find(
                jcut::trimmedEditorClipId(marker.clipId)) ==
            shift->persistentIds.end()) {
            continue;
        }
        if ((shift->delta > 0 &&
             marker.frame > std::numeric_limits<std::int64_t>::max() -
                     shift->delta) ||
            (shift->delta < 0 && marker.frame < -shift->delta)) {
            if (error) {
                *error = "render sync marker move is out of range";
            }
            return false;
        }
    }
    return true;
}

void applySelectedFrameShift(jcut::EditorDocumentCore* document,
                             const SelectedFrameShift& shift)
{
    if (!document) {
        return;
    }
    for (jcut::EditorClip& clip : document->clips) {
        if (shift.persistentIds.find(
                jcut::trimmedEditorClipId(clip.persistentId)) ==
            shift.persistentIds.end()) {
            continue;
        }
        clip.startFrame = static_cast<int>(
            static_cast<std::int64_t>(clip.startFrame) + shift.delta);
    }
    for (jcut::EditorRenderSyncMarker& marker :
         document->renderSyncMarkers) {
        if (shift.persistentIds.find(
                jcut::trimmedEditorClipId(marker.clipId)) !=
            shift.persistentIds.end()) {
            marker.frame += shift.delta;
        }
    }
    std::sort(
        document->renderSyncMarkers.begin(),
        document->renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& left,
           const jcut::EditorRenderSyncMarker& right) {
            if (left.frame != right.frame) {
                return left.frame < right.frame;
            }
            return left.clipId < right.clipId;
        });
}

std::string persistentClipIdForNumericId(int clipId)
{
    return "imgui-clip-" + std::to_string(clipId);
}

std::string uniquePersistentClipId(
    const std::vector<jcut::EditorClip>& clips,
    int clipId)
{
    const std::string base = persistentClipIdForNumericId(clipId);
    std::string candidate = base;
    int suffix = 2;
    const auto exists = [&](const std::string& value) {
        return std::any_of(clips.begin(), clips.end(), [&](const jcut::EditorClip& clip) {
            return clip.persistentId == value;
        });
    };
    while (exists(candidate)) {
        candidate = base + "-" + std::to_string(suffix++);
    }
    return candidate;
}

void ensurePersistentClipIds(jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }
    std::vector<std::string> used;
    used.reserve(document->clips.size());
    for (jcut::EditorClip& clip : document->clips) {
        std::string candidate = clip.persistentId.empty()
            ? persistentClipIdForNumericId(clip.id)
            : clip.persistentId;
        const std::string base = candidate;
        int suffix = 2;
        while (std::find(used.begin(), used.end(), candidate) != used.end()) {
            candidate = base + "-" + std::to_string(suffix++);
        }
        clip.persistentId = std::move(candidate);
        used.push_back(clip.persistentId);
    }
}

void normalizeClipRelationships(jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }
    for (jcut::EditorClip& clip : document->clips) {
        clip.clipRole = jcut::editorClipRoleForStorage(clip.clipRole);
        clip.linkedSourceClipId =
            jcut::trimmedEditorClipId(clip.linkedSourceClipId);
    }
}

ClipPersistentIdSet clipOwnershipClosure(
    const jcut::EditorDocumentCore& document,
    const ClipPersistentIdSet& seedIds,
    bool includeAncestors)
{
    ClipPersistentIdSet closure;
    closure.reserve(document.clips.size() + seedIds.size());
    for (const std::string& seedId : seedIds) {
        const std::string normalizedId = jcut::trimmedEditorClipId(seedId);
        if (!normalizedId.empty()) {
            closure.insert(normalizedId);
        }
    }

    if (includeAncestors) {
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const jcut::EditorClip& clip : document.clips) {
                const std::string clipId =
                    jcut::trimmedEditorClipId(clip.persistentId);
                if (closure.find(clipId) == closure.end() ||
                    jcut::canonicalEditorClipRole(clip.clipRole) !=
                        "mask_matte") {
                    continue;
                }
                const std::string parentId =
                    jcut::trimmedEditorClipId(clip.linkedSourceClipId);
                if (!parentId.empty()) {
                    expanded = closure.insert(parentId).second || expanded;
                }
            }
        }
    }

    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const jcut::EditorClip& clip : document.clips) {
            if (jcut::canonicalEditorClipRole(clip.clipRole) !=
                "mask_matte") {
                continue;
            }
            const std::string clipId =
                jcut::trimmedEditorClipId(clip.persistentId);
            const std::string parentId =
                jcut::trimmedEditorClipId(clip.linkedSourceClipId);
            if (clipId.empty() || closure.find(clipId) != closure.end() ||
                closure.find(parentId) == closure.end()) {
                continue;
            }
            expanded = closure.insert(clipId).second || expanded;
        }
    }
    return closure;
}

ClipPersistentIdSet selectedClipPersistentIds(
    const jcut::EditorDocumentCore& document)
{
    ClipPersistentIdSet selectedIds;
    selectedIds.reserve(document.clips.size());
    for (const jcut::EditorClip& clip : document.clips) {
        if (!clip.selected) {
            continue;
        }
        const std::string clipId =
            jcut::trimmedEditorClipId(clip.persistentId);
        if (!clipId.empty()) {
            selectedIds.insert(clipId);
        }
    }
    return selectedIds;
}

bool persistentClipIdInSet(const ClipPersistentIdSet& clipIds,
                           std::string_view clipId)
{
    return clipIds.find(jcut::trimmedEditorClipId(clipId)) != clipIds.end();
}

void normalizeMaskMatteParentCaches(jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }
    for (jcut::EditorClip& child : document->clips) {
        if (jcut::canonicalEditorClipRole(child.clipRole) != "mask_matte") {
            continue;
        }
        const std::string parentId =
            jcut::trimmedEditorClipId(child.linkedSourceClipId);
        const auto parent = std::find_if(
            document->clips.cbegin(), document->clips.cend(),
            [&](const jcut::EditorClip& candidate) {
                return jcut::trimmedEditorClipId(candidate.persistentId) ==
                    parentId;
            });
        if (parent == document->clips.cend() ||
            jcut::canonicalEditorClipRole(parent->clipRole) == "mask_matte") {
            continue;
        }

        // Mirrors Qt's normalizeMaskMatteClips timing boundary. Visual
        // treatment remains child-owned; these are parent-derived caches.
        child.sourcePath = parent->sourcePath;
        child.proxyPath = parent->proxyPath;
        child.useProxy = parent->useProxy;
        child.mediaKind = parent->mediaKind;
        child.sourceDurationFrames = parent->sourceDurationFrames;
        child.sourceInFrame = parent->sourceInFrame;
        child.sourceInSubframeSamples = parent->sourceInSubframeSamples;
        child.startFrame = parent->startFrame;
        child.startSubframeSamples = parent->startSubframeSamples;
        child.durationFrames = parent->durationFrames;
        child.durationSubframeSamples = parent->durationSubframeSamples;
        child.sourceFps = parent->sourceFps;
        child.playbackRate = parent->playbackRate;
        child.baseTranslationX = parent->baseTranslationX;
        child.baseTranslationY = parent->baseTranslationY;
        child.baseRotation = parent->baseRotation;
        child.baseScaleX = parent->baseScaleX;
        child.baseScaleY = parent->baseScaleY;
        child.transformKeyframes = parent->transformKeyframes;
    }
}

void eraseOwnedClipsAndMarkers(jcut::EditorDocumentCore* document,
                               const ClipPersistentIdSet& removedIds)
{
    if (!document || removedIds.empty()) {
        return;
    }
    document->clips.erase(
        std::remove_if(
            document->clips.begin(), document->clips.end(),
            [&](const jcut::EditorClip& clip) {
                return persistentClipIdInSet(removedIds, clip.persistentId);
            }),
        document->clips.end());
    document->renderSyncMarkers.erase(
        std::remove_if(
            document->renderSyncMarkers.begin(),
            document->renderSyncMarkers.end(),
            [&](const jcut::EditorRenderSyncMarker& marker) {
                return persistentClipIdInSet(removedIds, marker.clipId);
            }),
        document->renderSyncMarkers.end());
}

bool renderSyncMarkerLess(const jcut::EditorRenderSyncMarker& left,
                          const jcut::EditorRenderSyncMarker& right)
{
    if (left.frame != right.frame) {
        return left.frame < right.frame;
    }
    return left.clipId < right.clipId;
}

void normalizeRenderSyncMarkers(jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }

    std::vector<jcut::EditorRenderSyncMarker> normalized;
    normalized.reserve(document->renderSyncMarkers.size());
    for (jcut::EditorRenderSyncMarker marker : document->renderSyncMarkers) {
        marker.clipId =
            jcut::editorRenderSyncOwnerClipId(*document, marker.clipId);
        if (marker.clipId.empty()) {
            continue;
        }
        marker.count = std::clamp(
            marker.count,
            jcut::kEditorRenderSyncMinCount,
            jcut::kEditorRenderSyncMaxCount);
        const auto existing = std::find_if(
            normalized.begin(), normalized.end(),
            [&](const jcut::EditorRenderSyncMarker& value) {
                return value.clipId == marker.clipId &&
                    value.frame == marker.frame;
            });
        if (existing == normalized.end()) {
            normalized.push_back(std::move(marker));
        } else {
            // A source frame has exactly one mapping decision. Keep the last
            // serialized marker when normalizing documents from older builds.
            *existing = std::move(marker);
        }
    }
    std::sort(normalized.begin(), normalized.end(), renderSyncMarkerLess);
    document->renderSyncMarkers = std::move(normalized);
}

template <typename Keyframe>
void upsertKeyframe(std::vector<Keyframe>* keyframes, Keyframe keyframe)
{
    const auto existing = std::find_if(
        keyframes->begin(), keyframes->end(),
        [&](const Keyframe& value) { return value.frame == keyframe.frame; });
    if (existing == keyframes->end()) {
        keyframes->push_back(std::move(keyframe));
    } else {
        *existing = std::move(keyframe);
    }
    std::sort(keyframes->begin(), keyframes->end(),
              [](const Keyframe& left, const Keyframe& right) {
                  return left.frame < right.frame;
              });
}

template <typename Keyframe>
bool removeKeyframeAtFrame(std::vector<Keyframe>* keyframes,
                           std::int64_t frame)
{
    if (!keyframes) {
        return false;
    }
    const auto previousSize = keyframes->size();
    keyframes->erase(
        std::remove_if(
            keyframes->begin(), keyframes->end(),
            [&](const Keyframe& keyframe) { return keyframe.frame == frame; }),
        keyframes->end());
    return keyframes->size() != previousSize;
}

bool hasTrackId(const std::vector<jcut::EditorTrack>& tracks, int trackId)
{
    for (const jcut::EditorTrack& track : tracks) {
        if (track.id == trackId) {
            return true;
        }
    }
    return false;
}

bool isTitleTrackLabel(const std::string& label, bool exact = false)
{
    std::string normalized = trimmed(label);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return exact ? normalized == "titles"
                 : normalized.rfind("titles", 0) == 0;
}

std::string nextTitleTrackLabel(const std::vector<jcut::EditorTrack>& tracks)
{
    const std::size_t titleTrackCount = static_cast<std::size_t>(
        std::count_if(
            tracks.begin(), tracks.end(),
            [](const jcut::EditorTrack& track) {
                return isTitleTrackLabel(track.label);
            }));
    return titleTrackCount == 0
        ? std::string("Titles")
        : std::string("Titles ") + std::to_string(titleTrackCount + 1);
}

const jcut::EditorMediaItem* findMediaItem(const std::vector<jcut::EditorMediaItem>& mediaItems,
                                           const std::string& mediaId)
{
    for (const jcut::EditorMediaItem& mediaItem : mediaItems) {
        if (mediaItem.id == mediaId) {
            return &mediaItem;
        }
    }
    return nullptr;
}

int nextTrackId(const std::vector<jcut::EditorTrack>& tracks)
{
    int nextId = 1;
    for (const jcut::EditorTrack& track : tracks) {
        nextId = std::max(nextId, track.id + 1);
    }
    return nextId;
}

int nextClipId(const std::vector<jcut::EditorClip>& clips)
{
    int nextId = 1;
    for (const jcut::EditorClip& clip : clips) {
        nextId = std::max(nextId, clip.id + 1);
    }
    return nextId;
}

void ensureMediaItemForClip(jcut::EditorDocumentCore* document,
                            const std::string& sourcePath,
                            const std::string& label,
                            const std::string& mediaKind,
                            bool audioPresenceKnown = false,
                            bool hasAudio = false)
{
    if (sourcePath.empty()) {
        return;
    }
    for (jcut::EditorMediaItem& mediaItem : document->mediaItems) {
        if (mediaItem.id == sourcePath) {
            if (audioPresenceKnown) {
                mediaItem.audioPresenceKnown = true;
                mediaItem.hasAudio = hasAudio;
            }
            return;
        }
    }
    document->mediaItems.push_back({
        sourcePath,
        label.empty() ? sourcePath : label,
        mediaKind.empty() ? std::string("unknown") : mediaKind,
        audioPresenceKnown,
        hasAudio
    });
}

void pruneUnusedMediaItems(jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }
    std::vector<jcut::EditorMediaItem> filtered;
    filtered.reserve(document->mediaItems.size());
    for (const jcut::EditorMediaItem& mediaItem : document->mediaItems) {
        const bool used = std::any_of(
            document->clips.cbegin(), document->clips.cend(),
            [&mediaItem](const jcut::EditorClip& clip) {
                return !clip.sourcePath.empty() && clip.sourcePath == mediaItem.id;
            });
        // Explicitly imported library entries use the media-* namespace and
        // remain available even when they are not currently on the timeline.
        if (used || mediaItem.id.rfind("media-", 0) == 0) {
            filtered.push_back(mediaItem);
        }
    }
    document->mediaItems = std::move(filtered);
}

std::string fallbackLabelFromPath(const std::string& sourcePath)
{
    if (sourcePath.empty()) {
        return "media";
    }
    const std::size_t separator = sourcePath.find_last_of("/\\");
    if (separator == std::string::npos) {
        return sourcePath;
    }
    return sourcePath.substr(separator + 1);
}

template <typename Keyframe>
void splitKeyframes(
    std::vector<Keyframe>* left,
    std::vector<Keyframe>* right,
    std::int64_t splitFrame)
{
    if (!left || !right) {
        return;
    }
    const std::vector<Keyframe> original = *left;
    left->clear();
    right->clear();
    for (const Keyframe& keyframe : original) {
        if (keyframe.frame < splitFrame) {
            left->push_back(keyframe);
        } else {
            Keyframe shifted = keyframe;
            shifted.frame -= splitFrame;
            right->push_back(std::move(shifted));
        }
    }
}

template <typename Keyframe>
void trimKeyframesFromStart(std::vector<Keyframe>* keyframes, std::int64_t trimFrames)
{
    if (!keyframes || trimFrames == 0) {
        return;
    }
    std::vector<Keyframe> shifted;
    shifted.reserve(keyframes->size());
    for (const Keyframe& keyframe : *keyframes) {
        if (keyframe.frame < trimFrames) {
            continue;
        }
        Keyframe value = keyframe;
        value.frame -= trimFrames;
        shifted.push_back(std::move(value));
    }
    *keyframes = std::move(shifted);
}

void advanceClipSourceIn(jcut::EditorClip* clip, std::int64_t timelineFrames)
{
    if (!clip || timelineFrames == 0 || clip->mediaKind == "image" ||
        clip->mediaKind == "title" || clip->mediaKind == "graphics") {
        if (clip && (clip->mediaKind == "image" || clip->mediaKind == "title" ||
                     clip->mediaKind == "graphics")) {
            clip->sourceInFrame = 0;
            clip->sourceInSubframeSamples = 0;
        }
        return;
    }

    constexpr long double kAudioSampleRate = 48000.0L;
    const long double sourceFps = clip->sourceFps > 0.001
        ? static_cast<long double>(clip->sourceFps)
        : static_cast<long double>(kDefaultTimelineFps);
    const long double playbackRate = std::clamp<long double>(clip->playbackRate, 0.001L, 1000.0L);
    const auto samplesForSourceFrame = [&](std::int64_t frame) {
        return static_cast<std::int64_t>(std::llround(
            static_cast<long double>(std::max<std::int64_t>(0, frame)) *
            kAudioSampleRate / sourceFps));
    };
    const std::int64_t originalSamples =
        samplesForSourceFrame(clip->sourceInFrame) + clip->sourceInSubframeSamples;
    const std::int64_t consumedSamples = static_cast<std::int64_t>(std::floor(
        static_cast<long double>(timelineFrames) *
        (kAudioSampleRate / static_cast<long double>(kDefaultTimelineFps)) *
        playbackRate));
    const std::int64_t nextSamples = std::max<std::int64_t>(0, originalSamples + consumedSamples);
    std::int64_t nextFrame = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(std::floor(
               static_cast<long double>(nextSamples) * sourceFps / kAudioSampleRate)));
    while (nextFrame > 0 && samplesForSourceFrame(nextFrame) > nextSamples) {
        --nextFrame;
    }
    while (nextFrame < std::numeric_limits<std::int64_t>::max() &&
           samplesForSourceFrame(nextFrame + 1) <= nextSamples) {
        ++nextFrame;
    }
    if (clip->sourceDurationFrames > 0) {
        nextFrame = std::min(nextFrame, clip->sourceDurationFrames - 1);
    }
    clip->sourceInFrame = nextFrame;
    clip->sourceInSubframeSamples =
        std::max<std::int64_t>(0, nextSamples - samplesForSourceFrame(nextFrame));
}

void sortClipsByTimeline(std::vector<jcut::EditorClip>* clips)
{
    std::sort(clips->begin(), clips->end(),
              [](const jcut::EditorClip& left,
                 const jcut::EditorClip& right) {
                  if (left.trackId != right.trackId) {
                      return left.trackId < right.trackId;
                  }
                  if (left.startFrame != right.startFrame) {
                      return left.startFrame < right.startFrame;
                  }
                  return left.id < right.id;
              });
}

jcut::CommandResult splitSingleClipAtFrame(
    jcut::EditorDocumentCore* document,
    int clipId,
    int frame,
    int* trailingClipId = nullptr,
    bool ownedMaskMatte = false)
{
    if (!document) {
        return {false, "document unavailable"};
    }
    jcut::EditorClip* clip = findClip(&document->clips, clipId);
    if (!clip) {
        return {false, "clip not found"};
    }
    if (jcut::canonicalEditorClipRole(clip->clipRole) == "mask_matte" &&
        !ownedMaskMatte) {
        return {false, "mask matte must be split with its source"};
    }
    if (clip->locked && !ownedMaskMatte) {
        return {false, "locked clip cannot be split"};
    }
    const int clipStart = clip->startFrame;
    const int clipEnd = clip->startFrame + clip->durationFrames;
    if (frame <= clipStart || frame >= clipEnd) {
        return {false, "split frame outside clip"};
    }

    const int leadingDuration = frame - clipStart;
    const int trailingDuration = clipEnd - frame;
    if (leadingDuration < 1 || trailingDuration < 1) {
        return {false, "split would create empty clip"};
    }

    const int newClipId = nextClipId(document->clips);
    const std::int64_t localSplitFrame = leadingDuration;
    jcut::EditorClip trailingClip = *clip;
    trailingClip.id = newClipId;
    trailingClip.persistentId = uniquePersistentClipId(document->clips,
                                                       newClipId);
    trailingClip.startFrame = frame;
    trailingClip.durationFrames = trailingDuration;
    trailingClip.selected = true;

    advanceClipSourceIn(&trailingClip, localSplitFrame);
    if (clip->mediaKind == "image" || clip->mediaKind == "title" ||
        clip->mediaKind == "graphics") {
        trailingClip.sourceDurationFrames = trailingDuration;
    }
    trailingClip.durationSubframeSamples = clip->durationSubframeSamples;
    clip->durationSubframeSamples = 0;

    splitKeyframes(
        &clip->transformKeyframes,
        &trailingClip.transformKeyframes,
        localSplitFrame);
    splitKeyframes(
        &clip->gradingKeyframes,
        &trailingClip.gradingKeyframes,
        localSplitFrame);
    splitKeyframes(
        &clip->opacityKeyframes,
        &trailingClip.opacityKeyframes,
        localSplitFrame);
    splitKeyframes(
        &clip->titleKeyframes,
        &trailingClip.titleKeyframes,
        localSplitFrame);

    const std::vector<jcut::EditorCorrectionPolygon> originalPolygons =
        clip->correctionPolygons;
    clip->correctionPolygons.clear();
    trailingClip.correctionPolygons.clear();
    for (const jcut::EditorCorrectionPolygon& polygon : originalPolygons) {
        const std::int64_t polygonEnd = polygon.endFrame < 0
            ? std::numeric_limits<std::int64_t>::max()
            : polygon.endFrame;
        if (polygon.startFrame < localSplitFrame) {
            jcut::EditorCorrectionPolygon left = polygon;
            if (left.endFrame < 0 || left.endFrame >= localSplitFrame) {
                left.endFrame = localSplitFrame - 1;
            }
            clip->correctionPolygons.push_back(std::move(left));
        }
        if (polygonEnd >= localSplitFrame) {
            jcut::EditorCorrectionPolygon right = polygon;
            right.startFrame = std::max<std::int64_t>(
                0, right.startFrame - localSplitFrame);
            if (right.endFrame >= 0) {
                right.endFrame -= localSplitFrame;
            }
            trailingClip.correctionPolygons.push_back(std::move(right));
        }
    }

    clip->durationFrames = leadingDuration;
    if (clip->mediaKind == "image" || clip->mediaKind == "title" ||
        clip->mediaKind == "graphics") {
        clip->sourceDurationFrames = leadingDuration;
    }
    clip->selected = false;

    for (jcut::EditorRenderSyncMarker& marker : document->renderSyncMarkers) {
        if (marker.clipId == clip->persistentId && marker.frame >= frame) {
            marker.clipId = trailingClip.persistentId;
        }
    }
    document->clips.push_back(std::move(trailingClip));
    sortClipsByTimeline(&document->clips);
    if (trailingClipId) {
        *trailingClipId = newClipId;
    }
    return {true, "clip split"};
}

jcut::CommandResult splitClipAtFrame(jcut::EditorDocumentCore* document,
                                     int clipId,
                                     int frame,
                                     int* trailingClipId = nullptr)
{
    if (!document) {
        return {false, "document unavailable"};
    }
    const jcut::EditorClip* source = findClip(&document->clips, clipId);
    if (!source) {
        return {false, "clip not found"};
    }
    if (jcut::canonicalEditorClipRole(source->clipRole) == "mask_matte") {
        return {false, "mask matte must be split with its source"};
    }

    const std::string sourcePersistentId =
        jcut::trimmedEditorClipId(source->persistentId);
    std::vector<int> ownedMaskIds;
    for (const jcut::EditorClip& candidate : document->clips) {
        if (jcut::canonicalEditorClipRole(candidate.clipRole) != "mask_matte" ||
            jcut::trimmedEditorClipId(candidate.linkedSourceClipId) !=
                sourcePersistentId) {
            continue;
        }
        if (frame <= candidate.startFrame ||
            frame >= candidate.startFrame + candidate.durationFrames) {
            return {false, "owned mask matte does not intersect split frame"};
        }
        ownedMaskIds.push_back(candidate.id);
    }

    int trailingSourceId = 0;
    const jcut::CommandResult splitSource = splitSingleClipAtFrame(
        document, clipId, frame, &trailingSourceId);
    if (!splitSource.applied) {
        return splitSource;
    }
    const jcut::EditorClip* trailingSource =
        findClip(&document->clips, trailingSourceId);
    if (!trailingSource) {
        return {false, "split source result not found"};
    }
    const std::string trailingSourcePersistentId =
        trailingSource->persistentId;

    for (const int maskId : ownedMaskIds) {
        int trailingMaskId = 0;
        const jcut::CommandResult splitMask = splitSingleClipAtFrame(
            document, maskId, frame, &trailingMaskId, true);
        if (!splitMask.applied) {
            return splitMask;
        }
        jcut::EditorClip* trailingMask =
            findClip(&document->clips, trailingMaskId);
        if (!trailingMask) {
            return {false, "split mask result not found"};
        }
        trailingMask->linkedSourceClipId = trailingSourcePersistentId;
        trailingMask->selected = false;
    }
    if (trailingClipId) {
        *trailingClipId = trailingSourceId;
    }
    return {true, "clip aggregate split"};
}

void syncDocumentCounts(jcut::EditorDocumentCore* document)
{
    document->exportRequest.clipCount = document->clips.size();
    document->exportRequest.trackCount = document->tracks.size();
    document->exportRequest.renderSyncMarkerCount = document->renderSyncMarkers.size();
    document->exportRequest.exportRangeCount = document->exportRanges.size();
}

std::vector<jcut::EditorPoint> interpolatedEditorGradingCurve(
    const std::vector<jcut::EditorPoint>& previous,
    const std::vector<jcut::EditorPoint>& next,
    double amount)
{
    const std::vector<jcut::EditorPoint> previousPoints =
        jcut::sanitizeEditorGradingCurve(previous);
    const std::vector<jcut::EditorPoint> nextPoints =
        jcut::sanitizeEditorGradingCurve(next);
    if (previousPoints.size() != nextPoints.size()) {
        return previousPoints;
    }

    std::vector<jcut::EditorPoint> blended;
    blended.reserve(previousPoints.size());
    for (std::size_t index = 0; index < previousPoints.size(); ++index) {
        if (std::abs(previousPoints[index].x - nextPoints[index].x) >
            0.000001) {
            return previousPoints;
        }
        blended.push_back({
            previousPoints[index].x,
            previousPoints[index].y +
                ((nextPoints[index].y - previousPoints[index].y) * amount)});
    }
    return jcut::sanitizeEditorGradingCurve(blended);
}

jcut::EditorGradingKeyframe interpolatedEditorGradingKeyframe(
    const jcut::EditorGradingKeyframe& previous,
    const jcut::EditorGradingKeyframe& next,
    double amount)
{
    jcut::EditorGradingKeyframe result;
    const auto interpolate = [amount](double left, double right) {
        return left + ((right - left) * amount);
    };
    result.brightness = interpolate(previous.brightness, next.brightness);
    result.contrast = interpolate(previous.contrast, next.contrast);
    result.saturation = interpolate(previous.saturation, next.saturation);
    result.shadowsR = interpolate(previous.shadowsR, next.shadowsR);
    result.shadowsG = interpolate(previous.shadowsG, next.shadowsG);
    result.shadowsB = interpolate(previous.shadowsB, next.shadowsB);
    result.midtonesR = interpolate(previous.midtonesR, next.midtonesR);
    result.midtonesG = interpolate(previous.midtonesG, next.midtonesG);
    result.midtonesB = interpolate(previous.midtonesB, next.midtonesB);
    result.highlightsR = interpolate(previous.highlightsR, next.highlightsR);
    result.highlightsG = interpolate(previous.highlightsG, next.highlightsG);
    result.highlightsB = interpolate(previous.highlightsB, next.highlightsB);
    result.curvePointsR = interpolatedEditorGradingCurve(
        previous.curvePointsR, next.curvePointsR, amount);
    result.curvePointsG = interpolatedEditorGradingCurve(
        previous.curvePointsG, next.curvePointsG, amount);
    result.curvePointsB = interpolatedEditorGradingCurve(
        previous.curvePointsB, next.curvePointsB, amount);
    result.curvePointsLuma = interpolatedEditorGradingCurve(
        previous.curvePointsLuma, next.curvePointsLuma, amount);
    result.curveThreePointLock = previous.curveThreePointLock;
    result.curveSmoothingEnabled = previous.curveSmoothingEnabled;
    result.linearInterpolation = next.linearInterpolation;
    return result;
}

double normalizedEditorOpacity(double opacity)
{
    if (std::isnan(opacity)) {
        return 0.0;
    }
    return std::clamp(opacity, 0.0, 1.0);
}

double editorClipOpacityAtLocalFrame(const jcut::EditorClip& clip,
                                     std::int64_t localFrame)
{
    if (clip.opacityKeyframes.empty()) {
        return normalizedEditorOpacity(clip.opacity);
    }
    std::vector<jcut::EditorOpacityKeyframe> keyframes =
        clip.opacityKeyframes;
    std::sort(keyframes.begin(), keyframes.end(),
              [](const jcut::EditorOpacityKeyframe& left,
                 const jcut::EditorOpacityKeyframe& right) {
                  return left.frame < right.frame;
              });
    if (localFrame <= keyframes.front().frame) {
        return normalizedEditorOpacity(keyframes.front().opacity);
    }
    for (std::size_t index = 1; index < keyframes.size(); ++index) {
        const jcut::EditorOpacityKeyframe& previous = keyframes[index - 1];
        const jcut::EditorOpacityKeyframe& current = keyframes[index];
        if (localFrame < current.frame) {
            if (!current.linearInterpolation ||
                current.frame <= previous.frame) {
                return normalizedEditorOpacity(previous.opacity);
            }
            const double amount = static_cast<double>(
                localFrame - previous.frame) /
                static_cast<double>(current.frame - previous.frame);
            return normalizedEditorOpacity(
                previous.opacity +
                    ((current.opacity - previous.opacity) * amount));
        }
        if (localFrame == current.frame) {
            return normalizedEditorOpacity(current.opacity);
        }
    }
    return normalizedEditorOpacity(keyframes.back().opacity);
}

std::int64_t editorClipTimelineStartSamples(const jcut::EditorClip& clip)
{
    return std::max<std::int64_t>(
               0,
               static_cast<std::int64_t>(clip.startFrame) *
                   kEditorSamplesPerFrame) +
        clip.startSubframeSamples;
}

std::int64_t editorClipTimelineDurationSamples(const jcut::EditorClip& clip)
{
    return std::max<std::int64_t>(
        kEditorSamplesPerFrame,
        std::max<std::int64_t>(0, clip.durationFrames) *
                kEditorSamplesPerFrame +
            std::max<std::int64_t>(0, clip.durationSubframeSamples));
}

void normalizeEditorOpacityKeyframes(jcut::EditorClip* clip)
{
    if (!clip) {
        return;
    }
    std::sort(
        clip->opacityKeyframes.begin(),
        clip->opacityKeyframes.end(),
        [](const jcut::EditorOpacityKeyframe& left,
           const jcut::EditorOpacityKeyframe& right) {
            return left.frame < right.frame;
        });

    std::vector<jcut::EditorOpacityKeyframe> normalized;
    normalized.reserve(clip->opacityKeyframes.size());
    const std::int64_t maxFrame = std::max(0, clip->durationFrames - 1);
    for (jcut::EditorOpacityKeyframe keyframe : clip->opacityKeyframes) {
        keyframe.frame = std::clamp<std::int64_t>(
            keyframe.frame, 0, maxFrame);
        keyframe.opacity = normalizedEditorOpacity(keyframe.opacity);
        if (!normalized.empty() &&
            normalized.back().frame == keyframe.frame) {
            normalized.back() = std::move(keyframe);
        } else {
            normalized.push_back(std::move(keyframe));
        }
    }

    if (editorClipHasVisuals(*clip)) {
        if (normalized.empty()) {
            normalized.push_back(
                {0, normalizedEditorOpacity(clip->opacity), true});
        } else if (normalized.front().frame > 0) {
            jcut::EditorOpacityKeyframe first = normalized.front();
            first.frame = 0;
            normalized.insert(normalized.begin(), std::move(first));
        } else {
            normalized.front().frame = 0;
        }
    }

    clip->opacityKeyframes = std::move(normalized);
    if (!clip->opacityKeyframes.empty()) {
        clip->opacity = clip->opacityKeyframes.front().opacity;
    }
}

void applyEditorVisualCrossfade(jcut::EditorClip* clip,
                                bool fadeIn,
                                std::int64_t fadeFrames)
{
    if (!clip || !editorClipHasVisuals(*clip) ||
        clip->durationFrames <= 1 || fadeFrames <= 0) {
        return;
    }

    const std::int64_t localStartFrame = fadeIn
        ? 0
        : std::max<std::int64_t>(0, clip->durationFrames - fadeFrames);
    const std::int64_t localEndFrame = fadeIn
        ? std::min<std::int64_t>(clip->durationFrames - 1, fadeFrames)
        : clip->durationFrames - 1;
    if (localStartFrame >= localEndFrame) {
        return;
    }

    const double startState =
        editorClipOpacityAtLocalFrame(*clip, localStartFrame);
    const double endState =
        editorClipOpacityAtLocalFrame(*clip, localEndFrame);
    upsertKeyframe(
        &clip->opacityKeyframes,
        jcut::EditorOpacityKeyframe{
            localStartFrame,
            fadeIn ? 0.0 : normalizedEditorOpacity(startState),
            true});
    upsertKeyframe(
        &clip->opacityKeyframes,
        jcut::EditorOpacityKeyframe{
            localEndFrame,
            fadeIn ? normalizedEditorOpacity(endState) : 0.0,
            true});
    normalizeEditorOpacityKeyframes(clip);
}

jcut::CommandResult applyCrossfadeToEditorTrack(
    jcut::EditorDocumentCore* document,
    const jcut::CrossfadeTrackCommand& command)
{
    if (!document) {
        return {false, "document unavailable"};
    }
    const jcut::EditorTrack* track = findTrack(&document->tracks, command.trackId);
    if (!track) {
        return {false, "track not found"};
    }
    if (jcut::isGeneratedEditorChildTrack(*track)) {
        return {false, "generated child track cannot be crossfaded independently"};
    }
    if (!std::isfinite(command.seconds) || command.seconds <= 0.0) {
        return {false, "positive crossfade duration required"};
    }
    const long double requestedFadeSamples =
        static_cast<long double>(command.seconds) *
        static_cast<long double>(kEditorAudioSampleRate);
    if (requestedFadeSamples >
        static_cast<long double>(std::numeric_limits<int>::max())) {
        return {false, "crossfade duration is too large"};
    }

    std::vector<std::size_t> clipIndices;
    clipIndices.reserve(document->clips.size());
    for (std::size_t index = 0; index < document->clips.size(); ++index) {
        if (document->clips[index].trackId == command.trackId) {
            clipIndices.push_back(index);
        }
    }
    if (clipIndices.size() < 2) {
        return {false, "track needs at least two clips for a crossfade"};
    }
    for (const std::size_t index : clipIndices) {
        if (document->clips[index].locked) {
            return {false, "all clips on the track must be unlocked"};
        }
    }

    std::sort(
        clipIndices.begin(), clipIndices.end(),
        [&](std::size_t leftIndex, std::size_t rightIndex) {
            const jcut::EditorClip& left = document->clips[leftIndex];
            const jcut::EditorClip& right = document->clips[rightIndex];
            const std::int64_t leftStart =
                editorClipTimelineStartSamples(left);
            const std::int64_t rightStart =
                editorClipTimelineStartSamples(right);
            if (leftStart != rightStart) {
                return leftStart < rightStart;
            }
            return left.label < right.label;
        });

    const int fadeSamples = std::max(
        1,
        static_cast<int>(std::llround(requestedFadeSamples)));
    const std::int64_t fadeFrames = std::max<std::int64_t>(
        1,
        std::llround(command.seconds * kDefaultTimelineFps));
    bool changed = false;

    if (command.moveClips) {
        // Preflight the complete cascade before mutating the document. A
        // neutral startFrame is narrower than Qt's int64 timeline field, so a
        // very late, long clip must fail atomically instead of wrapping.
        std::vector<std::int64_t> targetStarts;
        targetStarts.reserve(clipIndices.size());
        for (const std::size_t clipIndex : clipIndices) {
            targetStarts.push_back(
                editorClipTimelineStartSamples(document->clips[clipIndex]));
        }
        for (std::size_t index = 0; index + 1 < clipIndices.size(); ++index) {
            const jcut::EditorClip& left =
                document->clips[clipIndices[index]];
            const std::int64_t leftEnd = targetStarts[index] +
                editorClipTimelineDurationSamples(left);
            const std::int64_t targetRightStart =
                std::max<std::int64_t>(0, leftEnd - fadeSamples);
            if (targetRightStart / kEditorSamplesPerFrame >
                std::numeric_limits<int>::max()) {
                return {false, "crossfade move is outside the neutral timeline range"};
            }
            targetStarts[index + 1] = targetRightStart;
        }
        for (std::size_t index = 0; index + 1 < clipIndices.size(); ++index) {
            jcut::EditorClip& right = document->clips[clipIndices[index + 1]];
            const std::int64_t targetRightStart = targetStarts[index + 1];
            if (editorClipTimelineStartSamples(right) != targetRightStart) {
                right.startFrame = static_cast<int>(
                    targetRightStart / kEditorSamplesPerFrame);
                right.startSubframeSamples =
                    targetRightStart % kEditorSamplesPerFrame;
                changed = true;
            }
        }
    }

    for (std::size_t index = 0; index + 1 < clipIndices.size(); ++index) {
        jcut::EditorClip& left = document->clips[clipIndices[index]];
        jcut::EditorClip& right = document->clips[clipIndices[index + 1]];
        if (left.hasAudio || left.mediaKind == "audio") {
            if (left.fadeSamples != fadeSamples) {
                left.fadeSamples = fadeSamples;
                changed = true;
            }
        }
        if (right.hasAudio || right.mediaKind == "audio") {
            if (right.fadeSamples != fadeSamples) {
                right.fadeSamples = fadeSamples;
                changed = true;
            }
        }

        const bool leftHasVisuals = editorClipHasVisuals(left);
        const bool rightHasVisuals = editorClipHasVisuals(right);
        applyEditorVisualCrossfade(&left, false, fadeFrames);
        applyEditorVisualCrossfade(&right, true, fadeFrames);
        if (leftHasVisuals || rightHasVisuals) {
            changed = true;
        }
    }

    if (!changed) {
        return {false, "crossfade did not change the track"};
    }
    normalizeMaskMatteParentCaches(document);
    sortClipsByTimeline(&document->clips);
    return {true, "track crossfade applied"};
}

bool recordsUndoHistory(const jcut::EditorCommand& command)
{
    return std::visit(
        [](const auto& typedCommand) {
            using T = std::decay_t<decltype(typedCommand)>;
            return std::is_same_v<T, jcut::SetProjectNameCommand> ||
                   std::is_same_v<T, jcut::ImportMediaCommand> ||
                   std::is_same_v<T, jcut::RemoveMediaCommand> ||
                   std::is_same_v<T, jcut::AddTrackCommand> ||
                   std::is_same_v<T, jcut::DeleteTrackCommand> ||
                   std::is_same_v<T, jcut::ReorderTrackCommand> ||
                   std::is_same_v<T, jcut::CrossfadeTrackCommand> ||
                   std::is_same_v<T, jcut::CutSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::PasteClipsCommand> ||
                   std::is_same_v<T, jcut::DuplicateSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::InsertClipFromMediaCommand> ||
                   std::is_same_v<T, jcut::AddClipCommand> ||
                   std::is_same_v<T, jcut::CreateTitleClipCommand> ||
                   std::is_same_v<T, jcut::DeleteClipCommand> ||
                   std::is_same_v<T, jcut::DeleteSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::SplitClipCommand> ||
                   std::is_same_v<T, jcut::SplitSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::TrimClipStartCommand> ||
                   std::is_same_v<T, jcut::TrimClipEndCommand> ||
                   std::is_same_v<T, jcut::SetClipLabelCommand> ||
                   std::is_same_v<T, jcut::SetClipLockedCommand> ||
                   std::is_same_v<T, jcut::SetClipPlaybackRateCommand> ||
                   std::is_same_v<T, jcut::MoveClipCommand> ||
                   std::is_same_v<T, jcut::MoveSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::ResizeClipCommand> ||
                   std::is_same_v<T, jcut::NudgeSelectedClipCommand> ||
                   std::is_same_v<T, jcut::SetClipGradingCommand> ||
                   std::is_same_v<T, jcut::ResetClipGradingCommand> ||
                   std::is_same_v<T, jcut::UpsertGradingKeyframeCommand> ||
                   std::is_same_v<T, jcut::SetClipOpacityCommand> ||
                   std::is_same_v<T, jcut::UpsertOpacityKeyframeCommand> ||
                   std::is_same_v<T, jcut::RemoveClipKeyframeCommand> ||
                   std::is_same_v<T, jcut::SetClipTransformCommand> ||
                   std::is_same_v<T, jcut::UpsertTransformKeyframeCommand> ||
                   std::is_same_v<T, jcut::SetClipMaskEffectCommand> ||
                   std::is_same_v<T, jcut::SetClipMaskCommand> ||
                   std::is_same_v<T, jcut::SetClipTranscriptOverlayCommand> ||
                   std::is_same_v<T, jcut::SetClipTranscriptActiveCutCommand> ||
                   std::is_same_v<T, jcut::UpsertTitleKeyframeCommand> ||
                   std::is_same_v<T, jcut::RemoveTitleKeyframeCommand> ||
                   std::is_same_v<T, jcut::ClearCorrectionPolygonsCommand> ||
                   std::is_same_v<T, jcut::SetCorrectionsEnabledCommand> ||
                   std::is_same_v<T, jcut::SetClipAudioCommand> ||
                   std::is_same_v<T, jcut::SetTrackPropertiesCommand> ||
                   std::is_same_v<T, jcut::SetTrackStateCommand> ||
                   std::is_same_v<T, jcut::AddRenderSyncMarkerCommand> ||
                   std::is_same_v<T, jcut::RemoveRenderSyncMarkerCommand> ||
                   std::is_same_v<T, jcut::ClearRenderSyncMarkersCommand> ||
                   std::is_same_v<T, jcut::SetExportRangeCommand> ||
                   std::is_same_v<T, jcut::SetExportSizeCommand> ||
                   std::is_same_v<T, jcut::SetExportFpsCommand> ||
                   std::is_same_v<T, jcut::SetExportOutputPathCommand> ||
                   std::is_same_v<T, jcut::SetExportFormatCommand> ||
                   std::is_same_v<T, jcut::SetExportImageSequenceFormatCommand> ||
                   std::is_same_v<T, jcut::SetExportUseProxyMediaCommand> ||
                   std::is_same_v<T, jcut::SetExportImageSequenceCommand>;
        },
        command);
}

bool reconcilesGeneratedTrackTopology(const jcut::EditorCommand& command)
{
    return std::visit(
        [](const auto& typedCommand) {
            using T = std::decay_t<decltype(typedCommand)>;
            return std::is_same_v<T, jcut::UndoCommand> ||
                std::is_same_v<T, jcut::RedoCommand> ||
                std::is_same_v<T, jcut::DeleteTrackCommand> ||
                std::is_same_v<T, jcut::ReorderTrackCommand> ||
                std::is_same_v<T, jcut::CutSelectedClipsCommand> ||
                std::is_same_v<T, jcut::PasteClipsCommand> ||
                std::is_same_v<T, jcut::DuplicateSelectedClipsCommand> ||
                std::is_same_v<T, jcut::DeleteClipCommand> ||
                std::is_same_v<T, jcut::DeleteSelectedClipsCommand> ||
                std::is_same_v<T, jcut::SplitClipCommand> ||
                std::is_same_v<T, jcut::SplitSelectedClipsCommand> ||
                std::is_same_v<T, jcut::MoveClipCommand> ||
                std::is_same_v<T, jcut::MoveSelectedClipsCommand> ||
                std::is_same_v<T, jcut::SetClipLabelCommand>;
        },
        command);
}

} // namespace

namespace jcut {

void reconcileEditorGeneratedChildTracks(EditorDocumentCore* document)
{
    if (!document) {
        return;
    }

    struct ChildBinding {
        std::size_t childIndex = 0;
        std::size_t parentIndex = 0;
        std::string childId;
        std::string parentId;
        int laneTrackId = 0;
    };

    std::unordered_map<std::string, std::size_t> clipIndexById;
    clipIndexById.reserve(document->clips.size());
    for (std::size_t index = 0; index < document->clips.size(); ++index) {
        const std::string clipId =
            trimmedEditorClipId(document->clips[index].persistentId);
        if (!clipId.empty()) {
            clipIndexById.emplace(clipId, index);
        }
    }

    std::unordered_map<int, std::size_t> trackIndexById;
    trackIndexById.reserve(document->tracks.size());
    int nextGeneratedTrackId = 1;
    for (std::size_t index = 0; index < document->tracks.size(); ++index) {
        trackIndexById.emplace(document->tracks[index].id, index);
        nextGeneratedTrackId = std::max(
            nextGeneratedTrackId, document->tracks[index].id + 1);
    }

    std::vector<ChildBinding> bindings;
    bindings.reserve(document->clips.size());
    std::unordered_set<std::string> boundChildIds;
    for (std::size_t childIndex = 0;
         childIndex < document->clips.size(); ++childIndex) {
        EditorClip& child = document->clips[childIndex];
        if (canonicalEditorClipRole(child.clipRole) != "mask_matte") {
            continue;
        }
        const std::string childId = trimmedEditorClipId(child.persistentId);
        const std::string parentId =
            trimmedEditorClipId(child.linkedSourceClipId);
        const auto parentIt = clipIndexById.find(parentId);
        if (childId.empty() || parentId.empty() ||
            parentIt == clipIndexById.end()) {
            continue;
        }
        const EditorClip& parent = document->clips[parentIt->second];
        if (canonicalEditorClipRole(parent.clipRole) == "mask_matte" ||
            trackIndexById.find(parent.trackId) == trackIndexById.end() ||
            !boundChildIds.insert(childId).second) {
            continue;
        }
        child.linkedSourceClipId = parentId;
        bindings.push_back(
            {childIndex, parentIt->second, childId, parentId, 0});
    }

    std::unordered_map<int, std::vector<std::size_t>> occupantsByTrackId;
    occupantsByTrackId.reserve(document->tracks.size());
    for (std::size_t clipIndex = 0;
         clipIndex < document->clips.size(); ++clipIndex) {
        occupantsByTrackId[document->clips[clipIndex].trackId].push_back(
            clipIndex);
    }

    std::unordered_set<int> claimedTrackIds;
    claimedTrackIds.reserve(bindings.size());
    const auto generatedTrackMatches = [&](const EditorTrack& track,
                                           const ChildBinding& binding) {
        return isGeneratedEditorChildTrack(track) &&
            trimmedEditorClipId(track.childClipId) == binding.childId;
    };
    for (ChildBinding& binding : bindings) {
        const EditorClip& child = document->clips[binding.childIndex];
        const EditorClip& parent = document->clips[binding.parentIndex];
        int laneTrackId = 0;

        const auto currentTrackIt = trackIndexById.find(child.trackId);
        if (currentTrackIt != trackIndexById.end()) {
            const EditorTrack& currentTrack =
                document->tracks[currentTrackIt->second];
            if (generatedTrackMatches(currentTrack, binding) &&
                claimedTrackIds.find(currentTrack.id) ==
                    claimedTrackIds.end()) {
                laneTrackId = currentTrack.id;
            }
        }
        if (laneTrackId == 0) {
            for (const EditorTrack& track : document->tracks) {
                if (generatedTrackMatches(track, binding) &&
                    claimedTrackIds.find(track.id) ==
                        claimedTrackIds.end()) {
                    laneTrackId = track.id;
                    break;
                }
            }
        }
        if (laneTrackId == 0 && currentTrackIt != trackIndexById.end()) {
            const EditorTrack& currentTrack =
                document->tracks[currentTrackIt->second];
            const auto occupants = occupantsByTrackId.find(currentTrack.id);
            const bool dedicatedExistingTrack =
                !isGeneratedEditorChildTrack(currentTrack) &&
                currentTrack.id != parent.trackId &&
                occupants != occupantsByTrackId.end() &&
                occupants->second.size() == 1 &&
                occupants->second.front() == binding.childIndex &&
                claimedTrackIds.find(currentTrack.id) ==
                    claimedTrackIds.end();
            if (dedicatedExistingTrack) {
                laneTrackId = currentTrack.id;
            }
        }
        if (laneTrackId == 0) {
            EditorTrack lane;
            lane.id = nextGeneratedTrackId++;
            lane.height = 44;
            lane.audioEnabled = false;
            lane.audioWaveformVisible = false;
            document->tracks.push_back(std::move(lane));
            laneTrackId = document->tracks.back().id;
            trackIndexById.emplace(
                laneTrackId, document->tracks.size() - 1);
        }
        binding.laneTrackId = laneTrackId;
        claimedTrackIds.insert(laneTrackId);
    }

    // A valid child binding owns its lane exclusively. Recover any malformed
    // ordinary occupant onto a new neutral base track; another valid matte is
    // assigned to its own lane below.
    for (const ChildBinding& binding : bindings) {
        const auto laneIt = trackIndexById.find(binding.laneTrackId);
        if (laneIt == trackIndexById.end()) {
            continue;
        }
        const EditorTrack& lane = document->tracks[laneIt->second];
        std::vector<std::size_t> foreignOccupants;
        const auto occupants = occupantsByTrackId.find(lane.id);
        if (occupants != occupantsByTrackId.end()) {
            for (const std::size_t clipIndex : occupants->second) {
                const std::string occupantId = trimmedEditorClipId(
                    document->clips[clipIndex].persistentId);
                if (occupantId == binding.childId ||
                    boundChildIds.find(occupantId) != boundChildIds.end()) {
                    continue;
                }
                foreignOccupants.push_back(clipIndex);
            }
        }
        if (!foreignOccupants.empty()) {
            EditorTrack recovered;
            recovered.id = nextGeneratedTrackId++;
            const EditorClip& firstOccupant =
                document->clips[foreignOccupants.front()];
            recovered.label = firstOccupant.label.empty()
                ? std::string("Track ") +
                    std::to_string(document->tracks.size() + 1)
                : firstOccupant.label;
            recovered.height = std::clamp(
                lane.height, kEditorTrackMinHeight, kEditorTrackMaxHeight);
            document->tracks.push_back(std::move(recovered));
            const int recoveredTrackId = document->tracks.back().id;
            trackIndexById.emplace(
                recoveredTrackId, document->tracks.size() - 1);
            for (const std::size_t clipIndex : foreignOccupants) {
                document->clips[clipIndex].trackId = recoveredTrackId;
            }
        }
    }

    for (const ChildBinding& binding : bindings) {
        EditorClip& child = document->clips[binding.childIndex];
        child.trackId = binding.laneTrackId;
        const auto laneIt = trackIndexById.find(binding.laneTrackId);
        if (laneIt == trackIndexById.end()) {
            continue;
        }
        EditorTrack& lane = document->tracks[laneIt->second];
        lane.generatedChildTrack = true;
        lane.parentClipId = binding.parentId;
        lane.childClipId = binding.childId;
        lane.label = std::string("↳ ") +
            (child.label.empty() ? std::string("Mask Matte") : child.label);
        lane.height = std::clamp(lane.height, kEditorTrackMinHeight, 56);
        lane.audioEnabled = false;
        lane.audioWaveformVisible = false;
    }

    std::unordered_set<int> removedTrackIds;
    for (EditorTrack& track : document->tracks) {
        if (!isGeneratedEditorChildTrack(track) ||
            claimedTrackIds.find(track.id) != claimedTrackIds.end()) {
            continue;
        }

        const std::string childId = trimmedEditorClipId(track.childClipId);
        const auto childIt = clipIndexById.find(childId);
        bool opaqueFutureBinding = false;
        if (childIt != clipIndexById.end()) {
            const std::string role = canonicalEditorClipRole(
                document->clips[childIt->second].clipRole);
            opaqueFutureBinding = role != "media" && role != "mask_matte" &&
                role != "effect_synth" && role != "speaker_title";
        }
        if (opaqueFutureBinding) {
            continue;
        }

        std::vector<std::size_t> occupants;
        for (std::size_t clipIndex = 0;
             clipIndex < document->clips.size(); ++clipIndex) {
            if (document->clips[clipIndex].trackId == track.id) {
                occupants.push_back(clipIndex);
            }
        }
        if (occupants.empty()) {
            removedTrackIds.insert(track.id);
            continue;
        }

        // Preserve the row and all playback state when deletion would discard
        // a malformed/future occupant. Only the stale derived relationship is
        // cleared.
        track.generatedChildTrack = false;
        track.parentClipId.clear();
        track.childClipId.clear();
        const EditorClip& firstOccupant = document->clips[occupants.front()];
        if (!firstOccupant.label.empty()) {
            track.label = firstOccupant.label;
        }
        track.height = std::clamp(
            track.height, kEditorTrackMinHeight, kEditorTrackMaxHeight);
    }
    if (!removedTrackIds.empty()) {
        document->tracks.erase(
            std::remove_if(
                document->tracks.begin(), document->tracks.end(),
                [&](const EditorTrack& track) {
                    return removedTrackIds.find(track.id) !=
                        removedTrackIds.end();
                }),
            document->tracks.end());
    }

    std::unordered_map<int, std::vector<const ChildBinding*>> childrenBySource;
    for (const ChildBinding& binding : bindings) {
        childrenBySource[document->clips[binding.parentIndex].trackId]
            .push_back(&binding);
    }
    for (auto& [sourceTrackId, children] : childrenBySource) {
        (void)sourceTrackId;
        std::sort(
            children.begin(), children.end(),
            [&](const ChildBinding* left, const ChildBinding* right) {
                const EditorClip& leftChild =
                    document->clips[left->childIndex];
                const EditorClip& rightChild =
                    document->clips[right->childIndex];
                if (leftChild.label != rightChild.label) {
                    return leftChild.label < rightChild.label;
                }
                return left->childId < right->childId;
            });
    }

    std::unordered_map<int, std::size_t> currentTrackIndexById;
    currentTrackIndexById.reserve(document->tracks.size());
    for (std::size_t index = 0; index < document->tracks.size(); ++index) {
        currentTrackIndexById.emplace(document->tracks[index].id, index);
    }
    std::vector<EditorTrack> orderedTracks;
    orderedTracks.reserve(document->tracks.size());
    std::unordered_set<int> placedTrackIds;
    placedTrackIds.reserve(document->tracks.size());
    for (const EditorTrack& track : document->tracks) {
        if (isGeneratedEditorChildTrack(track)) {
            continue;
        }
        orderedTracks.push_back(track);
        placedTrackIds.insert(track.id);
        const auto childrenIt = childrenBySource.find(track.id);
        if (childrenIt == childrenBySource.end()) {
            continue;
        }
        for (const ChildBinding* child : childrenIt->second) {
            const auto childTrackIt =
                currentTrackIndexById.find(child->laneTrackId);
            if (childTrackIt == currentTrackIndexById.end() ||
                !placedTrackIds.insert(child->laneTrackId).second) {
                continue;
            }
            orderedTracks.push_back(
                document->tracks[childTrackIt->second]);
        }
    }
    // Preserve opaque future bindings and any malformed unplaced lane rather
    // than guessing ownership from row position.
    for (const EditorTrack& track : document->tracks) {
        if (placedTrackIds.insert(track.id).second) {
            orderedTracks.push_back(track);
        }
    }
    if (orderedTracks.empty()) {
        EditorTrack fallback;
        fallback.id = nextGeneratedTrackId++;
        fallback.label = "Track 1";
        fallback.selected = true;
        orderedTracks.push_back(std::move(fallback));
    }
    document->tracks = std::move(orderedTracks);

    if (!document->tracks.empty() &&
        std::none_of(document->tracks.begin(), document->tracks.end(),
                     [](const EditorTrack& track) {
                         return track.selected;
                     })) {
        document->tracks.front().selected = true;
    }
}

int firstNonConflictingTrackIndex(const EditorDocumentCore& document,
                                  int preferredTrackIndex,
                                  const std::string& mediaKind,
                                  int startFrame,
                                  int durationFrames)
{
    const auto sameLaneKind = [&](const std::string& otherKind) {
        return (mediaKindHasVisuals(mediaKind) &&
                mediaKindHasVisuals(otherKind)) ||
            (mediaKind == "audio" && otherKind == "audio");
    };
    const std::int64_t proposedStart = std::max(0, startFrame);
    const std::int64_t proposedEnd = proposedStart + std::max(1, durationFrames);
    const auto isAvailable = [&](int trackIndex) {
        if (trackIndex < 0 ||
            trackIndex >= static_cast<int>(document.tracks.size())) {
            return false;
        }
        const EditorTrack& track = document.tracks[
            static_cast<std::size_t>(trackIndex)];
        if (isGeneratedEditorChildTrack(track)) {
            return false;
        }
        const int trackId = track.id;
        return std::none_of(
            document.clips.begin(), document.clips.end(),
            [&](const EditorClip& clip) {
                if (clip.trackId != trackId || !sameLaneKind(clip.mediaKind)) {
                    return false;
                }
                const std::int64_t clipStart = clip.startFrame;
                const std::int64_t clipEnd = clipStart + clip.durationFrames;
                return proposedEnd > clipStart && proposedStart < clipEnd;
            });
    };

    if (isAvailable(preferredTrackIndex)) {
        return preferredTrackIndex;
    }
    for (int trackIndex = 0;
         trackIndex < static_cast<int>(document.tracks.size());
         ++trackIndex) {
        if (isAvailable(trackIndex)) {
            return trackIndex;
        }
    }
    return -1;
}

EditorGradingKeyframe evaluateEditorClipGradingAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame)
{
    const std::int64_t frame = std::clamp<std::int64_t>(
        localFrame, 0, std::max(0, clip.durationFrames - 1));
    const double opacity = editorClipOpacityAtLocalFrame(clip, frame);
    const auto finish = [frame, opacity](EditorGradingKeyframe keyframe) {
        keyframe.frame = frame;
        keyframe.opacity = opacity;
        return keyframe;
    };

    if (clip.gradingKeyframes.empty()) {
        EditorGradingKeyframe base;
        base.brightness = clip.brightness;
        base.contrast = clip.contrast;
        base.saturation = clip.saturation;
        return finish(std::move(base));
    }

    std::vector<EditorGradingKeyframe> keyframes = clip.gradingKeyframes;
    std::sort(keyframes.begin(), keyframes.end(),
              [](const EditorGradingKeyframe& left,
                 const EditorGradingKeyframe& right) {
                  return left.frame < right.frame;
              });
    for (EditorGradingKeyframe& keyframe : keyframes) {
        keyframe.curvePointsR =
            sanitizeEditorGradingCurve(keyframe.curvePointsR);
        keyframe.curvePointsG =
            sanitizeEditorGradingCurve(keyframe.curvePointsG);
        keyframe.curvePointsB =
            sanitizeEditorGradingCurve(keyframe.curvePointsB);
        keyframe.curvePointsLuma =
            sanitizeEditorGradingCurve(keyframe.curvePointsLuma);
    }

    if (frame <= keyframes.front().frame) {
        return finish(keyframes.front());
    }
    for (std::size_t index = 1; index < keyframes.size(); ++index) {
        const EditorGradingKeyframe& previous = keyframes[index - 1];
        const EditorGradingKeyframe& current = keyframes[index];
        if (frame < current.frame) {
            if (!current.linearInterpolation ||
                current.frame <= previous.frame) {
                return finish(previous);
            }
            const double amount = static_cast<double>(frame - previous.frame) /
                static_cast<double>(current.frame - previous.frame);
            return finish(interpolatedEditorGradingKeyframe(
                previous, current, amount));
        }
        if (frame == current.frame) {
            return finish(current);
        }
    }
    return finish(keyframes.back());
}

EditorRuntime EditorRuntime::createDemo()
{
    EditorRuntime runtime;
    runtime.m_document.projectName = "Demo Session";
    runtime.m_document.mediaItems = {
        {"media-1", "Interview_A_CamA.mov", "video"},
        {"media-2", "Interview_A_CamB.mov", "video"},
        {"media-3", "Broll_Street_01.mp4", "video"},
        {"media-4", "Voiceover_Main.wav", "audio"},
        {"media-5", "LowerThird_Package", "graphics"},
    };
    runtime.m_document.tracks = {
        {1, "Video A", true},
        {2, "Video B", false},
        {3, "Graphics", false},
        {4, "Audio Mix", false},
    };
    runtime.m_document.clips = {
        {1, 1, "Interview A", 0, 420, true},
        {2, 2, "Interview B", 36, 396, false},
        {3, 3, "Lower Third", 120, 96, false},
        {4, 4, "VO Main", 0, 420, false},
    };
    runtime.m_document.transport.currentFrame = 1842;
    runtime.m_document.exportRequest.outputFormat = "mp4";
    runtime.m_document.exportRequest.outputSize = {1080, 1920};
    runtime.m_document.exportRequest.outputFps = 30.0;
    runtime.m_document.exportRequest.outputMode = render::RenderOutputMode::EncodedFile;
    ensurePersistentClipIds(&runtime.m_document);
    syncDocumentCounts(&runtime.m_document);
    return runtime;
}

EditorRuntime EditorRuntime::fromDocument(EditorDocumentCore document)
{
    EditorRuntime runtime;
    runtime.m_document = std::move(document);
    ensurePersistentClipIds(&runtime.m_document);
    normalizeClipRelationships(&runtime.m_document);
    reconcileEditorGeneratedChildTracks(&runtime.m_document);
    normalizeMaskMatteParentCaches(&runtime.m_document);
    normalizeRenderSyncMarkers(&runtime.m_document);
    syncDocumentCounts(&runtime.m_document);
    return runtime;
}

EditorDocumentCore EditorRuntime::snapshot() const
{
    return m_document;
}

bool EditorRuntime::copySelectedClipsToClipboard()
{
    m_clipClipboard.clear();
    m_renderSyncMarkerClipboard.clear();
    m_clipboardBaseTrackId = 0;

    const ClipPersistentIdSet selectedIds =
        selectedClipPersistentIds(m_document);
    if (selectedIds.empty()) {
        return false;
    }
    // Structural copy expands in both directions. A parent brings every owned
    // matte, while copying a matte brings its parent and sibling mattes so the
    // clipboard can never paste a child back onto the original source.
    const ClipPersistentIdSet copiedIds = clipOwnershipClosure(
        m_document, selectedIds, true);

    std::size_t minimumTrackIndex = m_document.tracks.size();
    for (const EditorClip& clip : m_document.clips) {
        if (!persistentClipIdInSet(copiedIds, clip.persistentId)) {
            continue;
        }
        const std::size_t trackIndex =
            trackIndexForId(m_document.tracks, clip.trackId);
        if (trackIndex == m_document.tracks.size()) {
            return false;
        }
        minimumTrackIndex = std::min(minimumTrackIndex, trackIndex);
    }
    if (minimumTrackIndex == m_document.tracks.size()) {
        return false;
    }

    m_clipboardBaseTrackId = m_document.tracks[minimumTrackIndex].id;
    for (const EditorClip& clip : m_document.clips) {
        if (!persistentClipIdInSet(copiedIds, clip.persistentId)) {
            continue;
        }
        const std::size_t trackIndex =
            trackIndexForId(m_document.tracks, clip.trackId);
        m_clipClipboard.push_back({clip, trackIndex - minimumTrackIndex});
    }
    std::sort(m_clipClipboard.begin(), m_clipClipboard.end(),
              [](const ClipboardClip& left, const ClipboardClip& right) {
                  if (left.clip.startFrame != right.clip.startFrame) {
                      return left.clip.startFrame < right.clip.startFrame;
                  }
                  if (left.trackOffset != right.trackOffset) {
                      return left.trackOffset < right.trackOffset;
                  }
                  return left.clip.id < right.clip.id;
              });

    for (const EditorRenderSyncMarker& marker : m_document.renderSyncMarkers) {
        if (persistentClipIdInSet(copiedIds, marker.clipId)) {
            m_renderSyncMarkerClipboard.push_back(marker);
        }
    }
    return !m_clipClipboard.empty();
}

CommandResult EditorRuntime::pasteClipboardAt(int targetFrame, int targetTrackId)
{
    if (m_clipClipboard.empty() || m_document.tracks.empty()) {
        return {false, "clip clipboard is empty"};
    }

    if (targetTrackId == 0) {
        const auto selectedTrack = std::find_if(
            m_document.tracks.begin(), m_document.tracks.end(),
            [](const EditorTrack& track) { return track.selected; });
        if (selectedTrack != m_document.tracks.end()) {
            targetTrackId = selectedTrack->id;
        } else if (hasTrackId(m_document.tracks, m_clipboardBaseTrackId)) {
            targetTrackId = m_clipboardBaseTrackId;
        } else {
            targetTrackId = m_document.tracks.front().id;
        }
    }

    const std::size_t targetTrackIndex =
        trackIndexForId(m_document.tracks, targetTrackId);
    if (targetTrackIndex == m_document.tracks.size()) {
        return {false, "target track not found"};
    }

    std::size_t maximumTrackOffset = 0;
    for (const ClipboardClip& entry : m_clipClipboard) {
        maximumTrackOffset = std::max(maximumTrackOffset, entry.trackOffset);
    }
    while (m_document.tracks.size() <=
           targetTrackIndex + maximumTrackOffset) {
        const int trackId = nextTrackId(m_document.tracks);
        m_document.tracks.push_back({
            trackId,
            std::string("Track ") + std::to_string(m_document.tracks.size() + 1),
            false
        });
    }

    int anchorFrame = m_clipClipboard.front().clip.startFrame;
    for (const ClipboardClip& entry : m_clipClipboard) {
        anchorFrame = std::min(anchorFrame, entry.clip.startFrame);
    }

    selectSingle(&m_document.clips, [](const EditorClip&) { return false; });
    std::unordered_map<std::string, std::string> pastedPersistentIds;
    const std::size_t firstPastedClipIndex = m_document.clips.size();
    for (const ClipboardClip& entry : m_clipClipboard) {
        EditorClip clip = entry.clip;
        const std::string sourcePersistentId =
            trimmedEditorClipId(clip.persistentId);
        clip.id = nextClipId(m_document.clips);
        clip.persistentId = uniquePersistentClipId(m_document.clips, clip.id);
        clip.trackId =
            m_document.tracks[targetTrackIndex + entry.trackOffset].id;
        const std::int64_t shiftedStart =
            static_cast<std::int64_t>(std::max(0, targetFrame)) +
            (static_cast<std::int64_t>(entry.clip.startFrame) - anchorFrame);
        clip.startFrame = static_cast<int>(std::clamp<std::int64_t>(
            shiftedStart, 0, std::numeric_limits<int>::max()));
        clip.selected = true;
        pastedPersistentIds[sourcePersistentId] = clip.persistentId;
        m_document.clips.push_back(std::move(clip));
    }

    for (std::size_t index = firstPastedClipIndex;
         index < m_document.clips.size();
         ++index) {
        EditorClip& clip = m_document.clips[index];
        if (canonicalEditorClipRole(clip.clipRole) != "mask_matte") {
            continue;
        }
        const auto remappedSource = pastedPersistentIds.find(
            trimmedEditorClipId(clip.linkedSourceClipId));
        if (remappedSource != pastedPersistentIds.end()) {
            clip.linkedSourceClipId = remappedSource->second;
        }
    }

    for (EditorRenderSyncMarker marker : m_renderSyncMarkerClipboard) {
        const auto pastedId = pastedPersistentIds.find(
            trimmedEditorClipId(marker.clipId));
        if (pastedId == pastedPersistentIds.end()) {
            continue;
        }
        marker.clipId = pastedId->second;
        marker.frame = std::max<std::int64_t>(
            0,
            static_cast<std::int64_t>(std::max(0, targetFrame)) +
                (marker.frame - anchorFrame));
        m_document.renderSyncMarkers.push_back(std::move(marker));
    }
    std::sort(m_document.renderSyncMarkers.begin(),
              m_document.renderSyncMarkers.end(),
              [](const EditorRenderSyncMarker& left,
                 const EditorRenderSyncMarker& right) {
                  if (left.frame != right.frame) {
                      return left.frame < right.frame;
                  }
                  if (left.clipId != right.clipId) {
                      return left.clipId < right.clipId;
                  }
                  return left.skipFrame < right.skipFrame;
              });
    return {true, "clips pasted"};
}

bool EditorRuntime::canUndo() const
{
    return !m_undoStack.empty();
}

bool EditorRuntime::canRedo() const
{
    return !m_redoStack.empty();
}

std::size_t EditorRuntime::undoDepth() const
{
    return m_undoStack.size();
}

std::size_t EditorRuntime::redoDepth() const
{
    return m_redoStack.size();
}

void EditorRuntime::beginHistoryTransaction()
{
    if (m_historyTransactionActive) {
        return;
    }
    m_historyTransactionActive = true;
    m_historyTransactionHasChanges = false;
}

void EditorRuntime::endHistoryTransaction()
{
    if (!m_historyTransactionActive) {
        return;
    }
    m_historyTransactionActive = false;
    if (m_historyTransactionHasChanges) {
        recordUndoSnapshot(std::move(m_historyTransactionSnapshot));
    }
    m_historyTransactionHasChanges = false;
    m_historyTransactionSnapshot = {};
}

CommandResult EditorRuntime::execute(const EditorCommand& command)
{
    if (m_historyTransactionActive &&
        (std::holds_alternative<UndoCommand>(command) ||
         std::holds_alternative<RedoCommand>(command))) {
        endHistoryTransaction();
    }
    syncDocumentCounts(&m_document);
    const bool recordHistory = recordsUndoHistory(command);
    EditorDocumentCore previousDocument;
    if (recordHistory) {
        previousDocument = m_document;
    }

    const CommandResult result = std::visit(
        [this](const auto& typedCommand) -> CommandResult {
            using T = std::decay_t<decltype(typedCommand)>;

            if constexpr (std::is_same_v<T, TogglePlaybackCommand>) {
                m_document.transport.playbackActive = !m_document.transport.playbackActive;
                return {true, m_document.transport.playbackActive ? "playback started" : "playback paused"};
            } else if constexpr (std::is_same_v<T, UndoCommand>) {
                if (m_undoStack.empty()) {
                    return {false, "nothing to undo"};
                }

                const EditorTransportState transport = m_document.transport;
                const EditorPanelState panels = m_document.panels;
                m_redoStack.push_back(std::move(m_document));
                m_document = std::move(m_undoStack.back());
                m_undoStack.pop_back();
                m_document.transport = transport;
                m_document.panels = panels;
                m_frameAccumulator = 0.0;
                return {true, "edit undone"};
            } else if constexpr (std::is_same_v<T, RedoCommand>) {
                if (m_redoStack.empty()) {
                    return {false, "nothing to redo"};
                }

                const EditorTransportState transport = m_document.transport;
                const EditorPanelState panels = m_document.panels;
                m_undoStack.push_back(std::move(m_document));
                m_document = std::move(m_redoStack.back());
                m_redoStack.pop_back();
                m_document.transport = transport;
                m_document.panels = panels;
                m_frameAccumulator = 0.0;
                return {true, "edit redone"};
            } else if constexpr (std::is_same_v<T, SetPlaybackActiveCommand>) {
                m_document.transport.playbackActive = typedCommand.active;
                return {true, typedCommand.active ? "playback started" : "playback paused"};
            } else if constexpr (std::is_same_v<T, SetPlaybackSpeedCommand>) {
                m_document.transport.playbackSpeed =
                    std::clamp(typedCommand.speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
                return {true, "playback speed updated"};
            } else if constexpr (std::is_same_v<T, SetPreviewZoomCommand>) {
                m_document.transport.previewZoom =
                    std::clamp(typedCommand.zoom, kMinPreviewZoom, kMaxPreviewZoom);
                return {true, "preview zoom updated"};
            } else if constexpr (std::is_same_v<T, SeekToFrameCommand>) {
                m_document.transport.currentFrame =
                    std::clamp(typedCommand.frame, 0, timelineEndFrame());
                m_frameAccumulator = 0.0;
                return {true, "playhead moved"};
            } else if constexpr (std::is_same_v<T, StepFrameCommand>) {
                m_document.transport.currentFrame =
                    std::clamp(m_document.transport.currentFrame + typedCommand.delta, 0, timelineEndFrame());
                m_frameAccumulator = 0.0;
                return {true, "playhead stepped"};
            } else if constexpr (std::is_same_v<T, SetProjectNameCommand>) {
                m_document.projectName = typedCommand.name.empty()
                    ? std::string("Untitled Project")
                    : typedCommand.name;
                return {true, "project name updated"};
            } else if constexpr (std::is_same_v<T, ImportMediaCommand>) {
                if (typedCommand.sourcePath.empty()) {
                    return {false, "media path required"};
                }
                ensureMediaItemForClip(
                    &m_document,
                    typedCommand.sourcePath,
                    typedCommand.label.empty() ? fallbackLabelFromPath(typedCommand.sourcePath)
                                               : typedCommand.label,
                    typedCommand.mediaKind,
                    typedCommand.audioPresenceKnown,
                    typedCommand.hasAudio);
                if (typedCommand.audioPresenceKnown) {
                    for (EditorClip& clip : m_document.clips) {
                        if (clip.sourcePath != typedCommand.sourcePath) {
                            continue;
                        }
                        clip.audioPresenceKnown = true;
                        clip.hasAudio = typedCommand.hasAudio;
                    }
                }
                return {true, "media imported"};
            } else if constexpr (std::is_same_v<T, RemoveMediaCommand>) {
                const auto mediaItem = std::find_if(
                    m_document.mediaItems.cbegin(), m_document.mediaItems.cend(),
                    [&](const EditorMediaItem& candidate) {
                        return candidate.id == typedCommand.mediaId;
                    });
                if (mediaItem == m_document.mediaItems.cend()) {
                    return {false, "media not found"};
                }
                const bool referenced = std::any_of(
                    m_document.clips.cbegin(), m_document.clips.cend(),
                    [&](const EditorClip& clip) {
                        return !clip.sourcePath.empty() &&
                            clip.sourcePath == typedCommand.mediaId;
                    });
                if (referenced) {
                    return {false, "media is used by timeline clips"};
                }
                m_document.mediaItems.erase(mediaItem);
                return {true, "media removed from project"};
            } else if constexpr (std::is_same_v<T, AddTrackCommand>) {
                selectSingle(&m_document.tracks, [](const EditorTrack&) { return false; });
                const std::size_t insertionIndex = typedCommand.insertionIndex < 0
                    ? m_document.tracks.size()
                    : static_cast<std::size_t>(std::clamp(
                          typedCommand.insertionIndex,
                          0,
                          static_cast<int>(m_document.tracks.size())));
                EditorTrack track;
                track.id = nextTrackId(m_document.tracks);
                track.label = typedCommand.label.empty()
                    ? std::string("Track ") +
                        std::to_string(insertionIndex + 1)
                    : typedCommand.label;
                track.selected = true;
                m_document.tracks.insert(
                    m_document.tracks.begin() +
                        static_cast<std::ptrdiff_t>(insertionIndex),
                    std::move(track));
                return {true, "track added"};
            } else if constexpr (std::is_same_v<T, DeleteTrackCommand>) {
                const auto removedTrack = std::find_if(
                    m_document.tracks.begin(), m_document.tracks.end(),
                    [&](const EditorTrack& track) {
                        return track.id == typedCommand.trackId;
                    });
                if (removedTrack == m_document.tracks.end()) {
                    return {false, "track not found"};
                }
                if (isGeneratedEditorChildTrack(*removedTrack)) {
                    return {false,
                            "generated child track must be removed with its source"};
                }

                ClipPersistentIdSet directClipIds;
                for (const EditorClip& clip : m_document.clips) {
                    if (clip.trackId == typedCommand.trackId) {
                        const std::string clipId =
                            trimmedEditorClipId(clip.persistentId);
                        if (!clipId.empty()) {
                            directClipIds.insert(clipId);
                        }
                    }
                }
                // Qt does not expose generated child tracks as independent
                // deletion targets. In the neutral model infer that case from
                // a matte whose parent is outside the requested track.
                for (const EditorClip& clip : m_document.clips) {
                    if (clip.trackId != typedCommand.trackId ||
                        canonicalEditorClipRole(clip.clipRole) !=
                            "mask_matte") {
                        continue;
                    }
                    if (!persistentClipIdInSet(
                            directClipIds, clip.linkedSourceClipId)) {
                        return {false,
                                "mask matte track must be removed with its source"};
                    }
                }

                const ClipPersistentIdSet removedClipIds =
                    clipOwnershipClosure(m_document, directClipIds, false);
                m_document.tracks.erase(removedTrack);
                eraseOwnedClipsAndMarkers(&m_document, removedClipIds);
                if (!m_document.tracks.empty() &&
                    std::none_of(m_document.tracks.begin(), m_document.tracks.end(),
                                 [](const EditorTrack& track) { return track.selected; })) {
                    m_document.tracks.front().selected = true;
                }
                if (!m_document.clips.empty() &&
                    std::none_of(m_document.clips.begin(), m_document.clips.end(),
                                 [](const EditorClip& clip) { return clip.selected; })) {
                    m_document.clips.front().selected = true;
                }
                return {true, "track deleted"};
            } else if constexpr (std::is_same_v<T, ReorderTrackCommand>) {
                const auto track = std::find_if(
                    m_document.tracks.begin(), m_document.tracks.end(),
                    [&](const EditorTrack& candidate) {
                        return candidate.id == typedCommand.trackId;
                    });
                if (track == m_document.tracks.end()) {
                    return {false, "track not found"};
                }
                if (isGeneratedEditorChildTrack(*track)) {
                    return {false,
                            "generated child track cannot be reordered independently"};
                }

                const std::size_t currentIndex = static_cast<std::size_t>(
                    std::distance(m_document.tracks.begin(), track));
                const int lastIndex =
                    static_cast<int>(m_document.tracks.size() - 1);
                const std::size_t targetIndex = static_cast<std::size_t>(
                    std::clamp(typedCommand.targetIndex, 0, lastIndex));
                if (isGeneratedEditorChildTrack(
                        m_document.tracks[targetIndex])) {
                    return {false,
                            "generated child track cannot be a reorder target"};
                }
                if (currentIndex == targetIndex) {
                    return {false, "track already at requested position"};
                }

                EditorTrack movedTrack = std::move(*track);
                m_document.tracks.erase(track);
                m_document.tracks.insert(
                    m_document.tracks.begin() +
                        static_cast<std::ptrdiff_t>(targetIndex),
                    std::move(movedTrack));
                return {true, "track reordered"};
            } else if constexpr (std::is_same_v<T, CrossfadeTrackCommand>) {
                return applyCrossfadeToEditorTrack(&m_document, typedCommand);
            } else if constexpr (std::is_same_v<T, SelectTrackCommand>) {
                if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                    return {false, "track not found"};
                }
                selectSingle(&m_document.tracks, [&](const EditorTrack& track) {
                    return track.id == typedCommand.trackId;
                });
                return {true, "track selected"};
            } else if constexpr (std::is_same_v<T, SelectClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips,
                                            typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (typedCommand.toggle) {
                    clip->selected = !clip->selected;
                    return {true, clip->selected ? "clip added to selection"
                                                 : "clip removed from selection"};
                }
                if (typedCommand.additive) {
                    clip->selected = true;
                    return {true, "clip added to selection"};
                }
                selectSingle(&m_document.clips,
                             [&](const EditorClip& candidate) {
                                 return candidate.id == typedCommand.clipId;
                             });
                return {true, "clip selected"};
            } else if constexpr (std::is_same_v<T, CopySelectedClipsCommand>) {
                return copySelectedClipsToClipboard()
                    ? CommandResult{true, "selected clips copied"}
                    : CommandResult{false, "no clips selected"};
            } else if constexpr (std::is_same_v<T, CutSelectedClipsCommand>) {
                const ClipPersistentIdSet selectedIds =
                    selectedClipPersistentIds(m_document);
                if (selectedIds.empty()) {
                    return {false, "no clips selected"};
                }
                for (const EditorClip& clip : m_document.clips) {
                    if (!clip.selected) {
                        continue;
                    }
                    if (canonicalEditorClipRole(clip.clipRole) ==
                        "mask_matte") {
                        if (!persistentClipIdInSet(
                                selectedIds, clip.linkedSourceClipId)) {
                            return {false,
                                    "mask matte must be cut with its source"};
                        }
                        continue;
                    }
                    if (clip.locked) {
                        return {false, "locked clips cannot be cut"};
                    }
                }
                if (!copySelectedClipsToClipboard()) {
                    return {false, "selected clips could not be copied"};
                }

                ClipPersistentIdSet removedPersistentIds;
                removedPersistentIds.reserve(m_clipClipboard.size());
                for (const ClipboardClip& entry : m_clipClipboard) {
                    const std::string clipId =
                        trimmedEditorClipId(entry.clip.persistentId);
                    if (!clipId.empty()) {
                        removedPersistentIds.insert(clipId);
                    }
                }
                eraseOwnedClipsAndMarkers(&m_document,
                                          removedPersistentIds);
                selectSingle(&m_document.clips,
                             [](const EditorClip&) { return false; });
                return {true, "selected clips cut"};
            } else if constexpr (std::is_same_v<T, PasteClipsCommand>) {
                return pasteClipboardAt(typedCommand.targetFrame,
                                        typedCommand.targetTrackId);
            } else if constexpr (
                std::is_same_v<T, DuplicateSelectedClipsCommand>) {
                if (!copySelectedClipsToClipboard()) {
                    return {false, "no clips selected"};
                }
                std::int64_t endFrame =
                    m_clipClipboard.front().clip.startFrame;
                for (const ClipboardClip& entry : m_clipClipboard) {
                    endFrame = std::max<std::int64_t>(
                        endFrame,
                        static_cast<std::int64_t>(entry.clip.startFrame) +
                            entry.clip.durationFrames);
                }
                CommandResult pasted = pasteClipboardAt(
                    static_cast<int>(std::clamp<std::int64_t>(
                        endFrame, 0, std::numeric_limits<int>::max())),
                    m_clipboardBaseTrackId);
                if (pasted.applied) {
                    pasted.message = "selected clips duplicated";
                }
                return pasted;
            } else if constexpr (std::is_same_v<T, SelectAllClipsCommand>) {
                if (m_document.clips.empty()) {
                    return {false, "no clips to select"};
                }
                for (EditorClip& candidate : m_document.clips) {
                    candidate.selected = true;
                }
                return {true, "all clips selected"};
            } else if constexpr (std::is_same_v<T, InsertClipFromMediaCommand>) {
                EditorTrack* targetTrack =
                    findTrack(&m_document.tracks, typedCommand.trackId);
                if (!targetTrack) {
                    return {false, "track not found"};
                }
                if (isGeneratedEditorChildTrack(*targetTrack)) {
                    return {false,
                            "ordinary media cannot be inserted on a generated child track"};
                }
                const EditorMediaItem* mediaItem = findMediaItem(m_document.mediaItems, typedCommand.mediaId);
                if (!mediaItem) {
                    return {false, "media not found"};
                }
                selectSingle(&m_document.clips, [](const EditorClip&) { return false; });
                const int clipId = nextClipId(m_document.clips);
                EditorClip clip;
                clip.id = clipId;
                clip.trackId = typedCommand.trackId;
                clip.label = mediaItem->label.empty()
                    ? std::string("Clip ") + std::to_string(clipId)
                    : mediaItem->label;
                clip.startFrame = std::max(0, typedCommand.startFrame);
                clip.durationFrames = std::max(1, typedCommand.durationFrames);
                clip.selected = true;
                clip.sourcePath = mediaItem->id;
                clip.persistentId = uniquePersistentClipId(m_document.clips, clipId);
                clip.mediaKind = mediaItem->kind.empty() ? "unknown" : mediaItem->kind;
                clip.videoEnabled = clip.mediaKind != "audio";
                clip.audioEnabled = clip.mediaKind != "image" && clip.mediaKind != "title" &&
                    clip.mediaKind != "graphics";
                clip.audioPresenceKnown = mediaItem->audioPresenceKnown;
                clip.hasAudio = clip.audioPresenceKnown
                    ? mediaItem->hasAudio
                    : mediaKindMayContainAudio(clip.mediaKind,
                                               clip.sourcePath);
                m_document.clips.push_back(std::move(clip));
                return {true, "clip inserted"};
            } else if constexpr (std::is_same_v<T, AddClipCommand>) {
                EditorTrack* targetTrack =
                    findTrack(&m_document.tracks, typedCommand.trackId);
                if (!targetTrack) {
                    return {false, "track not found"};
                }
                if (isGeneratedEditorChildTrack(*targetTrack)) {
                    return {false,
                            "ordinary clips cannot be added to a generated child track"};
                }
                selectSingle(&m_document.clips, [](const EditorClip&) { return false; });
                const int clipId = nextClipId(m_document.clips);
                const std::string label = typedCommand.label.empty()
                    ? std::string("Clip ") + std::to_string(clipId)
                    : typedCommand.label;
                EditorClip clip;
                clip.id = clipId;
                clip.trackId = typedCommand.trackId;
                clip.label = label;
                clip.startFrame = std::max(0, typedCommand.startFrame);
                clip.durationFrames = std::max(1, typedCommand.durationFrames);
                clip.selected = true;
                clip.sourcePath = typedCommand.sourcePath;
                clip.persistentId = uniquePersistentClipId(m_document.clips, clipId);
                clip.mediaKind = typedCommand.mediaKind.empty() ? "unknown" : typedCommand.mediaKind;
                clip.videoEnabled = clip.mediaKind != "audio";
                clip.audioEnabled = clip.mediaKind != "image" && clip.mediaKind != "title" &&
                    clip.mediaKind != "graphics";
                clip.audioPresenceKnown = typedCommand.audioPresenceKnown;
                clip.hasAudio = clip.audioPresenceKnown
                    ? typedCommand.hasAudio
                    : mediaKindMayContainAudio(clip.mediaKind,
                                               clip.sourcePath);
                m_document.clips.push_back(std::move(clip));
                ensureMediaItemForClip(
                    &m_document,
                    typedCommand.sourcePath,
                    label,
                    typedCommand.mediaKind,
                    typedCommand.audioPresenceKnown,
                    typedCommand.hasAudio);
                return {true, "clip added"};
            } else if constexpr (
                std::is_same_v<T, CreateTitleClipCommand>) {
                const int startFrame = std::max(0, typedCommand.startFrame);
                const int durationFrames = std::max(1, typedCommand.durationFrames);
                bool createdTrack = false;
                const auto appendTitleTrack = [&](std::string label) {
                    EditorTrack titleTrack;
                    titleTrack.id = nextTrackId(m_document.tracks);
                    titleTrack.label = std::move(label);
                    titleTrack.audioEnabled = false;
                    m_document.tracks.push_back(std::move(titleTrack));
                    createdTrack = true;
                    return static_cast<int>(m_document.tracks.size()) - 1;
                };
                int targetTrackIndex = -1;
                for (std::size_t index = 0;
                     index < m_document.tracks.size(); ++index) {
                    if (isTitleTrackLabel(m_document.tracks[index].label, true)) {
                        targetTrackIndex = static_cast<int>(index);
                        break;
                    }
                }

                // The Qt action creates the canonical unnumbered lane before
                // considering any pre-existing numbered/prefixed title lane.
                if (targetTrackIndex < 0) {
                    targetTrackIndex = appendTitleTrack("Titles");
                }

                if (firstNonConflictingTrackIndex(
                        m_document,
                        targetTrackIndex,
                        "title",
                        startFrame,
                        durationFrames) != targetTrackIndex) {
                    targetTrackIndex = -1;
                    for (std::size_t index = 0;
                         index < m_document.tracks.size(); ++index) {
                        if (!isTitleTrackLabel(m_document.tracks[index].label)) {
                            continue;
                        }
                        if (firstNonConflictingTrackIndex(
                                m_document,
                                static_cast<int>(index),
                                "title",
                                startFrame,
                                durationFrames) == static_cast<int>(index)) {
                            targetTrackIndex = static_cast<int>(index);
                            break;
                        }
                    }
                    if (targetTrackIndex < 0) {
                        targetTrackIndex = appendTitleTrack(
                            nextTitleTrackLabel(m_document.tracks));
                    }
                }

                const int clipId = nextClipId(m_document.clips);
                EditorClip titleClip;
                titleClip.id = clipId;
                titleClip.trackId = m_document.tracks[
                    static_cast<std::size_t>(targetTrackIndex)].id;
                titleClip.label = "Title";
                titleClip.startFrame = startFrame;
                titleClip.durationFrames = durationFrames;
                titleClip.selected = true;
                titleClip.persistentId = uniquePersistentClipId(
                    m_document.clips, clipId);
                titleClip.mediaKind = "title";
                titleClip.sourceDurationFrames = durationFrames;
                titleClip.videoEnabled = true;
                titleClip.audioEnabled = false;
                titleClip.hasAudio = false;
                titleClip.audioPresenceKnown = true;

                EditorTitleKeyframe initialTitle;
                initialTitle.frame = 0;
                initialTitle.text = "Title";
                titleClip.titleKeyframes.push_back(std::move(initialTitle));

                selectSingle(&m_document.clips,
                             [](const EditorClip&) { return false; });
                m_document.clips.push_back(std::move(titleClip));
                sortClipsByTimeline(&m_document.clips);
                return {true, createdTrack
                    ? "title created on new Titles track"
                    : "title created"};
            } else if constexpr (std::is_same_v<T, DeleteClipCommand>) {
                const EditorClip* removedClip = findClip(&m_document.clips, typedCommand.clipId);
                if (!removedClip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(removedClip->clipRole) ==
                    "mask_matte") {
                    return {false,
                            "mask matte must be deleted with its source"};
                }
                if (removedClip->locked) {
                    return {false, "locked clip cannot be deleted"};
                }
                ClipPersistentIdSet directClipIds;
                directClipIds.insert(
                    trimmedEditorClipId(removedClip->persistentId));
                const ClipPersistentIdSet removedClipIds =
                    clipOwnershipClosure(m_document, directClipIds, false);
                eraseOwnedClipsAndMarkers(&m_document, removedClipIds);
                if (!m_document.clips.empty() &&
                    std::none_of(m_document.clips.begin(), m_document.clips.end(),
                                 [](const EditorClip& clip) { return clip.selected; })) {
                    m_document.clips.front().selected = true;
                }
                return {true, "clip deleted"};
            } else if constexpr (
                std::is_same_v<T, DeleteSelectedClipsCommand>) {
                const ClipPersistentIdSet selectedIds =
                    selectedClipPersistentIds(m_document);
                if (selectedIds.empty()) {
                    return {false, "no clips selected"};
                }
                for (const EditorClip& clip : m_document.clips) {
                    if (!clip.selected) {
                        continue;
                    }
                    if (canonicalEditorClipRole(clip.clipRole) ==
                        "mask_matte") {
                        if (!persistentClipIdInSet(
                                selectedIds, clip.linkedSourceClipId)) {
                            return {false,
                                    "mask matte must be deleted with its source"};
                        }
                        continue;
                    }
                    if (clip.locked) {
                        return {false, "locked clips cannot be deleted"};
                    }
                }

                const ClipPersistentIdSet removedPersistentIds =
                    clipOwnershipClosure(m_document, selectedIds, false);
                eraseOwnedClipsAndMarkers(&m_document,
                                          removedPersistentIds);
                selectDeterministicClip(m_document.tracks,
                                        &m_document.clips);
                return {true, "selected clips deleted"};
            } else if constexpr (std::is_same_v<T, SplitClipCommand>) {
                return splitClipAtFrame(&m_document,
                                        typedCommand.clipId,
                                        typedCommand.frame);
            } else if constexpr (
                std::is_same_v<T, SplitSelectedClipsCommand>) {
                std::vector<int> selectedClipIds;
                selectedClipIds.reserve(m_document.clips.size());
                for (const EditorClip& clip : m_document.clips) {
                    if (clip.selected) {
                        selectedClipIds.push_back(clip.id);
                    }
                }
                if (selectedClipIds.empty()) {
                    return {false, "no clips selected"};
                }

                std::vector<int> trailingClipIds;
                trailingClipIds.reserve(selectedClipIds.size());
                for (const int clipId : selectedClipIds) {
                    const EditorClip* clip = findClip(&m_document.clips,
                                                      clipId);
                    if (!clip || clip->locked ||
                        typedCommand.frame <= clip->startFrame ||
                        typedCommand.frame >=
                            clip->startFrame + clip->durationFrames) {
                        continue;
                    }
                    int trailingClipId = 0;
                    const CommandResult split = splitClipAtFrame(
                        &m_document, clipId, typedCommand.frame,
                        &trailingClipId);
                    if (split.applied) {
                        trailingClipIds.push_back(trailingClipId);
                    }
                }
                if (trailingClipIds.empty()) {
                    return {false, "no selected clips intersect split frame"};
                }

                for (EditorClip& clip : m_document.clips) {
                    clip.selected = std::find(trailingClipIds.begin(),
                                              trailingClipIds.end(),
                                              clip.id) !=
                        trailingClipIds.end();
                }
                return {true, "selected clips split"};
            } else if constexpr (std::is_same_v<T, TrimClipStartCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false, "mask matte must be trimmed with its source"};
                }
                if (clip->locked) {
                    return {false, "locked clip cannot be trimmed"};
                }
                const int clipEnd = clip->startFrame + clip->durationFrames;
                if (typedCommand.startFrame < 0 || typedCommand.startFrame >= clipEnd) {
                    return {false, "trim start outside clip"};
                }
                const int nextDuration = clipEnd - typedCommand.startFrame;
                if (nextDuration < 1) {
                    return {false, "trim would create empty clip"};
                }
                const int trimFrames = typedCommand.startFrame - clip->startFrame;
                clip->startFrame = typedCommand.startFrame;
                clip->durationFrames = nextDuration;
                advanceClipSourceIn(clip, trimFrames);
                if (clip->mediaKind == "image" || clip->mediaKind == "title" ||
                    clip->mediaKind == "graphics") {
                    clip->sourceDurationFrames = nextDuration;
                }
                trimKeyframesFromStart(&clip->transformKeyframes, trimFrames);
                trimKeyframesFromStart(&clip->gradingKeyframes, trimFrames);
                trimKeyframesFromStart(&clip->opacityKeyframes, trimFrames);
                trimKeyframesFromStart(&clip->titleKeyframes, trimFrames);
                for (EditorCorrectionPolygon& polygon : clip->correctionPolygons) {
                    polygon.startFrame = std::max<std::int64_t>(
                        0, polygon.startFrame - trimFrames);
                    if (polygon.endFrame >= 0) {
                        polygon.endFrame = std::max<std::int64_t>(
                            polygon.startFrame, polygon.endFrame - trimFrames);
                    }
                }
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "clip start trimmed"};
            } else if constexpr (std::is_same_v<T, TrimClipEndCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false, "mask matte must be trimmed with its source"};
                }
                if (clip->locked) {
                    return {false, "locked clip cannot be trimmed"};
                }
                if (typedCommand.endFrame <= clip->startFrame) {
                    return {false, "trim end outside clip"};
                }
                const int nextDuration = typedCommand.endFrame - clip->startFrame;
                if (nextDuration < 1) {
                    return {false, "trim would create empty clip"};
                }
                clip->durationFrames = nextDuration;
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "clip end trimmed"};
            } else if constexpr (std::is_same_v<T, SetClipLabelCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->label = typedCommand.label.empty() ? std::string("clip") : typedCommand.label;
                return {true, "clip label updated"};
            } else if constexpr (std::is_same_v<T, SetClipLockedCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (clip->locked == typedCommand.locked) {
                    return {false, typedCommand.locked
                        ? "clip is already locked"
                        : "clip is already unlocked"};
                }
                clip->locked = typedCommand.locked;
                return {true, typedCommand.locked ? "clip locked" : "clip unlocked"};
            } else if constexpr (
                std::is_same_v<T, SetClipPlaybackRateCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false,
                            "mask matte playback rate follows its source"};
                }
                if (clip->locked) {
                    return {false, "locked clip playback rate cannot be changed"};
                }
                if (!std::isfinite(typedCommand.playbackRate) ||
                    typedCommand.playbackRate <= 0.0) {
                    return {false, "clip playback rate must be positive"};
                }

                constexpr double kMinimumClipRate = 0.001;
                constexpr double kMaximumClipRate = 100.0;
                const double nextRate = std::clamp(
                    typedCommand.playbackRate,
                    kMinimumClipRate,
                    kMaximumClipRate);
                const double previousRate =
                    std::isfinite(clip->playbackRate) && clip->playbackRate > 0.0
                    ? std::clamp(clip->playbackRate,
                                 kMinimumClipRate,
                                 kMaximumClipRate)
                    : 1.0;
                if (std::abs(previousRate - nextRate) <= 0.0001) {
                    return {false, "clip already uses requested playback rate"};
                }

                const int previousDuration = std::max(1, clip->durationFrames);
                const long double scaledDuration =
                    static_cast<long double>(previousDuration) *
                    static_cast<long double>(previousRate) /
                    static_cast<long double>(nextRate);
                const int nextDuration = static_cast<int>(std::clamp<long double>(
                    std::round(scaledDuration),
                    1.0L,
                    static_cast<long double>(std::numeric_limits<int>::max())));
                const std::int64_t rippleDelta =
                    static_cast<std::int64_t>(previousDuration) - nextDuration;
                const std::int64_t previousEnd =
                    static_cast<std::int64_t>(clip->startFrame) + previousDuration;
                const int trackId = clip->trackId;

                clip->playbackRate = nextRate;
                clip->durationFrames = nextDuration;

                std::unordered_map<std::string, std::int64_t> markerShifts;
                if (rippleDelta != 0) {
                    for (EditorClip& candidate : m_document.clips) {
                        if (candidate.id == typedCommand.clipId ||
                            canonicalEditorClipRole(candidate.clipRole) ==
                                "mask_matte" ||
                            candidate.trackId != trackId ||
                            static_cast<std::int64_t>(candidate.startFrame) <
                                previousEnd - 1) {
                            continue;
                        }
                        const int previousStart = candidate.startFrame;
                        candidate.startFrame = static_cast<int>(
                            std::clamp<std::int64_t>(
                                static_cast<std::int64_t>(previousStart) -
                                    rippleDelta,
                                0,
                                std::numeric_limits<int>::max()));
                        markerShifts[candidate.persistentId] =
                            static_cast<std::int64_t>(candidate.startFrame) -
                            previousStart;
                    }
                    for (EditorRenderSyncMarker& marker :
                         m_document.renderSyncMarkers) {
                        const auto shift = markerShifts.find(marker.clipId);
                        if (shift == markerShifts.end()) {
                            continue;
                        }
                        marker.frame = std::max<std::int64_t>(
                            0, marker.frame + shift->second);
                    }
                    std::sort(m_document.renderSyncMarkers.begin(),
                              m_document.renderSyncMarkers.end(),
                              renderSyncMarkerLess);
                }
                // The target matte and every matte owned by a rippled source
                // inherit their authoritative timing at the same boundary.
                normalizeMaskMatteParentCaches(&m_document);
                sortClipsByTimeline(&m_document.clips);
                return {true, "clip playback rate updated"};
            } else if constexpr (std::is_same_v<T, MoveClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false, "mask matte must be moved with its source"};
                }
                if (clip->locked) {
                    return {false, "locked clip cannot be moved"};
                }
                if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                    return {false, "track not found"};
                }
                const EditorTrack* targetTrack = findTrack(
                    &m_document.tracks, typedCommand.trackId);
                if (targetTrack &&
                    isGeneratedEditorChildTrack(*targetTrack)) {
                    return {false,
                            "ordinary clips cannot move onto a generated child track"};
                }

                const int nextStart = std::max(0, typedCommand.startFrame);
                const std::int64_t frameDelta =
                    static_cast<std::int64_t>(nextStart) - clip->startFrame;
                ClipPersistentIdSet sourceIds;
                sourceIds.insert(trimmedEditorClipId(clip->persistentId));
                const ClipPersistentIdSet aggregateIds =
                    clipOwnershipClosure(m_document, sourceIds, false);
                for (const EditorClip& candidate : m_document.clips) {
                    if (!persistentClipIdInSet(
                            aggregateIds, candidate.persistentId)) {
                        continue;
                    }
                    const std::int64_t shiftedStart =
                        static_cast<std::int64_t>(candidate.startFrame) +
                        frameDelta;
                    if (shiftedStart < 0 ||
                        shiftedStart > std::numeric_limits<int>::max()) {
                        return {false, "clip frame move is out of range"};
                    }
                }
                for (const EditorRenderSyncMarker& marker :
                     m_document.renderSyncMarkers) {
                    if (!persistentClipIdInSet(aggregateIds, marker.clipId)) {
                        continue;
                    }
                    if ((frameDelta > 0 &&
                         marker.frame >
                             std::numeric_limits<std::int64_t>::max() -
                                 frameDelta) ||
                        (frameDelta < 0 && marker.frame < -frameDelta)) {
                        return {false,
                                "render sync marker move is out of range"};
                    }
                }

                clip->trackId = typedCommand.trackId;
                for (EditorClip& candidate : m_document.clips) {
                    if (persistentClipIdInSet(
                            aggregateIds, candidate.persistentId)) {
                        candidate.startFrame = static_cast<int>(
                            static_cast<std::int64_t>(candidate.startFrame) +
                            frameDelta);
                    }
                }
                for (EditorRenderSyncMarker& marker :
                     m_document.renderSyncMarkers) {
                    if (persistentClipIdInSet(aggregateIds, marker.clipId)) {
                        marker.frame += frameDelta;
                    }
                }
                normalizeMaskMatteParentCaches(&m_document);
                std::sort(m_document.renderSyncMarkers.begin(),
                          m_document.renderSyncMarkers.end(),
                          renderSyncMarkerLess);
                return {true, "clip moved"};
            } else if constexpr (
                std::is_same_v<T, MoveSelectedClipsCommand>) {
                EditorClip* anchor = findClip(&m_document.clips,
                                              typedCommand.anchorClipId);
                if (!anchor) {
                    return {false, "anchor clip not found"};
                }
                if (canonicalEditorClipRole(anchor->clipRole) ==
                    "mask_matte") {
                    return {false, "mask matte must be moved with its source"};
                }
                if (!anchor->selected) {
                    return {false, "anchor clip is not selected"};
                }
                const std::size_t anchorTrackIndex =
                    trackIndexForId(m_document.tracks, anchor->trackId);
                const std::size_t targetTrackIndex =
                    trackIndexForId(m_document.tracks,
                                    typedCommand.targetTrackId);
                if (anchorTrackIndex == m_document.tracks.size() ||
                    targetTrackIndex == m_document.tracks.size()) {
                    return {false, "target or source track not found"};
                }
                if (isGeneratedEditorChildTrack(
                        m_document.tracks[targetTrackIndex])) {
                    return {false,
                            "ordinary clips cannot move onto a generated child track"};
                }

                SelectedFrameShift frameShift;
                std::string shiftError;
                if (!prepareSelectedFrameShift(
                        m_document,
                        static_cast<std::int64_t>(typedCommand.startFrame) -
                            anchor->startFrame,
                        &frameShift, &shiftError)) {
                    return {false, std::move(shiftError)};
                }

                const std::int64_t trackDelta =
                    static_cast<std::int64_t>(targetTrackIndex) -
                    static_cast<std::int64_t>(anchorTrackIndex);
                for (const EditorClip& clip : m_document.clips) {
                    if (!clip.selected ||
                        canonicalEditorClipRole(clip.clipRole) ==
                            "mask_matte") {
                        continue;
                    }
                    const std::size_t sourceTrackIndex =
                        trackIndexForId(m_document.tracks, clip.trackId);
                    if (sourceTrackIndex == m_document.tracks.size()) {
                        return {false, "selected clip source track not found"};
                    }
                    const std::int64_t destinationTrackIndex =
                        static_cast<std::int64_t>(sourceTrackIndex) +
                        trackDelta;
                    if (destinationTrackIndex < 0 ||
                        destinationTrackIndex >= static_cast<std::int64_t>(
                            m_document.tracks.size())) {
                        return {false, "selected clip track move is out of range"};
                    }
                    if (isGeneratedEditorChildTrack(
                            m_document.tracks[static_cast<std::size_t>(
                                destinationTrackIndex)])) {
                        return {false,
                                "selected clips cannot move onto a generated child track"};
                    }
                }

                for (EditorClip& clip : m_document.clips) {
                    if (!clip.selected ||
                        canonicalEditorClipRole(clip.clipRole) ==
                            "mask_matte") {
                        continue;
                    }
                    const std::size_t sourceTrackIndex =
                        trackIndexForId(m_document.tracks, clip.trackId);
                    const std::size_t destinationTrackIndex =
                        static_cast<std::size_t>(
                            static_cast<std::int64_t>(sourceTrackIndex) +
                            trackDelta);
                    clip.trackId =
                        m_document.tracks[destinationTrackIndex].id;
                }
                applySelectedFrameShift(&m_document, frameShift);
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "selected clips moved"};
            } else if constexpr (std::is_same_v<T, ResizeClipCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false, "mask matte must be resized with its source"};
                }
                if (clip->locked) {
                    return {false, "locked clip cannot be resized"};
                }
                clip->durationFrames = std::max(1, typedCommand.durationFrames);
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "clip resized"};
            } else if constexpr (std::is_same_v<T, NudgeSelectedClipCommand>) {
                if (typedCommand.deltaFrames == 0) {
                    return {false, "nudge delta required"};
                }
                SelectedFrameShift frameShift;
                std::string shiftError;
                if (!prepareSelectedFrameShift(
                        m_document, typedCommand.deltaFrames, &frameShift,
                        &shiftError)) {
                    return {false, std::move(shiftError)};
                }
                if (frameShift.delta == 0) {
                    return {false, "selected clips already at timeline boundary"};
                }
                applySelectedFrameShift(&m_document, frameShift);
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "selected clips nudged"};
            } else if constexpr (std::is_same_v<T, SetClipGradingCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->brightness = std::clamp(typedCommand.brightness, -1.0, 1.0);
                clip->contrast = std::clamp(typedCommand.contrast, 0.0, 4.0);
                clip->saturation = std::clamp(typedCommand.saturation, 0.0, 4.0);
                clip->gradingPreviewEnabled = typedCommand.previewEnabled;
                return {true, "clip grading updated"};
            } else if constexpr (std::is_same_v<T, ResetClipGradingCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->brightness = 0.0;
                clip->contrast = 1.0;
                clip->saturation = 1.0;
                clip->opacity = 1.0;
                clip->gradingKeyframes.clear();
                clip->opacityKeyframes.clear();
                // Qt normalization materializes one neutral base key for each
                // visual channel after clearing. Audio-only clips keep the
                // channels empty.
                if (editorClipHasVisuals(*clip)) {
                    clip->gradingKeyframes.push_back(EditorGradingKeyframe{});
                    clip->opacityKeyframes.push_back(EditorOpacityKeyframe{});
                }
                return {true, "clip grading reset"};
            } else if constexpr (std::is_same_v<T, UpsertGradingKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorGradingKeyframe keyframe = typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                keyframe.brightness = std::clamp(keyframe.brightness, -1.0, 1.0);
                keyframe.contrast = std::clamp(keyframe.contrast, 0.0, 4.0);
                keyframe.saturation = std::clamp(keyframe.saturation, 0.0, 4.0);
                keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                keyframe.shadowsR = std::clamp(keyframe.shadowsR, -2.0, 2.0);
                keyframe.shadowsG = std::clamp(keyframe.shadowsG, -2.0, 2.0);
                keyframe.shadowsB = std::clamp(keyframe.shadowsB, -2.0, 2.0);
                keyframe.midtonesR = std::clamp(keyframe.midtonesR, -2.0, 2.0);
                keyframe.midtonesG = std::clamp(keyframe.midtonesG, -2.0, 2.0);
                keyframe.midtonesB = std::clamp(keyframe.midtonesB, -2.0, 2.0);
                keyframe.highlightsR = std::clamp(keyframe.highlightsR, -2.0, 2.0);
                keyframe.highlightsG = std::clamp(keyframe.highlightsG, -2.0, 2.0);
                keyframe.highlightsB = std::clamp(keyframe.highlightsB, -2.0, 2.0);
                keyframe.curvePointsR =
                    sanitizeEditorGradingCurve(keyframe.curvePointsR);
                keyframe.curvePointsG =
                    sanitizeEditorGradingCurve(keyframe.curvePointsG);
                keyframe.curvePointsB =
                    sanitizeEditorGradingCurve(keyframe.curvePointsB);
                keyframe.curvePointsLuma =
                    sanitizeEditorGradingCurve(keyframe.curvePointsLuma);
                if (keyframe.curveThreePointLock) {
                    synchronizeEditorThreePointGradingCurves(&keyframe);
                }
                upsertKeyframe(&clip->gradingKeyframes, std::move(keyframe));
                return {true, "grading keyframe updated"};
            } else if constexpr (std::is_same_v<T, SetClipOpacityCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->opacity = std::clamp(typedCommand.opacity, 0.0, 1.0);
                return {true, "clip opacity updated"};
            } else if constexpr (std::is_same_v<T, UpsertOpacityKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorOpacityKeyframe keyframe = typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                upsertKeyframe(&clip->opacityKeyframes, std::move(keyframe));
                return {true, "opacity keyframe updated"};
            } else if constexpr (std::is_same_v<T, RemoveClipKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                bool removed = false;
                switch (typedCommand.channel) {
                case EditorKeyframeChannel::Grading:
                    removed = removeKeyframeAtFrame(
                        &clip->gradingKeyframes, typedCommand.frame);
                    break;
                case EditorKeyframeChannel::Opacity:
                    removed = removeKeyframeAtFrame(
                        &clip->opacityKeyframes, typedCommand.frame);
                    break;
                case EditorKeyframeChannel::Transform:
                    removed = removeKeyframeAtFrame(
                        &clip->transformKeyframes, typedCommand.frame);
                    break;
                }
                return removed
                    ? CommandResult{true, "clip keyframe removed"}
                    : CommandResult{false, "clip keyframe not found"};
            } else if constexpr (std::is_same_v<T, SetClipTransformCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->baseTranslationX = typedCommand.translationX;
                clip->baseTranslationY = typedCommand.translationY;
                clip->baseRotation = std::clamp(typedCommand.rotation, -360.0, 360.0);
                clip->baseScaleX = normalizedScale(typedCommand.scaleX);
                clip->baseScaleY = normalizedScale(typedCommand.scaleY);
                return {true, "clip transform updated"};
            } else if constexpr (std::is_same_v<T, UpsertTransformKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorTransformKeyframe keyframe = typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                keyframe.rotation = std::clamp(keyframe.rotation, -360.0, 360.0);
                keyframe.scaleX = normalizedScale(keyframe.scaleX);
                keyframe.scaleY = normalizedScale(keyframe.scaleY);
                upsertKeyframe(&clip->transformKeyframes, std::move(keyframe));
                return {true, "transform keyframe updated"};
            } else if constexpr (std::is_same_v<T, SetClipMaskEffectCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->maskEnabled = typedCommand.maskEnabled;
                clip->maskFeather = std::clamp(typedCommand.feather, 0.0, 512.0);
                clip->maskFeatherGamma = std::clamp(typedCommand.featherGamma, 0.1, 8.0);
                clip->maskFeatherFalloff = std::clamp(typedCommand.featherFalloff, 0, 5);
                clip->maskForegroundLayerEnabled = typedCommand.foregroundLayerEnabled;
                clip->maskRepeatEnabled = typedCommand.repeatEnabled;
                clip->maskRepeatDeltaX = typedCommand.repeatDeltaX;
                clip->maskRepeatDeltaY = typedCommand.repeatDeltaY;
                clip->effectPreset = typedCommand.effectPreset.empty() ? "none" : typedCommand.effectPreset;
                clip->effectRows = std::clamp(
                    typedCommand.effectRows,
                    kEditorEffectMinRows,
                    editorEffectMaxRowsForPreset(clip->effectPreset));
                clip->effectSpeed = std::clamp(
                    typedCommand.effectSpeed,
                    kEditorEffectMinSpeed,
                    kEditorEffectMaxSpeed);
                clip->effectScale = std::clamp(
                    typedCommand.effectScale,
                    kEditorEffectMinScale,
                    kEditorEffectMaxScale);
                clip->effectAlternateDirection = typedCommand.alternateDirection;
                return {true, "clip mask and effect updated"};
            } else if constexpr (std::is_same_v<T, SetClipMaskCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->maskEnabled = typedCommand.maskEnabled;
                clip->maskFeather = std::clamp(typedCommand.feather, 0.0, 512.0);
                clip->maskFeatherGamma = std::clamp(
                    typedCommand.featherGamma, 0.1, 8.0);
                clip->maskFeatherFalloff = std::clamp(
                    typedCommand.featherFalloff, 0, 5);
                clip->maskForegroundLayerEnabled =
                    typedCommand.foregroundLayerEnabled;
                clip->maskRepeatEnabled = typedCommand.repeatEnabled;
                clip->maskRepeatDeltaX = typedCommand.repeatDeltaX;
                clip->maskRepeatDeltaY = typedCommand.repeatDeltaY;
                return {true, "clip mask updated"};
            } else if constexpr (std::is_same_v<T, SetClipTranscriptOverlayCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->transcriptOverlay = typedCommand.overlay;
                clip->transcriptOverlay.backgroundOpacity =
                    std::clamp(clip->transcriptOverlay.backgroundOpacity, 0.0, 1.0);
                clip->transcriptOverlay.boxWidth = std::max(160.0, clip->transcriptOverlay.boxWidth);
                clip->transcriptOverlay.boxHeight = std::max(80.0, clip->transcriptOverlay.boxHeight);
                clip->transcriptOverlay.maxLines = std::max(1, clip->transcriptOverlay.maxLines);
                clip->transcriptOverlay.maxCharsPerLine =
                    std::max(8, clip->transcriptOverlay.maxCharsPerLine);
                clip->transcriptOverlay.fontPointSize =
                    std::max(12, clip->transcriptOverlay.fontPointSize);
                clip->transcriptOverlay.textOpacity =
                    std::clamp(clip->transcriptOverlay.textOpacity, 0.0, 1.0);
                return {true, "transcript overlay updated"};
            } else if constexpr (std::is_same_v<T, SetClipTranscriptActiveCutCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (clip->transcriptActiveCutPath == typedCommand.transcriptPath) {
                    return {false, "transcript cut unchanged"};
                }
                clip->transcriptActiveCutPath = typedCommand.transcriptPath;
                return {true, "active transcript cut updated"};
            } else if constexpr (std::is_same_v<T, UpsertTitleKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorTitleKeyframe keyframe = typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame, 0, std::max(0, clip->durationFrames - 1));
                keyframe.fontSize = std::clamp(keyframe.fontSize, 1.0, 1000.0);
                keyframe.opacity = std::clamp(keyframe.opacity, 0.0, 1.0);
                upsertKeyframe(&clip->titleKeyframes, std::move(keyframe));
                return {true, "title keyframe updated"};
            } else if constexpr (std::is_same_v<T, RemoveTitleKeyframeCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                const auto oldSize = clip->titleKeyframes.size();
                clip->titleKeyframes.erase(
                    std::remove_if(clip->titleKeyframes.begin(), clip->titleKeyframes.end(),
                                   [&](const EditorTitleKeyframe& keyframe) {
                                       return keyframe.frame == typedCommand.frame;
                                   }),
                    clip->titleKeyframes.end());
                return clip->titleKeyframes.size() == oldSize
                    ? CommandResult{false, "title keyframe not found"}
                    : CommandResult{true, "title keyframe removed"};
            } else if constexpr (std::is_same_v<T, ClearCorrectionPolygonsCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (clip->correctionPolygons.empty()) {
                    return {false, "no correction polygons"};
                }
                clip->correctionPolygons.clear();
                return {true, "correction polygons cleared"};
            } else if constexpr (std::is_same_v<T, SetCorrectionsEnabledCommand>) {
                m_document.exportRequest.correctionsEnabled = typedCommand.enabled;
                return {true, "corrections visibility updated"};
            } else if constexpr (std::is_same_v<T, SetClipAudioCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->audioEnabled = typedCommand.enabled;
                clip->audioGain = std::clamp(typedCommand.gain, 0.0, 8.0);
                clip->audioPan = std::clamp(typedCommand.pan, -1.0, 1.0);
                clip->audioSolo = typedCommand.solo;
                return {true, "clip audio updated"};
            } else if constexpr (std::is_same_v<T, SetTrackPropertiesCommand>) {
                const std::size_t trackIndex =
                    trackIndexForId(m_document.tracks, typedCommand.trackId);
                if (trackIndex == m_document.tracks.size()) {
                    return {false, "track not found"};
                }
                EditorTrack& track = m_document.tracks[trackIndex];
                if (!isGeneratedEditorChildTrack(track)) {
                    track.label = trimmed(typedCommand.label);
                    if (track.label.empty()) {
                        track.label = std::string("Track ") +
                            std::to_string(trackIndex + 1);
                    }
                }
                track.height = std::clamp(
                    typedCommand.height,
                    kEditorTrackMinHeight,
                    isGeneratedEditorChildTrack(track)
                        ? 56
                        : kEditorTrackMaxHeight);
                return {true, "track properties updated"};
            } else if constexpr (std::is_same_v<T, SetTrackStateCommand>) {
                EditorTrack* track = findTrack(&m_document.tracks, typedCommand.trackId);
                if (!track) {
                    return {false, "track not found"};
                }
                track->visualMode = std::clamp(typedCommand.visualMode, 0, 2);
                track->gradingPreviewEnabled =
                    typedCommand.gradingPreviewEnabled;
                track->audioGain = std::clamp(typedCommand.audioGain, 0.0, 8.0);
                if (isGeneratedEditorChildTrack(*track)) {
                    track->audioEnabled = false;
                    track->audioMuted = false;
                    track->audioSolo = false;
                    track->audioWaveformVisible = false;
                } else {
                    track->audioEnabled = typedCommand.audioEnabled;
                    track->audioMuted = typedCommand.audioMuted;
                    track->audioSolo = typedCommand.audioSolo;
                }
                return {true, "track state updated"};
            } else if constexpr (std::is_same_v<T, AddRenderSyncMarkerCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorRenderSyncMarker marker;
                const std::string clipPersistentId = clip->persistentId.empty()
                    ? persistentClipIdForNumericId(clip->id)
                    : clip->persistentId;
                marker.clipId = editorRenderSyncOwnerClipId(
                    m_document, clipPersistentId);
                if (marker.clipId.empty()) {
                    return {false, "render sync marker owner not found"};
                }
                marker.frame = std::max<std::int64_t>(0, typedCommand.frame);
                marker.skipFrame = typedCommand.skipFrame;
                marker.count = std::clamp(
                    typedCommand.count,
                    kEditorRenderSyncMinCount,
                    kEditorRenderSyncMaxCount);
                const auto existing = std::find_if(
                    m_document.renderSyncMarkers.begin(), m_document.renderSyncMarkers.end(),
                    [&](const EditorRenderSyncMarker& value) {
                        return value.clipId == marker.clipId &&
                               value.frame == marker.frame;
                    });
                if (existing == m_document.renderSyncMarkers.end()) {
                    m_document.renderSyncMarkers.push_back(std::move(marker));
                } else {
                    *existing = std::move(marker);
                }
                std::sort(m_document.renderSyncMarkers.begin(),
                          m_document.renderSyncMarkers.end(),
                          renderSyncMarkerLess);
                m_document.exportRequest.renderSyncMarkerCount =
                    m_document.renderSyncMarkers.size();
                return {true, "render sync marker updated"};
            } else if constexpr (std::is_same_v<T, RemoveRenderSyncMarkerCommand>) {
                const std::string ownerClipId = editorRenderSyncOwnerClipId(
                    m_document, typedCommand.clipId);
                const auto marker = std::find_if(
                    m_document.renderSyncMarkers.begin(),
                    m_document.renderSyncMarkers.end(),
                    [&](const EditorRenderSyncMarker& value) {
                        return value.clipId == ownerClipId &&
                            value.frame == typedCommand.frame &&
                            value.skipFrame == typedCommand.skipFrame;
                    });
                if (marker == m_document.renderSyncMarkers.end()) {
                    return {false, "render sync marker not found"};
                }
                m_document.renderSyncMarkers.erase(marker);
                m_document.exportRequest.renderSyncMarkerCount =
                    m_document.renderSyncMarkers.size();
                return {true, "render sync marker removed"};
            } else if constexpr (std::is_same_v<T, ClearRenderSyncMarkersCommand>) {
                const auto oldSize = m_document.renderSyncMarkers.size();
                if (typedCommand.clipId == 0) {
                    m_document.renderSyncMarkers.clear();
                } else {
                    const EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return {false, "clip not found"};
                    }
                    const std::string clipPersistentId = clip->persistentId.empty()
                        ? persistentClipIdForNumericId(clip->id)
                        : clip->persistentId;
                    const std::string persistentId = editorRenderSyncOwnerClipId(
                        m_document, clipPersistentId);
                    m_document.renderSyncMarkers.erase(
                        std::remove_if(m_document.renderSyncMarkers.begin(),
                                       m_document.renderSyncMarkers.end(),
                                       [&](const EditorRenderSyncMarker& marker) {
                                           return marker.clipId == persistentId;
                                       }),
                        m_document.renderSyncMarkers.end());
                }
                if (m_document.renderSyncMarkers.size() == oldSize) {
                    return {false, "no render sync markers"};
                }
                m_document.exportRequest.renderSyncMarkerCount =
                    m_document.renderSyncMarkers.size();
                return {true, "render sync markers cleared"};
            } else if constexpr (std::is_same_v<T, SetExportRangeCommand>) {
                if (typedCommand.startFrame < 0 ||
                    typedCommand.endFrame <= typedCommand.startFrame) {
                    return {false, "invalid export range"};
                }
                m_document.exportRanges = {{typedCommand.startFrame, typedCommand.endFrame}};
                m_document.exportRequest.exportStartFrame = typedCommand.startFrame;
                m_document.exportRequest.exportEndFrame = typedCommand.endFrame;
                m_document.exportRequest.exportRangeCount = 1;
                return {true, "export range updated"};
            } else if constexpr (std::is_same_v<T, SetWaveformVisibleCommand>) {
                m_document.panels.showWaveform = typedCommand.visible;
                return {true, "waveform visibility updated"};
            } else if constexpr (std::is_same_v<T, SetTranscriptVisibleCommand>) {
                m_document.panels.showTranscript = typedCommand.visible;
                return {true, "transcript visibility updated"};
            } else if constexpr (std::is_same_v<T, SetScopesVisibleCommand>) {
                m_document.panels.showScopes = typedCommand.visible;
                return {true, "scopes visibility updated"};
            } else if constexpr (std::is_same_v<T, SetExportSizeCommand>) {
                m_document.exportRequest.outputSize = {
                    std::max(16, typedCommand.width),
                    std::max(16, typedCommand.height)};
                return {true, "export size updated"};
            } else if constexpr (std::is_same_v<T, SetExportFpsCommand>) {
                m_document.exportRequest.outputFps = std::max(1.0, typedCommand.fps);
                return {true, "export fps updated"};
            } else if constexpr (std::is_same_v<T, SetExportOutputPathCommand>) {
                m_document.exportRequest.outputPath = typedCommand.path;
                return {true, "export output path updated"};
            } else if constexpr (std::is_same_v<T, SetExportFormatCommand>) {
                m_document.exportRequest.outputFormat = typedCommand.format.empty()
                    ? std::string("mp4")
                    : typedCommand.format;
                return {true, "export format updated"};
            } else if constexpr (std::is_same_v<T, SetExportImageSequenceFormatCommand>) {
                m_document.exportRequest.imageSequenceFormat = typedCommand.format.empty()
                    ? std::string("jpeg")
                    : typedCommand.format;
                return {true, "image sequence format updated"};
            } else if constexpr (std::is_same_v<T, SetExportUseProxyMediaCommand>) {
                m_document.exportRequest.useProxyMedia = typedCommand.enabled;
                return {true, "proxy export flag updated"};
            } else if constexpr (std::is_same_v<T, SetExportImageSequenceCommand>) {
                m_document.exportRequest.createVideoFromImageSequence = typedCommand.enabled;
                m_document.exportRequest.outputMode = typedCommand.enabled
                    ? render::RenderOutputMode::EncodedFileAndImageSequence
                    : render::RenderOutputMode::EncodedFile;
                if (typedCommand.enabled && m_document.exportRequest.imageSequenceFormat.empty()) {
                    m_document.exportRequest.imageSequenceFormat = "jpeg";
                }
                return {true, "image sequence mode updated"};
            }

            return {false, "unsupported command"};
        },
        command);

    if (result.applied && reconcilesGeneratedTrackTopology(command)) {
        reconcileEditorGeneratedChildTracks(&m_document);
    }
    syncDocumentCounts(&m_document);
    if (recordHistory && result.applied && toJson(previousDocument) != toJson(m_document)) {
        if (m_historyTransactionActive) {
            if (!m_historyTransactionHasChanges) {
                m_historyTransactionSnapshot = std::move(previousDocument);
                m_historyTransactionHasChanges = true;
            }
        } else {
            recordUndoSnapshot(std::move(previousDocument));
        }
    }
    return result;
}

void EditorRuntime::recordUndoSnapshot(EditorDocumentCore document)
{
    if (m_undoStack.size() >= kMaxHistoryEntries) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(std::move(document));
    m_redoStack.clear();
}

void EditorRuntime::tick(const TickParams& params)
{
    if (!m_document.transport.playbackActive) {
        m_frameAccumulator = 0.0;
        return;
    }

    const double deltaSeconds = std::max(0.0, params.deltaSeconds);
    if (deltaSeconds <= 0.0) {
        return;
    }

    const double fps = m_document.exportRequest.outputFps > 0.0
        ? m_document.exportRequest.outputFps
        : kDefaultTimelineFps;
    m_frameAccumulator += deltaSeconds * fps * m_document.transport.playbackSpeed;

    const int wholeFrames = static_cast<int>(m_frameAccumulator);
    if (wholeFrames <= 0) {
        return;
    }

    m_frameAccumulator -= static_cast<double>(wholeFrames);
    const int endFrame = timelineEndFrame();
    m_document.transport.currentFrame = std::min(m_document.transport.currentFrame + wholeFrames, endFrame);
    if (m_document.transport.currentFrame >= endFrame) {
        m_document.transport.currentFrame = endFrame;
        m_document.transport.playbackActive = false;
        m_frameAccumulator = 0.0;
    }
}

int EditorRuntime::timelineEndFrame() const
{
    int endFrame = 0;
    for (const EditorClip& clip : m_document.clips) {
        endFrame = std::max(endFrame, clip.startFrame + clip.durationFrames);
    }
    endFrame = std::max(endFrame, static_cast<int>(m_document.exportRequest.exportEndFrame));
    return endFrame;
}

} // namespace jcut
