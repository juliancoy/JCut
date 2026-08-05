#pragma once

namespace {

std::vector<int> filteredPersistedFaceTrackIds(
    const jcut::EditorClip& clip,
    const std::vector<jcut::FaceContinuityTrackCore>& tracks)
{
    std::vector<int> selectedTrackIds;
    selectedTrackIds.reserve(clip.selectedFaceTrackIds.size());
    for (const int trackId : clip.selectedFaceTrackIds) {
        if (trackId < 0) {
            continue;
        }
        const auto found = std::find_if(
            tracks.begin(),
            tracks.end(),
            [&](const auto& track) {
                return track.trackId == trackId;
            });
        if (found == tracks.end()) {
            continue;
        }
        if (std::find(
                selectedTrackIds.begin(),
                selectedTrackIds.end(),
                trackId) != selectedTrackIds.end()) {
            continue;
        }
        selectedTrackIds.push_back(trackId);
    }
    return selectedTrackIds;
}

void persistSelectedFaceTrackIds(ShellState* shellState,
                                 const jcut::EditorClip& clip,
                                 const std::vector<int>& trackIds)
{
    applyCommand(
        shellState,
        jcut::SetClipSelectedFaceTrackIdsCommand{clip.id, trackIds});
}

} // namespace

void drawInspectorTab09(
    ShellState* shellState, const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip* currentClip, const jcut::EditorTrack* currentTrack,
    std::int64_t currentClipLocalFrame, std::int64_t currentClipLastFrame,
    std::int64_t fadeEndFrame, const std::string& requestedInspectorTab,
    bool focusOutput, bool focusProjects)
{
    const auto inspectorTabFlags = [&requestedInspectorTab](const char* label) {
        return requestedInspectorTab == label
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
        if (ImGui::BeginTabItem(
                "Speakers", nullptr, inspectorTabFlags("Speakers"))) {
            drawInspectorHeading("Speakers", snapshot, currentClip);
            shellState->sectionAvatarDesiredKey.clear();
            shellState->sectionAvatarTracks.clear();
            if (!currentClip ||
                (currentClip->mediaKind != "audio" && !currentClip->hasAudio)) {
                ImGui::TextWrapped(
                    "Select a clip with audio and an attached transcript.");
            } else {
                ensureTranscriptInspectorCache(shellState, snapshot, *currentClip);
                TranscriptInspectorCache& cache = shellState->transcriptCache;
                const jcut::TranscriptCutSession& transcript = cache.session;
                if (!transcript.ok()) {
                    ImGui::TextWrapped("%s",
                        transcript.error.empty()
                            ? "No transcript speaker roster is available."
                            : transcript.error.c_str());
                } else {
                    const std::vector<jcut::TranscriptSpeakerProfileCore> profiles =
                        transcript.activeDocument->speakerProfiles();
                    ImGui::Text("%zu speaker%s in active cut",
                                profiles.size(), profiles.size() == 1 ? "" : "s");
                    ImGui::Checkbox(
                        "Show Contiguous Speaker Sections",
                        &cache.speakerSectionsExpanded);
                    if (cache.speakerSectionsExpanded) {
                        int minimumWords =
                            std::clamp(
                                currentClip->
                                    speakerSectionMinimumWords,
                                0,
                                1000);
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt(
                                "Minimum Section Words",
                                &minimumWords,
                                1,
                                10)) {
                            applyCommand(
                                shellState,
                                jcut::
                                    SetClipSpeakerSectionMinimumWordsCommand{
                                        currentClip->id,
                                        minimumWords});
                        }
                        const std::vector<jcut::SpeakerSectionCore>
                            sections =
                                jcut::projectSpeakerSectionsCore(
                                    transcript.activeDocument->root(),
                                    minimumWords,
                                    30.0);
                        const std::string sectionClipIdentity =
                            currentClip->persistentId.empty()
                            ? std::to_string(currentClip->id)
                            : currentClip->persistentId;
                        std::vector<
                            std::vector<
                                jcut::SpeakerTrackAssignmentCore>>
                            sectionAssignmentsByIndex;
                        sectionAssignmentsByIndex.reserve(
                            sections.size());
                        std::vector<int> sectionAvatarTrackIds;
                        for (const auto& section : sections) {
                            auto assignments =
                                jcut::
                                    transcriptSpeakerTrackAssignmentsAtFrame(
                                        transcript.activeDocument->
                                            root(),
                                        sectionClipIdentity,
                                        section.speakerId,
                                        section.startFrame);
                            for (const auto& assignment :
                                 assignments) {
                                if (assignment.trackId >= 0 &&
                                    std::find(
                                        sectionAvatarTrackIds.begin(),
                                        sectionAvatarTrackIds.end(),
                                        assignment.trackId) ==
                                        sectionAvatarTrackIds.end()) {
                                    sectionAvatarTrackIds.push_back(
                                        assignment.trackId);
                                }
                            }
                            sectionAssignmentsByIndex.push_back(
                                std::move(assignments));
                        }
                        constexpr std::size_t
                            kMaximumSectionAvatarTracks = 24;
                        std::vector<jcut::FaceContinuityTrackCore>
                            sectionAvatarTracks;
                        for (const int trackId :
                             sectionAvatarTrackIds) {
                            const auto found = std::find_if(
                                cache.faceInspection.tracks.begin(),
                                cache.faceInspection.tracks.end(),
                                [&](const auto& track) {
                                    return track.trackId ==
                                        trackId;
                                });
                            if (found !=
                                cache.faceInspection.tracks.end()) {
                                sectionAvatarTracks.push_back(*found);
                                if (sectionAvatarTracks.size() ==
                                    kMaximumSectionAvatarTracks) {
                                    break;
                                }
                            }
                        }
                        if (!sectionAvatarTracks.empty()) {
                            const std::string avatarSourcePath =
                                pathString(
                                    resolvedClipSourcePath(
                                        *shellState,
                                        *currentClip));
                            std::string avatarKey =
                                transcript.activePath + "::" +
                                sectionClipIdentity + "::" +
                                avatarSourcePath;
                            for (const auto& track :
                                 sectionAvatarTracks) {
                                avatarKey += "::" +
                                    std::to_string(track.trackId) +
                                    ":" +
                                    std::to_string(
                                        track.firstFrame) +
                                    ":" +
                                    std::to_string(track.x) +
                                    ":" +
                                    std::to_string(track.y) +
                                    ":" +
                                    std::to_string(track.box);
                            }
                            shellState->
                                sectionAvatarDesiredKey =
                                    std::move(avatarKey);
                            shellState->
                                sectionAvatarSourcePath =
                                    avatarSourcePath;
                            shellState->sectionAvatarTracks =
                                sectionAvatarTracks;
                        } else {
                            shellState->
                                sectionAvatarDesiredKey.clear();
                            shellState->sectionAvatarTracks.clear();
                        }
                        ImGui::Text(
                            "%zu contiguous section%s",
                            sections.size(),
                            sections.size() == 1 ? "" : "s");
                        if (ImGui::BeginTable(
                                "SpeakerSections",
                                7,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_ScrollY,
                                ImVec2(0.0f, 260.0f))) {
                            ImGui::TableSetupColumn(
                                "Faces",
                                ImGuiTableColumnFlags_WidthFixed,
                                80.0f);
                            ImGui::TableSetupColumn(
                                "Speaker",
                                ImGuiTableColumnFlags_WidthFixed,
                                110.0f);
                            ImGui::TableSetupColumn(
                                "Frames",
                                ImGuiTableColumnFlags_WidthFixed,
                                100.0f);
                            ImGui::TableSetupColumn(
                                "Tracks",
                                ImGuiTableColumnFlags_WidthFixed,
                                80.0f);
                            ImGui::TableSetupColumn(
                                "Words",
                                ImGuiTableColumnFlags_WidthFixed,
                                52.0f);
                            ImGui::TableSetupColumn(
                                "Transcript",
                                ImGuiTableColumnFlags_WidthStretch,
                                180.0f);
                            ImGui::TableSetupColumn(
                                "Actions",
                                ImGuiTableColumnFlags_WidthFixed,
                                370.0f);
                            ImGui::TableHeadersRow();
                            for (std::size_t sectionIndex = 0;
                                 sectionIndex < sections.size();
                                 ++sectionIndex) {
                                const auto& section =
                                    sections[sectionIndex];
                                ImGui::PushID(
                                    static_cast<int>(sectionIndex));
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                const auto& sectionAssignments =
                                    sectionAssignmentsByIndex[
                                        sectionIndex];
                                bool drewSectionAvatar = false;
                                if (shellState->
                                        sectionAvatarTextureId != 0 &&
                                    shellState->
                                        sectionAvatarLoadedKey ==
                                        shellState->
                                            sectionAvatarDesiredKey &&
                                    shellState->
                                        sectionAvatarSize.width > 0) {
                                    for (const auto& assignment :
                                         sectionAssignments) {
                                        const auto found =
                                            std::find_if(
                                                sectionAvatarTracks.
                                                    begin(),
                                                sectionAvatarTracks.
                                                    end(),
                                                [&](const auto& track) {
                                                    return track.trackId ==
                                                        assignment.
                                                            trackId;
                                                });
                                        if (found ==
                                            sectionAvatarTracks.end()) {
                                            continue;
                                        }
                                        const std::size_t avatarIndex =
                                            static_cast<std::size_t>(
                                                std::distance(
                                                    sectionAvatarTracks.
                                                        begin(),
                                                    found));
                                        const auto avatarUv =
                                            jcut::
                                                faceAvatarStripUvCore(
                                                    avatarIndex,
                                                    shellState->
                                                        sectionAvatarSize.
                                                        width,
                                                    80,
                                                    2);
                                        if (!avatarUv.valid()) {
                                            continue;
                                        }
                                        if (drewSectionAvatar) {
                                            ImGui::SameLine(
                                                0.0f, 2.0f);
                                        }
                                        ImGui::Image(
                                            shellState->
                                                sectionAvatarTextureId,
                                            ImVec2(32.0f, 32.0f),
                                            ImVec2(
                                                static_cast<float>(
                                                    avatarUv.left),
                                                0.0f),
                                            ImVec2(
                                                static_cast<float>(
                                                    avatarUv.right),
                                                1.0f));
                                        if (ImGui::
                                                IsItemHovered()) {
                                            ImGui::SetTooltip(
                                                "Continuity-track avatar T%d at source frame %lld",
                                                found->trackId,
                                                static_cast<
                                                    long long>(
                                                    found->
                                                        firstFrame));
                                        }
                                        drewSectionAvatar = true;
                                    }
                                }
                                if (!drewSectionAvatar) {
                                    ImGui::TextDisabled(
                                        sectionAssignments.empty()
                                        ? "-"
                                        : (shellState->
                                                sectionAvatarRunning
                                            ? "..."
                                            : "T"));
                                }
                                ImGui::TableNextColumn();
                                if (ImGui::Selectable(
                                        section.displayLabel.c_str(),
                                        cache.selectedSpeakerId ==
                                            section.speakerId,
                                        ImGuiSelectableFlags_AllowOverlap)) {
                                    cache.selectedSpeakerId =
                                        section.speakerId;
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%lld-%lld",
                                    static_cast<long long>(
                                        section.startFrame),
                                    static_cast<long long>(
                                        section.endFrame));
                                ImGui::TableNextColumn();
                                std::string trackLabels;
                                for (const auto& assignment :
                                     sectionAssignments) {
                                    if (!trackLabels.empty()) {
                                        trackLabels += ", ";
                                    }
                                    trackLabels += assignment.trackId >= 0
                                        ? "T" +
                                            std::to_string(
                                                assignment.trackId)
                                        : assignment.streamId;
                                }
                                ImGui::TextUnformatted(
                                    trackLabels.empty()
                                    ? "-"
                                    : trackLabels.c_str());
                                if (!sectionAssignments.empty() &&
                                    std::abs(
                                        sectionAssignments.front().
                                            rotationDegrees) > 0.0001) {
                                    ImGui::SetItemTooltip(
                                        "Section rotation %.1f degrees",
                                        sectionAssignments.front().
                                            rotationDegrees);
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%zu", section.wordCount);
                                ImGui::TableNextColumn();
                                std::string snippet;
                                for (const std::string& word :
                                     section.snippetWords) {
                                    if (!snippet.empty()) snippet += ' ';
                                    snippet += word;
                                }
                                if (section.wordCount >
                                    section.snippetWords.size()) {
                                    snippet += " ...";
                                }
                                ImGui::TextUnformatted(
                                    snippet.c_str());
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("View")) {
                                    applyCommand(
                                        shellState,
                                        jcut::SeekToFrameCommand{
                                            static_cast<int>(
                                                jcut::
                                                    faceTrackAnchorTimelineFrame(
                                                        section.startFrame,
                                                        currentClip->
                                                            sourceInFrame,
                                                        currentClip->
                                                            startFrame,
                                                        currentClip->
                                                            durationFrames,
                                                        currentClip->
                                                            playbackRate))});
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    !transcript.activeCutMutable);
                                if (ImGui::SmallButton("Skip") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionSkippedCore(
                                                &root,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                true,
                                                30.0,
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Unskip") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionSkippedCore(
                                                &root,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                false,
                                                30.0,
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    cache.selectedFaceTrackIds.empty());
                                if (ImGui::SmallButton("Assign") &&
                                    transcript.activeDocument) {
                                    std::vector<
                                        jcut::
                                            TranscriptTrackAssignmentAnchor>
                                        anchors;
                                    for (const auto& track :
                                         cache.faceInspection.tracks) {
                                        if (std::find(
                                                cache.
                                                    selectedFaceTrackIds.
                                                    begin(),
                                                cache.
                                                    selectedFaceTrackIds.
                                                    end(),
                                                track.trackId) ==
                                            cache.
                                                selectedFaceTrackIds.
                                                end()) {
                                            continue;
                                        }
                                        anchors.push_back({
                                            track.trackId,
                                            track.streamId,
                                            std::max<std::int64_t>(
                                                0,
                                                track.firstFrame),
                                            track.x,
                                            track.y,
                                            track.box});
                                    }
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionTrackAssignmentsCore(
                                                &root,
                                                sectionClipIdentity,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                section.wordCount,
                                                anchors,
                                                false,
                                                "contiguous_section_picker",
                                                {},
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(
                                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                                    ImGui::SetTooltip(
                                        "Add the continuity tracks selected below to this section.");
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(
                                    sectionAssignments.empty());
                                if (ImGui::SmallButton("Clear") &&
                                    transcript.activeDocument) {
                                    nlohmann::json root =
                                        transcript.activeDocument->root();
                                    std::string error;
                                    if (jcut::
                                            setSpeakerSectionTrackAssignmentsCore(
                                                &root,
                                                sectionClipIdentity,
                                                section.speakerId,
                                                section.startFrame,
                                                section.endFrame,
                                                section.wordCount,
                                                {},
                                                true,
                                                "contiguous_section_picker",
                                                {},
                                                &error)) {
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    } else if (!error.empty()) {
                                        cache.mutationError =
                                            std::move(error);
                                    }
                                }
                                ImGui::EndDisabled();
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Export")) {
                                    const auto ranges =
                                        jcut::
                                            editorTimelineRangesForTranscriptSection(
                                                snapshot,
                                                *currentClip,
                                                section.startFrame,
                                                section.endFrame);
                                    if (ranges.empty()) {
                                        cache.mutationError =
                                            "Section could not be mapped to timeline frames.";
                                    } else {
                                        const jcut::CommandResult
                                            rangeResult =
                                                applyCommand(
                                                    shellState,
                                                    jcut::
                                                        SetExportRangesCommand{
                                                            ranges});
                                        if (rangeResult.applied) {
                                            shellState->
                                                focusInspectorOutputRequested =
                                                    true;
                                            if (shellState->
                                                    exportOutputPath[0] ==
                                                '\0') {
                                                shellState->
                                                    statusMessage =
                                                        "section range loaded; choose an Output path and press Export";
                                            } else if (
                                                requestExportRender(
                                                    shellState)) {
                                                shellState->
                                                    statusMessage =
                                                        "speaker section export started";
                                            } else {
                                                shellState->
                                                    statusMessage =
                                                        "export already running";
                                            }
                                        } else {
                                            cache.mutationError =
                                                rangeResult.message;
                                        }
                                    }
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "Export this section with the current Output settings.");
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Options") &&
                                    transcript.activeDocument) {
                                    cache.
                                        speakerSectionOptionsSpeakerId =
                                            section.speakerId;
                                    cache.
                                        speakerSectionOptionsStartFrame =
                                            section.startFrame;
                                    cache.
                                        speakerSectionOptionsEndFrame =
                                            section.endFrame;
                                    cache.
                                        speakerSectionOptionsWordCount =
                                            section.wordCount;
                                    cache.speakerSectionOptionsDraft =
                                        jcut::speakerSectionOptionsCore(
                                            transcript.activeDocument->
                                                root(),
                                            sectionClipIdentity,
                                            section.speakerId,
                                            section.startFrame,
                                            section.endFrame);
                                    cache.
                                        speakerSectionOptionsPopupRequested =
                                            true;
                                }
                                ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        bool exportBusy = false;
                        {
                            std::lock_guard<std::mutex> lock(
                                shellState->exportMutex);
                            exportBusy =
                                shellState->exportRunning ||
                                shellState->exportRequested;
                        }
                        ImGui::BeginDisabled(
                            sections.empty() || exportBusy);
                        if (ImGui::Button(
                                "Export All Qualifying Sections")) {
                            shellState->
                                focusInspectorOutputRequested =
                                    true;
                            if (shellState->
                                    exportOutputPath[0] == '\0') {
                                shellState->statusMessage =
                                    "choose an Output path first; its directory and format are used for section files";
                            } else {
                                std::size_t skipped = 0;
                                const std::size_t queued =
                                    requestSpeakerSectionExportBatch(
                                        shellState,
                                        snapshot,
                                        *currentClip,
                                        transcript.activeDocument->
                                            root(),
                                        sections,
                                        &skipped);
                                if (queued > 0) {
                                    shellState->statusMessage =
                                        "queued " +
                                        std::to_string(queued) +
                                        " speaker section export" +
                                        (queued == 1 ? "" : "s") +
                                        (skipped > 0
                                            ? "; skipped " +
                                                std::to_string(
                                                    skipped) +
                                                " existing, duplicate, or unmapped"
                                            : "");
                                } else {
                                    shellState->statusMessage =
                                        "no speaker section exports were queued";
                                }
                            }
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(
                                ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip(
                                "Coalesce adjacent same-speaker rows and export each remaining section to the configured Output directory.");
                        }
                        if (std::exchange(
                                cache.
                                    speakerSectionOptionsPopupRequested,
                                false)) {
                            ImGui::OpenPopup(
                                "Speaker Section Options");
                        }
                        if (ImGui::BeginPopupModal(
                                "Speaker Section Options",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
                            auto& options =
                                cache.speakerSectionOptionsDraft;
                            ImGui::Text(
                                "%s | frames %lld-%lld",
                                cache.
                                    speakerSectionOptionsSpeakerId.
                                    c_str(),
                                static_cast<long long>(
                                    cache.
                                        speakerSectionOptionsStartFrame),
                                static_cast<long long>(
                                    cache.
                                        speakerSectionOptionsEndFrame));
                            ImGui::SetNextItemWidth(180.0f);
                            ImGui::InputDouble(
                                "Rotation (degrees)",
                                &options.rotationDegrees,
                                0.1,
                                1.0,
                                "%.1f");
                            options.rotationDegrees = std::clamp(
                                options.rotationDegrees,
                                -180.0,
                                180.0);
                            ImGui::SeparatorText("Grading");
                            ImGui::Checkbox(
                                "Add Grading Keyframes",
                                &options.gradingEnabled);
                            ImGui::InputDouble(
                                "Brightness",
                                &options.gradingBrightness,
                                0.01,
                                0.1,
                                "%.3f");
                            ImGui::InputDouble(
                                "Contrast",
                                &options.gradingContrast,
                                0.05,
                                0.25,
                                "%.3f");
                            ImGui::InputDouble(
                                "Saturation",
                                &options.gradingSaturation,
                                0.05,
                                0.25,
                                "%.3f");
                            ImGui::SeparatorText("Mask Override");
                            ImGui::Checkbox(
                                "Enable Mask Override",
                                &options.maskEnabled);
                            ImGui::InputDouble(
                                "Mask Opacity",
                                &options.maskOpacity,
                                0.01,
                                0.1,
                                "%.3f");
                            ImGui::InputDouble(
                                "Mask Feather",
                                &options.maskFeather,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::InputDouble(
                                "Mask Blur",
                                &options.maskBlur,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::InputDouble(
                                "Mask Dilate",
                                &options.maskDilate,
                                0.5,
                                5.0,
                                "%.2f");
                            ImGui::Checkbox(
                                "Invert Section Mask",
                                &options.maskInvert);
                            ImGui::BeginDisabled(
                                !transcript.activeCutMutable);
                            if (ImGui::Button("Apply") &&
                                transcript.activeDocument) {
                                nlohmann::json root =
                                    transcript.activeDocument->root();
                                std::string error;
                                if (jcut::setSpeakerSectionOptionsCore(
                                        &root,
                                        sectionClipIdentity,
                                        cache.
                                            speakerSectionOptionsSpeakerId,
                                        cache.
                                            speakerSectionOptionsStartFrame,
                                        cache.
                                            speakerSectionOptionsEndFrame,
                                        cache.
                                            speakerSectionOptionsWordCount,
                                        options,
                                        {},
                                        &error)) {
                                    {
                                        std::lock_guard<std::mutex>
                                            lock(
                                                shellState->
                                                    runtimeMutex);
                                        shellState->runtime.
                                            beginHistoryTransaction();
                                    }
                                    const bool transcriptSaved =
                                        saveTranscriptMutation(
                                            shellState,
                                            &cache,
                                            std::move(root));
                                    bool gradingApplied = true;
                                    if (transcriptSaved &&
                                        options.gradingEnabled) {
                                        const std::int64_t
                                            maximumLocalFrame =
                                                std::max(
                                                    0,
                                                    currentClip->
                                                        durationFrames -
                                                        1);
                                        const std::int64_t
                                            startLocalFrame =
                                                std::clamp<
                                                    std::int64_t>(
                                                    cache.
                                                        speakerSectionOptionsStartFrame -
                                                        std::max<
                                                            std::int64_t>(
                                                            0,
                                                            currentClip->
                                                                sourceInFrame),
                                                    0,
                                                    maximumLocalFrame);
                                        const std::int64_t
                                            endLocalFrame =
                                                std::clamp<
                                                    std::int64_t>(
                                                    cache.
                                                        speakerSectionOptionsEndFrame -
                                                        std::max<
                                                            std::int64_t>(
                                                            0,
                                                            currentClip->
                                                                sourceInFrame),
                                                    0,
                                                    maximumLocalFrame);
                                        std::vector<std::int64_t>
                                            frames{startLocalFrame};
                                        if (endLocalFrame !=
                                            startLocalFrame) {
                                            frames.push_back(
                                                endLocalFrame);
                                        }
                                        for (const std::int64_t frame :
                                             frames) {
                                            jcut::
                                                EditorGradingKeyframe
                                                    keyframe =
                                                        jcut::
                                                            evaluateEditorClipGradingAtLocalFrame(
                                                                *currentClip,
                                                                frame);
                                            keyframe.frame = frame;
                                            keyframe.brightness =
                                                std::clamp(
                                                    options.
                                                        gradingBrightness,
                                                    -10.0,
                                                    10.0);
                                            keyframe.contrast =
                                                std::clamp(
                                                    options.
                                                        gradingContrast,
                                                    0.05,
                                                    10.0);
                                            keyframe.saturation =
                                                std::clamp(
                                                    options.
                                                        gradingSaturation,
                                                    0.0,
                                                    10.0);
                                            keyframe.
                                                linearInterpolation =
                                                    true;
                                            const auto result =
                                                applyCommand(
                                                    shellState,
                                                    jcut::
                                                        UpsertGradingKeyframeCommand{
                                                            currentClip->
                                                                id,
                                                            keyframe});
                                            gradingApplied &=
                                                result.applied;
                                        }
                                    }
                                    {
                                        std::lock_guard<std::mutex>
                                            lock(
                                                shellState->
                                                    runtimeMutex);
                                        shellState->runtime.
                                            endHistoryTransaction();
                                    }
                                    if (transcriptSaved &&
                                        gradingApplied) {
                                        ImGui::CloseCurrentPopup();
                                    } else if (
                                        transcriptSaved &&
                                        !gradingApplied) {
                                        cache.mutationError =
                                            "Section options were saved, "
                                            "but grading keyframes could "
                                            "not be applied.";
                                    }
                                } else if (!error.empty()) {
                                    cache.mutationError =
                                        std::move(error);
                                } else {
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel")) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                    }
                    if (ImGui::BeginTable(
                            "SpeakersRoster",
                            6,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Organization");
                        ImGui::TableSetupColumn("Words", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                        ImGui::TableHeadersRow();
                        for (const auto& profile : profiles) {
                            ImGui::PushID(profile.id.c_str());
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            if (ImGui::Selectable(
                                    profile.id.c_str(),
                                    cache.selectedSpeakerId == profile.id,
                                    ImGuiSelectableFlags_SpanAllColumns |
                                        ImGuiSelectableFlags_AllowOverlap) &&
                                transcript.activeCutMutable) {
                                cache.selectedSpeakerId = profile.id;
                                cache.speakerNameDraft = profile.name;
                                cache.speakerOrganizationDraft = profile.organization;
                                cache.speakerXDraft = profile.x;
                                cache.speakerYDraft = profile.y;
                                cache.mutationError.clear();
                            }
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(profile.name.empty()
                                ? profile.id.c_str() : profile.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(profile.organization.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", profile.wordCount);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", profile.x);
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", profile.y);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty());
                    ImGui::SeparatorText("Speaker Profile");
                    ImGui::InputText("Name", &cache.speakerNameDraft);
                    ImGui::InputText("Organization", &cache.speakerOrganizationDraft);
                    ImGui::InputDouble(
                        "Title X (0-1)", &cache.speakerXDraft, 0.01, 0.1, "%.3f");
                    ImGui::InputDouble(
                        "Title Y (0-1)", &cache.speakerYDraft, 0.01, 0.1, "%.3f");
                    if (ImGui::Button("Save Speaker Profile") &&
                        transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        jcut::TranscriptSpeakerProfilePatch patch;
                        patch.name = cache.speakerNameDraft;
                        patch.organization = cache.speakerOrganizationDraft;
                        patch.x = cache.speakerXDraft;
                        patch.y = cache.speakerYDraft;
                        std::string error;
                        if (jcut::patchTranscriptSpeakerProfile(
                                &root, cache.selectedSpeakerId, patch, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!cache.mutationError.empty()) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                            "%s", cache.mutationError.c_str());
                    }

                    ImGui::SeparatorText("Animated Speaker Introductions");
                    const char* speakerTitleStyles[] = {
                        "Slide from left",
                        "Slide from right",
                        "Rise from bottom",
                        "Drop from top",
                        "3D wrap around speaker",
                    };
                    ImGui::Combo(
                        "Fly-in Style",
                        &cache.speakerTitleStyle,
                        speakerTitleStyles,
                        static_cast<int>(std::size(speakerTitleStyles)));
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Title Seconds",
                        &cache.speakerTitleDurationSeconds,
                        0.1, 0.5, "%.2f");
                    cache.speakerTitleDurationSeconds = std::clamp(
                        cache.speakerTitleDurationSeconds, 1.0, 30.0);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Delay Seconds",
                        &cache.speakerTitleDelaySeconds,
                        0.05, 0.25, "%.2f");
                    cache.speakerTitleDelaySeconds = std::clamp(
                        cache.speakerTitleDelaySeconds, 0.0, 10.0);
                    ImGui::Checkbox(
                        "Also show near section end",
                        &cache.speakerTitleShowAtSectionEnd);
                    ImGui::Checkbox(
                        "Respect Speech Filter Timing",
                        &cache.speakerTitleRespectSpeechFilterTiming);
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Repeat Cadence Seconds (0 = off)",
                        &cache.speakerTitleCadenceSeconds,
                        5.0, 60.0, "%.2f");
                    cache.speakerTitleCadenceSeconds = std::clamp(
                        cache.speakerTitleCadenceSeconds, 0.0, 3600.0);
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputDouble(
                        "Fly Seconds",
                        &cache.speakerTitleFlySeconds,
                        0.05, 0.25, "%.2f");
                    cache.speakerTitleFlySeconds = std::clamp(
                        cache.speakerTitleFlySeconds, 0.1, 10.0);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Include organization",
                        &cache.speakerTitleShowOrganization);
                    if (ImGui::Button("Generate Speaker Introductions") &&
                        transcript.activeDocument) {
                        jcut::SpeakerTitleFlyInSettingsCore settings;
                        settings.style =
                            static_cast<jcut::SpeakerTitleFlyInStyleCore>(
                                std::clamp(cache.speakerTitleStyle, 0, 4));
                        settings.titleDurationFrames =
                            std::max<std::int64_t>(1, std::llround(
                                cache.speakerTitleDurationSeconds * 30.0));
                        settings.titleStartDelayFrames =
                            std::max<std::int64_t>(0, std::llround(
                                cache.speakerTitleDelaySeconds * 30.0));
                        settings.showAtSectionEnd =
                            cache.speakerTitleShowAtSectionEnd;
                        settings.respectSpeechFilterTiming =
                            cache.speakerTitleRespectSpeechFilterTiming;
                        settings.cadenceFrames =
                            std::max<std::int64_t>(0, std::llround(
                                cache.speakerTitleCadenceSeconds * 30.0));
                        settings.flyInFrames =
                            std::max<std::int64_t>(1, std::llround(
                                cache.speakerTitleFlySeconds * 30.0));
                        settings.flyOutFrames = settings.flyInFrames;
                        settings.showSpeakerOrganization =
                            cache.speakerTitleShowOrganization;
                        std::vector<jcut::EditorClip> generated =
                            jcut::makeSpeakerTitleClipsCore(
                                *currentClip,
                                *transcript.activeDocument,
                                0,
                                settings);
                        if (generated.empty()) {
                            shellState->statusMessage =
                                "no speaker changes were found in the selected transcript range";
                        } else {
                            applyCommand(
                                shellState,
                                jcut::ReplaceSpeakerTitleClipsCommand{
                                    currentClip->id,
                                    std::move(generated)});
                        }
                    }

                    ImGui::SeparatorText("Transcript Mining");
                    const auto setMiningProposals =
                        [&](std::string label,
                            std::vector<jcut::TranscriptMiningProposal> proposals) {
                            cache.miningProposalLabel = std::move(label);
                            cache.miningProposals = std::move(proposals);
                            cache.miningProposalSelected.assign(
                                cache.miningProposals.size(), 1);
                        };
                    if (ImGui::Button("Find Speaker Names") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Speaker name candidates",
                            jcut::mineTranscriptSpeakerNames(
                                transcript.activeDocument->root()));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Find Organizations") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Organization candidates",
                            jcut::mineTranscriptOrganizations(
                                transcript.activeDocument->root()));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clean Spurious Labels") &&
                        transcript.activeDocument) {
                        setMiningProposals(
                            "Spurious speaker-label cleanup",
                            jcut::mineSpuriousSpeakerAssignments(
                                transcript.activeDocument->root()));
                    }
                    ImGui::BeginDisabled(
                        !transcript.activeDocument ||
                        shellState->aiTaskRunning ||
                        !shellState->featureAiSpeakerCleanup ||
                        !shellState->aiAccount.aiEnabled);
                    if (ImGui::Button("Mine Profiles with Cloud AI") &&
                        transcript.activeDocument) {
                        startCloudSpeakerMining(
                            shellState,
                            transcript.activeDocument->root(),
                            cache.sourceKey,
                            snapshot);
                    }
                    ImGui::EndDisabled();
                    if (shellState->aiTaskRunning &&
                        shellState->aiTaskPurpose ==
                            AiTaskPurpose::CloudSpeakerMining) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("Mining...");
                    }
                    if (!cache.miningProposalLabel.empty()) {
                        ImGui::Text(
                            "%s: %zu proposal%s",
                            cache.miningProposalLabel.c_str(),
                            cache.miningProposals.size(),
                            cache.miningProposals.size() == 1 ? "" : "s");
                    }
                    if (!cache.miningProposals.empty() &&
                        ImGui::BeginTable(
                            "TranscriptMiningProposals",
                            6,
                            ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0.0f, 180.0f))) {
                        ImGui::TableSetupColumn(
                            "Apply", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                        ImGui::TableSetupColumn("Target");
                        ImGui::TableSetupColumn("Field");
                        ImGui::TableSetupColumn("Current");
                        ImGui::TableSetupColumn("Proposed");
                        ImGui::TableSetupColumn(
                            "Confidence",
                            ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        ImGui::TableHeadersRow();
                        for (std::size_t index = 0;
                             index < cache.miningProposals.size(); ++index) {
                            const auto& proposal =
                                cache.miningProposals[index];
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            bool selected =
                                cache.miningProposalSelected[index] != 0;
                            if (ImGui::Checkbox("##apply", &selected)) {
                                cache.miningProposalSelected[index] =
                                    selected ? 1 : 0;
                            }
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(proposal.targetId.c_str());
                            ImGui::TableNextColumn();
                            const char* field =
                                proposal.field ==
                                    jcut::TranscriptMiningField::SpeakerName
                                ? "Name"
                                : (proposal.field ==
                                       jcut::TranscriptMiningField::
                                           SpeakerOrganization
                                    ? "Organization" : "Speaker");
                            ImGui::TextUnformatted(field);
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                proposal.currentValue.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                proposal.proposedValue.c_str());
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip(
                                    "%s", proposal.rationale.c_str());
                            }
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.0f%%", proposal.confidence * 100.0);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    const bool haveSelectedMiningProposal =
                        std::any_of(
                            cache.miningProposalSelected.begin(),
                            cache.miningProposalSelected.end(),
                            [](std::uint8_t selected) {
                                return selected != 0;
                            });
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        !haveSelectedMiningProposal);
                    if (ImGui::Button("Apply Selected Mining Proposals") &&
                        transcript.activeDocument) {
                        std::vector<jcut::TranscriptMiningProposal> selected;
                        for (std::size_t index = 0;
                             index < cache.miningProposals.size(); ++index) {
                            if (cache.miningProposalSelected[index]) {
                                selected.push_back(
                                    cache.miningProposals[index]);
                            }
                        }
                        nlohmann::json root =
                            transcript.activeDocument->root();
                        std::string error;
                        if (jcut::applyTranscriptMiningProposals(
                                &root, selected, &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                            cache.miningProposals.clear();
                            cache.miningProposalSelected.clear();
                            cache.miningProposalLabel.clear();
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::SeparatorText("Face Detection Job");
                    const jcut::FaceProcessingJobSnapshot faceJob =
                        shellState->faceProcessingJob.snapshot();
                    const int faceJobState = static_cast<int>(faceJob.state);
                    if (cache.faceJobLastState != faceJobState) {
                        cache.faceJobLastState = faceJobState;
                        if (faceJob.state ==
                            jcut::FaceProcessingJobSnapshot::State::Completed) {
                            shellState->statusMessage = faceJob.status;
                            const std::string completedClipIdentity =
                                currentClip->persistentId.empty()
                                    ? std::to_string(currentClip->id)
                                    : currentClip->persistentId;
                            cache.faceInspection = jcut::inspectFaceArtifacts(
                                transcript.activePath, completedClipIdentity);
                            cache.selectedFaceTrackIds =
                                filteredPersistedFaceTrackIds(
                                    *currentClip,
                                    cache.faceInspection.tracks);
                        } else if (faceJob.state ==
                                   jcut::FaceProcessingJobSnapshot::State::Failed ||
                                   faceJob.state ==
                                   jcut::FaceProcessingJobSnapshot::State::Paused) {
                            shellState->statusMessage = faceJob.status;
                        }
                    }
                    if (faceJob.state !=
                        jcut::FaceProcessingJobSnapshot::State::Idle) {
                        ImGui::TextWrapped("%s", faceJob.status.c_str());
                        if (!faceJob.outputDirectory.empty()) {
                            ImGui::TextDisabled(
                                "Artifacts: %s", faceJob.outputDirectory.c_str());
                        }
                        if (!faceJob.logPath.empty()) {
                            ImGui::TextDisabled("Log: %s", faceJob.logPath.c_str());
                        }
                    } else {
                        ImGui::TextDisabled(
                            "Runs the shared offscreen SCRFD/Vulkan generator and writes "
                            "the same resumable sidecar artifacts used by Qt.");
                    }
                    const fs::path faceSourceMediaPath =
                        resolvedClipSourcePath(*shellState, *currentClip);
                    const std::string faceClipIdentity =
                        currentClip->persistentId.empty()
                            ? std::to_string(currentClip->id)
                            : currentClip->persistentId;
                    const std::string faceOutputDirectory =
                        jcut::faceProcessingSidecarDirectory(
                            pathString(faceSourceMediaPath),
                            faceClipIdentity);
                    const jcut::FaceProcessingLaunchControl
                        savedFaceLaunchControl =
                            jcut::loadFaceProcessingLaunchControl(
                                faceOutputDirectory);
                    ImGui::BeginDisabled(faceJob.active());
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt("Stride", &cache.faceJobStride);
                    cache.faceJobStride = std::clamp(cache.faceJobStride, 1, 120);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputDouble(
                        "Threshold", &cache.faceJobThreshold, 0.01, 0.1, "%.3f");
                    cache.faceJobThreshold =
                        std::clamp(cache.faceJobThreshold, 0.0, 1.0);
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt("Workers", &cache.faceJobWorkers);
                    cache.faceJobWorkers =
                        std::clamp(cache.faceJobWorkers, 1, 10);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputInt(
                        "Pipeline Slots", &cache.faceJobPipelineSlots);
                    cache.faceJobPipelineSlots =
                        std::clamp(cache.faceJobPipelineSlots, 1, 10);
                    if (savedFaceLaunchControl.hasRecommendation) {
                        ImGui::TextDisabled(
                            "Saved benchmark recommendation: %d worker(s), %d slot(s)",
                            savedFaceLaunchControl.detectorWorkers,
                            savedFaceLaunchControl.detectorPipelineSlots);
                        if (ImGui::SmallButton(
                                "Apply Saved Topology Recommendation")) {
                            cache.faceJobWorkers =
                                savedFaceLaunchControl.detectorWorkers;
                            cache.faceJobPipelineSlots =
                                savedFaceLaunchControl.detectorPipelineSlots;
                        }
                    } else if (!savedFaceLaunchControl.error.empty()) {
                        ImGui::TextDisabled(
                            "%s", savedFaceLaunchControl.error.c_str());
                    }
                    ImGui::Checkbox(
                        "Benchmark topology before launch",
                        &cache.faceJobBenchmarkTopology);
                    ImGui::Checkbox(
                        "Apply selected clip grading during detection",
                        &cache.faceJobApplyClipGrading);
                    ImGui::Checkbox(
                        "Primary face only", &cache.faceJobPrimaryOnly);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Small-face fallback", &cache.faceJobSmallFaceFallback);
                    ImGui::Checkbox("SCRFD tiling", &cache.faceJobTiling);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Allow CPU upload compatibility",
                        &cache.faceJobAllowCpuFallback);
                    ImGui::Checkbox(
                        "Generator control window",
                        &cache.faceJobControlWindow);
                    ImGui::SameLine();
                    ImGui::Checkbox(
                        "Live preview window",
                        &cache.faceJobLivePreview);
                    ImGui::Checkbox(
                        "Restart from scratch (clear resume checkpoint)",
                        &cache.faceJobRestartFromScratch);
                    const fs::path configuredProxyPath =
                        resolvedClipProxyPath(*shellState, *currentClip);
                    const bool faceProxyAvailable =
                        !currentClip->proxyPath.empty() &&
                        isImportableMediaPath(configuredProxyPath);
                    ImGui::BeginDisabled(!faceProxyAvailable);
                    ImGui::Checkbox(
                        "Use proxy media as detector input",
                        &cache.faceJobUseProxySource);
                    ImGui::EndDisabled();
                    if (!faceProxyAvailable) {
                        cache.faceJobUseProxySource = false;
                        ImGui::SameLine();
                        ImGui::TextDisabled("(no playable proxy configured)");
                    }
                    if (ImGui::Button("Generate Detection + Continuity")) {
                        const fs::path mediaPath = cache.faceJobUseProxySource
                            ? configuredProxyPath
                            : faceSourceMediaPath;
                        jcut::FaceProcessingJobRequest request;
                        request.executablePath =
                            pathString(executableDirPath() /
                                       "jcut_vulkan_facedetections_offscreen");
                        request.mediaPath = pathString(mediaPath);
                        request.transcriptPath = transcript.activePath;
                        request.clipId = faceClipIdentity;
                        request.outputDirectory = faceOutputDirectory;
                        request.detectorSettingsPath =
                            pathString(faceSourceMediaPath.parent_path() /
                                (faceSourceMediaPath.stem().string() +
                                 "_detectorsettings.json"));
                        if (!fs::is_regular_file(request.detectorSettingsPath)) {
                            request.detectorSettingsPath.clear();
                        }
                        request.startFrame =
                            std::max<std::int64_t>(0, currentClip->sourceInFrame);
                        request.maxFrames = currentClip->sourceDurationFrames > 0
                            ? currentClip->sourceDurationFrames
                            : std::max(0, currentClip->durationFrames);
                        request.stride = cache.faceJobStride;
                        request.detectorWorkers = cache.faceJobWorkers;
                        request.detectorPipelineSlots =
                            cache.faceJobPipelineSlots;
                        request.threshold = cache.faceJobThreshold;
                        request.primaryFaceOnly = cache.faceJobPrimaryOnly;
                        request.smallFaceFallback =
                            cache.faceJobSmallFaceFallback;
                        request.scrfdTiling = cache.faceJobTiling;
                        request.allowCpuUploadFallback =
                            cache.faceJobAllowCpuFallback;
                        request.controlWindow =
                            cache.faceJobControlWindow;
                        request.livePreview =
                            cache.faceJobLivePreview;
                        request.restartFromScratch =
                            cache.faceJobRestartFromScratch;
                        request.benchmarkTopology =
                            cache.faceJobBenchmarkTopology;
                        request.applyClipGrading =
                            cache.faceJobApplyClipGrading;
                        if (request.applyClipGrading) {
                            request.clipJson =
                                jcut::toLegacyClipJson(*currentClip).dump();
                        }
                        std::string error;
                        if (!shellState->faceProcessingJob.start(request, &error)) {
                            shellState->statusMessage = error.empty()
                                ? "could not start face detection job" : error;
                        } else {
                            shellState->statusMessage =
                                "face detection and continuity generation started";
                        }
                    }
                    ImGui::EndDisabled();
                    if (faceJob.active()) {
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel Face Job")) {
                            shellState->faceProcessingJob.cancel();
                        }
                    }

                    ImGui::SeparatorText("Continuity Tracks");
                    const std::string clipIdentity = currentClip->persistentId.empty()
                        ? std::to_string(currentClip->id)
                        : currentClip->persistentId;
                    const std::string artifactContext =
                        transcript.activePath + "::" + clipIdentity;
                    if (cache.faceArtifactContext != artifactContext) {
                        cache.faceArtifactContext = artifactContext;
                        cache.faceInspection = jcut::inspectFaceArtifacts(
                            transcript.activePath, clipIdentity);
                        cache.selectedFaceTrackIds =
                            filteredPersistedFaceTrackIds(
                                *currentClip,
                                cache.faceInspection.tracks);
                    }
                    if (ImGui::SmallButton("Refresh Face Artifacts")) {
                        cache.faceInspection = jcut::inspectFaceArtifacts(
                            transcript.activePath, clipIdentity);
                        cache.selectedFaceTrackIds =
                            filteredPersistedFaceTrackIds(
                                *currentClip,
                                cache.faceInspection.tracks);
                    }
                    const auto assignments =
                        jcut::transcriptSpeakerTrackAssignments(
                            transcript.activeDocument->root(), clipIdentity);
                    const auto assignedIdentity = [&](int trackId) -> std::string {
                        const auto found = std::find_if(
                            assignments.begin(), assignments.end(),
                            [&](const auto& assignment) {
                                return assignment.trackId == trackId;
                            });
                        return found == assignments.end()
                            ? std::string{} : found->identityId;
                    };
                    if (!cache.faceInspection.error.empty()) {
                        ImGui::TextDisabled("%s", cache.faceInspection.error.c_str());
                    } else {
                        ImGui::Text(
                            "%zu tracks | detector: %s | frames: %lld | identity: %zu/%zu",
                            cache.faceInspection.tracks.size(),
                            cache.faceInspection.detectorMode.empty()
                                ? "unknown" : cache.faceInspection.detectorMode.c_str(),
                            static_cast<long long>(cache.faceInspection.rawFrameCount),
                            cache.faceInspection.identityAssignmentCount,
                            cache.faceInspection.identityClusterCount);
                        if (!cache.faceInspection.warning.empty()) {
                            ImGui::TextColored(
                                ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                                "%s", cache.faceInspection.warning.c_str());
                        }
                        if (ImGui::BeginTable(
                                "ContinuityTracks",
                                7,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollY,
                                ImVec2(0.0f, 210.0f))) {
                            ImGui::TableSetupColumn(
                                "Use", ImGuiTableColumnFlags_WidthFixed, 38.0f);
                            ImGui::TableSetupColumn(
                                "Track", ImGuiTableColumnFlags_WidthFixed, 52.0f);
                            ImGui::TableSetupColumn(
                                "Samples", ImGuiTableColumnFlags_WidthFixed, 62.0f);
                            ImGui::TableSetupColumn("Frames");
                            ImGui::TableSetupColumn("Position");
                            ImGui::TableSetupColumn(
                                "Score", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                            ImGui::TableSetupColumn("Assigned");
                            ImGui::TableHeadersRow();
                            for (const auto& track : cache.faceInspection.tracks) {
                                ImGui::PushID(track.trackId);
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                auto selected = std::find(
                                    cache.selectedFaceTrackIds.begin(),
                                    cache.selectedFaceTrackIds.end(),
                                    track.trackId);
                                bool checked =
                                    selected != cache.selectedFaceTrackIds.end();
                                if (ImGui::Checkbox("##track", &checked)) {
                                    if (checked && selected == cache.selectedFaceTrackIds.end()) {
                                        cache.selectedFaceTrackIds.push_back(track.trackId);
                                    } else if (!checked &&
                                               selected != cache.selectedFaceTrackIds.end()) {
                                        cache.selectedFaceTrackIds.erase(selected);
                                    }
                                    persistSelectedFaceTrackIds(
                                        shellState,
                                        *currentClip,
                                        cache.selectedFaceTrackIds);
                                }
                                ImGui::TableNextColumn();
                                ImGui::Text("%d", track.trackId);
                                ImGui::TableNextColumn();
                                ImGui::Text("%zu", track.sampleCount);
                                ImGui::TableNextColumn();
                                ImGui::Text("%lld-%lld",
                                    static_cast<long long>(track.firstFrame),
                                    static_cast<long long>(track.lastFrame));
                                ImGui::TableNextColumn();
                                ImGui::Text("%.2f, %.2f / %.2f",
                                    track.x, track.y, track.box);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.2f", track.score);
                                ImGui::TableNextColumn();
                                const std::string identity =
                                    assignedIdentity(track.trackId);
                                ImGui::TextUnformatted(
                                    identity.empty() ? "Unassigned" : identity.c_str());
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                    }
                    std::vector<jcut::FaceContinuityTrackCore>
                        referenceTracks;
                    constexpr std::size_t kMaxReferenceTracks = 8;
                    referenceTracks.reserve(std::min(
                        cache.selectedFaceTrackIds.size(),
                        kMaxReferenceTracks));
                    for (const int selectedTrackId :
                         cache.selectedFaceTrackIds) {
                        const auto found = std::find_if(
                            cache.faceInspection.tracks.begin(),
                            cache.faceInspection.tracks.end(),
                            [&](const auto& track) {
                                return track.trackId ==
                                    selectedTrackId;
                            });
                        if (found !=
                            cache.faceInspection.tracks.end()) {
                            referenceTracks.push_back(*found);
                            if (referenceTracks.size() ==
                                kMaxReferenceTracks) {
                                break;
                            }
                        }
                    }
                    if (!referenceTracks.empty()) {
                        std::string referenceKey = artifactContext;
                        for (const auto& track : referenceTracks) {
                            referenceKey += "::" +
                                std::to_string(track.trackId) + ":" +
                                std::to_string(track.firstFrame) + ":" +
                                std::to_string(track.x) + ":" +
                                std::to_string(track.y) + ":" +
                                std::to_string(track.box);
                        }
                        shellState->faceReferenceDesiredKey =
                            std::move(referenceKey);
                        shellState->faceReferenceSourcePath =
                            pathString(faceSourceMediaPath);
                        shellState->faceReferenceTracks =
                            referenceTracks;
                    } else {
                        shellState->faceReferenceDesiredKey.clear();
                        shellState->faceReferenceTracks.clear();
                    }
                    if (!referenceTracks.empty() &&
                        shellState->faceReferenceTextureId != 0 &&
                        shellState->faceReferenceLoadedKey ==
                            shellState->faceReferenceDesiredKey) {
                        ImGui::SeparatorText("Selected References");
                        const float naturalWidth =
                            static_cast<float>(
                                shellState->faceReferenceSize.width) *
                            0.75f;
                        const float displayWidth = std::min(
                            ImGui::GetContentRegionAvail().x,
                            naturalWidth);
                        const float displayHeight =
                            shellState->faceReferenceSize.width > 0
                            ? displayWidth *
                                static_cast<float>(
                                    shellState->
                                        faceReferenceSize.height) /
                                static_cast<float>(
                                    shellState->
                                        faceReferenceSize.width)
                            : 120.0f;
                        ImGui::Image(
                            shellState->faceReferenceTextureId,
                            ImVec2(displayWidth, displayHeight));
                        if (ImGui::BeginTable(
                                "##reference_diagnostics",
                                4,
                                ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Track");
                            ImGui::TableSetupColumn("Frame");
                            ImGui::TableSetupColumn("Center / box");
                            ImGui::TableSetupColumn("Score");
                            ImGui::TableHeadersRow();
                            for (const auto& track : referenceTracks) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text("T%d", track.trackId);
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%lld",
                                    static_cast<long long>(
                                        track.firstFrame));
                                ImGui::TableNextColumn();
                                ImGui::Text(
                                    "%.3f, %.3f / %.3f",
                                    track.x,
                                    track.y,
                                    track.box);
                                ImGui::TableNextColumn();
                                ImGui::Text("%.3f", track.score);
                            }
                            ImGui::EndTable();
                        }
                        if (cache.selectedFaceTrackIds.size() >
                            kMaxReferenceTracks) {
                            ImGui::TextDisabled(
                                "Showing the first %zu of %zu selected tracks.",
                                kMaxReferenceTracks,
                                cache.selectedFaceTrackIds.size());
                        }
                    } else if (!referenceTracks.empty() &&
                               shellState->
                                   faceReferenceRunning) {
                        ImGui::TextDisabled(
                            "Decoding selected face references...");
                    } else if (!referenceTracks.empty() &&
                               !shellState->
                                   faceReferenceError.empty()) {
                        ImGui::TextDisabled(
                            "%s",
                            shellState->
                                faceReferenceError.c_str());
                    }
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty() ||
                        cache.selectedFaceTrackIds.empty());
                    if (ImGui::Button("Assign Selected Tracks") &&
                        transcript.activeDocument) {
                        std::vector<jcut::TranscriptTrackAssignmentAnchor> anchors;
                        for (const auto& track : cache.faceInspection.tracks) {
                            if (std::find(
                                    cache.selectedFaceTrackIds.begin(),
                                    cache.selectedFaceTrackIds.end(),
                                    track.trackId) ==
                                cache.selectedFaceTrackIds.end()) {
                                continue;
                            }
                            anchors.push_back({
                                track.trackId,
                                track.streamId,
                                std::max<std::int64_t>(0, track.firstFrame),
                                track.x,
                                track.y,
                                track.box});
                        }
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::setTranscriptSpeakerTrackAssignments(
                                &root,
                                clipIdentity,
                                cache.selectedSpeakerId,
                                anchors,
                                false,
                                {},
                                &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(
                        !transcript.activeCutMutable ||
                        cache.selectedSpeakerId.empty());
                    if (ImGui::Button("Clear Speaker Tracks") &&
                        transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::setTranscriptSpeakerTrackAssignments(
                                &root,
                                clipIdentity,
                                cache.selectedSpeakerId,
                                {},
                                true,
                                {},
                                &error)) {
                            saveTranscriptMutation(
                                shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(cache.selectedFaceTrackIds.size() != 1);
                    if (ImGui::Button("View Track Reference")) {
                        const int selectedTrackId =
                            cache.selectedFaceTrackIds.front();
                        const auto selectedTrack = std::find_if(
                            cache.faceInspection.tracks.begin(),
                            cache.faceInspection.tracks.end(),
                            [&](const auto& track) {
                                return track.trackId == selectedTrackId;
                            });
                        if (selectedTrack != cache.faceInspection.tracks.end()) {
                            const std::int64_t timelineFrame =
                                jcut::faceTrackAnchorTimelineFrame(
                                    selectedTrack->firstFrame,
                                    currentClip->sourceInFrame,
                                    currentClip->startFrame,
                                    currentClip->durationFrames,
                                    currentClip->playbackRate);
                            applyCommand(
                                shellState,
                                jcut::SeekToFrameCommand{
                                    static_cast<int>(timelineFrame)});
                            shellState->statusMessage =
                                "showing selected face-track reference frame";
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::TextDisabled(
                        "The selected reference track is outlined in Program and shown as a "
                        "shared-policy face crop above.");
                }
            }
            ImGui::EndTabItem();
        }
}
