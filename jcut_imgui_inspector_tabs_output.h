#pragma once

void drawInspectorTab20(
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
                "Clip", nullptr, inspectorTabFlags("Clip"))) {
            const jcut::EditorClip* selectedClip = nullptr;
            for (const jcut::EditorClip& clip : snapshot.clips) {
                if (clip.selected) {
                    selectedClip = &clip;
                    break;
                }
            }
            if (selectedClip) {
                char clipLabel[256];
                std::snprintf(clipLabel, sizeof(clipLabel), "%s", selectedClip->label.c_str());
                if (ImGui::InputText("Label", clipLabel, sizeof(clipLabel),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    applyCommand(shellState, jcut::SetClipLabelCommand{selectedClip->id, clipLabel});
                }

                int trackId = selectedClip->trackId;
                const bool trackChanged = ImGui::InputInt("Track", &trackId);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (trackChanged) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, trackId, selectedClip->startFrame});
                }

                int startFrame = selectedClip->startFrame;
                const bool startChanged = ImGui::InputInt("Start", &startFrame);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (startChanged) {
                    applyCommand(shellState, jcut::MoveClipCommand{
                        selectedClip->id, selectedClip->trackId, startFrame});
                }

                int durationFrames = selectedClip->durationFrames;
                const bool durationChanged = ImGui::InputInt("Duration", &durationFrames);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (durationChanged) {
                    applyCommand(shellState, jcut::ResizeClipCommand{
                        selectedClip->id, durationFrames});
                }

                if (ImGui::Button("Delete Clip")) {
                    applyCommand(shellState, jcut::DeleteClipCommand{selectedClip->id});
                }
                ImGui::SameLine();
                if (ImGui::Button("Split At Playhead")) {
                    applyCommand(shellState, jcut::SplitClipCommand{
                        selectedClip->id,
                        snapshot.transport.currentFrame});
                }

                ImGui::Text("Clip %d", selectedClip->id);
                ImGui::TextUnformatted(selectedClip->sourcePath.empty()
                    ? "No source path"
                    : selectedClip->sourcePath.c_str());
                ImGui::SeparatorText("Proxy");
                if (shellState->proxyPathDraftClipId != selectedClip->id) {
                    shellState->proxyPathDraftClipId = selectedClip->id;
                    shellState->proxyPathDraft = selectedClip->proxyPath;
                }
                ImGui::InputText(
                    "Proxy Path", &shellState->proxyPathDraft);
                const bool configuredProxyUsable =
                    jcut::proxyPathIsUsable(selectedClip->proxyPath);
                ImGui::Text(
                    "Configured: %s | Playback: %s",
                    selectedClip->proxyPath.empty()
                        ? "none"
                        : (configuredProxyUsable ? "ready" : "missing"),
                    selectedClip->useProxy && configuredProxyUsable
                        ? "proxy" : "source");
                if (ImGui::Button("Attach Proxy")) {
                    fs::path proxyPath(shellState->proxyPathDraft);
                    if (proxyPath.is_relative()) {
                        const fs::path root =
                            !shellState->mediaRootDirectory.empty()
                            ? fs::path(shellState->mediaRootDirectory)
                            : fs::path(shellState->projectRootPath);
                        proxyPath = root / proxyPath;
                    }
                    proxyPath = proxyPath.lexically_normal();
                    if (!jcut::proxyPathIsUsable(pathString(proxyPath))) {
                        shellState->statusMessage =
                            "proxy path is not a readable media file or image sequence";
                    } else {
                        shellState->proxyPathDraft = pathString(proxyPath);
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id,
                                shellState->proxyPathDraft,
                                true});
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discover Proxy")) {
                    const fs::path sourcePath =
                        resolvedClipMediaPathForProbe(
                            *shellState, *selectedClip);
                    const std::string discovered =
                        jcut::discoverExistingProxyPath(
                            pathString(sourcePath));
                    if (discovered.empty()) {
                        shellState->statusMessage =
                            "no default .proxy, .proxy.mp4, or .proxy.mov was found";
                    } else {
                        shellState->proxyPathDraft = discovered;
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id, discovered, true});
                    }
                }
                const jcut::ProxyGenerationJobSnapshot proxyJob =
                    shellState->proxyGenerationJob.snapshot();
                const fs::path proxySource =
                    resolvedClipMediaPathForProbe(
                        *shellState, *selectedClip);
                const std::array<const char*, 3> proxyFormats{
                    "Image Sequence (JPEG)",
                    "H.264 (MP4)",
                    "Motion JPEG (MOV)"};
                shellState->proxyGenerationFormatIndex = std::clamp(
                    shellState->proxyGenerationFormatIndex, 0, 2);
                ImGui::Combo(
                    "Generation Format",
                    &shellState->proxyGenerationFormatIndex,
                    proxyFormats.data(),
                    static_cast<int>(proxyFormats.size()));
                const jcut::ProxyGenerationFormat proxyFormat =
                    shellState->proxyGenerationFormatIndex == 1
                    ? jcut::ProxyGenerationFormat::H264Mp4
                    : (shellState->proxyGenerationFormatIndex == 2
                        ? jcut::ProxyGenerationFormat::MjpegMov
                        : jcut::ProxyGenerationFormat::ImageSequenceJpeg);
                const std::string generatedProxyPath =
                    jcut::defaultProxyOutputPath(
                        pathString(proxySource), proxyFormat);
                const bool generatedProxyExists =
                    jcut::proxyPathIsUsable(generatedProxyPath);
                ImGui::Checkbox(
                    "Overwrite Existing Image Proxy",
                    &shellState->overwriteProxyGeneration);
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    generatedProxyPath.empty() ||
                    (generatedProxyExists &&
                     !shellState->overwriteProxyGeneration));
                if (ImGui::Button("Create Proxy")) {
                    std::string error;
                    if (shellState->proxyGenerationJob.start(
                            {selectedClip->id,
                             pathString(proxySource),
                             generatedProxyPath,
                             proxyFormat,
                             false,
                             shellState->overwriteProxyGeneration},
                            &error)) {
                        shellState->statusMessage =
                            "proxy generation started";
                    } else {
                        shellState->statusMessage = error.empty()
                            ? "proxy generation could not start"
                            : error;
                    }
                }
                ImGui::EndDisabled();
                if (generatedProxyExists &&
                    !shellState->overwriteProxyGeneration) {
                    ImGui::TextDisabled(
                        "Enable overwrite to regenerate the existing proxy artifact.");
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    proxyFormat !=
                        jcut::ProxyGenerationFormat::ImageSequenceJpeg ||
                    !generatedProxyExists);
                if (ImGui::Button("Continue Proxy")) {
                    std::string error;
                    if (shellState->proxyGenerationJob.start(
                            {selectedClip->id,
                             pathString(proxySource),
                             generatedProxyPath,
                             proxyFormat,
                             true,
                             false},
                            &error)) {
                        shellState->statusMessage =
                            "proxy continuation started";
                    } else {
                        shellState->statusMessage = error.empty()
                            ? "proxy continuation could not start"
                            : error;
                    }
                }
                ImGui::EndDisabled();
                if (proxyJob.state ==
                        jcut::ProxyGenerationJobSnapshot::State::Completed &&
                    proxyJob.clipId == selectedClip->id &&
                    jcut::proxyPathIsUsable(proxyJob.outputDirectory)) {
                    ImGui::SameLine();
                    if (ImGui::Button("Attach Generated Proxy")) {
                        shellState->proxyPathDraft =
                            proxyJob.outputDirectory;
                        applyCommand(
                            shellState,
                            jcut::SetClipProxyCommand{
                                selectedClip->id,
                                proxyJob.outputDirectory,
                                true});
                    }
                }
                const std::vector<std::string> allProxyCandidates =
                    jcut::proxyCandidatePaths(pathString(proxySource));
                const bool configuredIsManagedProxy =
                    std::find(
                        allProxyCandidates.begin(),
                        allProxyCandidates.end(),
                        selectedClip->proxyPath) != allProxyCandidates.end();
                const std::string proxyDeletionTarget =
                    configuredIsManagedProxy
                    ? selectedClip->proxyPath
                    : generatedProxyPath;
                ImGui::BeginDisabled(
                    proxyJob.active() ||
                    !jcut::proxyPathIsUsable(proxyDeletionTarget));
                if (ImGui::Button("Delete Proxy File...")) {
                    ImGui::OpenPopup("Confirm Proxy Deletion");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal(
                        "Confirm Proxy Deletion",
                        nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped(
                        "Permanently delete this generated proxy?\n%s",
                        proxyDeletionTarget.c_str());
                    ImGui::TextDisabled(
                        "This removes the proxy file or image directory. "
                        "The source media is not modified.");
                    if (ImGui::Button("Delete Permanently")) {
                        std::string error;
                        if (jcut::removeProxyArtifact(
                                pathString(proxySource),
                                proxyDeletionTarget,
                                &error)) {
                            if (selectedClip->proxyPath ==
                                proxyDeletionTarget) {
                                shellState->proxyPathDraft.clear();
                                applyCommand(
                                    shellState,
                                    jcut::SetClipProxyCommand{
                                        selectedClip->id, {}, false});
                            }
                            shellState->statusMessage =
                                "proxy artifact permanently deleted";
                            ImGui::CloseCurrentPopup();
                        } else {
                            shellState->statusMessage = error.empty()
                                ? "proxy artifact could not be deleted"
                                : error;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::BeginDisabled(selectedClip->proxyPath.empty());
                bool useProxy = selectedClip->useProxy;
                if (ImGui::Checkbox("Use Proxy for Playback", &useProxy)) {
                    applyCommand(
                        shellState,
                        jcut::SetClipProxyCommand{
                            selectedClip->id,
                            selectedClip->proxyPath,
                            useProxy});
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Proxy Association")) {
                    shellState->proxyPathDraft.clear();
                    applyCommand(
                        shellState,
                        jcut::SetClipProxyCommand{
                            selectedClip->id, {}, false});
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled(
                    "Association changes do not delete proxy files. "
                    "Proxy encoding remains a separate processing job.");
            } else {
                ImGui::TextUnformatted("No clip selected");
            }
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Waveform", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool transcript = snapshot.panels.showTranscript;
            if (ImGui::Checkbox("Transcript", &transcript)) {
                applyCommand(shellState, jcut::SetTranscriptVisibleCommand{transcript});
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab21(
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
        if (ImGui::BeginTabItem("Output",
                                nullptr,
                                focusOutput ? ImGuiTabItemFlags_SetSelected
                                            : ImGuiTabItemFlags_None)) {
            jcut::render::RenderProgressCore exportProgress;
            jcut::render::RenderResultCore exportResult;
            bool exportRunning = false;
            bool exportHasProgress = false;
            std::size_t exportQueueCurrent = 0;
            std::size_t exportQueueTotal = 0;
            std::size_t exportQueueCompleted = 0;
            std::size_t exportQueueFailed = 0;
            std::string exportQueueLabel;
            {
                std::lock_guard<std::mutex> lock(shellState->exportMutex);
                exportProgress = shellState->exportProgress;
                exportResult = shellState->exportResult;
                exportRunning = shellState->exportRunning;
                exportHasProgress = shellState->exportHasProgress;
                exportQueueCurrent =
                    shellState->exportQueueCurrent;
                exportQueueTotal =
                    shellState->exportQueueTotal;
                exportQueueCompleted =
                    shellState->exportQueueCompleted;
                exportQueueFailed =
                    shellState->exportQueueFailed;
                exportQueueLabel =
                    shellState->exportQueueLabel;
            }
            int width = snapshot.exportRequest.outputSize.width;
            int height = snapshot.exportRequest.outputSize.height;
            float fps = static_cast<float>(snapshot.exportRequest.outputFps);
            const std::array<const char*, 4> formats = {"mp4", "mov", "mkv", "webm"};
            int formatIndex = 0;
            for (int i = 0; i < static_cast<int>(formats.size()); ++i) {
                if (snapshot.exportRequest.outputFormat == formats[i]) {
                    formatIndex = i;
                    break;
                }
            }
            const bool widthChanged = ImGui::InputInt("Width", &width);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (widthChanged) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            const bool heightChanged = ImGui::InputInt("Height", &height);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (heightChanged) {
                applyCommand(shellState, jcut::SetExportSizeCommand{width, height});
            }
            const bool fpsChanged = ImGui::InputFloat("FPS", &fps, 0.5f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (fpsChanged) {
                applyCommand(shellState, jcut::SetExportFpsCommand{fps});
            }
            int exportStart = static_cast<int>(snapshot.exportRequest.exportStartFrame);
            int exportEnd = static_cast<int>(snapshot.exportRequest.exportEndFrame);
            bool exportRangeChanged = false;
            exportRangeChanged |= ImGui::InputInt("Export Start", &exportStart);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            exportRangeChanged |= ImGui::InputInt("Export End", &exportEnd);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (exportRangeChanged) {
                applyCommand(shellState, jcut::SetExportRangeCommand{exportStart, exportEnd});
            }
            ImGui::InputText("Output Path",
                             shellState->exportOutputPath.data(),
                             shellState->exportOutputPath.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                commitExportOutputPathDraft(shellState);
            }
            if (ImGui::Combo("Format", &formatIndex, formats.data(), static_cast<int>(formats.size()))) {
                applyCommand(shellState, jcut::SetExportFormatCommand{formats[formatIndex]});
            }
            bool useProxies = snapshot.exportRequest.useProxyMedia;
            if (ImGui::Checkbox("Use Proxies", &useProxies)) {
                applyCommand(shellState, jcut::SetExportUseProxyMediaCommand{useProxies});
            }
            bool imageSequence = snapshot.exportRequest.createVideoFromImageSequence;
            if (ImGui::Checkbox("Image Sequence", &imageSequence)) {
                applyCommand(shellState, jcut::SetExportImageSequenceCommand{imageSequence});
            }
            if (imageSequence) {
                const std::array<const char*, 3> sequenceFormats = {"jpeg", "png", "webp"};
                int sequenceFormatIndex = 0;
                for (int i = 0; i < static_cast<int>(sequenceFormats.size()); ++i) {
                    if (snapshot.exportRequest.imageSequenceFormat == sequenceFormats[i]) {
                        sequenceFormatIndex = i;
                        break;
                    }
                }
                if (ImGui::Combo("Sequence Format",
                                 &sequenceFormatIndex,
                                 sequenceFormats.data(),
                                 static_cast<int>(sequenceFormats.size()))) {
                    applyCommand(shellState,
                                 jcut::SetExportImageSequenceFormatCommand{
                                     sequenceFormats[sequenceFormatIndex]});
                }
            }
            ImGui::Separator();
            if (exportRunning) {
                if (exportQueueTotal > 1) {
                    ImGui::Text(
                        "Batch %zu / %zu | completed %zu | failed %zu",
                        exportQueueCurrent,
                        exportQueueTotal,
                        exportQueueCompleted,
                        exportQueueFailed);
                    if (!exportQueueLabel.empty()) {
                        ImGui::TextWrapped(
                            "%s",
                            exportQueueLabel.c_str());
                    }
                }
                const float completion = exportProgress.totalFrames > 0
                    ? static_cast<float>(exportProgress.framesCompleted) /
                        static_cast<float>(exportProgress.totalFrames)
                    : 0.0f;
                ImGui::ProgressBar(completion, ImVec2(-1.0f, 0.0f));
                ImGui::Text("Frame %lld / %lld",
                            static_cast<long long>(exportProgress.framesCompleted),
                            static_cast<long long>(exportProgress.totalFrames));
                if (!exportProgress.encoderLabel.empty()) {
                    ImGui::TextUnformatted(exportProgress.encoderLabel.c_str());
                }
                if (ImGui::Button("Cancel Export")) {
                    cancelExportRender(shellState);
                    shellState->statusMessage = "export cancellation requested";
                }
            } else {
                if (ImGui::Button("Export")) {
                    if (requestExportRender(shellState)) {
                        shellState->statusMessage = "export started";
                    } else {
                        shellState->statusMessage = "export already running";
                    }
                }
                if (exportHasProgress || !exportResult.message.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Frames %lld",
                                static_cast<long long>(exportResult.framesRendered));
                    if (!exportResult.encoderLabel.empty()) {
                        ImGui::TextUnformatted(exportResult.encoderLabel.c_str());
                    }
                    ImGui::TextUnformatted(exportResult.message.c_str());
                }
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab22(
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
        if (ImGui::BeginTabItem("Scopes")) {
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Visible", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            const PreviewHistogram histogram = currentPreviewHistogram(shellState);
            if (histogram.valid) {
                ImGui::PlotHistogram("Luma",
                                     histogram.luma.data(),
                                     static_cast<int>(histogram.luma.size()),
                                     0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 90.0f));
                ImGui::PlotLines("Red",
                                 histogram.red.data(),
                                 static_cast<int>(histogram.red.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
                ImGui::PlotLines("Green",
                                 histogram.green.data(),
                                 static_cast<int>(histogram.green.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
                ImGui::PlotLines("Blue",
                                 histogram.blue.data(),
                                 static_cast<int>(histogram.blue.size()),
                                 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 54.0f));
            } else {
                ImGui::TextWrapped("Scopes are unavailable while preview is using a zero-copy GPU frame without CPU readback.");
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab23(
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
        if (ImGui::BeginTabItem("Pipeline")) {
            drawInspectorHeading("Pipeline", snapshot, currentClip);
            jcut::standalone_render::PreviewRenderResult previewResult;
            bool lastUsedZeroCopy = false;
            bool zeroCopyAvailable = false;
            std::string zeroCopyFailure;
            {
                std::lock_guard<std::mutex> lock(shellState->previewMutex);
                previewResult = shellState->previewResult;
                lastUsedZeroCopy = shellState->previewLastUsedZeroCopy;
                zeroCopyAvailable = shellState->previewZeroCopyAvailable;
                zeroCopyFailure = shellState->previewZeroCopyFailureReason;
            }
            const std::vector<PipelineStageCore> stages =
                previewPipelineStages(
                    previewResult,
                    lastUsedZeroCopy,
                    zeroCopyAvailable,
                    zeroCopyFailure);
            shellState->selectedPipelineStage = std::clamp(
                shellState->selectedPipelineStage,
                0,
                std::max(0, static_cast<int>(stages.size()) - 1));
            if (ImGui::Button("Refresh Pipeline")) {
                requestPreviewRender(shellState);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(lastUsedZeroCopy);
            if (ImGui::Button("Retry Zero Copy")) {
                {
                    std::lock_guard<std::mutex> lock(
                        shellState->previewMutex);
                    shellState->previewCpuFallbackPreferred = false;
                    shellState->previewZeroCopyFailureReason.clear();
                }
                requestPreviewRender(shellState);
            }
            ImGui::EndDisabled();
            if (ImGui::BeginChild(
                    "PipelineGraph",
                    ImVec2(0.0f, 104.0f),
                    true,
                    ImGuiWindowFlags_HorizontalScrollbar)) {
                for (std::size_t index = 0;
                     index < stages.size();
                     ++index) {
                    const PipelineStageCore& stage = stages[index];
                    ImGui::PushID(static_cast<int>(index));
                    const ImVec4 color =
                        stage.state == "blocked"
                        ? ImVec4(0.42f, 0.16f, 0.16f, 1.0f)
                        : (stage.state == "fallback"
                               ? ImVec4(0.46f, 0.34f, 0.13f, 1.0f)
                               : (stage.active
                                      ? ImVec4(0.15f, 0.42f, 0.31f, 1.0f)
                                      : ImVec4(0.19f, 0.23f, 0.28f, 1.0f)));
                    ImGui::PushStyleColor(
                        ImGuiCol_Button, color);
                    ImGui::PushStyleColor(
                        ImGuiCol_ButtonHovered,
                        ImVec4(
                            std::min(1.0f, color.x + 0.12f),
                            std::min(1.0f, color.y + 0.12f),
                            std::min(1.0f, color.z + 0.12f),
                            1.0f));
                    const std::string cardLabel =
                        stage.label + "\n" + stage.state;
                    if (ImGui::Button(
                            cardLabel.c_str(),
                            ImVec2(112.0f, 68.0f))) {
                        shellState->selectedPipelineStage =
                            static_cast<int>(index);
                    }
                    ImGui::PopStyleColor(2);
                    if (index + 1 < stages.size()) {
                        ImGui::SameLine();
                        ImGui::TextUnformatted(">");
                        ImGui::SameLine();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (!stages.empty()) {
                const PipelineStageCore& selectedStage =
                    stages[static_cast<std::size_t>(
                        shellState->selectedPipelineStage)];
                ImGui::SeparatorText(selectedStage.label.c_str());
                ImGui::Text(
                    "%s • %s • %s",
                    selectedStage.kind.c_str(),
                    selectedStage.state.c_str(),
                    selectedStage.exact ? "exact" : "approximate");
                ImGui::TextWrapped(
                    "%s", selectedStage.detail.c_str());
                if (ImGui::BeginTable(
                        "PipelineStageFacts",
                        2,
                        ImGuiTableFlags_Borders |
                            ImGuiTableFlags_RowBg)) {
                    for (const auto& [label, value] :
                         selectedStage.facts) {
                        drawReadOnlyTableRow(label.c_str(), value);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab24(
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
        if (ImGui::BeginTabItem("System")) {
            drawInspectorHeading("System", snapshot, currentClip);
            const jcut::ImGuiAudioStatus audioStatus = shellState->audioRuntime.status();
            jcut::standalone_render::PreviewRenderResult decoderPreview;
            {
                std::lock_guard<std::mutex> lock(
                    shellState->previewMutex);
                decoderPreview = shellState->previewResult;
            }
            const jcut::EffectiveDecoderPolicyCore effectiveDecoder =
                jcut::effectiveDecoderPolicyCore(
                    shellState->decoderPolicy,
                    decoderPreview.hardwareAccelerated,
                    false,
                    static_cast<int>(std::max(
                        1U, std::thread::hardware_concurrency())));
            if (ImGui::BeginTable("SystemProfile", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                drawReadOnlyTableRow("Backend", "imgui");
                drawReadOnlyTableRow("Window", "x11/vulkan");
                drawReadOnlyTableRow("Media", std::to_string(snapshot.mediaItems.size()));
                drawReadOnlyTableRow("Clips", std::to_string(snapshot.clips.size()));
                drawReadOnlyTableRow("Tracks", std::to_string(snapshot.tracks.size()));
                drawReadOnlyTableRow("Audio", audioStatus.message);
                drawReadOnlyTableRow(
                    "Audio Buffer Requested",
                    std::to_string(audioStatus.requestedBufferFrames));
                drawReadOnlyTableRow(
                    "Audio Buffer Actual",
                    audioStatus.actualBufferFrames > 0
                    ? std::to_string(audioStatus.actualBufferFrames)
                    : std::string("stream closed"));
                drawReadOnlyTableRow(
                    "Audio Device Requested",
                    audioStatus.requestedOutputDeviceName.empty()
                    ? std::string("system default")
                    : audioStatus.requestedOutputDeviceName);
                drawReadOnlyTableRow(
                    "Audio Device Active",
                    audioStatus.activeOutputDeviceName.empty()
                    ? std::string("stream closed")
                    : audioStatus.activeOutputDeviceName);
                drawReadOnlyTableRow(
                    "Decode Requested",
                    jcut::decodePreferenceCoreName(
                        shellState->decoderPolicy.decodePreference));
                drawReadOnlyTableRow(
                    "Decode Effective",
                    jcut::decodePreferenceCoreName(
                        decoderPreview.sourcePath.empty()
                        ? effectiveDecoder.effectivePreference
                        : decoderPreview.effectiveDecodePreference));
                drawReadOnlyTableRow(
                    "Decode Device",
                    decoderPreview.hardwareDeviceLabel.empty()
                    ? std::string("software")
                    : decoderPreview.hardwareDeviceLabel);
                drawReadOnlyTableRow(
                    "H.26x Threads",
                    std::to_string(
                        effectiveDecoder.softwareThreadCount));
                drawReadOnlyTableRow(
                    "Deterministic",
                    shellState->decoderPolicy.deterministic
                    ? "yes" : "no");
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Decoder Policy");
            const std::array<const char*, 4> decodeLabels{
                "Auto", "Hardware Zero-Copy", "Hardware", "Software"};
            int decodeIndex = static_cast<int>(
                shellState->decoderPolicy.decodePreference);
            if (ImGui::Combo(
                    "Decode Preference",
                    &decodeIndex,
                    decodeLabels.data(),
                    static_cast<int>(decodeLabels.size()))) {
                shellState->decoderPolicy.decodePreference =
                    static_cast<jcut::DecodePreferenceCore>(
                        std::clamp(decodeIndex, 0, 3));
                saveUiPreferences(*shellState);
                requestPreviewRender(shellState);
            }
            const std::array<const char*, 6> hardwareDeviceLabels{
                "Auto",
                "CUDA (NVIDIA)",
                "VA-API",
                "VideoToolbox",
                "D3D11VA",
                "DXVA2"};
            int hardwareDeviceIndex = static_cast<int>(
                shellState->decoderPolicy.hardwareDevice);
            if (ImGui::Combo(
                    "Hardware Decode Device",
                    &hardwareDeviceIndex,
                    hardwareDeviceLabels.data(),
                    static_cast<int>(
                        hardwareDeviceLabels.size()))) {
                shellState->decoderPolicy.hardwareDevice =
                    static_cast<jcut::DecodeHardwareDeviceCore>(
                        std::clamp(hardwareDeviceIndex, 0, 5));
                saveUiPreferences(*shellState);
                requestPreviewRender(shellState);
            }
            const std::array<const char*, 4> threadingLabels{
                "Auto (Stability)",
                "Single Thread",
                "Slice Threads",
                "Frame + Slice Threads"};
            int threadingIndex = static_cast<int>(
                shellState->decoderPolicy.h26xThreadingMode);
            if (ImGui::Combo(
                    "H.264/H.265 CPU Threading",
                    &threadingIndex,
                    threadingLabels.data(),
                    static_cast<int>(threadingLabels.size()))) {
                shellState->decoderPolicy.h26xThreadingMode =
                    static_cast<jcut::H26xThreadingModeCore>(
                        std::clamp(threadingIndex, 0, 3));
                setLegacyStateOverride(
                    shellState,
                    "debugH26xSoftwareThreadingMode",
                    jcut::h26xThreadingModeCoreName(
                        shellState->decoderPolicy.h26xThreadingMode));
                requestPreviewRender(shellState);
            }
            bool deterministic =
                shellState->decoderPolicy.deterministic;
            if (ImGui::Checkbox(
                    "Deterministic Pipeline", &deterministic)) {
                shellState->decoderPolicy.deterministic = deterministic;
                setLegacyStateOverride(
                    shellState,
                    "debugDeterministicPipeline",
                    deterministic);
                setLegacyStateOverride(
                    shellState,
                    "debugDeterministicPipelineExplicit",
                    true);
                requestPreviewRender(shellState);
            }
            int decoderLanes =
                shellState->decoderPolicy.decoderLaneCount;
            if (ImGui::InputInt("Decoder Lane Count", &decoderLanes)) {
                shellState->decoderPolicy.decoderLaneCount =
                    std::clamp(decoderLanes, 0, 16);
                setLegacyStateOverride(
                    shellState,
                    "debugDecoderLaneCount",
                    shellState->decoderPolicy.decoderLaneCount);
            }
            if (!decoderPreview.hardwareFallbackReason.empty()) {
                ImGui::TextDisabled(
                    "Hardware fallback: %s",
                    decoderPreview.hardwareFallbackReason.c_str());
            }
            if (ImGui::Button("Restart Preview Decoder")) {
                requestPreviewRender(shellState);
                shellState->statusMessage =
                    "standalone preview decoder restarted";
            }
            if (shellState->decodeBenchmarkRunning &&
                shellState->decodeBenchmarkFuture.valid() &&
                shellState->decodeBenchmarkFuture.wait_for(
                    std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                shellState->decodeBenchmarkResult =
                    shellState->decodeBenchmarkFuture.get();
                shellState->decodeBenchmarkRunning = false;
                shellState->statusMessage =
                    shellState->decodeBenchmarkResult.message;
            }
            const bool benchmarkSourceAvailable =
                currentClip && currentClip->mediaKind != "title" &&
                !currentClip->sourcePath.empty();
            ImGui::SameLine();
            ImGui::BeginDisabled(
                shellState->decodeBenchmarkRunning ||
                !benchmarkSourceAvailable);
            if (ImGui::Button("Run Decode Benchmark") && currentClip) {
                const std::string benchmarkPath = pathString(
                    resolvedClipMediaPathForProbe(
                        *shellState, *currentClip));
                const jcut::DecoderPolicySettingsCore benchmarkPolicy =
                    shellState->decoderPolicy;
                shellState->decodeBenchmarkRunning = true;
                shellState->decodeBenchmarkFuture = std::async(
                    std::launch::async,
                    [benchmarkPath, benchmarkPolicy]() {
                        return jcut::standalone_render::
                            benchmarkStandaloneMediaDecode(
                                benchmarkPath, benchmarkPolicy);
                    });
            }
            ImGui::EndDisabled();
            if (shellState->decodeBenchmarkRunning) {
                ImGui::TextUnformatted("Decode benchmark running...");
            } else if (!shellState->decodeBenchmarkResult.message.empty()) {
                const auto& benchmark =
                    shellState->decodeBenchmarkResult;
                ImGui::Text(
                    "%s | %d frames | %d failed | %.1f fps | %lld ms",
                    benchmark.codecName.empty()
                    ? "unknown codec"
                    : benchmark.codecName.c_str(),
                    benchmark.framesDecoded,
                    benchmark.failedFrames,
                    benchmark.framesPerSecond,
                    static_cast<long long>(benchmark.elapsedMs));
                if (benchmark.hardwareAccelerated) {
                    ImGui::Text(
                        "Hardware decode active: %s",
                        benchmark.hardwareDeviceLabel.empty()
                        ? "device"
                        : benchmark.hardwareDeviceLabel.c_str());
                } else if (!benchmark.hardwareFallbackReason.empty()) {
                    ImGui::TextDisabled(
                        "Hardware fallback: %s",
                        benchmark.hardwareFallbackReason.c_str());
                }
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab25(
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
        if (ImGui::BeginTabItem("Projects",
                                nullptr,
                                focusProjects ? ImGuiTabItemFlags_SetSelected
                                              : ImGuiTabItemFlags_None)) {
            drawInspectorHeading("Projects", snapshot, currentClip);
            ImGui::TextWrapped("%s", shellState->projectId.empty()
                ? snapshot.projectName.c_str()
                : shellState->projectId.c_str());
            ImGui::TextWrapped("%s", shellState->statePath.empty()
                ? shellState->documentPath.c_str()
                : shellState->statePath.c_str());
            std::string projectListError;
            const std::vector<std::string> projectIds = shellState->usesQtProjectStorage
                ? jcut::availableImGuiProjectIds(&projectListError)
                : std::vector<std::string>{};
            if (!projectListError.empty()) {
                ImGui::TextWrapped("%s", projectListError.c_str());
            }
            if (!shellState->usesQtProjectStorage) {
                ImGui::TextDisabled("Project switching is available only for Qt project storage.");
            }
            ImGui::BeginDisabled(!shellState->usesQtProjectStorage);
            if (ImGui::BeginChild("ProjectList", ImVec2(-1.0f, 150.0f), true)) {
                for (const std::string& projectId : projectIds) {
                    const bool selected = projectId == shellState->projectId;
                    if (ImGui::Selectable(projectId.c_str(), selected) && !selected) {
                        if (!commitExportOutputPathDraft(shellState)) {
                            continue;
                        }
                        jcut::EditorDocumentCore currentDocument;
                        {
                            std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                            currentDocument = shellState->runtime.snapshot();
                        }
                        if (documentIsDirty(*shellState, currentDocument)) {
                            shellState->statusMessage = "save changes before switching projects";
                        } else {
                            std::string error;
                            const std::optional<jcut::ImGuiProjectSession> session =
                                jcut::activateImGuiProjectSession(projectId, &error);
                            if (session.has_value()) {
                                loadProjectSessionIntoShell(
                                    shellState,
                                    *session,
                                    "active project switched: " + projectId);
                            } else {
                                shellState->statusMessage = error;
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndDisabled();
            if (ImGui::Button("Save")) {
                saveCurrentDocument(shellState);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!shellState->usesQtProjectStorage);
            if (ImGui::Button("Save As")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::SaveAs, snapshot);
            }
            ImGui::EndDisabled();
            const bool projectDocumentDirty = documentIsDirty(*shellState, snapshot);
            ImGui::BeginDisabled(
                !shellState->usesQtProjectStorage || projectDocumentDirty);
            if (ImGui::Button("New")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::NewProject, snapshot);
            }
            ImGui::SameLine();
            if (ImGui::Button("Rename")) {
                requestProjectLifecycleAction(
                    shellState, ProjectLifecycleAction::Rename, snapshot);
            }
            ImGui::EndDisabled();
            if (projectDocumentDirty && shellState->usesQtProjectStorage) {
                ImGui::TextDisabled(
                    "Save changes before New, Rename, or project switching.");
            }
            ImGui::Separator();
            ImGui::TextWrapped("%s", shellState->projectRootPath.c_str());
            ImGui::EndTabItem();
        }
}

void drawInspectorTab26(
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
        if (ImGui::BeginTabItem("Preferences")) {
            drawInspectorHeading("Preferences", snapshot, currentClip);
            float uiFontSize = shellState->uiFontSize;
            if (ImGui::SliderFloat("UI Font Size", &uiFontSize, kMinUiFontSize, kMaxUiFontSize, "%.0f")) {
                changeUiFontSize(shellState, uiFontSize - shellState->uiFontSize);
            }
            bool waveform = snapshot.panels.showWaveform;
            if (ImGui::Checkbox("Enable Audio Preview Mode", &waveform)) {
                applyCommand(shellState, jcut::SetWaveformVisibleCommand{waveform});
            }
            bool transcript = snapshot.panels.showTranscript;
            if (ImGui::Checkbox("Enable Transcript Overlay", &transcript)) {
                applyCommand(shellState, jcut::SetTranscriptVisibleCommand{transcript});
            }
            bool scopes = snapshot.panels.showScopes;
            if (ImGui::Checkbox("Enable Scopes", &scopes)) {
                applyCommand(shellState, jcut::SetScopesVisibleCommand{scopes});
            }
            const std::array<int, 6> audioBufferChoices{
                128, 256, 512, 1024, 2048, 4096};
            const std::array<const char*, 6> audioBufferLabels{
                "128", "256", "512", "1024", "2048", "4096"};
            int audioBufferIndex = 3;
            for (int index = 0;
                 index < static_cast<int>(audioBufferChoices.size());
                 ++index) {
                if (audioBufferChoices[index] ==
                    shellState->audioBufferFrames) {
                    audioBufferIndex = index;
                    break;
                }
            }
            if (ImGui::Combo(
                    "Audio Buffer Frames",
                    &audioBufferIndex,
                    audioBufferLabels.data(),
                    static_cast<int>(audioBufferLabels.size()))) {
                shellState->audioBufferFrames =
                    audioBufferChoices[audioBufferIndex];
                shellState->audioRuntime.setBufferFrames(
                    static_cast<unsigned int>(
                        shellState->audioBufferFrames));
                saveUiPreferences(*shellState);
                shellState->statusMessage =
                    "audio buffer updated; playback output will restart";
            }
            const jcut::ImGuiAudioStatus audioStatus =
                shellState->audioRuntime.status();
            const std::string selectedDeviceLabel =
                shellState->audioOutputDeviceName.empty()
                ? "System Default"
                : shellState->audioOutputDeviceName;
            if (ImGui::BeginCombo(
                    "Audio Output Device",
                    selectedDeviceLabel.c_str())) {
                const bool defaultSelected =
                    shellState->audioOutputDeviceName.empty();
                if (ImGui::Selectable(
                        "System Default", defaultSelected)) {
                    shellState->audioOutputDeviceName.clear();
                    shellState->audioRuntime.setOutputDeviceName({});
                    saveUiPreferences(*shellState);
                    shellState->statusMessage =
                        "default audio output selected";
                }
                for (const jcut::ImGuiAudioOutputDevice& device :
                     audioStatus.outputDevices) {
                    const bool selected =
                        device.name == shellState->audioOutputDeviceName;
                    const std::string label = device.name +
                        (device.isDefault ? " (default)" : "");
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        shellState->audioOutputDeviceName = device.name;
                        shellState->audioRuntime.setOutputDeviceName(
                            device.name);
                        saveUiPreferences(*shellState);
                        shellState->statusMessage =
                            "audio output updated; playback will restart";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Audio Devices")) {
                shellState->audioRuntime.refreshOutputDevices();
                shellState->statusMessage =
                    "audio output devices refreshed";
            }
            ImGui::SeparatorText("Project Safety");
            int autosaveInterval = shellState->autosaveIntervalMinutes;
            if (ImGui::InputInt(
                    "Autosave Interval (min)", &autosaveInterval)) {
                shellState->autosaveIntervalMinutes =
                    std::clamp(autosaveInterval, 1, 120);
                setLegacyStateOverride(
                    shellState,
                    "autosaveIntervalMinutes",
                    shellState->autosaveIntervalMinutes);
                shellState->nextAutosaveAt =
                    std::chrono::steady_clock::now() +
                    std::chrono::minutes(
                        shellState->autosaveIntervalMinutes);
                shellState->statusMessage =
                    "autosave interval changed; save the project to keep it";
            }
            int autosaveBackups = shellState->autosaveMaxBackups;
            if (ImGui::InputInt(
                    "Autosave Backups", &autosaveBackups)) {
                shellState->autosaveMaxBackups =
                    std::clamp(autosaveBackups, 1, 200);
                setLegacyStateOverride(
                    shellState,
                    "autosaveMaxBackups",
                    shellState->autosaveMaxBackups);
                shellState->statusMessage =
                    "autosave retention changed; save the project to keep it";
            }
            int historyEntries = shellState->historyMaxEntries;
            if (ImGui::InputInt("History Entries", &historyEntries)) {
                shellState->historyMaxEntries =
                    std::clamp(historyEntries, 10, 500);
                setLegacyStateOverride(
                    shellState,
                    "historyMaxEntries",
                    shellState->historyMaxEntries);
                shellState->statusMessage =
                    "history retention changed; save the project to keep it";
            }
            int historyMegabytes = shellState->historyMaxMegabytes;
            if (ImGui::InputInt(
                    "History Size (MB)", &historyMegabytes)) {
                shellState->historyMaxMegabytes =
                    std::clamp(historyMegabytes, 1, 256);
                setLegacyStateOverride(
                    shellState,
                    "historyMaxMegabytes",
                    shellState->historyMaxMegabytes);
                shellState->statusMessage =
                    "history size changed; save the project to keep it";
            }
            ImGui::TextDisabled(
                "Autosave backups use Qt-compatible state_backup_*.json files. "
                "History limits apply on the next project save.");
            ImGui::SeparatorText("AI Access");
            if (ImGui::Checkbox(
                    "Enable AI Panel",
                    &shellState->featureAiPanel)) {
                setLegacyStateOverride(
                    shellState,
                    "feature_ai_panel",
                    shellState->featureAiPanel);
            }
            if (ImGui::Checkbox(
                    "Enable AI Speaker Cleanup",
                    &shellState->featureAiSpeakerCleanup)) {
                setLegacyStateOverride(
                    shellState,
                    "feature_ai_speaker_cleanup",
                    shellState->featureAiSpeakerCleanup);
            }
            int aiBudget = shellState->aiUsageBudgetCap;
            if (ImGui::InputInt(
                    "AI Project Budget", &aiBudget)) {
                shellState->aiUsageBudgetCap =
                    std::clamp(aiBudget, 1, 1000000);
                setLegacyStateOverride(
                    shellState,
                    "aiUsageBudgetCap",
                    shellState->aiUsageBudgetCap);
            }
            ImGui::TextDisabled(
                "Gateway, model, feature flags, and counters are project "
                "settings. Bearer tokens are never written to the project.");
            ImGui::EndTabItem();
        }
}
