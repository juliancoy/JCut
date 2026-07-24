#include "editor_runtime.h"
#include "editor_media_presence_core.h"

#include "editor_document_core_json.h"
#include "editor_grading_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr float kMinPlaybackSpeed = 0.1f;
constexpr float kMaxPlaybackSpeed = 3.0f;
constexpr float kMinPreviewZoom = 0.5f;
constexpr float kMaxPreviewZoom = 3.0f;
constexpr double kDefaultTimelineFps = 30.0;
constexpr std::int64_t kEditorAudioSampleRate = 48000;
constexpr std::int64_t kEditorSamplesPerFrame =
    kEditorAudioSampleRate / 30;

std::int64_t qtTimelineExtentFrame(
    const jcut::EditorDocumentCore& document)
{
    std::int64_t extent = 300;
    for (const jcut::EditorClip& clip : document.clips) {
        extent = std::max(
            extent,
            static_cast<std::int64_t>(clip.startFrame) +
                clip.durationFrames +
                static_cast<std::int64_t>(kDefaultTimelineFps));
    }
    return extent;
}

std::vector<jcut::export_range::Range> sharedExportRanges(
    const std::vector<jcut::EditorExportRange>& ranges)
{
    std::vector<jcut::export_range::Range> result;
    result.reserve(ranges.size());
    for (const jcut::EditorExportRange& range : ranges) {
        result.push_back({range.startFrame, range.endFrame});
    }
    return result;
}

void storeSharedExportRanges(
    const std::vector<jcut::export_range::Range>& ranges,
    std::vector<jcut::EditorExportRange>* destination)
{
    if (!destination) {
        return;
    }
    destination->clear();
    destination->reserve(ranges.size());
    for (const jcut::export_range::Range& range : ranges) {
        destination->push_back(
            {range.startFrame, range.endFrame});
    }
}

void synchronizeExportRequestRanges(
    jcut::EditorDocumentCore* document)
{
    if (!document || document->exportRanges.empty()) {
        return;
    }
    document->exportRequest.exportStartFrame =
        document->exportRanges.front().startFrame;
    document->exportRequest.exportEndFrame =
        document->exportRanges.back().endFrame;
    document->exportRequest.exportRangeCount =
        document->exportRanges.size();
}

int playbackStartFrame(
    const jcut::EditorDocumentCore& document,
    int timelineEndFrame)
{
    const auto ranges = jcut::normalizedPlaybackRangesCore(
        document.exportRanges, timelineEndFrame);
    if (ranges.empty()) {
        return document.transport.currentFrame >= timelineEndFrame
            ? 0
            : document.transport.currentFrame;
    }
    const auto playable = std::find_if(
        ranges.begin(), ranges.end(),
        [&](const jcut::PlaybackRangeCore& range) {
            return document.transport.currentFrame <= range.endFrame;
        });
    return static_cast<int>(
        playable == ranges.end()
            ? ranges.front().startFrame
            : std::max<std::int64_t>(
                  document.transport.currentFrame,
                  playable->startFrame));
}

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
    return jcut::editorMediaKindHasVisualsCore(mediaKind);
}

