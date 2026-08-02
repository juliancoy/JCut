#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchTimelineCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, SetProjectNameCommand>) {
                    m_document.projectName = typedCommand.name.empty()
                        ? std::string("Untitled Project")
                        : typedCommand.name;
                    return CommandResult{true, "project name updated"};
                } else if constexpr (std::is_same_v<T, ImportMediaCommand>) {
                    if (typedCommand.sourcePath.empty()) {
                        return CommandResult{false, "media path required"};
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
                    return CommandResult{true, "media imported"};
                } else if constexpr (std::is_same_v<T, RemoveMediaCommand>) {
                    const auto mediaItem = std::find_if(
                        m_document.mediaItems.cbegin(), m_document.mediaItems.cend(),
                        [&](const EditorMediaItem& candidate) {
                            return candidate.id == typedCommand.mediaId;
                        });
                    if (mediaItem == m_document.mediaItems.cend()) {
                        return CommandResult{false, "media not found"};
                    }
                    const bool referenced = std::any_of(
                        m_document.clips.cbegin(), m_document.clips.cend(),
                        [&](const EditorClip& clip) {
                            return !clip.sourcePath.empty() &&
                                clip.sourcePath == typedCommand.mediaId;
                        });
                    if (referenced) {
                        return CommandResult{false, "media is used by timeline clips"};
                    }
                    m_document.mediaItems.erase(mediaItem);
                    return CommandResult{true, "media removed from project"};
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
                    return CommandResult{true, "track added"};
                } else if constexpr (std::is_same_v<T, DeleteTrackCommand>) {
                    const auto removedTrack = std::find_if(
                        m_document.tracks.begin(), m_document.tracks.end(),
                        [&](const EditorTrack& track) {
                            return track.id == typedCommand.trackId;
                        });
                    if (removedTrack == m_document.tracks.end()) {
                        return CommandResult{false, "track not found"};
                    }
                    if (isGeneratedEditorChildTrack(*removedTrack)) {
                        return CommandResult{false,
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
                            return CommandResult{false,
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
                    return CommandResult{true, "track deleted"};
                } else if constexpr (std::is_same_v<T, ReorderTrackCommand>) {
                    const auto track = std::find_if(
                        m_document.tracks.begin(), m_document.tracks.end(),
                        [&](const EditorTrack& candidate) {
                            return candidate.id == typedCommand.trackId;
                        });
                    if (track == m_document.tracks.end()) {
                        return CommandResult{false, "track not found"};
                    }
                    if (isGeneratedEditorChildTrack(*track)) {
                        return CommandResult{false,
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
                        return CommandResult{false,
                                "generated child track cannot be a reorder target"};
                    }
                    if (currentIndex == targetIndex) {
                        return CommandResult{false, "track already at requested position"};
                    }

                    EditorTrack movedTrack = std::move(*track);
                    m_document.tracks.erase(track);
                    m_document.tracks.insert(
                        m_document.tracks.begin() +
                            static_cast<std::ptrdiff_t>(targetIndex),
                        std::move(movedTrack));
                    return CommandResult{true, "track reordered"};
                } else if constexpr (std::is_same_v<T, CrossfadeTrackCommand>) {
                    return applyCrossfadeToEditorTrack(&m_document, typedCommand);
                } else if constexpr (std::is_same_v<T, SelectTrackCommand>) {
                    if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                        return CommandResult{false, "track not found"};
                    }
                    selectSingle(&m_document.tracks, [&](const EditorTrack& track) {
                        return track.id == typedCommand.trackId;
                    });
                    return CommandResult{true, "track selected"};
                } else if constexpr (std::is_same_v<T, SelectClipCommand>) {
                    EditorClip* clip = findClip(&m_document.clips,
                                                typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (typedCommand.toggle) {
                        clip->selected = !clip->selected;
                        return CommandResult{true, clip->selected ? "clip added to selection"
                                                     : "clip removed from selection"};
                    }
                    if (typedCommand.additive) {
                        clip->selected = true;
                        return CommandResult{true, "clip added to selection"};
                    }
                    selectSingle(&m_document.clips,
                                 [&](const EditorClip& candidate) {
                                     return candidate.id == typedCommand.clipId;
                                 });
                    return CommandResult{true, "clip selected"};
                } else if constexpr (std::is_same_v<T, CopySelectedClipsCommand>) {
                    return copySelectedClipsToClipboard()
                        ? CommandResult{true, "selected clips copied"}
                        : CommandResult{false, "no clips selected"};
                } else if constexpr (std::is_same_v<T, CutSelectedClipsCommand>) {
                    const ClipPersistentIdSet selectedIds =
                        selectedClipPersistentIds(m_document);
                    if (selectedIds.empty()) {
                        return CommandResult{false, "no clips selected"};
                    }
                    for (const EditorClip& clip : m_document.clips) {
                        if (!clip.selected) {
                            continue;
                        }
                        if (isOwnedGeneratedEditorClip(clip)) {
                            if (!persistentClipIdInSet(
                                    selectedIds, clip.linkedSourceClipId)) {
                                return CommandResult{false,
                                        "generated child must be cut with its source"};
                            }
                            continue;
                        }
                        if (clip.locked) {
                            return CommandResult{false, "locked clips cannot be cut"};
                        }
                    }
                    if (!copySelectedClipsToClipboard()) {
                        return CommandResult{false, "selected clips could not be copied"};
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
                    return CommandResult{true, "selected clips cut"};
                } else if constexpr (std::is_same_v<T, PasteClipsCommand>) {
                    return pasteClipboardAt(typedCommand.targetFrame,
                                            typedCommand.targetTrackId);
                } else if constexpr (
                    std::is_same_v<T, DuplicateSelectedClipsCommand>) {
                    if (!copySelectedClipsToClipboard()) {
                        return CommandResult{false, "no clips selected"};
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
                        return CommandResult{false, "no clips to select"};
                    }
                    for (EditorClip& candidate : m_document.clips) {
                        candidate.selected = true;
                    }
                    return CommandResult{true, "all clips selected"};
                } else if constexpr (std::is_same_v<T, InsertClipFromMediaCommand>) {
                    EditorTrack* targetTrack =
                        findTrack(&m_document.tracks, typedCommand.trackId);
                    if (!targetTrack) {
                        return CommandResult{false, "track not found"};
                    }
                    if (isGeneratedEditorChildTrack(*targetTrack)) {
                        return CommandResult{false,
                                "ordinary media cannot be inserted on a generated child track"};
                    }
                    const EditorMediaItem* mediaItem = findMediaItem(m_document.mediaItems, typedCommand.mediaId);
                    if (!mediaItem) {
                        return CommandResult{false, "media not found"};
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
                    return CommandResult{true, "clip inserted"};
                } else if constexpr (std::is_same_v<T, AddClipCommand>) {
                    EditorTrack* targetTrack =
                        findTrack(&m_document.tracks, typedCommand.trackId);
                    if (!targetTrack) {
                        return CommandResult{false, "track not found"};
                    }
                    if (isGeneratedEditorChildTrack(*targetTrack)) {
                        return CommandResult{false,
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
                    return CommandResult{true, "clip added"};
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
                    return CommandResult{true, createdTrack
                        ? "title created on new Titles track"
                        : "title created"};
                } else if constexpr (
                    std::is_same_v<T, ReplaceSpeakerTitleClipsCommand>) {
                    EditorClip* source = findClip(
                        &m_document.clips, typedCommand.sourceClipId);
                    if (!source ||
                        canonicalEditorClipRole(source->clipRole) != "media" ||
                        source->persistentId.empty()) {
                        return CommandResult{false, "speaker-title source clip not found"};
                    }
                    const std::string sourcePersistentId = source->persistentId;
                    if (!typedCommand.generatedClips.empty()) {
                        source->transcriptOverlay.showSpeakerTitle = false;
                    }
                    std::unordered_set<int> previousTitleTrackIds;
                    int preservedZLevel = std::numeric_limits<int>::min();
                    bool preservedZLevelUserSet = false;
                    bool hasPreservedZLevel = false;
                    for (const EditorClip& clip : m_document.clips) {
                        if (isTranscriptGeneratedEditorTitle(clip) &&
                            trimmedEditorClipId(clip.linkedSourceClipId) ==
                                trimmedEditorClipId(sourcePersistentId)) {
                            previousTitleTrackIds.insert(clip.trackId);
                            if (!hasPreservedZLevel) {
                                preservedZLevel = clip.zLevel;
                                preservedZLevelUserSet = clip.zLevelUserSet;
                                hasPreservedZLevel = true;
                            }
                        }
                    }
                    std::vector<int> reusableTrackIds;
                    for (const int trackId : previousTitleTrackIds) {
                        const EditorTrack* previousTrack =
                            findTrack(&m_document.tracks, trackId);
                        const bool sharedGeneratedLane =
                            previousTrack &&
                            previousTrack->generatedChildTrack;
                        const bool dedicated = std::none_of(
                            m_document.clips.begin(),
                            m_document.clips.end(),
                            [&](const EditorClip& clip) {
                                if (clip.trackId != trackId ||
                                    (isTranscriptGeneratedEditorTitle(clip) &&
                                     trimmedEditorClipId(
                                         clip.linkedSourceClipId) ==
                                         trimmedEditorClipId(
                                             sourcePersistentId))) {
                                    return false;
                                }
                                return !sharedGeneratedLane ||
                                    !isTranscriptGeneratedEditorTitle(
                                        clip);
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
                        if (hasPreservedZLevel) {
                            generated.zLevel = preservedZLevel;
                            generated.zLevelUserSet = preservedZLevelUserSet;
                        }
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
                        return CommandResult{false, "no speaker introductions were generated"};
                    }
                    sortClipsByTimeline(&m_document.clips);
                    return CommandResult{true,
                            "replaced speaker introductions (" +
                                std::to_string(inserted) + " generated, " +
                                std::to_string(removed) + " removed)"};
                } else if constexpr (std::is_same_v<T, DeleteClipCommand>) {
                    const EditorClip* removedClip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!removedClip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(removedClip->clipRole) ==
                        "mask_matte") {
                        return CommandResult{false,
                                "mask matte must be deleted with its source"};
                    }
                    if (removedClip->locked) {
                        return CommandResult{false, "locked clip cannot be deleted"};
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
                    return CommandResult{true, "clip deleted"};
                } else if constexpr (
                    std::is_same_v<T, DeleteSelectedClipsCommand>) {
                    const ClipPersistentIdSet selectedIds =
                        selectedClipPersistentIds(m_document);
                    if (selectedIds.empty()) {
                        return CommandResult{false, "no clips selected"};
                    }
                    for (const EditorClip& clip : m_document.clips) {
                        if (!clip.selected) {
                            continue;
                        }
                        if (isOwnedGeneratedEditorClip(clip)) {
                            if (!persistentClipIdInSet(
                                    selectedIds, clip.linkedSourceClipId)) {
                                return CommandResult{false,
                                        "generated child must be deleted with its source"};
                            }
                            continue;
                        }
                        if (clip.locked) {
                            return CommandResult{false, "locked clips cannot be deleted"};
                        }
                    }

                    const ClipPersistentIdSet removedPersistentIds =
                        clipOwnershipClosure(m_document, selectedIds, false);
                    eraseOwnedClipsAndMarkers(&m_document,
                                              removedPersistentIds);
                    selectDeterministicClip(m_document.tracks,
                                            &m_document.clips);
                    return CommandResult{true, "selected clips deleted"};
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
                        return CommandResult{false, "no clips selected"};
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
                        return CommandResult{false, "no selected clips intersect split frame"};
                    }

                    for (EditorClip& clip : m_document.clips) {
                        clip.selected = std::find(trailingClipIds.begin(),
                                                  trailingClipIds.end(),
                                                  clip.id) !=
                            trailingClipIds.end();
                    }
                    return CommandResult{true, "selected clips split"};
                } else if constexpr (std::is_same_v<T, TrimClipStartCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false, "mask matte must be trimmed with its source"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "locked clip cannot be trimmed"};
                    }
                    const int clipEnd = clip->startFrame + clip->durationFrames;
                    if (typedCommand.startFrame < 0 || typedCommand.startFrame >= clipEnd) {
                        return CommandResult{false, "trim start outside clip"};
                    }
                    const int nextDuration = clipEnd - typedCommand.startFrame;
                    if (nextDuration < 1) {
                        return CommandResult{false, "trim would create empty clip"};
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
                    trimKeyframesFromStart(
                        &clip->effectEnabledKeyframes, trimFrames);
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
                    return CommandResult{true, "clip start trimmed"};
                } else if constexpr (std::is_same_v<T, TrimClipEndCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false, "mask matte must be trimmed with its source"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "locked clip cannot be trimmed"};
                    }
                    if (typedCommand.endFrame <= clip->startFrame) {
                        return CommandResult{false, "trim end outside clip"};
                    }
                    const int nextDuration = typedCommand.endFrame - clip->startFrame;
                    if (nextDuration < 1) {
                        return CommandResult{false, "trim would create empty clip"};
                    }
                    clip->durationFrames = nextDuration;
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true, "clip end trimmed"};
                } else if constexpr (std::is_same_v<T, SetClipLabelCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->label = typedCommand.label.empty() ? std::string("clip") : typedCommand.label;
                    return CommandResult{true, "clip label updated"};
                } else if constexpr (std::is_same_v<T, SetClipProxyCommand>) {
                    EditorClip* clip = findClip(
                        &m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false, "mask matte proxy follows its source"};
                    }
                    const std::string nextPath = trimmed(
                        typedCommand.proxyPath);
                    const bool nextUseProxy =
                        !nextPath.empty() && typedCommand.useProxy;
                    if (clip->proxyPath == nextPath &&
                        clip->useProxy == nextUseProxy) {
                        return CommandResult{false, "clip proxy is unchanged"};
                    }
                    clip->proxyPath = nextPath;
                    clip->useProxy = nextUseProxy;
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true, nextPath.empty()
                        ? "clip proxy association cleared"
                        : (nextUseProxy ? "clip proxy enabled"
                                        : "clip proxy disabled")};
                } else if constexpr (
                    std::is_same_v<T, RefreshClipMetadataCommand>) {
                    if (typedCommand.updates.empty()) {
                        return CommandResult{false, "no clip metadata updates supplied"};
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
                        return CommandResult{false, updatedCount > 0
                            ? "clip metadata is unchanged"
                            : "no matching clips found"};
                    }
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true,
                            std::to_string(updatedCount) +
                                " clip metadata record" +
                                (updatedCount == 1 ? "" : "s") +
                                " refreshed"};
                } else if constexpr (std::is_same_v<T, SetClipLockedCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (clip->locked == typedCommand.locked) {
                        return CommandResult{false, typedCommand.locked
                            ? "clip is already locked"
                            : "clip is already unlocked"};
                    }
                    clip->locked = typedCommand.locked;
                    return CommandResult{true, typedCommand.locked ? "clip locked" : "clip unlocked"};
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
                        return CommandResult{false, "no editable clips selected"};
                    }
                    if (!changed) {
                        return CommandResult{false, typedCommand.locked
                            ? "selected clips are already locked"
                            : "selected clips are already unlocked"};
                    }
                    return CommandResult{true, typedCommand.locked
                        ? "selected clips locked"
                        : "selected clips unlocked"};
                } else if constexpr (
                    std::is_same_v<T, SetClipPlaybackRateCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false,
                                "mask matte playback rate follows its source"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "locked clip playback rate cannot be changed"};
                    }
                    if (!std::isfinite(typedCommand.playbackRate) ||
                        typedCommand.playbackRate <= 0.0) {
                        return CommandResult{false, "clip playback rate must be positive"};
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
                        return CommandResult{false, "clip already uses requested playback rate"};
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
                    return CommandResult{true, "clip playback rate updated"};
                } else if constexpr (std::is_same_v<T, MoveClipCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false, "mask matte must be moved with its source"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "locked clip cannot be moved"};
                    }
                    if (!hasTrackId(m_document.tracks, typedCommand.trackId)) {
                        return CommandResult{false, "track not found"};
                    }
                    const EditorTrack* targetTrack = findTrack(
                        &m_document.tracks, typedCommand.trackId);
                    if (targetTrack &&
                        isGeneratedEditorChildTrack(*targetTrack)) {
                        return CommandResult{false,
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
                            return CommandResult{false, "clip frame move is out of range"};
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
                            return CommandResult{false,
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
                    return CommandResult{true, "clip moved"};
                } else if constexpr (
                    std::is_same_v<T, MoveSelectedClipsCommand>) {
                    EditorClip* anchor = findClip(&m_document.clips,
                                                  typedCommand.anchorClipId);
                    if (!anchor) {
                        return CommandResult{false, "anchor clip not found"};
                    }
                    if (isOwnedGeneratedEditorClip(*anchor)) {
                        return CommandResult{false,
                                "generated child must be moved with its source"};
                    }
                    if (!anchor->selected) {
                        return CommandResult{false, "anchor clip is not selected"};
                    }
                    const std::size_t anchorTrackIndex =
                        trackIndexForId(m_document.tracks, anchor->trackId);
                    const std::size_t targetTrackIndex =
                        trackIndexForId(m_document.tracks,
                                        typedCommand.targetTrackId);
                    if (anchorTrackIndex == m_document.tracks.size() ||
                        targetTrackIndex == m_document.tracks.size()) {
                        return CommandResult{false, "target or source track not found"};
                    }
                    if (isGeneratedEditorChildTrack(
                            m_document.tracks[targetTrackIndex])) {
                        return CommandResult{false,
                                "ordinary clips cannot move onto a generated child track"};
                    }

                    SelectedFrameShift frameShift;
                    std::string shiftError;
                    if (!prepareSelectedFrameShift(
                            m_document,
                            static_cast<std::int64_t>(typedCommand.startFrame) -
                                anchor->startFrame,
                            &frameShift, &shiftError)) {
                        return CommandResult{false, std::move(shiftError)};
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
                            return CommandResult{false, "selected clip source track not found"};
                        }
                        const std::int64_t destinationTrackIndex =
                            static_cast<std::int64_t>(sourceTrackIndex) +
                            trackDelta;
                        if (destinationTrackIndex < 0 ||
                            destinationTrackIndex >= static_cast<std::int64_t>(
                                m_document.tracks.size())) {
                            return CommandResult{false, "selected clip track move is out of range"};
                        }
                        if (isGeneratedEditorChildTrack(
                                m_document.tracks[static_cast<std::size_t>(
                                    destinationTrackIndex)])) {
                            return CommandResult{false,
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
                    return CommandResult{true, "selected clips moved"};
                } else if constexpr (std::is_same_v<T, ResizeClipCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (canonicalEditorClipRole(clip->clipRole) == "mask_matte") {
                        return CommandResult{false, "mask matte must be resized with its source"};
                    }
                    if (clip->locked) {
                        return CommandResult{false, "locked clip cannot be resized"};
                    }
                    clip->durationFrames = std::max(1, typedCommand.durationFrames);
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true, "clip resized"};
                } else if constexpr (std::is_same_v<T, NudgeSelectedClipCommand>) {
                    if (typedCommand.deltaFrames == 0) {
                        return CommandResult{false, "nudge delta required"};
                    }
                    SelectedFrameShift frameShift;
                    std::string shiftError;
                    if (!prepareSelectedFrameShift(
                            m_document, typedCommand.deltaFrames, &frameShift,
                            &shiftError)) {
                        return CommandResult{false, std::move(shiftError)};
                    }
                    if (frameShift.delta == 0) {
                        return CommandResult{false, "selected clips already at timeline boundary"};
                    }
                    applySelectedFrameShift(&m_document, frameShift);
                    normalizeMaskMatteParentCaches(&m_document);
                    return CommandResult{true, "selected clips nudged"};
                } 
    return std::nullopt;
}
