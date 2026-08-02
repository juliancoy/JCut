#pragma once

void drawInspectorTab03(
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
                "Masks", nullptr, inspectorTabFlags("Masks"))) {
            drawInspectorHeading("Masks", snapshot, currentClip);
            const jcut::EditorClip* maskSourceClip = nullptr;
            const jcut::EditorClip* maskEditClip = nullptr;
            if (currentClip && currentClip->clipRole == "mask_matte") {
                maskEditClip = currentClip;
                const auto source = std::find_if(
                    snapshot.clips.begin(), snapshot.clips.end(),
                    [&](const jcut::EditorClip& candidate) {
                        return candidate.persistentId == currentClip->linkedSourceClipId &&
                            candidate.clipRole != "mask_matte";
                    });
                if (source != snapshot.clips.end()) maskSourceClip = &*source;
            } else if (currentClip && currentClip->clipRole != "mask_matte" &&
                       currentClip->mediaKind == "video") {
                maskSourceClip = currentClip;
            }
            if (maskSourceClip &&
                shellState->maskSidecarContextClipId != maskSourceClip->id) {
                shellState->maskSidecarContextClipId = maskSourceClip->id;
                shellState->maskSidecarDirectoryDraft =
                    maskEditClip ? maskEditClip->maskFramesDir : maskSourceClip->maskFramesDir;
                const fs::path sourcePath = resolvedClipMediaPathForProbe(
                    *shellState, *maskSourceClip);
                fs::path preferred(shellState->maskSidecarDirectoryDraft);
                if (preferred.is_relative() && !shellState->projectRootPath.empty()) {
                    preferred = fs::path(shellState->projectRootPath) / preferred;
                }
                shellState->maskSidecars = jcut::masks::discoverMaskSidecarsCore(
                    sourcePath, preferred);
            }
            if (!maskSourceClip) {
                shellState->maskSidecarContextClipId = -1;
                shellState->maskSidecars.clear();
            }
            ImGui::SeparatorText("BiRefNet Alpha Generator");
            const jcut::jobs::BiRefNetJobSnapshotCore birefnetJob =
                shellState->birefnetJob.snapshot();
            if (birefnetJob.state !=
                    jcut::jobs::ProcessJobSnapshotCore::State::Idle) {
                ImGui::TextWrapped("%s", birefnetJob.status.c_str());
                if (birefnetJob.totalFrames > 0) {
                    ImGui::ProgressBar(
                        static_cast<float>(
                            std::clamp(
                                birefnetJob.percent / 100.0,
                                0.0,
                                1.0)),
                        ImVec2(-1.0f, 0.0f));
                    ImGui::TextDisabled(
                        "%lld / %lld frames",
                        static_cast<long long>(
                            birefnetJob.currentFrame),
                        static_cast<long long>(
                            birefnetJob.totalFrames));
                }
                if (!birefnetJob.livePreviewPath.empty()) {
                    ImGui::TextDisabled(
                        "Live preview: %s",
                        birefnetJob.livePreviewPath.c_str());
                }
                if (shellState->birefnetLivePreviewTextureId != 0 &&
                    shellState->birefnetLivePreviewSize.valid()) {
                    const float availableWidth = std::max(
                        160.0f,
                        ImGui::GetContentRegionAvail().x);
                    const float width = std::min(
                        availableWidth, 960.0f);
                    const float height = width *
                        static_cast<float>(
                            shellState->
                                birefnetLivePreviewSize.height) /
                        static_cast<float>(
                            shellState->
                                birefnetLivePreviewSize.width);
                    ImGui::Image(
                        shellState->
                            birefnetLivePreviewTextureId,
                        ImVec2(width, height));
                } else if (!shellState->
                               birefnetLivePreviewError.empty()) {
                    ImGui::TextDisabled(
                        "%s",
                        shellState->
                            birefnetLivePreviewError.c_str());
                } else if (birefnetJob.active()) {
                    ImGui::TextDisabled(
                        "Waiting for the first BiRefNet live preview...");
                }
            }
            if (maskSourceClip) {
                ImGui::BeginDisabled(birefnetJob.active());
                inputTextForString<512>(
                    "BiRefNet Model",
                    &shellState->birefnetModel);
                inputTextForString<512>(
                    "BiRefNet Revision",
                    &shellState->birefnetRevision);
                inputTextForString<512>(
                    "BiRefNet Model Cache",
                    &shellState->birefnetModelCachePath);
                inputTextForString<512>(
                    "BiRefNet Runtime Cache",
                    &shellState->birefnetRuntimeCachePath);
                const char* devices[] = {"CUDA", "CPU"};
                ImGui::SetNextItemWidth(140.0f);
                ImGui::Combo(
                    "BiRefNet Device",
                    &shellState->birefnetDevice,
                    devices,
                    IM_ARRAYSIZE(devices));
                ImGui::BeginDisabled(
                    shellState->birefnetDevice == 1);
                ImGui::Checkbox(
                    "BiRefNet FP16",
                    &shellState->birefnetFp16);
                ImGui::EndDisabled();
                if (shellState->birefnetDevice == 1) {
                    shellState->birefnetFp16 = false;
                }
                ImGui::SetNextItemWidth(140.0f);
                ImGui::SliderFloat(
                    "Alpha Tolerance (%)",
                    &shellState->birefnetAlphaTolerancePercent,
                    0.0f,
                    99.0f,
                    "%.1f");
                ImGui::Checkbox(
                    "Run BiRefNet Docker container as root",
                    &shellState->birefnetDockerRoot);
                ImGui::Checkbox(
                    "Restart and replace prior BiRefNet output",
                    &shellState->birefnetRestart);
                if (ImGui::Button("Run BiRefNet Job")) {
                    startBiRefNetJob(
                        shellState, *maskSourceClip);
                    saveUiPreferences(*shellState);
                }
                ImGui::EndDisabled();
                if (birefnetJob.active()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel BiRefNet Job")) {
                        shellState->birefnetJob.cancel();
                    }
                }
            } else {
                ImGui::TextWrapped(
                    "Select a video source or generated Mask Matte child to run BiRefNet.");
            }
            ImGui::SeparatorText("Prompt Mask Generator");
            const jcut::masks::PromptMaskJobSnapshot promptJob =
                shellState->promptMaskJob.snapshot();
            const int promptJobState =
                static_cast<int>(promptJob.state);
            if (shellState->promptMaskLastState != promptJobState) {
                shellState->promptMaskLastState = promptJobState;
                if (promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Completed) {
                    if (shellState->promptMaskSourceClipId > 0 &&
                        !promptJob.selectedMaskPath.empty()) {
                        applyCommand(
                            shellState,
                            jcut::MaterializeMaskMatteCommand{
                                shellState->promptMaskSourceClipId,
                                promptJob.selectedMaskPath,
                                promptJob.selectedMaskId,
                                promptJob.selectedMaskName});
                    }
                    shellState->statusMessage = promptJob.status;
                    shellState->maskSidecarContextClipId = -1;
                } else if (
                    promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Failed ||
                    promptJob.state ==
                        jcut::masks::PromptMaskJobSnapshot::State::Paused) {
                    shellState->statusMessage = promptJob.status;
                }
            }
            if (promptJob.state !=
                    jcut::masks::PromptMaskJobSnapshot::State::Idle) {
                ImGui::TextWrapped("%s", promptJob.status.c_str());
                if (!promptJob.selectedMaskPath.empty()) {
                    ImGui::TextDisabled(
                        "Result: %s",
                        promptJob.selectedMaskPath.c_str());
                }
                if (!promptJob.logPath.empty()) {
                    ImGui::TextDisabled(
                        "Log: %s", promptJob.logPath.c_str());
                }
            }
            if (maskSourceClip) {
                ImGui::BeginDisabled(promptJob.active());
                inputTextForString<512>(
                    "Text Prompt", &shellState->promptMaskPrompt);
                inputTextForString<512>(
                    "SAM3 Model Cache",
                    &shellState->promptMaskModelCachePath);
                inputTextForString<512>(
                    "SAM3 Runtime Cache",
                    &shellState->promptMaskRuntimeCachePath);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputInt(
                    "Scale Width",
                    &shellState->promptMaskScaleWidth,
                    64,
                    256);
                shellState->promptMaskScaleWidth = std::clamp(
                    shellState->promptMaskScaleWidth, 0, 8192);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputInt(
                    "Prescale Width",
                    &shellState->promptMaskPrescaleWidth,
                    64,
                    256);
                shellState->promptMaskPrescaleWidth = std::clamp(
                    shellState->promptMaskPrescaleWidth, 0, 8192);
                ImGui::BeginDisabled(
                    shellState->promptMaskVideoMode ||
                    shellState->promptMaskWriteBinaryMasks);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::InputFloat(
                    "Extract FPS",
                    &shellState->promptMaskExtractFps,
                    1.0f,
                    5.0f,
                    "%.3f");
                shellState->promptMaskExtractFps = std::clamp(
                    shellState->promptMaskExtractFps, 0.0f, 240.0f);
                ImGui::EndDisabled();
                const char* frameFormats[] = {"JPEG", "PNG"};
                ImGui::BeginDisabled(
                    shellState->promptMaskVideoMode);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::Combo(
                    "Intermediate Frames",
                    &shellState->promptMaskFrameFormat,
                    frameFormats,
                    IM_ARRAYSIZE(frameFormats));
                ImGui::EndDisabled();
                ImGui::Checkbox(
                    "Enable torch.compile",
                    &shellState->promptMaskCompileModel);
                const bool videoModeChanged = ImGui::Checkbox(
                    "Run SAM video mode",
                    &shellState->promptMaskVideoMode);
                if (videoModeChanged) {
                    shellState->promptMaskWriteBinaryMasks =
                        !shellState->promptMaskVideoMode;
                    shellState->promptMaskUnionCurrent = false;
                    shellState->promptMaskWritePreviewFrames = false;
                }
                ImGui::BeginDisabled(shellState->promptMaskVideoMode);
                ImGui::Checkbox(
                    "Write binary mask frames",
                    &shellState->promptMaskWriteBinaryMasks);
                const bool canUnion =
                    maskEditClip && !maskEditClip->maskFramesDir.empty();
                ImGui::BeginDisabled(
                    !shellState->promptMaskWriteBinaryMasks ||
                    !canUnion);
                ImGui::Checkbox(
                    "Union with selected matte",
                    &shellState->promptMaskUnionCurrent);
                ImGui::EndDisabled();
                if (!canUnion) {
                    shellState->promptMaskUnionCurrent = false;
                }
                ImGui::Checkbox(
                    "Write masked preview frames",
                    &shellState->promptMaskWritePreviewFrames);
                ImGui::EndDisabled();
                ImGui::Checkbox(
                    "Export centers JSONL",
                    &shellState->promptMaskExportCenters);
                ImGui::Checkbox(
                    "Run Docker container as root",
                    &shellState->promptMaskDockerRoot);
                ImGui::Checkbox(
                    "Restart and replace prior prompt outputs",
                    &shellState->promptMaskRestart);
                if (ImGui::Button("Run Prompt Mask Job")) {
                    const std::optional<fs::path> script =
                        sam3ScriptPath();
                    jcut::masks::PromptMaskJobRequest request;
                    request.scriptPath =
                        script ? script->string() : std::string{};
                    request.mediaPath =
                        resolvedClipMediaPathForProbe(
                            *shellState, *maskSourceClip).string();
                    request.prompt = shellState->promptMaskPrompt;
                    request.modelCachePath =
                        shellState->promptMaskModelCachePath;
                    request.runtimeCachePath =
                        shellState->promptMaskRuntimeCachePath;
                    request.currentMaskDirectory =
                        canUnion ? maskEditClip->maskFramesDir
                                 : std::string{};
                    if (!request.currentMaskDirectory.empty()) {
                        fs::path current(
                            request.currentMaskDirectory);
                        if (current.is_relative() &&
                            !shellState->projectRootPath.empty()) {
                            current =
                                fs::path(shellState->projectRootPath) /
                                current;
                        }
                        request.currentMaskDirectory =
                            current.lexically_normal().string();
                    }
                    request.scaleWidth =
                        shellState->promptMaskScaleWidth;
                    request.prescaleWidth =
                        shellState->promptMaskPrescaleWidth;
                    request.extractFps =
                        shellState->promptMaskExtractFps;
                    request.intermediateFramesFormat =
                        shellState->promptMaskFrameFormat == 1
                            ? "png"
                            : "jpg";
                    request.compileModel =
                        shellState->promptMaskCompileModel;
                    request.videoMode =
                        shellState->promptMaskVideoMode;
                    request.writeBinaryMasks =
                        shellState->promptMaskWriteBinaryMasks;
                    request.unionWithCurrentMask =
                        shellState->promptMaskUnionCurrent;
                    request.writeMaskPreviewFrames =
                        shellState->promptMaskWritePreviewFrames;
                    request.exportCentersJson =
                        shellState->promptMaskExportCenters;
                    request.runDockerAsRoot =
                        shellState->promptMaskDockerRoot;
                    request.restartPolicy =
                        shellState->promptMaskRestart
                            ? jcut::masks::
                                  PromptMaskRestartPolicy::Restart
                            : jcut::masks::
                                  PromptMaskRestartPolicy::Resume;
                    std::string error;
                    shellState->promptMaskSourceClipId =
                        maskSourceClip->id;
                    if (!shellState->promptMaskJob.start(
                            request, &error)) {
                        shellState->statusMessage =
                            error.empty()
                                ? "prompt-mask job could not start"
                                : error;
                    } else {
                        shellState->promptMaskLastState = -1;
                        shellState->statusMessage =
                            "SAM3 prompt-mask job started";
                        saveUiPreferences(*shellState);
                    }
                }
                ImGui::EndDisabled();
                if (promptJob.active()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel Prompt Mask Job")) {
                        shellState->promptMaskJob.cancel();
                    }
                }
            } else {
                ImGui::TextWrapped(
                    "Select a video source or generated Mask Matte child to run SAM3.");
            }
            ImGui::SeparatorText("Generated Mask Sidecars");
            if (maskSourceClip) {
                ImGui::TextWrapped(
                    "Source: %s. Each ready sidecar materializes as its own generated child lane.",
                    maskSourceClip->label.c_str());
                if (ImGui::Button("Refresh Sidecars")) {
                    shellState->maskSidecarContextClipId = -1;
                }
                ImGui::SetNextItemWidth(
                    std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f));
                inputTextForString<512>(
                    "Sidecar Directory", &shellState->maskSidecarDirectoryDraft);
                ImGui::SameLine();
                if (ImGui::Button("Inspect / Use")) {
                    fs::path directory(shellState->maskSidecarDirectoryDraft);
                    if (directory.is_relative() && !shellState->projectRootPath.empty()) {
                        directory = fs::path(shellState->projectRootPath) / directory;
                    }
                    const auto sidecar = jcut::masks::inspectMaskSidecarCore(
                        directory, resolvedClipMediaPathForProbe(*shellState, *maskSourceClip));
                    if (!sidecar.valid()) {
                        shellState->statusMessage = "mask sidecar contains no frame_*.png files";
                    } else if (!sidecar.ready) {
                        shellState->statusMessage = sidecar.readinessIssue.empty()
                            ? "mask sidecar is not render-ready" : sidecar.readinessIssue;
                    } else {
                        applyCommand(shellState, jcut::MaterializeMaskMatteCommand{
                            maskSourceClip->id,
                            sidecar.directory.string(), sidecar.id, sidecar.displayName});
                        shellState->maskSidecarContextClipId = -1;
                    }
                }
                if (ImGui::BeginTable(
                        "MaskSidecars", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Sidecar");
                    ImGui::TableSetupColumn("Frames");
                    ImGui::TableSetupColumn("Status");
                    ImGui::TableSetupColumn("Action");
                    ImGui::TableHeadersRow();
                    for (const auto& sidecar : shellState->maskSidecars) {
                        ImGui::PushID(sidecar.id.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(sidecar.displayName.c_str());
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", sidecar.directory.string().c_str());
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%lld", static_cast<long long>(sidecar.frameCount));
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(sidecar.ready
                            ? "Ready"
                            : (sidecar.readinessIssue.empty()
                                   ? "Unavailable" : sidecar.readinessIssue.c_str()));
                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(!sidecar.ready);
                        if (ImGui::SmallButton("Use")) {
                            applyCommand(shellState, jcut::MaterializeMaskMatteCommand{
                                maskSourceClip->id,
                                sidecar.directory.string(), sidecar.id,
                                sidecar.displayName});
                            shellState->maskSidecarContextClipId = -1;
                        }
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextWrapped("Select a video source or generated Mask Matte child.");
            }
            ImGui::SeparatorText("Selected Matte Treatment");
            if (maskSourceClip && !maskEditClip) {
                ImGui::TextDisabled(
                    "Choose a ready sidecar above; treatment controls belong to its child lane.");
            }
            if (maskEditClip) {
                int zLevel = maskEditClip->zLevel == std::numeric_limits<int>::min()
                    ? -std::max(0, maskEditClip->trackId - 1) * 100
                    : maskEditClip->zLevel;
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::InputInt("Z Level", &zLevel)) {
                    applyCommand(shellState, jcut::SetClipZLevelCommand{
                        maskEditClip->id, zLevel, false});
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Automatic Z")) {
                    applyCommand(shellState, jcut::SetClipZLevelCommand{
                        maskEditClip->id, 0, true});
                }
            }
            bool enabled = currentClip ? currentClip->maskEnabled : false;
            float radius = currentClip ? static_cast<float>(currentClip->maskFeather) : 0.0f;
            float gamma = currentClip ? static_cast<float>(currentClip->maskFeatherGamma) : 1.0f;
            int falloff = currentClip ? currentClip->maskFeatherFalloff : 0;
            bool foreground = currentClip ? currentClip->maskForegroundLayerEnabled : false;
            bool repeat = currentClip ? currentClip->maskRepeatEnabled : false;
            float repeatX = currentClip ? static_cast<float>(currentClip->maskRepeatDeltaX) : 160.0f;
            float repeatY = currentClip ? static_cast<float>(currentClip->maskRepeatDeltaY) : 0.0f;
            float dilate = currentClip ? static_cast<float>(currentClip->maskDilate) : 0.0f;
            float erode = currentClip ? static_cast<float>(currentClip->maskErode) : 0.0f;
            float blur = currentClip ? static_cast<float>(currentClip->maskBlur) : 0.0f;
            bool temporalStabilize = currentClip
                ? currentClip->maskTemporalStabilizeEnabled : false;
            float temporalStabilizeStrength = currentClip
                ? static_cast<float>(currentClip->maskTemporalStabilizeStrength)
                : 0.75f;
            int temporalStabilizeMotionRadius = currentClip
                ? currentClip->maskTemporalStabilizeMotionRadius : 4;
            bool invert = currentClip ? currentClip->maskInvert : false;
            bool showOnly = currentClip ? currentClip->maskShowOnly : false;
            float opacity = currentClip ? static_cast<float>(currentClip->maskOpacity) : 1.0f;
            bool gradeEnabled = currentClip ? currentClip->maskGradeEnabled : false;
            float gradeBrightness = currentClip
                ? static_cast<float>(currentClip->maskGradeBrightness) : 0.0f;
            float gradeContrast = currentClip
                ? static_cast<float>(currentClip->maskGradeContrast) : 1.0f;
            float gradeSaturation = currentClip
                ? static_cast<float>(currentClip->maskGradeSaturation) : 1.0f;
            std::vector<jcut::EditorPoint> gradeCurveR = currentClip
                ? currentClip->maskGradeCurvePointsR
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveG = currentClip
                ? currentClip->maskGradeCurvePointsG
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveB = currentClip
                ? currentClip->maskGradeCurvePointsB
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            std::vector<jcut::EditorPoint> gradeCurveLuma = currentClip
                ? currentClip->maskGradeCurvePointsLuma
                : std::vector<jcut::EditorPoint>{{0.0, 0.0}, {1.0, 1.0}};
            bool gradeCurveSmoothing = currentClip
                ? currentClip->maskGradeCurveSmoothingEnabled : false;
            bool shadowEnabled = currentClip ? currentClip->maskDropShadowEnabled : false;
            float shadowRadius = currentClip
                ? static_cast<float>(currentClip->maskDropShadowRadius) : 12.0f;
            float shadowOffsetX = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOffsetX) : 0.0f;
            float shadowOffsetY = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOffsetY) : 4.0f;
            float shadowOpacity = currentClip
                ? static_cast<float>(currentClip->maskDropShadowOpacity) : 0.45f;
            ImGui::BeginDisabled(!maskEditClip);
            bool maskChanged = false;
            maskChanged |= ImGui::Checkbox("Enabled", &enabled);
            maskChanged |= ImGui::SliderFloat("Feather Radius", &radius, 0.0f, 256.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Feather Gamma", &gamma, 0.1f, 8.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderInt("Falloff", &falloff, 0, 5);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Dilate", &dilate, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Erode", &erode, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat("Mask Blur", &blur, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::Checkbox(
                "Temporal Stabilize", &temporalStabilize);
            ImGui::BeginDisabled(!temporalStabilize);
            maskChanged |= ImGui::SliderFloat(
                "Stabilize Strength",
                &temporalStabilizeStrength,
                0.0f,
                1.0f,
                "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderInt(
                "Motion Tolerance",
                &temporalStabilizeMotionRadius,
                0,
                32,
                "%d px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            maskChanged |= ImGui::Checkbox("Invert", &invert);
            ImGui::SameLine();
            maskChanged |= ImGui::Checkbox("Show Mask Only", &showOnly);
            maskChanged |= ImGui::SliderFloat("Mask Opacity", &opacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::SeparatorText("Masked Grade");
            maskChanged |= ImGui::Checkbox("Enable Mask Grade", &gradeEnabled);
            maskChanged |= ImGui::SliderFloat(
                "Mask Brightness", &gradeBrightness, -1.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Mask Contrast", &gradeContrast, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Mask Saturation", &gradeSaturation, 0.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::Checkbox(
                "Mask Curve Smoothing", &gradeCurveSmoothing);
            bool maskCurveChanged = false;
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Red Curve", &gradeCurveR, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Green Curve", &gradeCurveG, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Blue Curve", &gradeCurveB, false, gradeCurveSmoothing);
            maskCurveChanged |= drawGradingCurvePointEditor(
                "Mask Luma Curve", &gradeCurveLuma, false, gradeCurveSmoothing);
            if (maskCurveChanged) {
                beginRuntimeHistoryTransaction(shellState);
                maskChanged = true;
            }
            ImGui::SeparatorText("Mask Shadow");
            maskChanged |= ImGui::Checkbox("Enable Mask Shadow", &shadowEnabled);
            maskChanged |= ImGui::SliderFloat(
                "Shadow Radius", &shadowRadius, 0.0f, 200.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Shadow Offset X", &shadowOffsetX, 1.0f, -500.0f, 500.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Shadow Offset Y", &shadowOffsetY, 1.0f, -500.0f, 500.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::SliderFloat(
                "Shadow Opacity", &shadowOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::SeparatorText("Mask Layers");
            maskChanged |= ImGui::Checkbox("Foreground Layer", &foreground);
            maskChanged |= ImGui::Checkbox("Repeat", &repeat);
            maskChanged |= ImGui::DragFloat("Repeat X", &repeatX, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            maskChanged |= ImGui::DragFloat("Repeat Y", &repeatY, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (maskChanged && maskEditClip) {
                jcut::SetClipMaskCommand command;
                command.clipId = maskEditClip->id;
                command.maskEnabled = enabled;
                command.feather = radius;
                command.featherGamma = gamma;
                command.featherFalloff = falloff;
                command.foregroundLayerEnabled = foreground;
                command.repeatEnabled = repeat;
                command.repeatDeltaX = repeatX;
                command.repeatDeltaY = repeatY;
                command.dilate = dilate;
                command.erode = erode;
                command.blur = blur;
                command.temporalStabilizeEnabled = temporalStabilize;
                command.temporalStabilizeStrength =
                    temporalStabilizeStrength;
                command.temporalStabilizeMotionRadius =
                    temporalStabilizeMotionRadius;
                command.invert = invert;
                command.showOnly = showOnly;
                command.opacity = opacity;
                command.gradeEnabled = gradeEnabled;
                command.gradeBrightness = gradeBrightness;
                command.gradeContrast = gradeContrast;
                command.gradeSaturation = gradeSaturation;
                command.gradeCurvePointsR = std::move(gradeCurveR);
                command.gradeCurvePointsG = std::move(gradeCurveG);
                command.gradeCurvePointsB = std::move(gradeCurveB);
                command.gradeCurvePointsLuma = std::move(gradeCurveLuma);
                command.gradeCurveSmoothingEnabled = gradeCurveSmoothing;
                command.dropShadowEnabled = shadowEnabled;
                command.dropShadowRadius = shadowRadius;
                command.dropShadowOffsetX = shadowOffsetX;
                command.dropShadowOffsetY = shadowOffsetY;
                command.dropShadowOpacity = shadowOpacity;
                applyCommand(shellState, std::move(command));
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
}

void drawInspectorTab04(
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
        if (ImGui::BeginTabItem("Corrections")) {
            drawInspectorHeading("Corrections", snapshot, currentClip);
            const bool supportsCorrections = currentClip && currentClip->mediaKind != "audio";
            if (!supportsCorrections || shellState->correctionClipId != currentClip->id) {
                shellState->correctionDrawMode = false;
                shellState->correctionClipId = currentClip ? currentClip->id : -1;
                shellState->selectedCorrectionPolygon = -1;
                shellState->correctionDraftPoints.clear();
            }
            bool enabled = snapshot.exportRequest.correctionsEnabled;
            if (ImGui::Checkbox("Enable Corrections", &enabled)) {
                applyCommand(shellState, jcut::SetCorrectionsEnabledCommand{enabled});
            }
            int deletePolygon = -1;
            if (ImGui::BeginTable("PolygonRanges", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("On");
                ImGui::TableSetupColumn("Start");
                ImGui::TableSetupColumn("End");
                ImGui::TableSetupColumn("Points");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (std::size_t polygonIndex = 0;
                         polygonIndex < currentClip->correctionPolygons.size();
                         ++polygonIndex) {
                        const jcut::EditorCorrectionPolygon& polygon =
                            currentClip->correctionPolygons[polygonIndex];
                        ImGui::PushID(static_cast<int>(polygonIndex));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        bool polygonEnabled = polygon.enabled;
                        if (ImGui::Checkbox("##enabled", &polygonEnabled)) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].enabled = polygonEnabled;
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        std::int64_t startFrame = polygon.startFrame;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        const bool startChanged =
                            ImGui::InputScalar("##start", ImGuiDataType_S64, &startFrame);
                        beginRuntimeHistoryTransactionForLastItem(shellState);
                        if (startChanged) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].startFrame = std::max<std::int64_t>(0, startFrame);
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        std::int64_t endFrame = polygon.endFrame;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        const bool endChanged =
                            ImGui::InputScalar("##end", ImGuiDataType_S64, &endFrame);
                        beginRuntimeHistoryTransactionForLastItem(shellState);
                        if (endChanged) {
                            auto polygons = currentClip->correctionPolygons;
                            polygons[polygonIndex].endFrame = endFrame;
                            applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                                currentClip->id, std::move(polygons)});
                        }
                        ImGui::TableNextColumn();
                        const std::string pointLabel = std::to_string(polygon.pointsNormalized.size());
                        if (ImGui::Selectable(pointLabel.c_str(),
                                              shellState->selectedCorrectionPolygon ==
                                                  static_cast<int>(polygonIndex))) {
                            shellState->selectedCorrectionPolygon = static_cast<int>(polygonIndex);
                        }
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Delete")) {
                            deletePolygon = static_cast<int>(polygonIndex);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            if (deletePolygon >= 0 && currentClip) {
                auto polygons = currentClip->correctionPolygons;
                polygons.erase(polygons.begin() + deletePolygon);
                applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                    currentClip->id, std::move(polygons)});
                shellState->selectedCorrectionPolygon = -1;
            }
            ImGui::BeginDisabled(!supportsCorrections);
            if (ImGui::Button(shellState->correctionDrawMode ? "Drawing Polygon..." : "Draw Polygon")) {
                shellState->correctionDrawMode = !shellState->correctionDrawMode;
                shellState->correctionDraftPoints.clear();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->correctionDraftPoints.size() < 3);
            if (ImGui::Button("Close Polygon") && currentClip) {
                auto polygons = currentClip->correctionPolygons;
                jcut::EditorCorrectionPolygon polygon;
                polygon.pointsNormalized = shellState->correctionDraftPoints;
                polygons.push_back(std::move(polygon));
                applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                    currentClip->id, std::move(polygons)});
                shellState->selectedCorrectionPolygon =
                    static_cast<int>(currentClip->correctionPolygons.size());
                shellState->correctionDraftPoints.clear();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(shellState->correctionDraftPoints.empty());
            if (ImGui::Button("Cancel Draft")) {
                shellState->correctionDraftPoints.clear();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (shellState->correctionDrawMode) {
                ImGui::TextDisabled("Click the Program monitor to add points (%zu drafted).",
                                    shellState->correctionDraftPoints.size());
            }
            if (currentClip && shellState->selectedCorrectionPolygon >= 0 &&
                shellState->selectedCorrectionPolygon <
                    static_cast<int>(currentClip->correctionPolygons.size())) {
                const int polygonIndex = shellState->selectedCorrectionPolygon;
                const auto& points = currentClip->correctionPolygons[polygonIndex].pointsNormalized;
                ImGui::SeparatorText("Selected Polygon Vertices");
                int deletePoint = -1;
                for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                    ImGui::PushID(static_cast<int>(pointIndex));
                    float coordinates[2] = {
                        static_cast<float>(points[pointIndex].x),
                        static_cast<float>(points[pointIndex].y)};
                    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 70.0f));
                    const bool vertexChanged = ImGui::DragFloat2(
                        "##vertex", coordinates, 0.0025f, 0.0f, 1.0f, "%.4f");
                    beginRuntimeHistoryTransactionForLastItem(shellState);
                    if (vertexChanged) {
                        auto polygons = currentClip->correctionPolygons;
                        polygons[polygonIndex].pointsNormalized[pointIndex] = {
                            coordinates[0], coordinates[1]};
                        applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                            currentClip->id, std::move(polygons)});
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        deletePoint = static_cast<int>(pointIndex);
                    }
                    ImGui::PopID();
                }
                if (deletePoint >= 0) {
                    auto polygons = currentClip->correctionPolygons;
                    auto& editablePoints = polygons[polygonIndex].pointsNormalized;
                    editablePoints.erase(editablePoints.begin() + deletePoint);
                    applyCommand(shellState, jcut::SetClipCorrectionPolygonsCommand{
                        currentClip->id, std::move(polygons)});
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!currentClip || currentClip->correctionPolygons.empty());
            if (ImGui::Button("Clear All Polygons") && currentClip) {
                applyCommand(shellState, jcut::ClearCorrectionPolygonsCommand{currentClip->id});
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
}

void drawInspectorTab05(
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
        if (ImGui::BeginTabItem("Titles")) {
            drawInspectorHeading("Titles", snapshot, currentClip);
            if (ImGui::Button("Create Title At Playhead")) {
                const jcut::CommandResult result = applyCommand(
                    shellState,
                    jcut::CreateTitleClipCommand{
                        snapshot.transport.currentFrame,
                        jcut::kEditorDefaultTitleDurationFrames});
                if (result.applied) {
                    shellState->titleDraftClipId = -1;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Titles lane");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Uses the unnumbered Titles track first. Overlaps route "
                    "to a free Titles track or create a numbered lane.");
            }
            ImGui::Separator();
            if (!currentClip) {
                shellState->titleDraftClipId = -1;
            }
            if (currentClip && shellState->titleDraftClipId != currentClip->id) {
                jcut::EditorTitleKeyframe draft;
                draft.frame = currentClipLocalFrame;
                const auto keyframeAtPlayhead = std::find_if(
                    currentClip->titleKeyframes.begin(),
                    currentClip->titleKeyframes.end(),
                    [&](const jcut::EditorTitleKeyframe& keyframe) {
                        return keyframe.frame == currentClipLocalFrame;
                    });
                if (keyframeAtPlayhead != currentClip->titleKeyframes.end()) {
                    draft = *keyframeAtPlayhead;
                }
                hydrateTitleDraft(shellState, currentClip->id, draft);
            }
            ImGui::BeginDisabled(!currentClip);
            ImGui::InputTextMultiline("Title Text",
                                      &shellState->titleDraft.text,
                                      ImVec2(-1.0f, 90.0f));
            ImGui::DragScalar("X",
                              ImGuiDataType_Double,
                              &shellState->titleDraft.translationX,
                              1.0f,
                              nullptr,
                              nullptr,
                              "%.1f");
            ImGui::DragScalar("Y",
                              ImGuiDataType_Double,
                              &shellState->titleDraft.translationY,
                              1.0f,
                              nullptr,
                              nullptr,
                              "%.1f");
            const double minimumFontSize = 8.0;
            const double maximumFontSize = 240.0;
            ImGui::SliderScalar("Font Size",
                                ImGuiDataType_Double,
                                &shellState->titleDraft.fontSize,
                                &minimumFontSize,
                                &maximumFontSize,
                                "%.0f");
            const double minimumOpacity = 0.0;
            const double maximumOpacity = 1.0;
            ImGui::SliderScalar("Title Opacity",
                                ImGuiDataType_Double,
                                &shellState->titleDraft.opacity,
                                &minimumOpacity,
                                &maximumOpacity,
                                "%.2f");
            inputTextForString<128>("Font Family", &shellState->titleDraft.fontFamily);
            ImGui::Checkbox("Bold", &shellState->titleDraft.bold);
            ImGui::SameLine();
            ImGui::Checkbox("Italic", &shellState->titleDraft.italic);
            editHexRgbColor("Title Color",
                            &shellState->titleDraft.color,
                            "#ffffff");
            ImGui::Checkbox("Auto Fit To Output",
                            &shellState->titleDraft.autoFitToOutput);
            inputTextForString<512>("Logo Path", &shellState->titleDraft.logoPath);
            const auto materialCombo = [](const char* label, std::string* value) {
                const std::array<std::pair<const char*, const char*>, 5> options{{
                    {"Solid", "solid"},
                    {"Neon", "neon"},
                    {"Diagonal Stripes", "diagonal_stripes"},
                    {"Grid", "grid"},
                    {"Image Pattern", "image_pattern"},
                }};
                const char* preview = "Solid";
                for (const auto& [name, stored] : options) {
                    if (*value == stored) preview = name;
                }
                bool changed = false;
                if (ImGui::BeginCombo(label, preview)) {
                    for (const auto& [name, stored] : options) {
                        const bool selected = *value == stored;
                        if (ImGui::Selectable(name, selected)) {
                            *value = stored;
                            changed = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                return changed;
            };
            const auto sliderDouble = [](const char* label, double* value,
                                         double minimum, double maximum,
                                         const char* format) {
                return ImGui::SliderScalar(label, ImGuiDataType_Double, value,
                                           &minimum, &maximum, format);
            };
            ImGui::SeparatorText("Material");
            materialCombo("Text Material", &shellState->titleDraft.textMaterialStyle);
            ImGui::BeginDisabled(shellState->titleDraft.textMaterialStyle != "image_pattern");
            inputTextForString<512>("Text Pattern Path",
                                    &shellState->titleDraft.textPatternImagePath);
            sliderDouble("Text Pattern Scale",
                         &shellState->titleDraft.textPatternScale,
                         0.1, 8.0, "%.2f");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Shadow");
            ImGui::Checkbox("Drop Shadow", &shellState->titleDraft.dropShadowEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.dropShadowEnabled);
            editHexRgbColor("Shadow Color", &shellState->titleDraft.dropShadowColor,
                            "#000000");
            sliderDouble("Shadow Opacity", &shellState->titleDraft.dropShadowOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Shadow X", &shellState->titleDraft.dropShadowOffsetX,
                         -200.0, 200.0, "%.1f px");
            sliderDouble("Shadow Y", &shellState->titleDraft.dropShadowOffsetY,
                         -200.0, 200.0, "%.1f px");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Window");
            ImGui::Checkbox("Title Window", &shellState->titleDraft.windowEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.windowEnabled);
            editHexRgbColor("Window Color", &shellState->titleDraft.windowColor,
                            "#000000");
            sliderDouble("Window Opacity", &shellState->titleDraft.windowOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Window Padding", &shellState->titleDraft.windowPadding,
                         0.0, 400.0, "%.1f px");
            sliderDouble("Window Width", &shellState->titleDraft.windowWidth,
                         0.0, 3840.0, "%.1f px");
            ImGui::EndDisabled();
            ImGui::Checkbox("Window Frame", &shellState->titleDraft.windowFrameEnabled);
            ImGui::BeginDisabled(!shellState->titleDraft.windowFrameEnabled);
            editHexRgbColor("Window Frame Color",
                            &shellState->titleDraft.windowFrameColor, "#ffffff");
            sliderDouble("Window Frame Opacity",
                         &shellState->titleDraft.windowFrameOpacity,
                         0.0, 1.0, "%.2f");
            sliderDouble("Window Frame Width",
                         &shellState->titleDraft.windowFrameWidth,
                         0.0, 120.0, "%.1f px");
            sliderDouble("Window Frame Gap",
                         &shellState->titleDraft.windowFrameGap,
                         0.0, 200.0, "%.1f px");
            materialCombo("Window Frame Material",
                          &shellState->titleDraft.windowFrameMaterialStyle);
            ImGui::BeginDisabled(
                shellState->titleDraft.windowFrameMaterialStyle != "image_pattern");
            inputTextForString<512>("Window Frame Pattern Path",
                                    &shellState->titleDraft.windowFramePatternImagePath);
            sliderDouble("Window Frame Pattern Scale",
                         &shellState->titleDraft.windowFramePatternScale,
                         0.1, 8.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            ImGui::SeparatorText("3D / Extrusion");
            ImGui::Checkbox("3D Transform", &shellState->titleDraft.vulkan3DEnabled);
            ImGui::Checkbox("3D Extrusion", &shellState->titleDraft.vulkan3DExtrudeEnabled);
            const std::array<std::pair<const char*, const char*>, 3> extrudeModes{{
                {"None", "none"},
                {"Stacked Copies", "stacked_copies"},
                {"Eroded Solid", "eroded_solid"},
            }};
            const char* extrudePreview = "None";
            for (const auto& [name, stored] : extrudeModes) {
                if (shellState->titleDraft.textExtrudeMode == stored) extrudePreview = name;
            }
            if (ImGui::BeginCombo("Extrusion Mode", extrudePreview)) {
                for (const auto& [name, stored] : extrudeModes) {
                    const bool selected = shellState->titleDraft.textExtrudeMode == stored;
                    if (ImGui::Selectable(name, selected)) {
                        shellState->titleDraft.textExtrudeMode = stored;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(!shellState->titleDraft.vulkan3DExtrudeEnabled ||
                                 shellState->titleDraft.textExtrudeMode == "none");
            sliderDouble("Extrusion Depth",
                         &shellState->titleDraft.vulkan3DExtrudeDepth,
                         0.0, 2.0, "%.2f");
            sliderDouble("Bevel Scale", &shellState->titleDraft.vulkan3DBevelScale,
                         0.0, 2.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!shellState->titleDraft.vulkan3DEnabled);
            sliderDouble("Yaw", &shellState->titleDraft.vulkan3DYawDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("Pitch", &shellState->titleDraft.vulkan3DPitchDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("Roll", &shellState->titleDraft.vulkan3DRollDegrees,
                         -360.0, 360.0, "%.1f deg");
            sliderDouble("3D Depth", &shellState->titleDraft.vulkan3DDepth,
                         -10.0, 10.0, "%.2f");
            sliderDouble("3D Scale", &shellState->titleDraft.vulkan3DScale,
                         0.01, 10.0, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Linear Interpolation",
                            &shellState->titleDraft.linearInterpolation);
            if (ImGui::Button("Add/Update At Playhead") && currentClip) {
                jcut::EditorTitleKeyframe keyframe = shellState->titleDraft;
                keyframe.frame = currentClipLocalFrame;
                applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                    currentClip->id, std::move(keyframe)});
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove At Playhead") && currentClip) {
                applyCommand(shellState, jcut::RemoveTitleKeyframeCommand{
                    currentClip->id, currentClipLocalFrame});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("TitleKeys", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Text");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Style");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorTitleKeyframe& keyframe : currentClip->titleKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (drawFrameSeekCell(
                                shellState,
                                keyframe.frame,
                                static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                                "title-frame-" + std::to_string(keyframe.frame))) {
                            hydrateTitleDraft(shellState, currentClip->id, keyframe);
                        }
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::TitleKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Transform,
                            keyframe.frame);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", keyframe.text.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f, %.0f", keyframe.translationX, keyframe.translationY);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s %.0f | %.2f | %s%s | %s",
                                           keyframe.fontFamily.c_str(),
                                           keyframe.fontSize,
                                           keyframe.opacity,
                                           keyframe.bold ? "Bold" : "Regular",
                                           keyframe.italic ? " Italic" : "",
                                           keyframe.color.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "title-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load")) {
                            hydrateTitleDraft(shellState, currentClip->id, keyframe);
                            applyCommand(shellState, jcut::SeekToFrameCommand{
                                static_cast<int>(std::clamp<std::int64_t>(
                                    static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                                    0,
                                    std::numeric_limits<int>::max()))});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorTitleKeyframe updated = keyframe;
                            updated.linearInterpolation = !updated.linearInterpolation;
                            applyCommand(shellState, jcut::UpsertTitleKeyframeCommand{
                                currentClip->id, std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            applyCommand(shellState, jcut::RemoveTitleKeyframeCommand{
                                currentClip->id, keyframe.frame});
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab06(
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
                "Sync", nullptr, inspectorTabFlags("Sync"))) {
            drawInspectorHeading("Sync", snapshot, currentClip);
            ImGui::BeginDisabled(!currentClip);
            if (ImGui::Button("Duplicate Frame") && currentClip) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    currentClip->id, snapshot.transport.currentFrame, false, 1});
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip Frame") && currentClip) {
                applyCommand(shellState, jcut::AddRenderSyncMarkerCommand{
                    currentClip->id, snapshot.transport.currentFrame, true, 1});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(snapshot.renderSyncMarkers.empty());
            if (ImGui::Button("Clear All Sync Points")) {
                applyCommand(shellState, jcut::ClearRenderSyncMarkersCommand{});
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable("SyncPoints", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Clip");
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("Operation");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                for (std::size_t markerIndex = 0;
                     markerIndex < snapshot.renderSyncMarkers.size();
                     ++markerIndex) {
                    const jcut::EditorRenderSyncMarker& marker =
                        snapshot.renderSyncMarkers[markerIndex];
                    const jcut::EditorClip* owner =
                        clipForPersistentId(snapshot, marker.clipId);
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(markerIndex));
                    ImGui::TableNextColumn();
                    const std::string syncOwnerLabel =
                        marker.clipId + "##sync-owner";
                    if (ImGui::Selectable(syncOwnerLabel.c_str())) {
                        applyCommand(
                            shellState,
                            jcut::SeekToFrameCommand{
                                static_cast<int>(
                                    std::clamp<std::int64_t>(
                                        marker.frame,
                                        0,
                                        std::numeric_limits<int>::max()))});
                    }
                    markSyncDeleteTargetForLastItem(shellState, marker);
                    ImGui::TableNextColumn();
                    std::int64_t frame = marker.frame;
                    ImGui::SetNextItemWidth(84.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool frameChanged = ImGui::InputScalar(
                        "##frame",
                        ImGuiDataType_S64,
                        &frame,
                        nullptr,
                        nullptr,
                        "%lld",
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::EndDisabled();
                    if (frameChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            frame,
                            marker.skipFrame,
                            marker.count);
                    }
                    ImGui::TableNextColumn();
                    int count = marker.count;
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool countChanged = ImGui::InputInt(
                        "##count",
                        &count,
                        1,
                        10,
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::EndDisabled();
                    if (countChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            marker.frame,
                            marker.skipFrame,
                            std::clamp(
                                count,
                                jcut::kEditorRenderSyncMinCount,
                                jcut::kEditorRenderSyncMaxCount));
                    }
                    ImGui::TableNextColumn();
                    constexpr const char* operations[] = {"Duplicate", "Skip"};
                    int operation = marker.skipFrame ? 1 : 0;
                    ImGui::SetNextItemWidth(92.0f);
                    ImGui::BeginDisabled(!owner);
                    const bool operationChanged = ImGui::Combo(
                        "##operation", &operation, operations, IM_ARRAYSIZE(operations));
                    ImGui::EndDisabled();
                    if (operationChanged && owner) {
                        replaceRenderSyncMarker(
                            shellState,
                            marker,
                            owner->id,
                            marker.frame,
                            operation == 1,
                            marker.count);
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Seek")) {
                        applyCommand(shellState,
                                     jcut::SeekToFrameCommand{
                                         static_cast<int>(std::clamp<std::int64_t>(
                                             marker.frame,
                                             0,
                                             std::numeric_limits<int>::max()))});
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        applyCommand(shellState,
                                     jcut::RemoveRenderSyncMarkerCommand{
                                         marker.clipId,
                                         marker.frame,
                                         marker.skipFrame});
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
}

void drawInspectorTab07(
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
        if (ImGui::BeginTabItem("Transform")) {
            drawInspectorHeading("Transform", snapshot, currentClip);
            float tx = currentClip ? static_cast<float>(currentClip->baseTranslationX) : 0.0f;
            float ty = currentClip ? static_cast<float>(currentClip->baseTranslationY) : 0.0f;
            float rotation = currentClip ? static_cast<float>(currentClip->baseRotation) : 0.0f;
            float scaleX = currentClip ? static_cast<float>(currentClip->baseScaleX) : 1.0f;
            float scaleY = currentClip ? static_cast<float>(currentClip->baseScaleY) : 1.0f;
            ImGui::BeginDisabled(!currentClip);
            bool transformChanged = false;
            transformChanged |= ImGui::DragFloat("Translate X", &tx, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::DragFloat("Translate Y", &ty, 1.0f);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Rotation", &rotation, -180.0f, 180.0f, "%.1f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Scale X", &scaleX, -4.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            transformChanged |= ImGui::SliderFloat("Scale Y", &scaleY, -4.0f, 4.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (transformChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipTransformCommand{
                    currentClip->id, tx, ty, rotation, scaleX, scaleY});
            }
            if (ImGui::Button("Flip Horizontal") && currentClip) {
                applyCommand(shellState, jcut::SetClipTransformCommand{
                    currentClip->id, tx, ty, rotation, -scaleX, scaleY});
            }
            if (ImGui::CollapsingHeader(
                    "Automatic Speaker Framing",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                bool framingEnabled =
                    currentClip && currentClip->speakerFramingEnabled;
                float bakedTargetX = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetXNorm)
                    : 0.5f;
                float bakedTargetY = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetYNorm)
                    : 0.35f;
                float bakedTargetBox = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingBakedTargetBoxNorm)
                    : -1.0f;
                float minimumConfidence = currentClip
                    ? static_cast<float>(
                          currentClip->speakerFramingMinConfidence)
                    : 0.08f;
                int manualTrackId = currentClip
                    ? currentClip->speakerFramingManualTrackId : -1;
                std::string manualStreamId = currentClip
                    ? currentClip->speakerFramingManualStreamId
                    : std::string{};
                int centerSmoothingFrames = currentClip
                    ? currentClip->speakerFramingCenterSmoothingFrames : 0;
                int zoomSmoothingFrames = currentClip
                    ? currentClip->speakerFramingZoomSmoothingFrames : 0;
                int smoothingMode = currentClip
                    ? currentClip->speakerFramingSmoothingMode : 0;
                float centerSmoothingStrength = currentClip
                    ? static_cast<float>(
                          currentClip
                              ->speakerFramingCenterSmoothingStrength)
                    : 1.0f;
                float zoomSmoothingStrength = currentClip
                    ? static_cast<float>(
                          currentClip
                              ->speakerFramingZoomSmoothingStrength)
                    : 1.0f;
                int gapHoldFrames = currentClip
                    ? currentClip->speakerFramingGapHoldFrames : 0;
                bool framingSettingsChanged = false;
                framingSettingsChanged |= ImGui::Checkbox(
                    "Enabled##speakerFraming", &framingEnabled);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target X",
                    &bakedTargetX,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target Y",
                    &bakedTargetY,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Baked Target Box",
                    &bakedTargetBox,
                    -1.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Minimum Confidence",
                    &minimumConfidence,
                    0.0f,
                    1.0f,
                    "%.3f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::InputInt(
                    "Manual Track ID", &manualTrackId);
                framingSettingsChanged |= inputTextForString<128>(
                    "Manual Stream ID", &manualStreamId);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Center Smoothing Frames",
                    &centerSmoothingFrames,
                    0,
                    500);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Zoom Smoothing Frames",
                    &zoomSmoothingFrames,
                    0,
                    500);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                constexpr const char* smoothingModes[] = {
                    "Centered",
                    "Causal",
                    "Forward"};
                framingSettingsChanged |= ImGui::Combo(
                    "Smoothing Mode",
                    &smoothingMode,
                    smoothingModes,
                    IM_ARRAYSIZE(smoothingModes));
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Center Smoothing Strength",
                    &centerSmoothingStrength,
                    0.0f,
                    5.0f,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderFloat(
                    "Zoom Smoothing Strength",
                    &zoomSmoothingStrength,
                    0.0f,
                    5.0f,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                framingSettingsChanged |= ImGui::SliderInt(
                    "Gap Hold Frames", &gapHoldFrames, 0, 240);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                if (framingSettingsChanged && currentClip) {
                    applyCommand(
                        shellState,
                        jcut::SetClipSpeakerFramingCommand{
                            currentClip->id,
                            framingEnabled,
                            bakedTargetX,
                            bakedTargetY,
                            bakedTargetBox,
                            minimumConfidence,
                            manualTrackId,
                            manualStreamId,
                            centerSmoothingFrames,
                            zoomSmoothingFrames,
                            smoothingMode,
                            centerSmoothingStrength,
                            zoomSmoothingStrength,
                            gapHoldFrames});
                }

                if (ImGui::Button("Enable At Playhead") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingEnabledKeyframeCommand{
                            currentClip->id,
                            {currentClipLocalFrame, true}});
                }
                ImGui::SameLine();
                if (ImGui::Button("Disable At Playhead") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingEnabledKeyframeCommand{
                            currentClip->id,
                            {currentClipLocalFrame, false}});
                }

                const auto latestTransformAtPlayhead =
                    [&](const std::vector<jcut::EditorTransformKeyframe>&
                            keyframes,
                        jcut::EditorTransformKeyframe fallback) {
                        for (const auto& keyframe : keyframes) {
                            if (keyframe.frame >
                                currentClipLocalFrame) {
                                break;
                            }
                            fallback = keyframe;
                        }
                        fallback.frame = currentClipLocalFrame;
                        return fallback;
                    };
                jcut::EditorTransformKeyframe framingDraft{
                    currentClipLocalFrame,
                    "Baked Framing",
                    0.0,
                    0.0,
                    0.0,
                    1.0,
                    1.0,
                    true};
                jcut::EditorTransformKeyframe targetDraft{
                    currentClipLocalFrame,
                    "Framing Target",
                    0.5,
                    0.35,
                    0.0,
                    -1.0,
                    -1.0,
                    true};
                if (currentClip) {
                    framingDraft = latestTransformAtPlayhead(
                        currentClip->speakerFramingKeyframes,
                        framingDraft);
                    targetDraft = latestTransformAtPlayhead(
                        currentClip->speakerFramingTargetKeyframes,
                        targetDraft);
                }
                ImGui::SeparatorText("Baked Transform At Playhead");
                ImGui::InputDouble(
                    "Baked Translate X",
                    &framingDraft.translationX,
                    1.0,
                    10.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Translate Y",
                    &framingDraft.translationY,
                    1.0,
                    10.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Rotation",
                    &framingDraft.rotation,
                    0.1,
                    1.0,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Scale X",
                    &framingDraft.scaleX,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Baked Scale Y",
                    &framingDraft.scaleY,
                    0.01,
                    0.1,
                    "%.3f");
                if (ImGui::Button("Set Baked Transform") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingKeyframeCommand{
                            currentClip->id, framingDraft});
                }
                ImGui::SeparatorText("Dynamic Target At Playhead");
                ImGui::InputDouble(
                    "Target X",
                    &targetDraft.translationX,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Target Y",
                    &targetDraft.translationY,
                    0.01,
                    0.1,
                    "%.3f");
                ImGui::InputDouble(
                    "Target Box",
                    &targetDraft.scaleX,
                    0.01,
                    0.1,
                    "%.3f");
                targetDraft.scaleY = targetDraft.scaleX;
                if (ImGui::Button("Set Dynamic Target") &&
                    currentClip) {
                    applyCommand(
                        shellState,
                        jcut::UpsertSpeakerFramingTargetKeyframeCommand{
                            currentClip->id, targetDraft});
                }

                const auto drawFramingRows =
                    [&](const char* tableId,
                        const char* label,
                        const std::vector<jcut::EditorTransformKeyframe>&
                            keyframes,
                        jcut::EditorKeyframeChannel channel) {
                        ImGui::SeparatorText(label);
                        if (!ImGui::BeginTable(
                                tableId,
                                4,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg)) {
                            return;
                        }
                        ImGui::TableSetupColumn("Frame");
                        ImGui::TableSetupColumn("Position");
                        ImGui::TableSetupColumn("Scale");
                        ImGui::TableSetupColumn("Actions");
                        ImGui::TableHeadersRow();
                        for (const auto& keyframe : keyframes) {
                            ImGui::PushID(
                                static_cast<int>(keyframe.frame));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%lld",
                                static_cast<long long>(
                                    keyframe.frame));
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.3f, %.3f",
                                keyframe.translationX,
                                keyframe.translationY);
                            ImGui::TableNextColumn();
                            ImGui::Text(
                                "%.3f, %.3f",
                                keyframe.scaleX,
                                keyframe.scaleY);
                            ImGui::TableNextColumn();
                            if (ImGui::SmallButton("Seek")) {
                                applyCommand(
                                    shellState,
                                    jcut::SeekToFrameCommand{
                                        currentClip->startFrame +
                                        static_cast<int>(
                                            keyframe.frame)});
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Remove")) {
                                applyCommand(
                                    shellState,
                                    jcut::RemoveClipKeyframeCommand{
                                        currentClip->id,
                                        channel,
                                        keyframe.frame});
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    };
                if (currentClip) {
                    ImGui::SeparatorText("Enable Keyframes");
                    for (const auto& keyframe :
                         currentClip
                             ->speakerFramingEnabledKeyframes) {
                        ImGui::PushID(
                            static_cast<int>(keyframe.frame));
                        ImGui::Text(
                            "Frame %lld: %s",
                            static_cast<long long>(keyframe.frame),
                            keyframe.enabled ? "Enabled" : "Disabled");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Seek")) {
                            applyCommand(
                                shellState,
                                jcut::SeekToFrameCommand{
                                    currentClip->startFrame +
                                    static_cast<int>(keyframe.frame)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            applyCommand(
                                shellState,
                                jcut::RemoveClipKeyframeCommand{
                                    currentClip->id,
                                    jcut::EditorKeyframeChannel::
                                        SpeakerFramingEnabled,
                                    keyframe.frame});
                        }
                        ImGui::PopID();
                    }
                    drawFramingRows(
                        "SpeakerFramingBakedKeys",
                        "Baked Transform Keyframes",
                        currentClip->speakerFramingKeyframes,
                        jcut::EditorKeyframeChannel::
                            SpeakerFraming);
                    drawFramingRows(
                        "SpeakerFramingTargetKeys",
                        "Dynamic Target Keyframes",
                        currentClip
                            ->speakerFramingTargetKeyframes,
                        jcut::EditorKeyframeChannel::
                            SpeakerFramingTarget);
                }
            }
            const jcut::EditorTransformKeyframe transformInitial{
                currentClipLocalFrame,
                {},
                tx,
                ty,
                rotation,
                scaleX,
                scaleY,
                true};
            drawKeyframeDraftEditor<
                jcut::EditorTransformKeyframe,
                jcut::UpsertTransformKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Transform,
                    currentClipLastFrame,
                    "TransformKeyframeEditor",
                    transformInitial,
                    [](jcut::EditorTransformKeyframe* draft) {
                        inputTextForString<128>(
                            "Transform Title", &draft->title);
                        ImGui::InputDouble(
                            "Key Translate X", &draft->translationX, 1.0, 10.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Translate Y", &draft->translationY, 1.0, 10.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Rotation", &draft->rotation, 0.1, 1.0, "%.3f");
                        ImGui::InputDouble(
                            "Key Scale X", &draft->scaleX, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Key Scale Y", &draft->scaleY, 0.01, 0.1, "%.3f");
                    });
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "TransformKeys",
                    7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Title");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Rotation");
                ImGui::TableSetupColumn("Scale");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorTransformKeyframe& keyframe : currentClip->transformKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "transform-frame-" + std::to_string(keyframe.frame));
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Transform,
                            keyframe.frame);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.title.empty() ? "-" : keyframe.title.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f, %.0f", keyframe.translationX, keyframe.translationY);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", keyframe.rotation);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f, %.2f", keyframe.scaleX, keyframe.scaleY);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "transform-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Transform,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorTransformKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertTransformKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Transform,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
}