bool editorClipHasVisuals(const jcut::EditorClip& clip)
{
    return jcut::editorClipHasVisualsCore(clip);
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

    // A generated child can participate only through its selected parent. Its
    // persisted lock is a relationship invariant, not a reason to block a
    // parent edit.
    for (const jcut::EditorClip& clip : document.clips) {
        if (!clip.selected) {
            continue;
        }
        if (jcut::isOwnedGeneratedEditorClip(clip)) {
            const std::string parentId =
                jcut::trimmedEditorClipId(clip.linkedSourceClipId);
            if (parentId.empty() || selectedIds.find(parentId) == selectedIds.end()) {
                if (error) {
                    *error = "generated child must be moved with its source";
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
                    !jcut::isOwnedGeneratedEditorClip(clip)) {
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
            if (!jcut::isOwnedGeneratedEditorClip(clip)) {
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
                   std::is_same_v<T, jcut::ReplaceSpeakerTitleClipsCommand> ||
                   std::is_same_v<T, jcut::DeleteClipCommand> ||
                   std::is_same_v<T, jcut::DeleteSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::SplitClipCommand> ||
                   std::is_same_v<T, jcut::SplitSelectedClipsCommand> ||
                   std::is_same_v<T, jcut::TrimClipStartCommand> ||
                   std::is_same_v<T, jcut::TrimClipEndCommand> ||
                   std::is_same_v<T, jcut::SetClipLabelCommand> ||
                   std::is_same_v<T, jcut::SetClipProxyCommand> ||
                   std::is_same_v<T, jcut::RefreshClipMetadataCommand> ||
                   std::is_same_v<T, jcut::SetClipLockedCommand> ||
                   std::is_same_v<T, jcut::SetSelectedClipsLockedCommand> ||
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
                   std::is_same_v<T, jcut::SetClipSourceTransformLockedCommand> ||
                   std::is_same_v<T, jcut::SetClipSpeakerFramingCommand> ||
                   std::is_same_v<T, jcut::SetClipSpeakerSectionMinimumWordsCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingEnabledKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingTargetKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertTransformKeyframeCommand> ||
                   std::is_same_v<T, jcut::CommitPreviewTransformCommand> ||
                   std::is_same_v<T, jcut::SetClipMaskEffectCommand> ||
                   std::is_same_v<T, jcut::SetClipMaskCommand> ||
                   std::is_same_v<T, jcut::MaterializeMaskMatteCommand> ||
                   std::is_same_v<T, jcut::SetClipZLevelCommand> ||
                   std::is_same_v<T, jcut::SetClipTranscriptOverlayCommand> ||
                   std::is_same_v<T, jcut::SetClipTranscriptActiveCutCommand> ||
                   std::is_same_v<T, jcut::UpsertTitleKeyframeCommand> ||
                   std::is_same_v<T, jcut::RemoveTitleKeyframeCommand> ||
                   std::is_same_v<T, jcut::SetClipCorrectionPolygonsCommand> ||
                   std::is_same_v<T, jcut::ClearCorrectionPolygonsCommand> ||
                   std::is_same_v<T, jcut::SetCorrectionsEnabledCommand> ||
                   std::is_same_v<T, jcut::SetClipAudioCommand> ||
                   std::is_same_v<T, jcut::SetAudioDynamicsCommand> ||
                   std::is_same_v<T, jcut::SetAudioTreatmentCommand> ||
                   std::is_same_v<T, jcut::SetTrackPropertiesCommand> ||
                   std::is_same_v<T, jcut::SetTrackStateCommand> ||
                   std::is_same_v<T, jcut::AddRenderSyncMarkerCommand> ||
                   std::is_same_v<T, jcut::RemoveRenderSyncMarkerCommand> ||
                   std::is_same_v<T, jcut::ClearRenderSyncMarkersCommand> ||
                   std::is_same_v<T, jcut::SetExportRangeCommand> ||
                   std::is_same_v<T, jcut::SetExportRangesCommand> ||
                   std::is_same_v<T, jcut::EditExportRangesCommand> ||
                   std::is_same_v<T, jcut::SetExportSizeCommand> ||
                   std::is_same_v<T, jcut::SetExportFpsCommand> ||
                   std::is_same_v<T, jcut::SetExportOutputPathCommand> ||
                   std::is_same_v<T, jcut::SetExportFormatCommand> ||
                   std::is_same_v<T, jcut::SetExportImageSequenceFormatCommand> ||
                   std::is_same_v<T, jcut::SetExportUseProxyMediaCommand> ||
                   std::is_same_v<T, jcut::SetTranscriptHistoryDocumentCommand> ||
                   std::is_same_v<T, jcut::SetExportImageSequenceCommand>;
        },
        command);
}

bool transcriptHistoryDocumentsEqual(
    const jcut::EditorDocumentCore& lhs,
    const jcut::EditorDocumentCore& rhs)
{
    if (lhs.transcriptHistoryDocuments.size() !=
        rhs.transcriptHistoryDocuments.size()) {
        return false;
    }
    for (std::size_t index = 0;
         index < lhs.transcriptHistoryDocuments.size(); ++index) {
        const auto& left = lhs.transcriptHistoryDocuments[index];
        const auto& right = rhs.transcriptHistoryDocuments[index];
        if (left.path != right.path ||
            left.jsonPayload != right.jsonPayload) {
            return false;
        }
    }
    return true;
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
                std::is_same_v<T, jcut::MaterializeMaskMatteCommand> ||
                std::is_same_v<T, jcut::ReplaceSpeakerTitleClipsCommand> ||
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

    // Transcript-generated speaker introductions are one immutable collection
    // per source clip. Unlike Mask Mattes, several generated clips share the
    // same child lane, including when their title animations overlap.
    struct SpeakerTitleGroup {
        std::string parentId;
        std::size_t parentIndex = 0;
        std::vector<std::size_t> childIndices;
        int laneTrackId = 0;
    };
    std::vector<SpeakerTitleGroup> titleGroups;
    std::unordered_map<std::string, std::size_t> titleGroupIndexByParent;
    for (std::size_t clipIndex = 0;
         clipIndex < document->clips.size(); ++clipIndex) {
        EditorClip& title = document->clips[clipIndex];
        if (!isTranscriptGeneratedEditorTitle(title)) {
            continue;
        }
        const std::string parentId =
            trimmedEditorClipId(title.linkedSourceClipId);
        const auto parentIt = clipIndexById.find(parentId);
        if (parentId.empty() || parentIt == clipIndexById.end() ||
            isOwnedGeneratedEditorClip(
                document->clips[parentIt->second])) {
            continue;
        }
        title.linkedSourceClipId = parentId;
        title.syncLockedToSource = true;
        title.locked = true;
        auto [groupIt, inserted] =
            titleGroupIndexByParent.emplace(
                parentId, titleGroups.size());
        if (inserted) {
            titleGroups.push_back(
                {parentId, parentIt->second, {}, 0});
        }
        titleGroups[groupIt->second].childIndices.push_back(
            clipIndex);
    }

    std::unordered_map<int, std::vector<std::size_t>>
        currentOccupantsByTrackId;
    for (std::size_t clipIndex = 0;
         clipIndex < document->clips.size(); ++clipIndex) {
        currentOccupantsByTrackId[
            document->clips[clipIndex].trackId].push_back(clipIndex);
    }
    std::unordered_map<int, std::size_t> currentTrackIndexByTrackId;
    for (std::size_t trackIndex = 0;
         trackIndex < document->tracks.size(); ++trackIndex) {
        currentTrackIndexByTrackId.emplace(
            document->tracks[trackIndex].id, trackIndex);
    }
    std::unordered_set<int> claimedSpeakerTrackIds;
    std::unordered_set<int> obsoleteSpeakerTrackIds;
    for (SpeakerTitleGroup& group : titleGroups) {
        std::unordered_set<std::size_t> groupChildren(
            group.childIndices.begin(), group.childIndices.end());
        std::vector<int> dedicatedTrackIds;
        for (const auto& [trackId, occupants] :
             currentOccupantsByTrackId) {
            if (trackId ==
                    document->clips[group.parentIndex].trackId ||
                occupants.empty()) {
                continue;
            }
            const bool dedicated = std::all_of(
                occupants.begin(), occupants.end(),
                [&](std::size_t occupantIndex) {
                    return groupChildren.contains(occupantIndex);
                });
            if (dedicated) {
                dedicatedTrackIds.push_back(trackId);
            }
        }
        std::sort(dedicatedTrackIds.begin(),
                  dedicatedTrackIds.end());

        for (const EditorTrack& track : document->tracks) {
            if (isGeneratedEditorChildTrack(track) &&
                trimmedEditorClipId(track.parentClipId) ==
                    group.parentId &&
                claimedSpeakerTrackIds.find(track.id) ==
                    claimedSpeakerTrackIds.end()) {
                group.laneTrackId = track.id;
                break;
            }
        }
        if (group.laneTrackId == 0) {
            const auto dedicated = std::find_if(
                dedicatedTrackIds.begin(),
                dedicatedTrackIds.end(),
                [&](int trackId) {
                    return claimedSpeakerTrackIds.find(trackId) ==
                        claimedSpeakerTrackIds.end();
                });
            if (dedicated != dedicatedTrackIds.end()) {
                group.laneTrackId = *dedicated;
            }
        }
        if (group.laneTrackId == 0) {
            EditorTrack lane;
            lane.id = nextGeneratedTrackId++;
            lane.height = 44;
            lane.audioEnabled = false;
            lane.audioWaveformVisible = false;
            document->tracks.push_back(std::move(lane));
            group.laneTrackId = document->tracks.back().id;
            currentTrackIndexByTrackId.emplace(
                group.laneTrackId,
                document->tracks.size() - 1);
        }
        claimedSpeakerTrackIds.insert(group.laneTrackId);
        for (const int trackId : dedicatedTrackIds) {
            if (trackId != group.laneTrackId) {
                obsoleteSpeakerTrackIds.insert(trackId);
            }
        }

        for (const std::size_t childIndex :
             group.childIndices) {
            document->clips[childIndex].trackId =
                group.laneTrackId;
        }
        const auto laneIt =
            currentTrackIndexByTrackId.find(group.laneTrackId);
        if (laneIt != currentTrackIndexByTrackId.end()) {
            EditorTrack& lane = document->tracks[laneIt->second];
            lane.generatedChildTrack = true;
            lane.parentClipId = group.parentId;
            lane.childClipId = trimmedEditorClipId(
                document->clips[
                    group.childIndices.front()].persistentId);
            lane.label =
                "↳ Transcript • Speaker Introductions";
            lane.height = std::clamp(
                lane.height, kEditorTrackMinHeight, 56);
            lane.audioEnabled = false;
            lane.audioWaveformVisible = false;
        }
    }

    if (!obsoleteSpeakerTrackIds.empty()) {
        document->tracks.erase(
            std::remove_if(
                document->tracks.begin(),
                document->tracks.end(),
                [&](const EditorTrack& track) {
                    if (!obsoleteSpeakerTrackIds.contains(
                            track.id)) {
                        return false;
                    }
                    return std::none_of(
                        document->clips.begin(),
                        document->clips.end(),
                        [&](const EditorClip& clip) {
                            return clip.trackId == track.id;
                        });
                }),
            document->tracks.end());
    }

    // Re-run the adjacency projection now that speaker-title groups have been
    // materialized as child tracks.
    std::unordered_map<int, std::vector<int>>
        generatedTrackIdsBySourceTrackId;
    for (const EditorTrack& track : document->tracks) {
        if (!isGeneratedEditorChildTrack(track)) {
            continue;
        }
        const auto parentIt = clipIndexById.find(
            trimmedEditorClipId(track.parentClipId));
        if (parentIt != clipIndexById.end()) {
            generatedTrackIdsBySourceTrackId[
                document->clips[parentIt->second].trackId]
                .push_back(track.id);
        }
    }
    std::unordered_map<int, EditorTrack> trackById;
    trackById.reserve(document->tracks.size());
    for (const EditorTrack& track : document->tracks) {
        trackById.emplace(track.id, track);
    }
    std::vector<EditorTrack> tracksWithGeneratedChildren;
    tracksWithGeneratedChildren.reserve(document->tracks.size());
    std::unordered_set<int> placedGeneratedTrackIds;
    for (const EditorTrack& track : document->tracks) {
        if (isGeneratedEditorChildTrack(track)) {
            continue;
        }
        tracksWithGeneratedChildren.push_back(track);
        const auto children =
            generatedTrackIdsBySourceTrackId.find(track.id);
        if (children == generatedTrackIdsBySourceTrackId.end()) {
            continue;
        }
        for (const int childTrackId : children->second) {
            const auto child = trackById.find(childTrackId);
            if (child != trackById.end() &&
                placedGeneratedTrackIds.insert(
                    childTrackId).second) {
                tracksWithGeneratedChildren.push_back(
                    child->second);
            }
        }
    }
    for (const EditorTrack& track : document->tracks) {
        if (isGeneratedEditorChildTrack(track) &&
            placedGeneratedTrackIds.insert(track.id).second) {
            tracksWithGeneratedChildren.push_back(track);
        }
    }
    document->tracks = std::move(
        tracksWithGeneratedChildren);

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

EditorTransformKeyframe evaluateEditorClipTransformAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame)
{
    const std::int64_t frame = std::clamp<std::int64_t>(
        localFrame, 0, std::max(0, clip.durationFrames - 1));
    EditorTransformKeyframe offset;
    offset.frame = frame;
    if (!clip.transformKeyframes.empty()) {
        std::vector<EditorTransformKeyframe> keyframes =
            clip.transformKeyframes;
        std::sort(keyframes.begin(), keyframes.end(),
            [](const EditorTransformKeyframe& left,
               const EditorTransformKeyframe& right) {
                return left.frame < right.frame;
            });
        offset = keyframes.front();
        if (frame >= keyframes.back().frame) {
            offset = keyframes.back();
        } else if (frame > keyframes.front().frame) {
            for (std::size_t index = 1; index < keyframes.size(); ++index) {
                const EditorTransformKeyframe& previous = keyframes[index - 1];
                const EditorTransformKeyframe& current = keyframes[index];
                if (frame == current.frame) {
                    offset = current;
                    break;
                }
                if (frame < current.frame) {
                    if (!current.linearInterpolation ||
                        current.frame <= previous.frame) {
                        offset = previous;
                    } else {
                        const double amount =
                            static_cast<double>(frame - previous.frame) /
                            static_cast<double>(current.frame - previous.frame);
                        offset.frame = frame;
                        offset.title = previous.title;
                        offset.translationX = previous.translationX +
                            (current.translationX - previous.translationX) * amount;
                        offset.translationY = previous.translationY +
                            (current.translationY - previous.translationY) * amount;
                        offset.rotation = previous.rotation +
                            (current.rotation - previous.rotation) * amount;
                        offset.scaleX = previous.scaleX +
                            (current.scaleX - previous.scaleX) * amount;
                        offset.scaleY = previous.scaleY +
                            (current.scaleY - previous.scaleY) * amount;
                        offset.linearInterpolation = current.linearInterpolation;
                    }
                    break;
                }
            }
        }
    }
    const auto boundedScale = [](double value) {
        if (!std::isfinite(value)) {
            return 1.0;
        }
        if (std::abs(value) < 0.01) {
            return value < 0.0 ? -0.01 : 0.01;
        }
        return value;
    };
    offset.frame = frame;
    offset.translationX += clip.baseTranslationX;
    offset.translationY += clip.baseTranslationY;
    offset.rotation += clip.baseRotation;
    offset.scaleX = boundedScale(clip.baseScaleX * offset.scaleX);
    offset.scaleY = boundedScale(clip.baseScaleY * offset.scaleY);
    return offset;
}

EditorTransformKeyframe evaluateEditorClipRenderTransformAtTimelineFrame(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame)
{
    const auto evaluateOwn = [&](const EditorClip& candidate) {
        return evaluateEditorClipTransformAtLocalFrame(
            candidate,
            timelineFrame -
                static_cast<std::int64_t>(candidate.startFrame));
    };
    const bool followsSource =
        clip.sourceTransformLocked ||
        canonicalEditorClipRole(clip.clipRole) == "mask_matte";
    std::string sourceId =
        trimmedEditorClipId(clip.linkedSourceClipId);
    if (!followsSource || sourceId.empty()) {
        return evaluateOwn(clip);
    }

    std::unordered_set<std::string> visited;
    visited.insert(trimmedEditorClipId(clip.persistentId));
    const EditorClip* source = nullptr;
    while (!sourceId.empty()) {
        if (!visited.insert(sourceId).second) {
            return evaluateOwn(clip);
        }
        const auto sourceIt = std::find_if(
            document.clips.begin(),
            document.clips.end(),
            [&](const EditorClip& candidate) {
                return trimmedEditorClipId(
                           candidate.persistentId) == sourceId;
            });
        if (sourceIt == document.clips.end()) {
            return evaluateOwn(clip);
        }
        source = &*sourceIt;
        const std::string parentId =
            trimmedEditorClipId(source->linkedSourceClipId);
        if (!source->sourceTransformLocked || parentId.empty()) {
            break;
        }
        sourceId = parentId;
    }
    if (!source) {
        return evaluateOwn(clip);
    }
    EditorTransformKeyframe result = evaluateOwn(*source);
    result.frame = std::clamp<std::int64_t>(
        timelineFrame - static_cast<std::int64_t>(clip.startFrame),
        0,
        std::max(0, clip.durationFrames - 1));
    return result;
}

EditorTransformKeyframe
evaluateEditorClipBakedSpeakerFramingAtLocalFrame(
    const EditorClip& clip,
    double localFrame,
    int sourceWidth,
    int sourceHeight,
    int outputWidth,
    int outputHeight,
    bool* applied)
{
    speaker_framing::State state;
    state.enabled = clip.speakerFramingEnabled;
    state.bakedTargetXNorm =
        clip.speakerFramingBakedTargetXNorm;
    state.bakedTargetYNorm =
        clip.speakerFramingBakedTargetYNorm;
    state.bakedTargetBoxNorm =
        clip.speakerFramingBakedTargetBoxNorm;
    state.enabledKeyframes.reserve(
        clip.speakerFramingEnabledKeyframes.size());
    for (const EditorBoolKeyframe& keyframe :
         clip.speakerFramingEnabledKeyframes) {
        state.enabledKeyframes.push_back(
            {keyframe.frame, keyframe.enabled});
    }
    const auto copyTransforms =
        [](const std::vector<EditorTransformKeyframe>& source) {
            std::vector<speaker_framing::Transform> result;
            result.reserve(source.size());
            for (const EditorTransformKeyframe& keyframe : source) {
                result.push_back({
                    keyframe.frame,
                    keyframe.translationX,
                    keyframe.translationY,
                    keyframe.rotation,
                    keyframe.scaleX,
                    keyframe.scaleY,
                    keyframe.linearInterpolation});
            }
            return result;
        };
    state.framingKeyframes =
        copyTransforms(clip.speakerFramingKeyframes);
    state.targetKeyframes =
        copyTransforms(clip.speakerFramingTargetKeyframes);
    const bool enabled = speaker_framing::enabledAt(
        state,
        static_cast<std::int64_t>(std::floor(localFrame)));
    const bool hasBakedFraming =
        enabled && !state.framingKeyframes.empty();
    if (applied) {
        *applied = hasBakedFraming;
    }
    const speaker_framing::Transform value =
        speaker_framing::evaluateBaked(
            state,
            localFrame,
            {
                static_cast<double>(sourceWidth),
                static_cast<double>(sourceHeight)},
            {
                static_cast<double>(outputWidth),
                static_cast<double>(outputHeight)});
    EditorTransformKeyframe result;
    result.frame = value.frame;
    result.translationX = value.translationX;
    result.translationY = value.translationY;
    result.rotation = value.rotation;
    result.scaleX = value.scaleX;
    result.scaleY = value.scaleY;
    result.linearInterpolation = value.linearInterpolation;
    return result;
}

EditorTransformKeyframe
evaluateEditorClipSpeakerFramingForFaceBoxAtLocalFrame(
    const EditorClip& clip,
    double localFrame,
    double locationXNorm,
    double locationYNorm,
    double boxSizeNorm,
    double rotationDegrees,
    int sourceWidth,
    int sourceHeight,
    int outputWidth,
    int outputHeight,
    bool* applied)
{
    speaker_framing::State state;
    state.enabled = clip.speakerFramingEnabled;
    state.enabledKeyframes.reserve(
        clip.speakerFramingEnabledKeyframes.size());
    for (const EditorBoolKeyframe& keyframe :
         clip.speakerFramingEnabledKeyframes) {
        state.enabledKeyframes.push_back(
            {keyframe.frame, keyframe.enabled});
    }
    state.targetKeyframes.reserve(
        clip.speakerFramingTargetKeyframes.size());
    for (const EditorTransformKeyframe& keyframe :
         clip.speakerFramingTargetKeyframes) {
        state.targetKeyframes.push_back({
            keyframe.frame,
            keyframe.translationX,
            keyframe.translationY,
            keyframe.rotation,
            keyframe.scaleX,
            keyframe.scaleY,
            keyframe.linearInterpolation});
    }
    const bool enabled = speaker_framing::enabledAt(
        state,
        static_cast<std::int64_t>(std::floor(localFrame)));
    const speaker_framing::Transform target =
        speaker_framing::evaluate(
            state.targetKeyframes,
            localFrame,
            true,
            speaker_framing::Transform{
                0, 0.5, 0.35, 0.0, -1.0, -1.0, true});
    const bool canApply =
        enabled &&
        clip.speakerFramingKeyframes.empty() &&
        target.scaleX > 0.0 &&
        boxSizeNorm > 0.0;
    if (applied) {
        *applied = canApply;
    }
    const speaker_framing::Transform value =
        canApply
        ? speaker_framing::evaluateFaceBox(
              state,
              localFrame,
              locationXNorm,
              locationYNorm,
              boxSizeNorm,
              rotationDegrees,
              {
                  static_cast<double>(sourceWidth),
                  static_cast<double>(sourceHeight)},
              {
                  static_cast<double>(outputWidth),
                  static_cast<double>(outputHeight)})
        : speaker_framing::Transform{};
    EditorTransformKeyframe result;
    result.frame = value.frame;
    result.translationX = value.translationX;
    result.translationY = value.translationY;
    result.rotation = value.rotation;
    result.scaleX = value.scaleX;
    result.scaleY = value.scaleY;
    result.linearInterpolation = value.linearInterpolation;
    return result;
}

EditorTitleKeyframe evaluateEditorClipTitleAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame)
{
    const std::int64_t frame = std::clamp<std::int64_t>(
        localFrame, 0, std::max(0, clip.durationFrames - 1));
    EditorTitleKeyframe result;
    result.frame = frame;
    if (clip.titleKeyframes.empty()) {
        return result;
    }

    std::vector<EditorTitleKeyframe> keyframes = clip.titleKeyframes;
    std::stable_sort(keyframes.begin(), keyframes.end(),
        [](const EditorTitleKeyframe& left,
           const EditorTitleKeyframe& right) {
            return left.frame < right.frame;
        });
    result = keyframes.front();
    if (frame >= keyframes.back().frame) {
        result = keyframes.back();
    } else if (frame > keyframes.front().frame) {
        for (std::size_t index = 1; index < keyframes.size(); ++index) {
            const EditorTitleKeyframe& previous = keyframes[index - 1];
            const EditorTitleKeyframe& current = keyframes[index];
            if (frame == current.frame) {
                result = current;
                break;
            }
            if (frame < current.frame) {
                result = previous;
                if (current.linearInterpolation && current.frame > previous.frame) {
                    const double amount =
                        static_cast<double>(frame - previous.frame) /
                        static_cast<double>(current.frame - previous.frame);
                    const auto interpolate = [amount](double left, double right) {
                        return left + (right - left) * amount;
                    };
                    result.translationX = interpolate(
                        previous.translationX, current.translationX);
                    result.translationY = interpolate(
                        previous.translationY, current.translationY);
                    result.fontSize = interpolate(
                        previous.fontSize, current.fontSize);
                    result.opacity = interpolate(
                        previous.opacity, current.opacity);
                    result.windowWidth = interpolate(
                        previous.windowWidth, current.windowWidth);
                    result.vulkan3DEnabled =
                        previous.vulkan3DEnabled || current.vulkan3DEnabled;
                    result.vulkan3DExtrudeEnabled =
                        previous.vulkan3DExtrudeEnabled || current.vulkan3DExtrudeEnabled;
                    result.textExtrudeMode = previous.textExtrudeMode != "none"
                        ? previous.textExtrudeMode : current.textExtrudeMode;
                    result.vulkan3DExtrudeDepth = interpolate(
                        previous.vulkan3DExtrudeDepth, current.vulkan3DExtrudeDepth);
                    result.vulkan3DBevelScale = interpolate(
                        previous.vulkan3DBevelScale, current.vulkan3DBevelScale);
                    result.vulkan3DYawDegrees = interpolate(
                        previous.vulkan3DYawDegrees, current.vulkan3DYawDegrees);
                    result.vulkan3DPitchDegrees = interpolate(
                        previous.vulkan3DPitchDegrees, current.vulkan3DPitchDegrees);
                    result.vulkan3DRollDegrees = interpolate(
                        previous.vulkan3DRollDegrees, current.vulkan3DRollDegrees);
                    result.vulkan3DDepth = interpolate(
                        previous.vulkan3DDepth, current.vulkan3DDepth);
                    result.vulkan3DScale = interpolate(
                        previous.vulkan3DScale, current.vulkan3DScale);
                    result.textPatternScale = interpolate(
                        previous.textPatternScale, current.textPatternScale);
                    result.windowFramePatternScale = interpolate(
                        previous.windowFramePatternScale,
                        current.windowFramePatternScale);
                    result.linearInterpolation = true;
                }
                break;
            }
        }
    }
    result.frame = frame;
    result.fontSize = std::clamp(
        std::isfinite(result.fontSize) ? result.fontSize : 48.0,
        1.0, 1024.0);
    result.opacity = std::clamp(
        std::isfinite(result.opacity) ? result.opacity : 1.0,
        0.0, 1.0);
    return result;
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
        if (!isOwnedGeneratedEditorClip(clip)) {
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

void EditorRuntime::clearHistory()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_historyTransactionSnapshot = {};
    m_historyTransactionActive = false;
    m_historyTransactionHasChanges = false;
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

            // Transcript speaker titles are materialized output from the
            // generator. Selection remains available for inspection, but every
            // clip-targeted mutation must go through replacement/regeneration
            // of the complete source-owned set.
            if constexpr (requires { typedCommand.clipId; }) {
                if constexpr (
                    std::is_same_v<
                        std::remove_cvref_t<decltype(typedCommand.clipId)>,
                        int> &&
                    !std::is_same_v<T, SelectClipCommand>) {
                    if (const EditorClip* target =
                            findClip(&m_document.clips, typedCommand.clipId);
                        target && isTranscriptGeneratedEditorTitle(*target)) {
                        return {
                            false,
                            "generated transcript titles can only be changed by regenerating them"};
                    }
                }
            }

            if constexpr (std::is_same_v<T, TogglePlaybackCommand>) {
                const bool nextActive =
                    !m_document.transport.playbackActive;
                if (nextActive) {
                    m_document.transport.currentFrame =
                        playbackStartFrame(
                            m_document, timelineEndFrame());
                }
                m_document.transport.playbackActive = nextActive;
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
                if (typedCommand.active &&
                    !m_document.transport.playbackActive) {
                    m_document.transport.currentFrame =
                        playbackStartFrame(
                            m_document, timelineEndFrame());
                }
                m_document.transport.playbackActive = typedCommand.active;
                return {true, typedCommand.active ? "playback started" : "playback paused"};
            } else if constexpr (std::is_same_v<T, SetPlaybackSpeedCommand>) {
                m_document.transport.playbackSpeed =
                    std::clamp(typedCommand.speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
                return {true, "playback speed updated"};
            } else if constexpr (
                std::is_same_v<T, SetPlaybackLoopEnabledCommand>) {
                m_document.transport.playbackLoopEnabled =
                    typedCommand.enabled;
                return {true, typedCommand.enabled
                    ? "playback loop enabled"
                    : "playback loop disabled"};
            } else if constexpr (
                std::is_same_v<T, SetPreviewViewModeCommand>) {
                m_document.transport.previewViewMode =
                    typedCommand.mode == "audio" ? "audio" : "video";
                return {true, "preview view mode updated"};
            } else if constexpr (
                std::is_same_v<T, SetTransportAudioCommand>) {
                m_document.transport.audioMuted = typedCommand.muted;
                m_document.transport.audioVolume =
                    std::clamp(typedCommand.volume, 0.0f, 1.0f);
                return {true, "transport audio updated"};
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
                const int endFrame = timelineEndFrame();
                const auto ranges = normalizedPlaybackRangesCore(
                    m_document.exportRanges, endFrame);
                const int direction = typedCommand.delta < 0 ? -1 : 1;
                for (int step = 0;
                     step < std::abs(typedCommand.delta);
                     ++step) {
                    m_document.transport.currentFrame = static_cast<int>(
                        stepPlaybackFrameCore(
                            ranges,
                            m_document.transport.currentFrame,
                            direction,
                            endFrame));
                }
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
                    if (isOwnedGeneratedEditorClip(clip)) {
                        if (!persistentClipIdInSet(
                                selectedIds, clip.linkedSourceClipId)) {
                            return {false,
                                    "generated child must be cut with its source"};
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
            } else if constexpr (
                std::is_same_v<T, ReplaceSpeakerTitleClipsCommand>) {
                EditorClip* source = findClip(
                    &m_document.clips, typedCommand.sourceClipId);
                if (!source ||
                    canonicalEditorClipRole(source->clipRole) != "media" ||
                    source->persistentId.empty()) {
                    return {false, "speaker-title source clip not found"};
                }
                const std::string sourcePersistentId = source->persistentId;
                if (!typedCommand.generatedClips.empty()) {
                    source->transcriptOverlay.showSpeakerTitle = false;
                }
                std::unordered_set<int> previousTitleTrackIds;
                for (const EditorClip& clip : m_document.clips) {
                    if (isTranscriptGeneratedEditorTitle(clip) &&
                        trimmedEditorClipId(clip.linkedSourceClipId) ==
                            trimmedEditorClipId(sourcePersistentId)) {
                        previousTitleTrackIds.insert(clip.trackId);
                    }
                }
                std::vector<int> reusableTrackIds;
                for (const int trackId : previousTitleTrackIds) {
                    const bool dedicated = std::none_of(
                        m_document.clips.begin(),
                        m_document.clips.end(),
                        [&](const EditorClip& clip) {
                            return clip.trackId == trackId &&
                                !(isTranscriptGeneratedEditorTitle(clip) &&
                                  trimmedEditorClipId(
                                      clip.linkedSourceClipId) ==
                                      trimmedEditorClipId(
                                          sourcePersistentId));
                        });
                    if (dedicated && trackId != source->trackId) {
                        reusableTrackIds.push_back(trackId);
                    }
                }
                std::sort(reusableTrackIds.begin(),
                          reusableTrackIds.end());
                const std::size_t before = m_document.clips.size();
                m_document.clips.erase(
                    std::remove_if(
                        m_document.clips.begin(), m_document.clips.end(),
                        [&](const EditorClip& clip) {
                            return isTranscriptGeneratedEditorTitle(clip) &&
                                trimmedEditorClipId(
                                    clip.linkedSourceClipId) ==
                                    trimmedEditorClipId(
                                        sourcePersistentId);
                        }),
                    m_document.clips.end());
                const int removed = static_cast<int>(
                    before - m_document.clips.size());
                int targetTrackId = reusableTrackIds.empty()
                    ? 0 : reusableTrackIds.front();
                if (!typedCommand.generatedClips.empty() &&
                    targetTrackId == 0) {
                    EditorTrack track;
                    track.id = nextTrackId(m_document.tracks);
                    track.height = 44;
                    track.audioEnabled = false;
                    track.audioWaveformVisible = false;
                    m_document.tracks.push_back(std::move(track));
                    targetTrackId = m_document.tracks.back().id;
                }
                int inserted = 0;
                std::string representativeChildId;
                for (EditorClip generated : typedCommand.generatedClips) {
                    generated.durationFrames =
                        std::max(1, generated.durationFrames);
                    generated.clipRole = "speaker_title";
                    generated.linkedSourceClipId = sourcePersistentId;
                    generated.syncLockedToSource = true;
                    generated.mediaKind = "title";
                    generated.videoEnabled = true;
                    generated.audioEnabled = false;
                    generated.audioPresenceKnown = true;
                    generated.hasAudio = false;
                    generated.locked = true;
                    generated.id = nextClipId(m_document.clips);
                    generated.persistentId = uniquePersistentClipId(
                        m_document.clips, generated.id);
                    generated.trackId = targetTrackId;
                    generated.selected = false;
                    if (representativeChildId.empty()) {
                        representativeChildId =
                            generated.persistentId;
                    }
                    m_document.clips.push_back(std::move(generated));
                    ++inserted;
                }
                if (targetTrackId != 0) {
                    EditorTrack* track = findTrack(
                        &m_document.tracks, targetTrackId);
                    if (track) {
                        track->generatedChildTrack = true;
                        track->parentClipId =
                            trimmedEditorClipId(sourcePersistentId);
                        track->childClipId = representativeChildId;
                        track->label =
                            "↳ Transcript • Speaker Introductions";
                        track->height = std::clamp(
                            track->height, kEditorTrackMinHeight, 56);
                        track->audioEnabled = false;
                        track->audioWaveformVisible = false;
                    }
                }
                const std::unordered_set<int> retainedTrackIds = {
                    targetTrackId};
                m_document.tracks.erase(
                    std::remove_if(
                        m_document.tracks.begin(),
                        m_document.tracks.end(),
                        [&](const EditorTrack& track) {
                            if (retainedTrackIds.contains(track.id) ||
                                std::find(reusableTrackIds.begin(),
                                          reusableTrackIds.end(),
                                          track.id) ==
                                    reusableTrackIds.end()) {
                                return false;
                            }
                            return std::none_of(
                                m_document.clips.begin(),
                                m_document.clips.end(),
                                [&](const EditorClip& clip) {
                                    return clip.trackId == track.id;
                                });
                        }),
                    m_document.tracks.end());
                if (removed == 0 && inserted == 0) {
                    return {false, "no speaker introductions were generated"};
                }
                sortClipsByTimeline(&m_document.clips);
                return {true,
                        "replaced speaker introductions (" +
                            std::to_string(inserted) + " generated, " +
                            std::to_string(removed) + " removed)"};
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
                    if (isOwnedGeneratedEditorClip(clip)) {
                        if (!persistentClipIdInSet(
                                selectedIds, clip.linkedSourceClipId)) {
                            return {false,
                                    "generated child must be deleted with its source"};
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
            } else if constexpr (std::is_same_v<T, SetClipProxyCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                    return {false, "mask matte proxy follows its source"};
                }
                const std::string nextPath = trimmed(
                    typedCommand.proxyPath);
                const bool nextUseProxy =
                    !nextPath.empty() && typedCommand.useProxy;
                if (clip->proxyPath == nextPath &&
                    clip->useProxy == nextUseProxy) {
                    return {false, "clip proxy is unchanged"};
                }
                clip->proxyPath = nextPath;
                clip->useProxy = nextUseProxy;
                normalizeMaskMatteParentCaches(&m_document);
                return {true, nextPath.empty()
                    ? "clip proxy association cleared"
                    : (nextUseProxy ? "clip proxy enabled"
                                    : "clip proxy disabled")};
            } else if constexpr (
                std::is_same_v<T, RefreshClipMetadataCommand>) {
                if (typedCommand.updates.empty()) {
                    return {false, "no clip metadata updates supplied"};
                }
                bool changed = false;
                int updatedCount = 0;
                for (const EditorClipMetadataUpdate& update :
                     typedCommand.updates) {
                    EditorClip* clip = findClip(
                        &m_document.clips, update.clipId);
                    if (!clip ||
                        canonicalEditorClipRole(clip->clipRole) ==
                            "mask_matte") {
                        continue;
                    }
                    const std::string mediaKind =
                        update.mediaKind.empty()
                        ? clip->mediaKind
                        : update.mediaKind;
                    const double sourceFps =
                        std::isfinite(update.sourceFps) &&
                            update.sourceFps > 0.001
                        ? update.sourceFps
                        : 30.0;
                    const std::int64_t sourceDuration =
                        std::max<std::int64_t>(
                            0, update.sourceDurationFrames);
                    const int duration =
                        std::max(1, update.durationFrames);
                    const std::int64_t sourceInFrame =
                        sourceDuration > 0
                        ? std::clamp<std::int64_t>(
                              clip->sourceInFrame,
                              0,
                              sourceDuration - 1)
                        : std::max<std::int64_t>(
                              0, clip->sourceInFrame);
                    changed = changed ||
                        clip->mediaKind != mediaKind ||
                        clip->hasAudio != update.hasAudio ||
                        std::abs(clip->sourceFps - sourceFps) > 0.0001 ||
                        clip->sourceDurationFrames != sourceDuration ||
                        clip->sourceInFrame != sourceInFrame ||
                        clip->durationFrames != duration;
                    clip->mediaKind = mediaKind;
                    clip->hasAudio = update.hasAudio;
                    clip->sourceFps = sourceFps;
                    clip->sourceDurationFrames = sourceDuration;
                    clip->sourceInFrame = sourceInFrame;
                    clip->durationFrames = duration;
                    ++updatedCount;
                }
                if (!changed) {
                    return {false, updatedCount > 0
                        ? "clip metadata is unchanged"
                        : "no matching clips found"};
                }
                normalizeMaskMatteParentCaches(&m_document);
                return {true,
                        std::to_string(updatedCount) +
                            " clip metadata record" +
                            (updatedCount == 1 ? "" : "s") +
                            " refreshed"};
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
                std::is_same_v<T, SetSelectedClipsLockedCommand>) {
                bool found = false;
                bool changed = false;
                for (EditorClip& clip : m_document.clips) {
                    if (!clip.selected ||
                        isOwnedGeneratedEditorClip(clip)) {
                        continue;
                    }
                    found = true;
                    if (clip.locked != typedCommand.locked) {
                        clip.locked = typedCommand.locked;
                        changed = true;
                    }
                }
                if (!found) {
                    return {false, "no editable clips selected"};
                }
                if (!changed) {
                    return {false, typedCommand.locked
                        ? "selected clips are already locked"
                        : "selected clips are already unlocked"};
                }
                return {true, typedCommand.locked
                    ? "selected clips locked"
                    : "selected clips unlocked"};
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
                if (isOwnedGeneratedEditorClip(*anchor)) {
                    return {false,
                            "generated child must be moved with its source"};
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
                        isOwnedGeneratedEditorClip(clip)) {
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
                        isOwnedGeneratedEditorClip(clip)) {
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
                clip->brightness = std::clamp(typedCommand.brightness, -10.0, 10.0);
                clip->contrast = std::clamp(typedCommand.contrast, -10.0, 10.0);
                clip->saturation = std::clamp(typedCommand.saturation, -10.0, 10.0);
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
                keyframe.brightness = std::clamp(keyframe.brightness, -10.0, 10.0);
                keyframe.contrast = std::clamp(keyframe.contrast, -10.0, 10.0);
                keyframe.saturation = std::clamp(keyframe.saturation, -10.0, 10.0);
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
                case EditorKeyframeChannel::SpeakerFramingEnabled:
                    removed = removeKeyframeAtFrame(
                        &clip->speakerFramingEnabledKeyframes,
                        typedCommand.frame);
                    break;
                case EditorKeyframeChannel::SpeakerFraming:
                    removed = removeKeyframeAtFrame(
                        &clip->speakerFramingKeyframes,
                        typedCommand.frame);
                    break;
                case EditorKeyframeChannel::SpeakerFramingTarget:
                    removed = removeKeyframeAtFrame(
                        &clip->speakerFramingTargetKeyframes,
                        typedCommand.frame);
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
            } else if constexpr (
                std::is_same_v<T, SetClipSourceTransformLockedCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (clip->locked) {
                    return {false, "clip is locked"};
                }
                if (!editorClipHasVisuals(*clip)) {
                    return {false, "clip has no visual transform"};
                }
                if (typedCommand.locked &&
                    trimmedEditorClipId(
                        clip->linkedSourceClipId).empty()) {
                    return {false, "clip has no linked source"};
                }
                clip->sourceTransformLocked =
                    typedCommand.locked;
                return {
                    true,
                    typedCommand.locked
                        ? "clip transform locked to source"
                        : "clip source transform unlocked"};
            } else if constexpr (
                std::is_same_v<T, SetClipSpeakerFramingCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                if (clip->locked) {
                    return {false, "clip is locked"};
                }
                if (!editorClipHasVisuals(*clip)) {
                    return {false, "clip has no visual framing"};
                }
                clip->speakerFramingEnabled = typedCommand.enabled;
                clip->speakerFramingBakedTargetXNorm =
                    std::clamp(
                        typedCommand.bakedTargetXNorm, 0.0, 1.0);
                clip->speakerFramingBakedTargetYNorm =
                    std::clamp(
                        typedCommand.bakedTargetYNorm, 0.0, 1.0);
                clip->speakerFramingBakedTargetBoxNorm =
                    std::clamp(
                        typedCommand.bakedTargetBoxNorm, -1.0, 1.0);
                clip->speakerFramingMinConfidence =
                    std::clamp(
                        typedCommand.minConfidence, 0.0, 1.0);
                clip->speakerFramingManualTrackId =
                    std::max(-1, typedCommand.manualTrackId);
                clip->speakerFramingManualStreamId =
                    trimmed(typedCommand.manualStreamId);
                clip->speakerFramingCenterSmoothingFrames =
                    std::clamp(
                        typedCommand.centerSmoothingFrames, 0, 500);
                clip->speakerFramingZoomSmoothingFrames =
                    std::clamp(
                        typedCommand.zoomSmoothingFrames, 0, 500);
                clip->speakerFramingSmoothingMode =
                    std::clamp(typedCommand.smoothingMode, 0, 2);
                clip->speakerFramingCenterSmoothingStrength =
                    std::clamp(
                        typedCommand.centerSmoothingStrength,
                        0.0,
                        5.0);
                clip->speakerFramingZoomSmoothingStrength =
                    std::clamp(
                        typedCommand.zoomSmoothingStrength,
                        0.0,
                        5.0);
                clip->speakerFramingGapHoldFrames =
                    std::clamp(
                        typedCommand.gapHoldFrames, 0, 240);
                return {true, "speaker framing settings updated"};
            } else if constexpr (
                std::is_same_v<
                    T,
                    SetClipSpeakerSectionMinimumWordsCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                const int minimumWords =
                    std::clamp(typedCommand.minimumWords, 0, 1000);
                if (clip->speakerSectionMinimumWords == minimumWords) {
                    return {
                        false,
                        "speaker section minimum is unchanged"};
                }
                clip->speakerSectionMinimumWords = minimumWords;
                return {
                    true,
                    "speaker section minimum updated"};
            } else if constexpr (
                std::is_same_v<
                    T,
                    UpsertSpeakerFramingEnabledKeyframeCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorBoolKeyframe keyframe = typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame,
                    0,
                    std::max(0, clip->durationFrames - 1));
                upsertKeyframe(
                    &clip->speakerFramingEnabledKeyframes,
                    std::move(keyframe));
                return {
                    true,
                    "speaker framing enable keyframe updated"};
            } else if constexpr (
                std::is_same_v<
                    T,
                    UpsertSpeakerFramingKeyframeCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorTransformKeyframe keyframe =
                    typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame,
                    0,
                    std::max(0, clip->durationFrames - 1));
                keyframe.rotation =
                    std::clamp(keyframe.rotation, -360.0, 360.0);
                keyframe.scaleX = normalizedScale(keyframe.scaleX);
                keyframe.scaleY = normalizedScale(keyframe.scaleY);
                upsertKeyframe(
                    &clip->speakerFramingKeyframes,
                    std::move(keyframe));
                return {true, "speaker framing keyframe updated"};
            } else if constexpr (
                std::is_same_v<
                    T,
                    UpsertSpeakerFramingTargetKeyframeCommand>) {
                EditorClip* clip = findClip(
                    &m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                EditorTransformKeyframe keyframe =
                    typedCommand.keyframe;
                keyframe.frame = std::clamp<std::int64_t>(
                    keyframe.frame,
                    0,
                    std::max(0, clip->durationFrames - 1));
                keyframe.translationX =
                    std::clamp(keyframe.translationX, 0.0, 1.0);
                keyframe.translationY =
                    std::clamp(keyframe.translationY, 0.0, 1.0);
                keyframe.rotation = 0.0;
                keyframe.scaleX =
                    std::clamp(keyframe.scaleX, -1.0, 1.0);
                keyframe.scaleY = keyframe.scaleX;
                upsertKeyframe(
                    &clip->speakerFramingTargetKeyframes,
                    std::move(keyframe));
                return {
                    true,
                    "speaker framing target keyframe updated"};
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
            } else if constexpr (std::is_same_v<T, CommitPreviewTransformCommand>) {
                EditorClip* requestedClip = findClip(&m_document.clips, typedCommand.clipId);
                if (!requestedClip) {
                    return {false, "clip not found"};
                }
                EditorClip* clip = requestedClip;
                if (canonicalEditorClipRole(requestedClip->clipRole) == "mask_matte") {
                    const std::string ownerId =
                        trimmedEditorClipId(requestedClip->linkedSourceClipId);
                    const auto owner = std::find_if(
                        m_document.clips.begin(), m_document.clips.end(),
                        [&](const EditorClip& candidate) {
                            return trimmedEditorClipId(candidate.persistentId) == ownerId &&
                                canonicalEditorClipRole(candidate.clipRole) != "mask_matte" &&
                                candidate.mediaKind == "video";
                        });
                    if (owner == m_document.clips.end()) {
                        return {false, "mask matte transform owner not found"};
                    }
                    clip = &*owner;
                }
                if (!editorClipHasVisuals(*clip)) {
                    return {false, "clip has no visual transform"};
                }
                const std::int64_t localFrame = std::clamp<std::int64_t>(
                    typedCommand.localFrame, 0,
                    std::max(0, clip->durationFrames - 1));
                if (localFrame > 0 && std::none_of(
                        clip->transformKeyframes.begin(), clip->transformKeyframes.end(),
                        [](const EditorTransformKeyframe& keyframe) {
                            return keyframe.frame == 0;
                        })) {
                    clip->transformKeyframes.push_back(EditorTransformKeyframe{});
                }
                EditorTransformKeyframe keyframe;
                keyframe.frame = localFrame;
                keyframe.translationX = typedCommand.translationX - clip->baseTranslationX;
                keyframe.translationY = typedCommand.translationY - clip->baseTranslationY;
                keyframe.rotation = std::clamp(
                    typedCommand.rotation - clip->baseRotation, -360.0, 360.0);
                keyframe.scaleX = normalizedScale(
                    normalizedScale(typedCommand.scaleX) /
                    normalizedScale(clip->baseScaleX));
                keyframe.scaleY = normalizedScale(
                    normalizedScale(typedCommand.scaleY) /
                    normalizedScale(clip->baseScaleY));
                for (const EditorTransformKeyframe& existing : clip->transformKeyframes) {
                    if (existing.frame > localFrame) {
                        keyframe.linearInterpolation = existing.linearInterpolation;
                        break;
                    }
                }
                upsertKeyframe(&clip->transformKeyframes, std::move(keyframe));
                normalizeMaskMatteParentCaches(&m_document);
                return {true, "preview transform committed"};
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
                clip->edgeFillEffect =
                    typedCommand.edgeFillEffect.empty() ? "none" : typedCommand.edgeFillEffect;
                clip->edgeFillPixels = std::clamp(typedCommand.edgeFillPixels, 1, 512);
                clip->edgeFillPower = std::clamp(typedCommand.edgeFillPower, 0.25, 8.0);
                clip->edgeFillOpacity = std::clamp(typedCommand.edgeFillOpacity, 0.0, 1.0);
                clip->edgeFillBrightness = std::clamp(typedCommand.edgeFillBrightness, -1.0, 1.0);
                clip->edgeFillSaturation = std::clamp(typedCommand.edgeFillSaturation, 0.0, 3.0);
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
                clip->effectSkipAwareTiming = typedCommand.skipAwareTiming;
                clip->differenceReferenceFrames = std::clamp(
                    typedCommand.differenceReferenceFrames, 1, 300);
                clip->differenceThreshold = std::clamp(
                    typedCommand.differenceThreshold, 0.0, 1.0);
                clip->differenceSoftness = std::clamp(
                    typedCommand.differenceSoftness, 0.0, 1.0);
                clip->temporalEchoCount = std::clamp(
                    typedCommand.temporalEchoCount, 1, 12);
                clip->temporalEchoSpacingFrames = std::clamp(
                    typedCommand.temporalEchoSpacingFrames, 1, 120);
                clip->temporalEchoDecay = std::clamp(
                    typedCommand.temporalEchoDecay, 0.0, 1.0);
                static constexpr std::array<std::string_view, 6> kTilingPatterns = {
                    "grid", "encircle", "spiral_xy", "spiral_xz", "spiral_yz", "diamond"};
                clip->tilingPattern = std::find(
                    kTilingPatterns.begin(), kTilingPatterns.end(), typedCommand.tilingPattern) !=
                        kTilingPatterns.end()
                    ? typedCommand.tilingPattern
                    : "grid";
                clip->tilingSpacing = std::clamp(typedCommand.tilingSpacing, 0.1, 8.0);
                clip->tilingWrap = typedCommand.tilingWrap;
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
                clip->maskRepeatDeltaX = std::clamp(
                    typedCommand.repeatDeltaX, -100000.0, 100000.0);
                clip->maskRepeatDeltaY = std::clamp(
                    typedCommand.repeatDeltaY, -100000.0, 100000.0);
                clip->maskDilate = std::clamp(typedCommand.dilate, 0.0, 200.0);
                clip->maskErode = std::clamp(typedCommand.erode, 0.0, 200.0);
                clip->maskBlur = std::clamp(typedCommand.blur, 0.0, 200.0);
                clip->maskInvert = typedCommand.invert;
                clip->maskShowOnly = typedCommand.showOnly;
                clip->maskOpacity = std::clamp(typedCommand.opacity, 0.0, 1.0);
                clip->maskGradeEnabled = typedCommand.gradeEnabled;
                clip->maskGradeBrightness = std::clamp(
                    typedCommand.gradeBrightness, -1.0, 1.0);
                clip->maskGradeContrast = std::clamp(
                    typedCommand.gradeContrast, 0.0, 4.0);
                clip->maskGradeSaturation = std::clamp(
                    typedCommand.gradeSaturation, 0.0, 4.0);
                clip->maskGradeCurvePointsR = sanitizeEditorGradingCurve(
                    typedCommand.gradeCurvePointsR);
                clip->maskGradeCurvePointsG = sanitizeEditorGradingCurve(
                    typedCommand.gradeCurvePointsG);
                clip->maskGradeCurvePointsB = sanitizeEditorGradingCurve(
                    typedCommand.gradeCurvePointsB);
                clip->maskGradeCurvePointsLuma = sanitizeEditorGradingCurve(
                    typedCommand.gradeCurvePointsLuma);
                clip->maskGradeCurveSmoothingEnabled =
                    typedCommand.gradeCurveSmoothingEnabled;
                clip->maskDropShadowEnabled = typedCommand.dropShadowEnabled;
                clip->maskDropShadowRadius = std::clamp(
                    typedCommand.dropShadowRadius, 0.0, 200.0);
                clip->maskDropShadowOffsetX = std::clamp(
                    typedCommand.dropShadowOffsetX, -500.0, 500.0);
                clip->maskDropShadowOffsetY = std::clamp(
                    typedCommand.dropShadowOffsetY, -500.0, 500.0);
                clip->maskDropShadowOpacity = std::clamp(
                    typedCommand.dropShadowOpacity, 0.0, 1.0);
                return {true, "clip mask updated"};
            } else if constexpr (std::is_same_v<T, MaterializeMaskMatteCommand>) {
                EditorClip* source = findClip(&m_document.clips, typedCommand.sourceClipId);
                if (!source || canonicalEditorClipRole(source->clipRole) != "media" ||
                    source->mediaKind != "video" || source->persistentId.empty()) {
                    return {false, "source video clip not found"};
                }
                const std::string directory = trimmed(typedCommand.sidecarDirectory);
                const std::string sidecarId = trimmed(typedCommand.sidecarId);
                if (directory.empty() || sidecarId.empty()) {
                    return {false, "mask sidecar is invalid"};
                }
                const std::string sourcePersistentId = source->persistentId;
                const auto existing = std::find_if(
                    m_document.clips.begin(), m_document.clips.end(),
                    [&](const EditorClip& candidate) {
                        return canonicalEditorClipRole(candidate.clipRole) == "mask_matte" &&
                            trimmedEditorClipId(candidate.linkedSourceClipId) ==
                                trimmedEditorClipId(sourcePersistentId) &&
                            (candidate.generatedFromMaskId == sidecarId ||
                             candidate.maskFramesDir == directory);
                    });
                if (existing != m_document.clips.end()) {
                    for (EditorClip& clip : m_document.clips) clip.selected = false;
                    existing->selected = true;
                    return {false, "mask matte already exists"};
                }

                EditorClip child = *source;
                int nextId = 1;
                for (EditorClip& clip : m_document.clips) {
                    nextId = std::max(nextId, clip.id + 1);
                    clip.selected = false;
                }
                child.id = nextId;
                child.persistentId = sourcePersistentId + "-mask-" + sidecarId;
                int suffix = 2;
                const auto persistentExists = [&](const std::string& id) {
                    return std::any_of(
                        m_document.clips.begin(), m_document.clips.end(),
                        [&](const EditorClip& clip) { return clip.persistentId == id; });
                };
                const std::string persistentBase = child.persistentId;
                while (persistentExists(child.persistentId)) {
                    child.persistentId = persistentBase + "-" + std::to_string(suffix++);
                }
                child.clipRole = "mask_matte";
                child.linkedSourceClipId = sourcePersistentId;
                child.generatedFromMaskId = sidecarId;
                child.syncLockedToSource = true;
                child.sourceTransformLocked = true;
                child.label = source->label.empty() ? "Generated Mask"
                    : source->label + " · " +
                        (typedCommand.sidecarLabel.empty()
                             ? std::string("Generated")
                             : typedCommand.sidecarLabel) + " Mask";
                child.selected = true;
                child.locked = true;
                child.videoEnabled = true;
                child.hasAudio = false;
                child.audioPresenceKnown = true;
                child.audioEnabled = false;
                child.audioLinkedToVideo = false;
                child.audioBusId.clear();
                child.audioSourcePath.clear();
                child.audioSourceStatus = "generated";
                child.audioStreamIndex = -1;
                child.audioGain = 1.0;
                child.audioPan = 0.0;
                child.audioSolo = false;
                child.maskEnabled = true;
                child.maskFramesDir = directory;
                child.maskShowOnly = false;
                child.maskForegroundLayerEnabled = false;
                child.effectPreset = "none";
                child.effectRows = 32;
                child.effectSpeed = 1.0;
                child.effectScale = 1.0;
                child.effectAlternateDirection = true;
                child.effectSkipAwareTiming = true;
                child.maskRepeatEnabled = false;
                child.maskRepeatDeltaX = 160.0;
                child.maskRepeatDeltaY = 0.0;
                child.differenceReferenceFrames = 1;
                child.differenceThreshold = 0.10;
                child.differenceSoftness = 0.05;
                child.temporalEchoCount = 4;
                child.temporalEchoSpacingFrames = 2;
                child.temporalEchoDecay = 0.65;
                child.tilingPattern = "grid";
                child.tilingSpacing = 1.0;
                child.tilingWrap = true;
                child.opacity = 1.0;
                child.opacityKeyframes.clear();
                child.correctionPolygons.clear();
                child.titleKeyframes.clear();
                child.transcriptOverlay = {};
                child.brightness = source->maskGradeEnabled
                    ? source->maskGradeBrightness : 0.0;
                child.contrast = source->maskGradeEnabled
                    ? source->maskGradeContrast : 1.0;
                child.saturation = source->maskGradeEnabled
                    ? source->maskGradeSaturation : 1.0;
                child.gradingKeyframes.clear();
                child.maskGradeEnabled = false;
                child.maskGradeBrightness = 0.0;
                child.maskGradeContrast = 1.0;
                child.maskGradeSaturation = 1.0;
                child.maskGradeCurvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
                child.maskGradeCurvePointsG = child.maskGradeCurvePointsR;
                child.maskGradeCurvePointsB = child.maskGradeCurvePointsR;
                child.maskGradeCurvePointsLuma = child.maskGradeCurvePointsR;
                child.maskGradeCurveSmoothingEnabled = true;
                const auto effectiveZ = [](const EditorClip& clip) {
                    return clip.zLevel != std::numeric_limits<int>::min()
                        ? clip.zLevel : -std::max(0, clip.trackId - 1) * 100;
                };
                int nextZ = effectiveZ(*source) + 1;
                for (const EditorClip& candidate : m_document.clips) {
                    if (canonicalEditorClipRole(candidate.clipRole) == "mask_matte" &&
                        trimmedEditorClipId(candidate.linkedSourceClipId) ==
                            trimmedEditorClipId(sourcePersistentId)) {
                        nextZ = std::max(nextZ, effectiveZ(candidate) + 1);
                    }
                }
                child.zLevel = nextZ;
                child.zLevelUserSet = false;
                m_document.clips.push_back(std::move(child));
                return {true, "mask matte materialized"};
            } else if constexpr (std::is_same_v<T, SetClipZLevelCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) return {false, "clip not found"};
                clip->zLevel = typedCommand.automatic
                    ? std::numeric_limits<int>::min()
                    : std::clamp(typedCommand.zLevel, -100000, 100000);
                clip->zLevelUserSet = !typedCommand.automatic;
                return {true, "clip z level updated"};
            } else if constexpr (std::is_same_v<T, SetClipTranscriptOverlayCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                clip->transcriptOverlay = typedCommand.overlay;
                clip->transcriptOverlay.backgroundOpacity =
                    std::clamp(clip->transcriptOverlay.backgroundOpacity, 0.0, 1.0);
                clip->transcriptOverlay.backgroundCornerRadius =
                    std::clamp(clip->transcriptOverlay.backgroundCornerRadius, 0.0, 128.0);
                clip->transcriptOverlay.backgroundPadding =
                    std::clamp(clip->transcriptOverlay.backgroundPadding, 0.0, 400.0);
                clip->transcriptOverlay.backgroundFrameOpacity =
                    std::clamp(clip->transcriptOverlay.backgroundFrameOpacity, 0.0, 1.0);
                clip->transcriptOverlay.backgroundFrameWidth =
                    std::clamp(clip->transcriptOverlay.backgroundFrameWidth, 0.0, 120.0);
                clip->transcriptOverlay.backgroundFrameGap =
                    std::clamp(clip->transcriptOverlay.backgroundFrameGap, 0.0, 200.0);
                clip->transcriptOverlay.shadowOpacity =
                    std::clamp(clip->transcriptOverlay.shadowOpacity, 0.0, 1.0);
                clip->transcriptOverlay.shadowOffsetX =
                    std::clamp(clip->transcriptOverlay.shadowOffsetX, -128.0, 128.0);
                clip->transcriptOverlay.shadowOffsetY =
                    std::clamp(clip->transcriptOverlay.shadowOffsetY, -128.0, 128.0);
                clip->transcriptOverlay.textOutlineWidth =
                    std::clamp(clip->transcriptOverlay.textOutlineWidth, 0.0, 24.0);
                clip->transcriptOverlay.textOutlineOpacity =
                    std::clamp(clip->transcriptOverlay.textOutlineOpacity, 0.0, 1.0);
                if (clip->transcriptOverlay.textExtrudeMode != "stacked_copies" &&
                    clip->transcriptOverlay.textExtrudeMode != "eroded_solid") {
                    clip->transcriptOverlay.textExtrudeMode = "none";
                }
                clip->transcriptOverlay.textExtrudeDepth =
                    std::clamp(clip->transcriptOverlay.textExtrudeDepth, 0.0, 2.0);
                clip->transcriptOverlay.textExtrudeBevelScale =
                    std::clamp(clip->transcriptOverlay.textExtrudeBevelScale, 0.0, 2.0);
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
                const auto normalizeMaterial = [](std::string value) {
                    if (value == "neon" || value == "diagonal_stripes" ||
                        value == "grid" || value == "image_pattern") return value;
                    return std::string("solid");
                };
                keyframe.textMaterialStyle = normalizeMaterial(
                    keyframe.textMaterialStyle);
                keyframe.windowFrameMaterialStyle = normalizeMaterial(
                    keyframe.windowFrameMaterialStyle);
                keyframe.textPatternScale = std::clamp(
                    keyframe.textPatternScale, 0.1, 8.0);
                keyframe.dropShadowOpacity = std::clamp(
                    keyframe.dropShadowOpacity, 0.0, 1.0);
                keyframe.dropShadowOffsetX = std::clamp(
                    keyframe.dropShadowOffsetX, -200.0, 200.0);
                keyframe.dropShadowOffsetY = std::clamp(
                    keyframe.dropShadowOffsetY, -200.0, 200.0);
                keyframe.windowOpacity = std::clamp(
                    keyframe.windowOpacity, 0.0, 1.0);
                keyframe.windowPadding = std::clamp(
                    keyframe.windowPadding, 0.0, 400.0);
                keyframe.windowWidth = std::clamp(
                    keyframe.windowWidth, 0.0, 10000.0);
                keyframe.windowFrameOpacity = std::clamp(
                    keyframe.windowFrameOpacity, 0.0, 1.0);
                keyframe.windowFrameWidth = std::clamp(
                    keyframe.windowFrameWidth, 0.0, 120.0);
                keyframe.windowFrameGap = std::clamp(
                    keyframe.windowFrameGap, 0.0, 200.0);
                keyframe.windowFramePatternScale = std::clamp(
                    keyframe.windowFramePatternScale, 0.1, 8.0);
                if (keyframe.textExtrudeMode != "stacked_copies" &&
                    keyframe.textExtrudeMode != "eroded_solid") {
                    keyframe.textExtrudeMode = "none";
                }
                keyframe.vulkan3DExtrudeDepth = std::clamp(
                    keyframe.vulkan3DExtrudeDepth, 0.0, 2.0);
                keyframe.vulkan3DBevelScale = std::clamp(
                    keyframe.vulkan3DBevelScale, 0.0, 2.0);
                keyframe.vulkan3DYawDegrees = std::clamp(
                    keyframe.vulkan3DYawDegrees, -360.0, 360.0);
                keyframe.vulkan3DPitchDegrees = std::clamp(
                    keyframe.vulkan3DPitchDegrees, -360.0, 360.0);
                keyframe.vulkan3DRollDegrees = std::clamp(
                    keyframe.vulkan3DRollDegrees, -360.0, 360.0);
                keyframe.vulkan3DDepth = std::clamp(
                    keyframe.vulkan3DDepth, -10.0, 10.0);
                keyframe.vulkan3DScale = std::clamp(
                    keyframe.vulkan3DScale, 0.01, 10.0);
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
            } else if constexpr (std::is_same_v<T, SetClipCorrectionPolygonsCommand>) {
                EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                if (!clip) {
                    return {false, "clip not found"};
                }
                std::vector<EditorCorrectionPolygon> polygons = typedCommand.polygons;
                for (EditorCorrectionPolygon& polygon : polygons) {
                    polygon.startFrame = std::max<std::int64_t>(0, polygon.startFrame);
                    if (polygon.endFrame >= 0) {
                        polygon.endFrame = std::max(polygon.startFrame, polygon.endFrame);
                    }
                    for (EditorPoint& point : polygon.pointsNormalized) {
                        point.x = std::clamp(point.x, 0.0, 1.0);
                        point.y = std::clamp(point.y, 0.0, 1.0);
                    }
                }
                clip->correctionPolygons = std::move(polygons);
                return {true, "correction polygons updated"};
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
            } else if constexpr (std::is_same_v<T, SetAudioDynamicsCommand>) {
                const audio::DynamicsSettingsCore normalized =
                    audio::normalizedDynamicsSettingsCore(
                        typedCommand.settings);
                if (m_document.audioDynamics == normalized) {
                    return {false, "audio dynamics unchanged"};
                }
                m_document.audioDynamics = normalized;
                return {true, "audio dynamics updated"};
            } else if constexpr (std::is_same_v<T, SetAudioTreatmentCommand>) {
                if (m_document.audioTreatment == typedCommand.treatment) {
                    return {false, "audio treatment unchanged"};
                }
                m_document.audioTreatment = typedCommand.treatment;
                return {true, "audio treatment updated"};
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
                    typedCommand.endFrame < typedCommand.startFrame) {
                    return {false, "invalid export range"};
                }
                m_document.exportRanges = {{typedCommand.startFrame, typedCommand.endFrame}};
                synchronizeExportRequestRanges(&m_document);
                return {true, "export range updated"};
            } else if constexpr (
                std::is_same_v<T, SetExportRangesCommand>) {
                if (typedCommand.ranges.empty()) {
                    return {false, "export ranges are empty"};
                }
                std::vector<export_range::Range> ranges =
                    sharedExportRanges(typedCommand.ranges);
                export_range::normalize(
                    &ranges,
                    qtTimelineExtentFrame(m_document));
                std::vector<EditorExportRange> next;
                storeSharedExportRanges(ranges, &next);
                if (next == m_document.exportRanges) {
                    return {false, "export ranges unchanged"};
                }
                m_document.exportRanges = std::move(next);
                synchronizeExportRequestRanges(&m_document);
                return {true, "export ranges updated"};
            } else if constexpr (std::is_same_v<T, EditExportRangesCommand>) {
                const std::int64_t extent =
                    qtTimelineExtentFrame(m_document);
                std::vector<export_range::Range> ranges =
                    sharedExportRanges(m_document.exportRanges);
                if (!export_range::apply(
                        &ranges,
                        extent,
                        typedCommand.edit,
                        typedCommand.frame)) {
                    return {
                        false,
                        "export range cannot split at playhead"};
                }
                storeSharedExportRanges(
                    ranges, &m_document.exportRanges);
                synchronizeExportRequestRanges(&m_document);
                return {true, "export ranges updated"};
            } else if constexpr (std::is_same_v<T, SetWaveformVisibleCommand>) {
                m_document.panels.showWaveform = typedCommand.visible;
                return {true, "waveform visibility updated"};
            } else if constexpr (std::is_same_v<T, SetTranscriptVisibleCommand>) {
                m_document.panels.showTranscript = typedCommand.visible;
                return {true, "transcript visibility updated"};
            } else if constexpr (
                std::is_same_v<T, SeedTranscriptHistoryDocumentCommand> ||
                std::is_same_v<T, SetTranscriptHistoryDocumentCommand>) {
                if (typedCommand.path.empty() || typedCommand.jsonPayload.empty()) {
                    return {false, "transcript history document requires path and payload"};
                }
                auto document = std::find_if(
                    m_document.transcriptHistoryDocuments.begin(),
                    m_document.transcriptHistoryDocuments.end(),
                    [&](const EditorDocumentCore::TranscriptHistoryDocument& candidate) {
                        return candidate.path == typedCommand.path;
                    });
                if (document == m_document.transcriptHistoryDocuments.end()) {
                    m_document.transcriptHistoryDocuments.push_back(
                        {typedCommand.path, typedCommand.jsonPayload});
                    return {true, "transcript history document seeded"};
                }
                if (document->jsonPayload == typedCommand.jsonPayload) {
                    return {false, "transcript history document unchanged"};
                }
                document->jsonPayload = typedCommand.jsonPayload;
                return {true, "transcript history document updated"};
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
    if (recordHistory && result.applied &&
        (toJson(previousDocument) != toJson(m_document) ||
         !transcriptHistoryDocumentsEqual(previousDocument, m_document))) {
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
    const auto ranges = normalizedPlaybackRangesCore(
        m_document.exportRanges, endFrame);
    const PlaybackAdvanceCore advance = advancePlaybackFramesCore(
        ranges,
        m_document.transport.currentFrame,
        wholeFrames,
        endFrame);
    m_document.transport.currentFrame =
        static_cast<int>(advance.frame);
    if (advance.reachedEnd) {
        if (m_document.transport.playbackLoopEnabled) {
            m_document.transport.currentFrame = static_cast<int>(
                ranges.empty() ? 0 : ranges.front().startFrame);
        } else {
            m_document.transport.playbackActive = false;
        }
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
