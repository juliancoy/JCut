#pragma once

void drawInspectorTab10(
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
                "Properties", nullptr, inspectorTabFlags("Properties"))) {
            drawInspectorHeading("Properties", snapshot, currentClip);
            drawClipSummaryTable(snapshot, currentClip);
            const bool visualClip = currentClip && currentClip->videoEnabled;
            int zLevel = currentClip &&
                    currentClip->zLevel != std::numeric_limits<int>::min()
                ? currentClip->zLevel
                : (currentClip
                       ? -std::max(0, currentClip->trackId - 1) * 100
                       : 0);
            bool automaticZ = !currentClip || !currentClip->zLevelUserSet;
            ImGui::SeparatorText("Compositing Order");
            ImGui::BeginDisabled(!visualClip);
            if (ImGui::Checkbox("Automatic Z from timeline row", &automaticZ) &&
                currentClip) {
                applyCommand(
                    shellState,
                    jcut::SetClipZLevelCommand{
                        currentClip->id, zLevel, automaticZ});
            }
            ImGui::BeginDisabled(automaticZ);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputInt("Z Level", &zLevel) && currentClip) {
                applyCommand(
                    shellState,
                    jcut::SetClipZLevelCommand{
                        currentClip->id, zLevel, false});
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                "Higher Z draws in front; timeline nesting is unchanged.");
            if (currentClip &&
                currentClip->clipRole == "speaker_title") {
                ImGui::TextDisabled(
                    "This changes the complete generated title layer.");
            }
            if (currentTrack) {
                ImGui::Separator();
                ImGui::Text("Track %d", currentTrack->id);
                ImGui::TextWrapped("%s", currentTrack->label.c_str());
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab11(
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
        if (ImGui::BeginTabItem("Clips")) {
            drawInspectorHeading("Clips", snapshot, currentClip);
            drawClipsTable(shellState, snapshot);
            ImGui::EndTabItem();
        }
}

void drawInspectorTab12(
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
        if (ImGui::BeginTabItem("History")) {
            drawInspectorHeading("History", snapshot, currentClip);
            bool canUndo = false;
            bool canRedo = false;
            std::size_t undoDepth = 0;
            std::size_t redoDepth = 0;
            {
                std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                canUndo = shellState->runtime.canUndo();
                canRedo = shellState->runtime.canRedo();
                undoDepth = shellState->runtime.undoDepth();
                redoDepth = shellState->runtime.redoDepth();
            }
            ImGui::BeginDisabled(!canUndo);
            if (ImGui::Button("Undo")) {
                applyCommand(shellState, jcut::UndoCommand{});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canRedo);
            if (ImGui::Button("Redo")) {
                applyCommand(shellState, jcut::RedoCommand{});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("HistoryTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Stack");
                ImGui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();
                drawReadOnlyTableRow("Undo", std::to_string(undoDepth) + " snapshots");
                drawReadOnlyTableRow("Redo", std::to_string(redoDepth) + " snapshots");
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Saved Project History");
            if (!shellState->usesQtProjectStorage) {
                ImGui::TextDisabled(
                    "Saved history navigation is available only for Qt project storage.");
            } else {
                if (ImGui::SmallButton("Refresh Saved History")) {
                    shellState->projectHistoryRefreshRequested = true;
                }
                if (shellState->projectHistoryRefreshRequested) {
                    std::string historyError;
                    const std::optional<
                        std::vector<jcut::ImGuiProjectHistoryEntry>> entries =
                            jcut::listImGuiProjectHistoryEntries(
                                currentProjectSession(*shellState, snapshot),
                                &historyError);
                    shellState->projectHistoryEntries =
                        entries.value_or(
                            std::vector<jcut::ImGuiProjectHistoryEntry>{});
                    shellState->projectHistoryError = std::move(historyError);
                    shellState->projectHistoryRefreshRequested = false;
                }
                if (!shellState->projectHistoryError.empty()) {
                    ImGui::TextWrapped(
                        "%s", shellState->projectHistoryError.c_str());
                } else {
                    const bool savedHistoryBlocked =
                        documentIsDirty(*shellState, snapshot);
                    if (savedHistoryBlocked) {
                        ImGui::TextDisabled(
                            "Save changes before restoring a saved project-history snapshot.");
                    }
                    if (ImGui::BeginTable(
                            "SavedProjectHistoryTable",
                            5,
                            ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0.0f, 170.0f))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Snapshot");
                        ImGui::TableSetupColumn("Project");
                        ImGui::TableSetupColumn("Frame");
                        ImGui::TableSetupColumn("Clips");
                        ImGui::TableSetupColumn("Action");
                        ImGui::TableHeadersRow();
                        for (const jcut::ImGuiProjectHistoryEntry& entry :
                             shellState->projectHistoryEntries) {
                            ImGui::PushID(static_cast<int>(entry.index));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", entry.index + 1);
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped(
                                "%s",
                                entry.projectName.empty()
                                    ? "Untitled Project"
                                    : entry.projectName.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%lld",
                                static_cast<long long>(entry.currentFrame));
                            ImGui::TableNextColumn();
                            ImGui::Text("%zu", entry.clipCount);
                            ImGui::TableNextColumn();
                            if (entry.isActive) {
                                ImGui::TextUnformatted("Active");
                            } else {
                                ImGui::BeginDisabled(savedHistoryBlocked);
                                if (ImGui::SmallButton("Restore") &&
                                    commitExportOutputPathDraft(shellState)) {
                                    jcut::EditorDocumentCore currentDocument;
                                    {
                                        std::lock_guard<std::mutex> lock(
                                            shellState->runtimeMutex);
                                        currentDocument =
                                            shellState->runtime.snapshot();
                                    }
                                    if (documentIsDirty(
                                            *shellState,
                                            currentDocument)) {
                                        shellState->statusMessage =
                                            "save changes before restoring project history";
                                    } else {
                                        std::string activationError;
                                        const std::optional<
                                            jcut::ImGuiProjectSession> restored =
                                                jcut::activateImGuiProjectHistoryEntry(
                                                    currentProjectSession(
                                                        *shellState,
                                                        currentDocument),
                                                    entry.index,
                                                    &activationError);
                                        if (restored.has_value()) {
                                            loadProjectSessionIntoShell(
                                                shellState,
                                                *restored,
                                                "project history snapshot restored");
                                        } else {
                                            shellState->statusMessage =
                                                activationError;
                                        }
                                    }
                                }
                                ImGui::EndDisabled();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab13(
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
        if (ImGui::BeginTabItem("Tracks")) {
            drawInspectorHeading("Tracks", snapshot, currentClip);
            drawTracksTable(shellState, snapshot);
            ImGui::EndTabItem();
        }
}

void drawInspectorTab14(
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
        if (ImGui::BeginTabItem("Preview")) {
            drawInspectorHeading("Preview", snapshot, currentClip);
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Show Scopes", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            float zoom = snapshot.transport.previewZoom;
            if (ImGui::SliderFloat("Zoom", &zoom, 0.5f, 3.0f, "%.2fx")) {
                applyCommand(shellState, jcut::SetPreviewZoomCommand{zoom});
            }
            if (ImGui::Button("Reset")) {
                shellState->previewPanX = 0.0f;
                shellState->previewPanY = 0.0f;
                applyCommand(shellState, jcut::SetPreviewZoomCommand{1.0f});
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab15(
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
        if (ImGui::BeginTabItem("Audio")) {
            drawInspectorHeading("Audio", snapshot, currentClip);
            const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Waveform", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool clipAudioEnabled = currentClip ? currentClip->audioEnabled : false;
            float clipGain = currentClip ? static_cast<float>(currentClip->audioGain) : 1.0f;
            float clipPan = currentClip ? static_cast<float>(currentClip->audioPan) : 0.0f;
            bool clipSolo = currentClip ? currentClip->audioSolo : false;
            ImGui::BeginDisabled(!currentClip);
            bool audioChanged = false;
            audioChanged |= ImGui::Checkbox("Clip Audio", &clipAudioEnabled);
            audioChanged |= ImGui::SliderFloat("Clip Gain", &clipGain, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            audioChanged |= ImGui::SliderFloat("Clip Pan", &clipPan, -1.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            audioChanged |= ImGui::Checkbox("Clip Solo", &clipSolo);
            if (audioChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipAudioCommand{
                    currentClip->id, clipAudioEnabled, clipGain, clipPan, clipSolo});
            }
            ImGui::EndDisabled();
            {
                static constexpr const char* treatmentLabels[] = {
                    "Preserve Pitch",
                    "Rubber Band at Any Speed",
                    "Harmonic Speech Isolation"};
                const jcut::EditorAudioTreatment treatmentValues[] = {
                    jcut::EditorAudioTreatment::PreservePitch,
                    jcut::EditorAudioTreatment::RubberBand,
                    jcut::EditorAudioTreatment::HarmonicSpeechIsolation};
                int treatmentIndex = 0;
                for (int index = 0; index < 3; ++index) {
                    if (snapshot.audioTreatment == treatmentValues[index]) {
                        treatmentIndex = index;
                    }
                }
                if (ImGui::Combo(
                        "Preview Audio Treatment",
                        &treatmentIndex,
                        treatmentLabels,
                        3)) {
                    applyCommand(
                        shellState,
                        jcut::SetAudioTreatmentCommand{
                            treatmentValues[treatmentIndex]});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Harmonic Speech Isolation uses two Rubber Band "
                        "stages and may buffer while the processed source "
                        "is prepared.");
                }
            }
            if (ImGui::CollapsingHeader(
                    "Master Dynamics",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                jcut::audio::DynamicsSettingsCore dynamics =
                    snapshot.audioDynamics;
                bool dynamicsChanged = false;
                const auto sliderDouble =
                    [&](const char* label,
                        double* value,
                        float minimum,
                        float maximum,
                        const char* format) {
                        float edited = static_cast<float>(*value);
                        const bool changed = ImGui::SliderFloat(
                            label, &edited, minimum, maximum, format);
                        beginRuntimeHistoryTransactionForLastItem(
                            shellState);
                        if (changed) *value = edited;
                        return changed;
                    };
                dynamicsChanged |= ImGui::Checkbox(
                    "Amplify", &dynamics.amplifyEnabled);
                if (dynamics.amplifyEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Amplify dB",
                        &dynamics.amplifyDb,
                        -24.0f,
                        24.0f,
                        "%.1f dB");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Normalize", &dynamics.normalizeEnabled);
                if (dynamics.normalizeEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Normalize Target",
                        &dynamics.normalizeTargetDb,
                        -24.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Transcript Normalize",
                    &dynamics.transcriptNormalizeEnabled);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Normalize each active transcript word toward "
                        "-0.45 dBFS, capped at 2.5x gain.");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Selective Normalize",
                    &dynamics.selectiveNormalizeEnabled);
                if (dynamics.selectiveNormalizeEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Minimum Segment",
                        &dynamics.selectiveNormalizeMinSegmentSeconds,
                        0.1f,
                        30.0f,
                        "%.1f s");
                    dynamicsChanged |= sliderDouble(
                        "Selective Peak",
                        &dynamics.selectiveNormalizePeakDb,
                        -36.0f,
                        0.0f,
                        "%.1f dBFS");
                    dynamicsChanged |= ImGui::SliderInt(
                        "Selective Passes",
                        &dynamics.selectiveNormalizePasses,
                        1,
                        8);
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Peak Reduction",
                    &dynamics.peakReductionEnabled);
                if (dynamics.peakReductionEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Peak Threshold",
                        &dynamics.peakThresholdDb,
                        -24.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Compressor", &dynamics.compressorEnabled);
                if (dynamics.compressorEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Compressor Threshold",
                        &dynamics.compressorThresholdDb,
                        -30.0f,
                        -1.0f,
                        "%.1f dBFS");
                    dynamicsChanged |= sliderDouble(
                        "Compressor Ratio",
                        &dynamics.compressorRatio,
                        1.0f,
                        20.0f,
                        "%.1f:1");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Soft Clip", &dynamics.softClipEnabled);
                dynamicsChanged |= ImGui::Checkbox(
                    "Limiter", &dynamics.limiterEnabled);
                if (dynamics.limiterEnabled) {
                    dynamicsChanged |= sliderDouble(
                        "Limiter Threshold",
                        &dynamics.limiterThresholdDb,
                        -12.0f,
                        0.0f,
                        "%.1f dBFS");
                }
                dynamicsChanged |= ImGui::Checkbox(
                    "Stereo to Mono",
                    &dynamics.stereoToMonoEnabled);
                if (dynamicsChanged) {
                    applyCommand(
                        shellState,
                        jcut::SetAudioDynamicsCommand{dynamics});
                }
            }
            if (ImGui::BeginTable("AudioStatus", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Initialized", audioStatus.initialized ? "yes" : "no");
                drawReadOnlyTableRow("Timeline", audioStatus.timelineConfigured ? "configured" : "pending");
                drawReadOnlyTableRow("Buffering", audioStatus.buffering ? "yes" : "no");
                drawReadOnlyTableRow("Playback", audioStatus.playbackActive ? "active" : "idle");
                drawReadOnlyTableRow("Sources", std::to_string(audioStatus.scheduledSourcePaths.size()));
                if (!audioStatus.scheduledSourcePaths.empty()) {
                    drawReadOnlyTableRow("Active Source", audioStatus.scheduledSourcePaths.front());
                }
                drawReadOnlyTableRow("Status", audioStatus.message);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab16(
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
                "Jobs", nullptr, inspectorTabFlags("Jobs"))) {
            drawInspectorHeading("Processing Jobs", snapshot, currentClip);
            bool exportActive = false;
            bool exportQueued = false;
            jcut::render::RenderProgressCore progress;
            jcut::render::RenderResultCore result;
            {
                std::lock_guard<std::mutex> lock(shellState->exportMutex);
                exportActive = shellState->exportRunning;
                exportQueued = shellState->exportRequested;
                progress = shellState->exportProgress;
                result = shellState->exportResult;
            }
            const jcut::FaceProcessingJobSnapshot faceJob =
                shellState->faceProcessingJob.snapshot();
            const jcut::ProxyGenerationJobSnapshot proxyJob =
                shellState->proxyGenerationJob.snapshot();
            const jcut::masks::PromptMaskJobSnapshot promptMaskJob =
                shellState->promptMaskJob.snapshot();
            const jcut::jobs::TranscriptionJobSnapshotCore
                transcriptionJob =
                    shellState->transcriptionJob.snapshot();
            const jcut::jobs::BiRefNetJobSnapshotCore birefnetJob =
                shellState->birefnetJob.snapshot();
            const auto faceStateLabel = [](jcut::FaceProcessingJobSnapshot::State state) {
                using State = jcut::FaceProcessingJobSnapshot::State;
                switch (state) {
                case State::Idle: return "idle";
                case State::Starting: return "starting";
                case State::Running: return "running";
                case State::Canceling: return "canceling";
                case State::Paused: return "paused";
                case State::Completed: return "complete";
                case State::Failed: return "failed";
                }
                return "unknown";
            };
            const auto proxyStateLabel = [](jcut::ProxyGenerationJobSnapshot::State state) {
                using State = jcut::ProxyGenerationJobSnapshot::State;
                switch (state) {
                case State::Idle: return "idle";
                case State::Starting: return "starting";
                case State::Running: return "running";
                case State::Canceling: return "canceling";
                case State::Completed: return "complete";
                case State::Canceled: return "canceled";
                case State::Failed: return "failed";
                }
                return "unknown";
            };
            const auto promptMaskStateLabel =
                [](jcut::masks::PromptMaskJobSnapshot::State state) {
                    using State =
                        jcut::masks::PromptMaskJobSnapshot::State;
                    switch (state) {
                    case State::Idle: return "idle";
                    case State::Starting: return "starting";
                    case State::Running: return "running";
                    case State::Canceling: return "canceling";
                    case State::Paused: return "paused";
                    case State::Completed: return "complete";
                    case State::Failed: return "failed";
                    }
                    return "unknown";
                };
            const auto processStateLabel =
                [](jcut::jobs::ProcessJobSnapshotCore::State state) {
                    using State =
                        jcut::jobs::ProcessJobSnapshotCore::State;
                    switch (state) {
                    case State::Idle: return "idle";
                    case State::Starting: return "starting";
                    case State::Running: return "running";
                    case State::Canceling: return "canceling";
                    case State::Completed: return "complete";
                    case State::Canceled: return "canceled";
                    case State::Failed: return "failed";
                    }
                    return "unknown";
                };
            if (ImGui::BeginTable("JobsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Progress");
                ImGui::TableHeadersRow();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Timeline export");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(exportActive ? "running" :
                                       (exportQueued ? "queued" :
                                        (result.message.empty() ? "idle" :
                                         (result.success ? "complete" : "failed"))));
                ImGui::TableNextColumn();
                ImGui::Text("%lld / %lld",
                            static_cast<long long>(progress.framesCompleted),
                            static_cast<long long>(progress.totalFrames));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Face detection");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(faceStateLabel(faceJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(faceJob.active()
                    ? "external worker"
                    : (faceJob.exitCode >= 0
                        ? ("exit " + std::to_string(faceJob.exitCode)).c_str()
                        : "-"));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Proxy generation");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(proxyStateLabel(proxyJob.state));
                ImGui::TableNextColumn();
                ImGui::Text(
                    "%lld / %lld",
                    static_cast<long long>(proxyJob.framesCompleted),
                    static_cast<long long>(proxyJob.totalFrames));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("SAM3 prompt mask");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    promptMaskStateLabel(promptMaskJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    promptMaskJob.active()
                        ? "external worker"
                        : (promptMaskJob.exitCode >= 0
                               ? ("exit " +
                                  std::to_string(
                                      promptMaskJob.exitCode))
                                     .c_str()
                               : "-"));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("WhisperX transcription");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    processStateLabel(transcriptionJob.state));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    transcriptionJob.outputReady
                        ? "transcript ready"
                        : (transcriptionJob.active()
                               ? "external worker"
                               : (transcriptionJob.exitCode >= 0
                                      ? ("exit " +
                                         std::to_string(
                                             transcriptionJob.exitCode))
                                            .c_str()
                                      : "-")));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("BiRefNet alpha");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    processStateLabel(birefnetJob.state));
                ImGui::TableNextColumn();
                if (birefnetJob.totalFrames > 0) {
                    ImGui::Text(
                        "%lld / %lld (%.1f%%)",
                        static_cast<long long>(
                            birefnetJob.currentFrame),
                        static_cast<long long>(
                            birefnetJob.totalFrames),
                        birefnetJob.percent);
                } else {
                    ImGui::TextUnformatted(
                        birefnetJob.outputReady
                            ? "alpha ready"
                            : (birefnetJob.active()
                                   ? "external worker"
                                   : "-"));
                }
                ImGui::EndTable();
            }
            ImGui::BeginDisabled(!exportActive && !exportQueued);
            if (ImGui::Button("Cancel Export")) {
                cancelExportRender(shellState);
                shellState->statusMessage = "export cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!proxyJob.active());
            if (ImGui::Button("Cancel Proxy Generation")) {
                shellState->proxyGenerationJob.cancel();
                shellState->statusMessage =
                    "proxy-generation cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!faceJob.active());
            if (ImGui::Button("Cancel Face Detection")) {
                shellState->faceProcessingJob.cancel();
                shellState->statusMessage =
                    "face-detection cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!promptMaskJob.active());
            if (ImGui::Button("Cancel Prompt Mask")) {
                shellState->promptMaskJob.cancel();
                shellState->statusMessage =
                    "prompt-mask cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!transcriptionJob.active());
            if (ImGui::Button("Cancel Transcription")) {
                shellState->transcriptionJob.cancel();
                shellState->statusMessage =
                    "transcription cancellation requested";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!birefnetJob.active());
            if (ImGui::Button("Cancel BiRefNet")) {
                shellState->birefnetJob.cancel();
                shellState->statusMessage =
                    "BiRefNet cancellation requested";
            }
            ImGui::EndDisabled();
            if (!result.message.empty()) {
                ImGui::TextWrapped("%s", result.message.c_str());
            }
            if (!faceJob.status.empty()) {
                ImGui::TextWrapped("Face detection: %s", faceJob.status.c_str());
            }
            if (!faceJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "Output: %s", faceJob.outputDirectory.c_str());
            }
            if (!faceJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Manifest: %s", faceJob.manifestPath.c_str());
            }
            if (!faceJob.logPath.empty()) {
                ImGui::TextWrapped("Log: %s", faceJob.logPath.c_str());
            }
            if (!proxyJob.status.empty()) {
                ImGui::TextWrapped(
                    "Proxy generation: %s", proxyJob.status.c_str());
            }
            if (!proxyJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "Proxy output: %s",
                    proxyJob.outputDirectory.c_str());
            }
            if (!proxyJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Proxy manifest: %s",
                    proxyJob.manifestPath.c_str());
            }
            if (!promptMaskJob.status.empty()) {
                ImGui::TextWrapped(
                    "Prompt mask: %s",
                    promptMaskJob.status.c_str());
            }
            if (!promptMaskJob.selectedMaskPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask output: %s",
                    promptMaskJob.selectedMaskPath.c_str());
            }
            if (!promptMaskJob.manifestPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask manifest: %s",
                    promptMaskJob.manifestPath.c_str());
            }
            if (!promptMaskJob.logPath.empty()) {
                ImGui::TextWrapped(
                    "Prompt-mask log: %s",
                    promptMaskJob.logPath.c_str());
            }
            if (!transcriptionJob.status.empty()) {
                ImGui::TextWrapped(
                    "Transcription: %s",
                    transcriptionJob.status.c_str());
            }
            if (!transcriptionJob.outputTranscriptPath.empty()) {
                ImGui::TextWrapped(
                    "Transcript output: %s",
                    transcriptionJob.outputTranscriptPath.c_str());
            }
            if (transcriptionJob.active()) {
                ImGui::InputText(
                    "Transcription stdin",
                    &shellState->transcriptionStdinDraft);
                ImGui::SameLine();
                if (ImGui::Button("Send stdin")) {
                    std::string stdinError;
                    if (shellState->transcriptionJob.writeStdin(
                            shellState->transcriptionStdinDraft,
                            &stdinError)) {
                        shellState->transcriptionStdinDraft.clear();
                    } else {
                        shellState->statusMessage =
                            std::move(stdinError);
                    }
                }
            }
            if (!birefnetJob.status.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet: %s", birefnetJob.status.c_str());
            }
            if (!birefnetJob.outputDirectory.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet output: %s",
                    birefnetJob.outputDirectory.c_str());
            }
            if (!birefnetJob.livePreviewPath.empty()) {
                ImGui::TextWrapped(
                    "BiRefNet live preview: %s",
                    birefnetJob.livePreviewPath.c_str());
            }
            ImGui::SeparatorText("Artifact Inspection");
            const auto loadJobsTextPreview =
                [&](std::string label, std::string path) {
                    shellState->jobsTextPreviewLabel = std::move(label);
                    shellState->jobsTextPreviewPath = std::move(path);
                    shellState->jobsTextPreview = readTextFileTail(
                        fs::path(shellState->jobsTextPreviewPath),
                        64U * 1024U,
                        &shellState->jobsTextPreviewError);
                };
            const auto drawJobsArtifactButton =
                [&](const char* buttonLabel,
                    const char* previewLabel,
                    const std::string& path) {
                    ImGui::BeginDisabled(path.empty());
                    if (ImGui::Button(buttonLabel)) {
                        loadJobsTextPreview(previewLabel, path);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                };
            drawJobsArtifactButton(
                "Face Manifest", "Face-detection manifest",
                faceJob.manifestPath);
            drawJobsArtifactButton(
                "Face Log", "Face-detection log", faceJob.logPath);
            drawJobsArtifactButton(
                "Proxy Manifest", "Proxy manifest",
                proxyJob.manifestPath);
            drawJobsArtifactButton(
                "Mask Manifest", "Prompt-mask manifest",
                promptMaskJob.manifestPath);
            drawJobsArtifactButton(
                "Transcript Manifest", "Transcription manifest",
                transcriptionJob.manifestPath);
            drawJobsArtifactButton(
                "Transcript Log", "Transcription log",
                transcriptionJob.logPath);
            drawJobsArtifactButton(
                "BiRefNet Manifest", "BiRefNet manifest",
                birefnetJob.manifestPath);
            drawJobsArtifactButton(
                "BiRefNet Log", "BiRefNet log",
                birefnetJob.logPath);
            drawJobsArtifactButton(
                "BiRefNet Progress", "BiRefNet progress",
                birefnetJob.progressPath);
            ImGui::BeginDisabled(promptMaskJob.logPath.empty());
            if (ImGui::Button("Mask Log")) {
                loadJobsTextPreview(
                    "Prompt-mask log", promptMaskJob.logPath);
            }
            ImGui::EndDisabled();
            if (!shellState->jobsTextPreviewPath.empty()) {
                ImGui::Separator();
                ImGui::Text(
                    "%s (%zu bytes shown)",
                    shellState->jobsTextPreviewLabel.c_str(),
                    shellState->jobsTextPreview.size());
                ImGui::TextWrapped(
                    "%s", shellState->jobsTextPreviewPath.c_str());
                if (!shellState->jobsTextPreviewError.empty()) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                        "%s",
                        shellState->jobsTextPreviewError.c_str());
                }
                if (ImGui::Button("Refresh Artifact")) {
                    loadJobsTextPreview(
                        shellState->jobsTextPreviewLabel.c_str(),
                        shellState->jobsTextPreviewPath);
                }
                ImGui::SameLine();
                if (ImGui::Button("Close Artifact")) {
                    shellState->jobsTextPreviewLabel.clear();
                    shellState->jobsTextPreviewPath.clear();
                    shellState->jobsTextPreview.clear();
                    shellState->jobsTextPreviewError.clear();
                }
                if (!shellState->jobsTextPreviewPath.empty()) {
                    ImGui::InputTextMultiline(
                        "##JobsArtifactText",
                        &shellState->jobsTextPreview,
                        ImVec2(-1.0f, 220.0f),
                        ImGuiInputTextFlags_ReadOnly);
                }
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab17(
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
        if (ImGui::BeginTabItem("AI Assist")) {
            drawInspectorHeading("AI Assist", snapshot, currentClip);
            ImGui::TextWrapped(
                "%s",
                shellState->aiAccount.status.empty()
                    ? "Configure credentials and refresh account access."
                    : shellState->aiAccount.status.c_str());
            ImGui::Text(
                "Project usage %d/%d (failures %d)",
                shellState->aiUsageRequests,
                shellState->aiUsageBudgetCap,
                shellState->aiUsageFailures);
            std::vector<std::string> modelOptions =
                shellState->aiAccount.entitlements.models;
            if (modelOptions.empty()) {
                modelOptions = {
                    "deepseek-chat",
                    "gpt-4o-mini",
                    "mistral-small",
                    "qwen2.5-7b-instruct"};
            }
            if (ImGui::BeginCombo(
                    "Model", shellState->aiSelectedModel.c_str())) {
                for (const std::string& model : modelOptions) {
                    const bool selected =
                        model == shellState->aiSelectedModel;
                    if (ImGui::Selectable(model.c_str(), selected)) {
                        shellState->aiSelectedModel = model;
                        setLegacyStateOverride(
                            shellState,
                            "aiSelectedModel",
                            shellState->aiSelectedModel);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginChild(
                    "AiChatHistory", ImVec2(-1.0f, 210.0f), true)) {
                for (const AiChatMessage& message :
                     shellState->aiChatMessages) {
                    ImGui::TextColored(
                        message.role == "Error"
                            ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                            : ImVec4(0.45f, 0.75f, 1.0f, 1.0f),
                        "%s",
                        message.role.c_str());
                    ImGui::TextWrapped("%s", message.content.c_str());
                    ImGui::Spacing();
                }
                if (shellState->aiTaskRunning) {
                    ImGui::TextDisabled("Waiting for AI response...");
                }
                if (ImGui::GetScrollY() >=
                    ImGui::GetScrollMaxY() - 4.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
            const bool submitOnEnter = ImGui::InputTextMultiline(
                "##AiChatPrompt",
                &shellState->aiChatPrompt,
                ImVec2(-1.0f, 72.0f),
                ImGuiInputTextFlags_CtrlEnterForNewLine |
                    ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::BeginDisabled(
                shellState->aiTaskRunning ||
                shellState->aiChatPrompt.empty() ||
                !shellState->aiAccount.aiEnabled);
            if (ImGui::Button("Send") || submitOnEnter) {
                startAiChatRequest(shellState, snapshot);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->aiChatMessages.empty());
            if (ImGui::Button("Clear Chat")) {
                shellState->aiChatMessages.clear();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextWrapped(
                "Deterministic speaker-name, organization, and spurious-label "
                "mining remains available in Speakers without cloud access.");
            ImGui::EndTabItem();
        }
}

void drawInspectorTab18(
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
        if (ImGui::BeginTabItem("Access")) {
            drawInspectorHeading("Subscriptions & Purchases", snapshot, currentClip);
            const jcut::ai::AccessTokenProfileCore tokenProfile =
                jcut::ai::parseAccessTokenProfileCore(
                    shellState->aiSessionToken);
            const std::string accountIdentity =
                !shellState->aiUserId.empty()
                    ? shellState->aiUserId
                    : !shellState->aiAccount.entitlements.userId.empty()
                        ? shellState->aiAccount.entitlements.userId
                        : tokenProfile.displayIdentity();
            if (!accountIdentity.empty()) {
                drawAiProfileAvatar(
                    *shellState, accountIdentity);
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(accountIdentity.c_str());
                const bool subscribed =
                    shellState->aiAccount.usage.hasSubscription;
                const bool entitled =
                    shellState->aiAccount.entitlements.entitled;
                ImGui::TextColored(
                    subscribed
                        ? ImVec4(0.95f, 0.76f, 0.24f, 1.0f)
                        : entitled
                            ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f)
                            : ImVec4(0.65f, 0.69f, 0.74f, 1.0f),
                    "%s",
                    subscribed
                        ? "SUBSCRIBED"
                        : entitled ? "AI ENABLED" : "BASIC");
                if (shellState->aiAvatarRunning) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Loading profile image...");
                }
                ImGui::EndGroup();
            } else {
                ImGui::TextDisabled("Not signed in");
            }
            if (ImGui::InputText(
                    "Gateway",
                    &shellState->aiGatewayBaseUrl,
                    ImGuiInputTextFlags_EnterReturnsTrue)) {
                shellState->aiGatewayBaseUrl =
                    jcut::ai::normalizeGatewayBaseUrl(
                        shellState->aiGatewayBaseUrl);
                setLegacyStateOverride(
                    shellState,
                    "aiProxyBaseUrl",
                    shellState->aiGatewayBaseUrl);
            }
            ImGui::InputText(
                "Session Token",
                &shellState->aiSessionToken,
                ImGuiInputTextFlags_Password);
            ImGui::TextDisabled(
                "Token is never stored in the project. "
                "JCUT_AI_AUTH_TOKEN is loaded at startup.");
            ImGui::BeginDisabled(
                shellState->aiBrowserLoginRunning ||
                !jcut::ai::isSupabaseGatewayBase(
                    shellState->aiGatewayBaseUrl));
            if (ImGui::Button("Log In with Browser")) {
                startAiBrowserLogin(shellState);
            }
            ImGui::EndDisabled();
            if (shellState->aiBrowserLoginRunning) {
                ImGui::SameLine();
                if (ImGui::Button("Cancel Login")) {
                    shellState->aiBrowserLoginCancelRequested.store(true);
                }
            }
            ImGui::BeginDisabled(shellState->aiSessionToken.empty());
            if (ImGui::Button("Save Login Securely")) {
                const jcut::ai::CredentialStoreResultCore stored =
                    jcut::ai::storeCredentialsCore(
                        jcut::ai::StoredCredentialsCore{
                            shellState->aiSessionToken,
                            shellState->aiRefreshToken,
                            shellState->aiUserId});
                shellState->aiCredentialStatus = stored.ok
                    ? (stored.usedSystemStore
                           ? "Login saved in the system secret store."
                           : "Login saved in the private config fallback.")
                    : stored.error;
                appendAiActivity(
                    shellState,
                    stored.ok ? "Credentials" : "Credential error",
                    shellState->aiCredentialStatus);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->aiRefreshToken.empty() ||
                shellState->aiTokenRefreshRunning);
            if (ImGui::Button("Refresh Login Token")) {
                startAiTokenRefresh(shellState);
            }
            ImGui::EndDisabled();
            if (!shellState->aiCredentialStatus.empty()) {
                ImGui::TextDisabled(
                    "%s", shellState->aiCredentialStatus.c_str());
            }
            ImGui::BeginDisabled(
                shellState->aiAccountRefreshRunning ||
                shellState->aiGatewayBaseUrl.empty() ||
                shellState->aiSessionToken.empty() ||
                !shellState->featureAiPanel);
            if (ImGui::Button("Refresh Access")) {
                startAiAccountRefresh(shellState);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->aiSessionToken.empty());
            if (ImGui::Button("Log Out")) {
                const jcut::ai::CredentialStoreResultCore cleared =
                    jcut::ai::clearStoredCredentialsCore();
                shellState->aiSessionToken.clear();
                shellState->aiRefreshToken.clear();
                shellState->aiUserId.clear();
                shellState->aiAccount = {};
                shellState->aiAccount.status =
                    "Session credentials cleared.";
                shellState->aiCredentialStatus = cleared.ok
                    ? "Stored login cleared."
                    : cleared.error;
                appendAiActivity(
                    shellState,
                    cleared.ok ? "Logout" : "Logout error",
                    shellState->aiCredentialStatus);
            }
            ImGui::EndDisabled();
            if (shellState->aiAccountRefreshRunning) {
                ImGui::SameLine();
                ImGui::TextDisabled("Refreshing...");
            }
            ImGui::BeginDisabled(
                shellState->aiCheckoutRunning ||
                shellState->aiSessionToken.empty() ||
                !shellState->featureAiPanel);
            if (ImGui::Button("Subscribe / Open Checkout")) {
                startAiCheckout(shellState);
            }
            ImGui::EndDisabled();
            if (shellState->aiCheckoutRunning) {
                ImGui::SameLine();
                ImGui::TextDisabled("Opening checkout...");
            }
            ImGui::TextWrapped(
                "%s",
                shellState->aiAccount.status.empty()
                    ? "No account data loaded."
                    : shellState->aiAccount.status.c_str());
            if (ImGui::BeginTable("AccessTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Item");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Period");
                ImGui::TableSetupColumn("Source");
                ImGui::TableHeadersRow();
                for (const jcut::ai::AccessRowCore& row :
                     shellState->aiAccount.rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.type.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.item.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.status.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(row.period.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(row.source.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::SeparatorText("AI Activity");
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->aiActivityEntries.empty());
            if (ImGui::SmallButton("Clear Activity")) {
                shellState->aiActivityEntries.clear();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "AiActivityTable",
                    3,
                    ImGuiTableFlags_Borders |
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY,
                    ImVec2(-1.0f, 180.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(
                    "Time",
                    ImGuiTableColumnFlags_WidthFixed,
                    72.0f);
                ImGui::TableSetupColumn(
                    "Phase",
                    ImGuiTableColumnFlags_WidthFixed,
                    110.0f);
                ImGui::TableSetupColumn(
                    "Summary",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const AiActivityEntry& entry :
                     shellState->aiActivityEntries) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entry.time.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(entry.phase.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped(
                        "%s", entry.summary.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab19(
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
        if (ImGui::BeginTabItem("Project")) {
            char projectName[256];
            std::snprintf(projectName, sizeof(projectName), "%s", snapshot.projectName.c_str());
            if (ImGui::InputText("Name", projectName, sizeof(projectName),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                applyCommand(shellState, jcut::SetProjectNameCommand{projectName});
            }
            if (ImGui::Button("Add Track")) {
                applyCommand(shellState, jcut::AddTrackCommand{});
            }
            ImGui::SameLine();
            const jcut::EditorTrack* selectedTrack = nullptr;
            for (const jcut::EditorTrack& track : snapshot.tracks) {
                if (track.selected) {
                    selectedTrack = &track;
                    break;
                }
            }
            if (ImGui::Button("Add Clip") && selectedTrack) {
                applyCommand(shellState, jcut::AddClipCommand{
                    selectedTrack->id,
                    {},
                    snapshot.transport.currentFrame,
                    90,
                    {},
                    {}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Track") && selectedTrack) {
                applyCommand(shellState, jcut::DeleteTrackCommand{selectedTrack->id});
            }
            ImGui::Text("Tracks %zu", snapshot.tracks.size());
            ImGui::Text("Clips %zu", snapshot.clips.size());
            ImGui::Text("Media %zu", snapshot.mediaItems.size());
            ImGui::EndTabItem();
        }
}
