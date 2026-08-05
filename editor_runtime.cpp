#include "editor_runtime.h"
#include "editor_media_presence_core.h"

#include "editor_document_core_json.h"
#include "editor_grading_core.h"
#include "keyframe_sequence.h"

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
    jcut::keyframes::upsertByFrame(keyframes, std::move(keyframe));
    jcut::keyframes::sortByFrame(keyframes);
}

template <typename Keyframe>
bool removeKeyframeAtFrame(std::vector<Keyframe>* keyframes,
                           std::int64_t frame)
{
    if (!keyframes) {
        return false;
    }
    return jcut::keyframes::removeIf(
        keyframes,
        [frame](const Keyframe& keyframe) { return keyframe.frame == frame; });
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

void synchronizeTranscriptSubtitleChildren(
    jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }

    std::unordered_map<std::string, bool>
        sourceOverlayEnabledById;
    for (const jcut::EditorClip& clip : document->clips) {
        if (jcut::canonicalEditorClipRole(
                clip.clipRole) == "media") {
            const std::string sourceId =
                jcut::trimmedEditorClipId(clip.persistentId);
            if (!sourceId.empty()) {
                sourceOverlayEnabledById.emplace(
                    sourceId,
                    clip.transcriptOverlay.enabled);
            }
        }
    }

    std::unordered_set<std::string> retainedSubtitleParents;
    document->clips.erase(
        std::remove_if(
            document->clips.begin(),
            document->clips.end(),
            [&](const jcut::EditorClip& clip) {
                if (!jcut::isTranscriptGeneratedEditorSubtitle(
                        clip)) {
                    return false;
                }
                const std::string parentId =
                    jcut::trimmedEditorClipId(
                        clip.linkedSourceClipId);
                const auto source =
                    sourceOverlayEnabledById.find(parentId);
                if (source ==
                        sourceOverlayEnabledById.end() ||
                    !source->second) {
                    return true;
                }
                return !retainedSubtitleParents.insert(
                            parentId)
                            .second;
            }),
        document->clips.end());

    std::vector<std::string> sourceIds;
    sourceIds.reserve(document->clips.size());
    for (const jcut::EditorClip& clip : document->clips) {
        if (jcut::canonicalEditorClipRole(
                clip.clipRole) == "media") {
            const std::string sourceId =
                jcut::trimmedEditorClipId(clip.persistentId);
            if (!sourceId.empty()) {
                sourceIds.push_back(sourceId);
            }
        }
    }

    for (const std::string& sourceId : sourceIds) {
        const auto sourceIt = std::find_if(
            document->clips.cbegin(),
            document->clips.cend(),
            [&](const jcut::EditorClip& clip) {
                return jcut::canonicalEditorClipRole(
                           clip.clipRole) == "media" &&
                    jcut::trimmedEditorClipId(
                        clip.persistentId) == sourceId;
            });
        if (sourceIt == document->clips.cend()) {
            continue;
        }
        const jcut::EditorClip source = *sourceIt;
        if (!source.transcriptOverlay.enabled) {
            continue;
        }

        jcut::EditorClip* child = nullptr;
        const auto existingChild = std::find_if(
            document->clips.begin(),
            document->clips.end(),
            [&](const jcut::EditorClip& clip) {
                return jcut::isTranscriptGeneratedEditorSubtitle(
                           clip) &&
                    jcut::trimmedEditorClipId(
                        clip.linkedSourceClipId) == sourceId;
            });
        if (existingChild != document->clips.end()) {
            child = &*existingChild;
        } else {
            jcut::EditorClip generated;
            generated.id = nextClipId(document->clips);
            generated.persistentId = uniquePersistentClipId(
                document->clips, generated.id);
            generated.trackId = source.trackId;
            document->clips.push_back(std::move(generated));
            child = &document->clips.back();
        }

        const int childId = child->id;
        const int childTrackId = child->trackId;
        const std::string childPersistentId =
            child->persistentId;
        const int childZLevel = child->zLevel;
        const bool childZLevelUserSet =
            child->zLevelUserSet;
        *child = source;
        child->id = childId;
        child->persistentId = childPersistentId;
        child->trackId = childTrackId;
        child->clipRole = "transcript_subtitle";
        child->label = "Transcript Subtitles";
        child->linkedSourceClipId = sourceId;
        child->syncLockedToSource = true;
        child->sourceTransformLocked = true;
        child->locked = true;
        child->selected = false;
        child->videoEnabled = false;
        child->audioEnabled = false;
        child->audioPresenceKnown = true;
        child->hasAudio = true;
        child->titleKeyframes.clear();
        if (childZLevelUserSet) {
            child->zLevel = childZLevel;
            child->zLevelUserSet = true;
        }

        jcut::EditorTrack* lane =
            findTrack(&document->tracks, child->trackId);
        if (!lane || !jcut::isGeneratedEditorChildTrack(*lane) ||
            jcut::trimmedEditorClipId(lane->childClipId) !=
                childPersistentId) {
            jcut::EditorTrack generatedTrack;
            generatedTrack.id = nextTrackId(document->tracks);
            generatedTrack.height = 44;
            generatedTrack.audioEnabled = false;
            generatedTrack.audioWaveformVisible = false;
            document->tracks.push_back(std::move(generatedTrack));
            lane = &document->tracks.back();
            child->trackId = lane->id;
        }
        lane->generatedChildTrack = true;
        lane->parentClipId = sourceId;
        lane->childClipId = childPersistentId;
        lane->label = "↳ Transcript • Subtitles";
        lane->height = std::clamp(
            lane->height, jcut::kEditorTrackMinHeight, 56);
        lane->audioEnabled = false;
        lane->audioWaveformVisible = false;
    }
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

void normalizeSpeakerTitleParentBounds(
    jcut::EditorDocumentCore* document)
{
    if (!document) {
        return;
    }
    struct ParentRange {
        int startFrame = 0;
        int endFrame = 0;
    };
    std::unordered_map<std::string, ParentRange> parentRanges;
    parentRanges.reserve(document->clips.size());
    for (const jcut::EditorClip& clip : document->clips) {
        if (jcut::isOwnedGeneratedEditorClip(clip)) {
            continue;
        }
        const std::string clipId =
            jcut::trimmedEditorClipId(clip.persistentId);
        if (!clipId.empty()) {
            parentRanges.emplace(
                clipId,
                ParentRange{
                    clip.startFrame,
                    clip.startFrame +
                        std::max(1, clip.durationFrames)});
        }
    }

    document->clips.erase(
        std::remove_if(
            document->clips.begin(),
            document->clips.end(),
            [&](jcut::EditorClip& title) {
                if (!jcut::isTranscriptGeneratedEditorTitle(
                        title)) {
                    return false;
                }
                const auto parent = parentRanges.find(
                    jcut::trimmedEditorClipId(
                        title.linkedSourceClipId));
                if (parent == parentRanges.end()) {
                    return true;
                }
                const int titleStart = title.startFrame;
                const int titleEnd =
                    title.startFrame +
                    std::max(1, title.durationFrames);
                const int boundedStart = std::max(
                    titleStart, parent->second.startFrame);
                const int boundedEnd = std::min(
                    titleEnd, parent->second.endFrame);
                if (boundedEnd <= boundedStart) {
                    return true;
                }

                const int trimmedFrames =
                    boundedStart - titleStart;
                if (trimmedFrames > 0) {
                    trimKeyframesFromStart(
                        &title.transformKeyframes,
                        trimmedFrames);
                    trimKeyframesFromStart(
                        &title.gradingKeyframes,
                        trimmedFrames);
                    trimKeyframesFromStart(
                        &title.opacityKeyframes,
                        trimmedFrames);
                    trimKeyframesFromStart(
                        &title.titleKeyframes,
                        trimmedFrames);
                }
                title.startFrame = boundedStart;
                title.durationFrames =
                    boundedEnd - boundedStart;
                title.sourceDurationFrames =
                    title.durationFrames;
                const std::int64_t lastFrame =
                    title.durationFrames - 1;
                for (jcut::EditorTitleKeyframe& keyframe :
                     title.titleKeyframes) {
                    keyframe.frame = std::clamp<std::int64_t>(
                        keyframe.frame, 0, lastFrame);
                }
                return false;
            }),
        document->clips.end());
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
    bool ownedChild = false)
{
    if (!document) {
        return {false, "document unavailable"};
    }
    jcut::EditorClip* clip = findClip(&document->clips, clipId);
    if (!clip) {
        return {false, "clip not found"};
    }
    if (jcut::isOwnedGeneratedEditorClip(*clip) && !ownedChild) {
        return {false, "generated child must be split with its source"};
    }
    if (clip->locked && !ownedChild) {
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
        &clip->effectEnabledKeyframes,
        &trailingClip.effectEnabledKeyframes,
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
    if (jcut::isOwnedGeneratedEditorClip(*source)) {
        return {false, "generated child must be split with its source"};
    }

    const std::string sourcePersistentId =
        jcut::trimmedEditorClipId(source->persistentId);
    std::vector<int> ownedChildIds;
    for (const jcut::EditorClip& candidate : document->clips) {
        if (!jcut::isOwnedGeneratedEditorClip(candidate) ||
            jcut::trimmedEditorClipId(candidate.linkedSourceClipId) !=
                sourcePersistentId) {
            continue;
        }
        ownedChildIds.push_back(candidate.id);
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

    for (const int childId : ownedChildIds) {
        jcut::EditorClip* child = findClip(&document->clips, childId);
        if (!child) {
            return {false, "owned child not found"};
        }
        const int childEnd = child->startFrame + child->durationFrames;
        if (child->startFrame >= frame) {
            child->linkedSourceClipId = trailingSourcePersistentId;
            child->selected = false;
            continue;
        }
        if (childEnd <= frame) {
            continue;
        }

        int trailingChildId = 0;
        const jcut::CommandResult splitChild = splitSingleClipAtFrame(
            document, childId, frame, &trailingChildId, true);
        if (!splitChild.applied) {
            return splitChild;
        }
        jcut::EditorClip* trailingChild =
            findClip(&document->clips, trailingChildId);
        if (!trailingChild) {
            return {false, "split child result not found"};
        }
        trailingChild->linkedSourceClipId = trailingSourcePersistentId;
        trailingChild->selected = false;
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

std::string normalizedEditorTransformInterpolationMode(const std::string& mode,
                                                       bool linearInterpolation)
{
    std::string normalized;
    normalized.reserve(mode.size());
    for (char ch : mode) {
        normalized.push_back(ch == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (!linearInterpolation || normalized == "step" || normalized == "hold") {
        return "step";
    }
    if (normalized == "perspective_linear" ||
        normalized == "perspective" ||
        normalized == "parallax_linear" ||
        normalized == "depth_linear") {
        return "perspective_linear";
    }
    return "linear";
}

bool editorTransformUsesPerspectiveDepth(const jcut::EditorTransformKeyframe& keyframe)
{
    return normalizedEditorTransformInterpolationMode(
               keyframe.interpolationMode, keyframe.linearInterpolation) ==
        "perspective_linear";
}

double safeEditorPerspectiveScaleMagnitude(double scale)
{
    return std::max(0.0001, std::abs(normalizedScale(scale)));
}

double editorPerspectiveLinearScale(double previous, double current, double amount)
{
    const double previousDepth = 1.0 / safeEditorPerspectiveScaleMagnitude(previous);
    const double currentDepth = 1.0 / safeEditorPerspectiveScaleMagnitude(current);
    const double depth = std::max(
        0.0001,
        previousDepth + ((currentDepth - previousDepth) * amount));
    const double sign = ((amount < 0.5 ? previous : current) < 0.0) ? -1.0 : 1.0;
    return normalizedScale(sign / depth);
}

double editorPerspectiveDepthForTransform(const jcut::EditorTransformKeyframe& keyframe)
{
    const double x = safeEditorPerspectiveScaleMagnitude(keyframe.scaleX);
    const double y = safeEditorPerspectiveScaleMagnitude(keyframe.scaleY);
    return 1.0 / std::sqrt(std::max(0.00000001, x * y));
}

jcut::EditorTransformKeyframe interpolatedEditorTransformKeyframe(
    const jcut::EditorTransformKeyframe& previous,
    const jcut::EditorTransformKeyframe& current,
    std::int64_t frame,
    double amount)
{
    jcut::EditorTransformKeyframe offset;
    offset.frame = frame;
    offset.title = previous.title;
    offset.rotation = previous.rotation +
        (current.rotation - previous.rotation) * amount;
    offset.linearInterpolation = current.linearInterpolation;
    offset.interpolationMode = normalizedEditorTransformInterpolationMode(
        current.interpolationMode, current.linearInterpolation);
    if (!editorTransformUsesPerspectiveDepth(current)) {
        offset.translationX = previous.translationX +
            (current.translationX - previous.translationX) * amount;
        offset.translationY = previous.translationY +
            (current.translationY - previous.translationY) * amount;
        offset.scaleX = previous.scaleX +
            (current.scaleX - previous.scaleX) * amount;
        offset.scaleY = previous.scaleY +
            (current.scaleY - previous.scaleY) * amount;
        return offset;
    }

    offset.scaleX = editorPerspectiveLinearScale(previous.scaleX, current.scaleX, amount);
    offset.scaleY = editorPerspectiveLinearScale(previous.scaleY, current.scaleY, amount);
    const double previousDepth = editorPerspectiveDepthForTransform(previous);
    const double currentDepth = editorPerspectiveDepthForTransform(current);
    const double depth = std::max(
        0.0001,
        previousDepth + ((currentDepth - previousDepth) * amount));
    const double previousWorldX = previous.translationX * previousDepth;
    const double currentWorldX = current.translationX * currentDepth;
    const double previousWorldY = previous.translationY * previousDepth;
    const double currentWorldY = current.translationY * currentDepth;
    offset.translationX = (previousWorldX + ((currentWorldX - previousWorldX) * amount)) / depth;
    offset.translationY = (previousWorldY + ((currentWorldY - previousWorldY) * amount)) / depth;
    return offset;
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
    const std::int64_t maxFrame = std::max(0, clip->durationFrames - 1);
    jcut::keyframes::normalizeSequence(
        &clip->opacityKeyframes, maxFrame,
        [](jcut::EditorOpacityKeyframe& keyframe) {
            keyframe.opacity = normalizedEditorOpacity(keyframe.opacity);
        });

    if (editorClipHasVisuals(*clip)) {
        if (clip->opacityKeyframes.empty()) {
            clip->opacityKeyframes.push_back(
                {0, normalizedEditorOpacity(clip->opacity), true});
        } else if (clip->opacityKeyframes.front().frame > 0) {
            jcut::EditorOpacityKeyframe first = clip->opacityKeyframes.front();
            first.frame = 0;
            clip->opacityKeyframes.insert(
                clip->opacityKeyframes.begin(), std::move(first));
        } else {
            clip->opacityKeyframes.front().frame = 0;
        }
    }

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
                   std::is_same_v<T, jcut::SetClipSelectedFaceTrackIdsCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingEnabledKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertSpeakerFramingTargetKeyframeCommand> ||
                   std::is_same_v<T, jcut::UpsertTransformKeyframeCommand> ||
                   std::is_same_v<T, jcut::CommitPreviewTransformCommand> ||
                   std::is_same_v<T, jcut::SetClipMaskEffectCommand> ||
                   std::is_same_v<T, jcut::UpsertEffectEnabledKeyframeCommand> ||
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
                std::is_same_v<T, jcut::TrimClipStartCommand> ||
                std::is_same_v<T, jcut::TrimClipEndCommand> ||
                std::is_same_v<T, jcut::MoveClipCommand> ||
                std::is_same_v<T, jcut::MoveSelectedClipsCommand> ||
                std::is_same_v<T, jcut::ResizeClipCommand> ||
                std::is_same_v<T, jcut::RefreshClipMetadataCommand> ||
                std::is_same_v<T, jcut::MaterializeMaskMatteCommand> ||
                std::is_same_v<T, jcut::ReplaceSpeakerTitleClipsCommand> ||
                std::is_same_v<T, jcut::SetClipTranscriptOverlayCommand> ||
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
    synchronizeTranscriptSubtitleChildren(document);
    normalizeSpeakerTitleParentBounds(document);

    struct ChildBinding {
        std::size_t childIndex = 0;
        std::size_t parentIndex = 0;
        std::string childId;
        std::string parentId;
        std::string sidecarIdentity;
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
        std::string sidecarIdentity = trimmed(child.generatedFromMaskId);
        if (sidecarIdentity.empty()) {
            sidecarIdentity = trimmed(child.maskFramesDir);
        }
        if (sidecarIdentity.empty()) {
            sidecarIdentity = std::string("legacy-track:") +
                std::to_string(child.trackId);
        }
        bindings.push_back(
            {childIndex, parentIt->second, childId, parentId,
             std::move(sidecarIdentity), 0});
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
    std::unordered_map<std::string, int>
        laneTrackIdBySourceTrackAndSidecar;
    laneTrackIdBySourceTrackAndSidecar.reserve(bindings.size());
    const auto generatedTrackMatches = [&](const EditorTrack& track,
                                           const ChildBinding& binding) {
        return isGeneratedEditorChildTrack(track) &&
            trimmedEditorClipId(track.childClipId) == binding.childId;
    };
    const auto trackContainsOnlyBoundChildren = [&](int trackId) {
        const auto occupants = occupantsByTrackId.find(trackId);
        if (occupants == occupantsByTrackId.end() ||
            occupants->second.empty()) {
            return false;
        }
        return std::all_of(
            occupants->second.begin(), occupants->second.end(),
            [&](std::size_t clipIndex) {
                return boundChildIds.find(trimmedEditorClipId(
                           document->clips[clipIndex].persistentId)) !=
                    boundChildIds.end();
            });
    };
    for (ChildBinding& binding : bindings) {
        const EditorClip& child = document->clips[binding.childIndex];
        const EditorClip& parent = document->clips[binding.parentIndex];
        const std::string laneKey =
            std::to_string(parent.trackId) + "\x1f" +
            binding.sidecarIdentity;
        int laneTrackId = 0;
        const auto sharedLane =
            laneTrackIdBySourceTrackAndSidecar.find(laneKey);
        if (sharedLane !=
            laneTrackIdBySourceTrackAndSidecar.end()) {
            laneTrackId = sharedLane->second;
        }

        const auto currentTrackIt = trackIndexById.find(child.trackId);
        if (laneTrackId == 0 &&
            currentTrackIt != trackIndexById.end()) {
            const EditorTrack& currentTrack =
                document->tracks[currentTrackIt->second];
            if ((generatedTrackMatches(currentTrack, binding) ||
                 (isGeneratedEditorChildTrack(currentTrack) &&
                  trackContainsOnlyBoundChildren(currentTrack.id))) &&
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
                trackContainsOnlyBoundChildren(currentTrack.id) &&
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
        laneTrackIdBySourceTrackAndSidecar.emplace(
            laneKey, laneTrackId);
    }

    // Each sidecar collection on a source track owns one generated lane.
    // Recover any malformed ordinary occupant onto a neutral base track.
    std::unordered_set<int> recoveredLaneTrackIds;
    for (const ChildBinding& binding : bindings) {
        if (!recoveredLaneTrackIds.insert(
                binding.laneTrackId).second) {
            continue;
        }
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

    std::unordered_set<int> initializedLaneTrackIds;
    for (const ChildBinding& binding : bindings) {
        EditorClip& child = document->clips[binding.childIndex];
        child.trackId = binding.laneTrackId;
        if (!initializedLaneTrackIds.insert(
                binding.laneTrackId).second) {
            continue;
        }
        const auto laneIt = trackIndexById.find(binding.laneTrackId);
        if (laneIt == trackIndexById.end()) {
            continue;
        }
        EditorTrack& lane = document->tracks[laneIt->second];
        lane.generatedChildTrack = true;
        lane.parentClipId = binding.parentId;
        lane.childClipId = binding.childId;
        const std::string sourceLabel =
            document->clips[binding.parentIndex].label.empty()
                ? std::string("Masks")
                : document->clips[binding.parentIndex].label;
        const std::string sidecarLabel = child.label.empty()
            ? std::string("Mask")
            : child.label;
        lane.label = std::string("↳ ") + sourceLabel +
            " • " + sidecarLabel;
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
                role != "effect_synth" && role != "speaker_title" &&
                role != "transcript_subtitle";
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
        const bool speakerTitleLane = std::all_of(
            occupants.begin(), occupants.end(),
            [&](std::size_t clipIndex) {
                return isTranscriptGeneratedEditorTitle(
                    document->clips[clipIndex]);
            });
        if (speakerTitleLane) {
            continue;
        }
        const bool subtitleLane = std::all_of(
            occupants.begin(), occupants.end(),
            [&](std::size_t clipIndex) {
                return isTranscriptGeneratedEditorSubtitle(
                    document->clips[clipIndex]);
            });
        if (subtitleLane) {
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

        const int currentLaneTrackId =
            document->clips[group.childIndices.front()].trackId;
        const auto currentLane =
            currentTrackIndexByTrackId.find(currentLaneTrackId);
        const auto currentOccupants =
            currentOccupantsByTrackId.find(currentLaneTrackId);
        const bool groupAlreadySharesGeneratedLane =
            currentLane != currentTrackIndexByTrackId.end() &&
            currentOccupants != currentOccupantsByTrackId.end() &&
            isGeneratedEditorChildTrack(
                document->tracks[currentLane->second]) &&
            std::all_of(
                group.childIndices.begin(), group.childIndices.end(),
                [&](std::size_t childIndex) {
                    return document->clips[childIndex].trackId ==
                        currentLaneTrackId;
                }) &&
            std::all_of(
                currentOccupants->second.begin(),
                currentOccupants->second.end(),
                [&](std::size_t occupantIndex) {
                    return isTranscriptGeneratedEditorTitle(
                        document->clips[occupantIndex]);
                });
        if (groupAlreadySharesGeneratedLane) {
            group.laneTrackId = currentLaneTrackId;
        } else {
            for (const EditorTrack& track : document->tracks) {
                if (isGeneratedEditorChildTrack(track) &&
                    trimmedEditorClipId(track.parentClipId) ==
                        group.parentId &&
                    std::any_of(
                        currentOccupantsByTrackId[track.id].begin(),
                        currentOccupantsByTrackId[track.id].end(),
                        [&](std::size_t occupantIndex) {
                            return isTranscriptGeneratedEditorTitle(
                                document->clips[occupantIndex]);
                        }) &&
                    claimedSpeakerTrackIds.find(track.id) ==
                        claimedSpeakerTrackIds.end()) {
                    group.laneTrackId = track.id;
                    break;
                }
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
                        offset = interpolatedEditorTransformKeyframe(
                            previous, current, frame, amount);
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
    const auto demoClip = [](int id,
                             int trackId,
                             std::string label,
                             int startFrame,
                             int durationFrames,
                             bool selected) {
        EditorClip clip;
        clip.id = id;
        clip.trackId = trackId;
        clip.label = std::move(label);
        clip.startFrame = startFrame;
        clip.durationFrames = durationFrames;
        clip.selected = selected;
        return clip;
    };
    runtime.m_document.clips = {
        demoClip(1, 1, "Interview A", 0, 420, true),
        demoClip(2, 2, "Interview B", 36, 396, false),
        demoClip(3, 3, "Lower Third", 120, 96, false),
        demoClip(4, 4, "VO Main", 0, 420, false),
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

#include "editor_runtime_transport_commands.h"
#include "editor_runtime_timeline_commands.h"
#include "editor_runtime_clip_edit_commands.h"
#include "editor_runtime_audio_track_commands.h"
#include "editor_runtime_sync_range_commands.h"
#include "editor_runtime_panel_export_commands.h"

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
                    !std::is_same_v<T, SelectClipCommand> &&
                    !std::is_same_v<T, SetClipZLevelCommand>) {
                    if (const EditorClip* target =
                            findClip(&m_document.clips, typedCommand.clipId);
                        target && isTranscriptGeneratedEditorTitle(*target)) {
                        return {
                            false,
                            "generated transcript titles can only be changed by regenerating them"};
                    }
                }
            }

            if (const auto result = dispatchTransportCommand(typedCommand)) {
                return *result;
            }
            if (const auto result = dispatchTimelineCommand(typedCommand)) {
                return *result;
            }
            if (const auto result = dispatchClipEditCommand(typedCommand)) {
                return *result;
            }
            if (const auto result = dispatchAudioTrackCommand(typedCommand)) {
                return *result;
            }
            if (const auto result = dispatchSyncRangeCommand(typedCommand)) {
                return *result;
            }
            if (const auto result = dispatchPanelExportCommand(typedCommand)) {
                return *result;
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
