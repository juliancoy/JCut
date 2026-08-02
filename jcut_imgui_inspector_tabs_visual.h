#pragma once

void drawInspectorTab00(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip* currentClip,
    const jcut::EditorTrack* currentTrack,
    std::int64_t currentClipLocalFrame,
    std::int64_t currentClipLastFrame,
    std::int64_t fadeEndFrame,
    const std::string& requestedInspectorTab,
    bool focusOutput,
    bool focusProjects)
{
    const auto inspectorTabFlags = [&requestedInspectorTab](const char* label) {
        return requestedInspectorTab == label
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    };
        if (ImGui::BeginTabItem(
                "Grade", nullptr, inspectorTabFlags("Grade"))) {
            drawInspectorHeading("Grade", snapshot, currentClip);
            float saturation = currentClip ? static_cast<float>(currentClip->saturation) : 1.0f;
            float brightness = currentClip ? static_cast<float>(currentClip->brightness) : 0.0f;
            float contrast = currentClip ? static_cast<float>(currentClip->contrast) : 1.0f;
            bool gradePreview = currentClip ? currentClip->gradingPreviewEnabled : false;
            ImGui::BeginDisabled(!currentClip);
            bool gradingChanged = false;
            gradingChanged |= ImGui::SliderFloat("Saturation", &saturation, -10.0f, 10.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Brightness", &brightness, -10.0f, 10.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::SliderFloat("Contrast", &contrast, -10.0f, 10.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            gradingChanged |= ImGui::Checkbox("Preview", &gradePreview);
            if (gradingChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipGradingCommand{
                    currentClip->id, brightness, contrast, saturation, gradePreview});
            }
            const PreviewHistogram histogram = currentPreviewHistogram(shellState);
            if (ImGui::BeginTabBar("GradeChannels")) {
                for (const char* channel : {"Red", "Green", "Blue", "Brightness"}) {
                    if (ImGui::BeginTabItem(channel)) {
                        const std::array<float, PreviewHistogram::kBinCount>* values = &histogram.luma;
                        if (std::strcmp(channel, "Red") == 0) {
                            values = &histogram.red;
                        } else if (std::strcmp(channel, "Green") == 0) {
                            values = &histogram.green;
                        } else if (std::strcmp(channel, "Blue") == 0) {
                            values = &histogram.blue;
                        }
                        if (histogram.valid) {
                            ImGui::PlotHistogram("Histogram",
                                                 values->data(),
                                                 static_cast<int>(values->size()),
                                                 0,
                                                 nullptr,
                                                 0.0f,
                                                 1.0f,
                                                 ImVec2(-1.0f, 72.0f));
                        } else {
                            ImGui::TextDisabled("Histogram requires a CPU preview frame");
                            ImGui::Dummy(ImVec2(-1.0f, 54.0f));
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            const jcut::EditorGradingKeyframe gradingInitial = currentClip
                ? jcut::evaluateEditorClipGradingAtLocalFrame(
                      *currentClip, currentClipLocalFrame)
                : jcut::EditorGradingKeyframe{};
            drawKeyframeDraftEditor<
                jcut::EditorGradingKeyframe,
                jcut::UpsertGradingKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Grading,
                    currentClipLastFrame,
                    "GradeKeyframeEditor",
                    gradingInitial,
                    [](jcut::EditorGradingKeyframe* draft) {
                        ImGui::InputDouble(
                            "Brightness", &draft->brightness, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Contrast", &draft->contrast, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Saturation", &draft->saturation, 0.01, 0.1, "%.3f");
                        ImGui::InputDouble(
                            "Grade Opacity", &draft->opacity, 0.01, 0.1, "%.3f");
                        if (ImGui::TreeNode("Lift / Gamma / Gain")) {
                            const auto editToneRgb = [](
                                const char* label,
                                double* red,
                                double* green,
                                double* blue) -> bool {
                                std::array<double, 3> values{
                                    *red, *green, *blue};
                                constexpr double minimum = -2.0;
                                constexpr double maximum = 2.0;
                                if (ImGui::SliderScalarN(
                                        label,
                                        ImGuiDataType_Double,
                                        values.data(),
                                        static_cast<int>(values.size()),
                                        &minimum,
                                        &maximum,
                                        "%.3f")) {
                                    *red = values[0];
                                    *green = values[1];
                                    *blue = values[2];
                                    return true;
                                }
                                return false;
                            };
                            bool toneValuesChanged = false;
                            toneValuesChanged |= editToneRgb(
                                "Lift RGB",
                                &draft->shadowsR,
                                &draft->shadowsG,
                                &draft->shadowsB);
                            toneValuesChanged |= editToneRgb(
                                "Gamma RGB",
                                &draft->midtonesR,
                                &draft->midtonesG,
                                &draft->midtonesB);
                            toneValuesChanged |= editToneRgb(
                                "Gain RGB",
                                &draft->highlightsR,
                                &draft->highlightsG,
                                &draft->highlightsB);
                            if (toneValuesChanged && draft->curveThreePointLock) {
                                jcut::synchronizeEditorThreePointGradingCurves(draft);
                            }
                            ImGui::TreePop();
                        }
                        if (ImGui::Button("Normalize curves")) {
                            jcut::normalizeEditorGradingCurves(*draft);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Fold Luma into RGB and simplify each channel to at most 12 points");
                        }
                        if (ImGui::Checkbox(
                                "Three-point lock", &draft->curveThreePointLock) &&
                            draft->curveThreePointLock) {
                            jcut::synchronizeEditorThreePointGradingCurves(draft);
                        }
                        ImGui::Checkbox(
                            "Curve smoothing", &draft->curveSmoothingEnabled);
                        if (ImGui::TreeNode("Curves")) {
                            const auto applyLockedTone = [](
                                const std::vector<jcut::EditorPoint>& curve,
                                double* shadows,
                                double* midtones,
                                double* highlights) {
                                const jcut::EditorToneValues tones =
                                    jcut::editorToneValuesFromThreePointCurve(curve);
                                *shadows = tones.shadows;
                                *midtones = tones.midtones;
                                *highlights = tones.highlights;
                            };
                            const bool redCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Red",
                                    &draft->curvePointsR,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            const bool greenCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Green",
                                    &draft->curvePointsG,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            const bool blueCurveChanged =
                                drawGradingCurvePointEditor(
                                    "Blue",
                                    &draft->curvePointsB,
                                    draft->curveThreePointLock,
                                    draft->curveSmoothingEnabled);
                            if (draft->curveThreePointLock) {
                                if (redCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsR,
                                        &draft->shadowsR,
                                        &draft->midtonesR,
                                        &draft->highlightsR);
                                }
                                if (greenCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsG,
                                        &draft->shadowsG,
                                        &draft->midtonesG,
                                        &draft->highlightsG);
                                }
                                if (blueCurveChanged) {
                                    applyLockedTone(
                                        draft->curvePointsB,
                                        &draft->shadowsB,
                                        &draft->midtonesB,
                                        &draft->highlightsB);
                                }
                            }
                            drawGradingCurvePointEditor(
                                "Luma",
                                &draft->curvePointsLuma,
                                false,
                                draft->curveSmoothingEnabled);
                            ImGui::TreePop();
                        }
                    });
            if (ImGui::TreeNode("Auto Oppose Settings")) {
                jcut::EditorAutoOpposeSettingsCore& settings =
                    shellState->autoOpposeSettings;
                ImGui::SliderInt(
                    "Analysis Density", &settings.sampleTarget, 30, 2000);
                ImGui::SliderInt(
                    "Min Event Gap", &settings.minEventGapFrames, 1, 300);
                ImGui::SliderInt(
                    "Max Events", &settings.maxEvents, 1, 200);
                constexpr double kJumpThresholdMinimum = 0.01;
                constexpr double kJumpThresholdMaximum = 0.5;
                ImGui::SliderScalar(
                    "Luma Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpLumaThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpLumaThreshold =
                    std::clamp(settings.jumpLumaThreshold, 0.01, 0.5);
                ImGui::SliderScalar(
                    "Saturation Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpSaturationThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpSaturationThreshold =
                    std::clamp(settings.jumpSaturationThreshold, 0.01, 0.5);
                ImGui::SliderScalar(
                    "Contrast Jump Threshold",
                    ImGuiDataType_Double,
                    &settings.jumpContrastThreshold,
                    &kJumpThresholdMinimum,
                    &kJumpThresholdMaximum,
                    "%.3f");
                settings.jumpContrastThreshold =
                    std::clamp(settings.jumpContrastThreshold, 0.01, 0.5);
                constexpr double kOpposeStrengthMinimum = 0.5;
                constexpr double kOpposeStrengthMaximum = 6.0;
                ImGui::SliderScalar(
                    "Brightness Oppose Strength",
                    ImGuiDataType_Double,
                    &settings.brightnessStrength,
                    &kOpposeStrengthMinimum,
                    &kOpposeStrengthMaximum,
                    "%.2f");
                ImGui::TreePop();
            }
            ImGui::BeginDisabled(
                !currentClip || shellState->autoOpposeRunning);
            if (ImGui::Button(
                    shellState->autoOpposeRunning
                        ? "Auto Oppose: Analyzing..."
                        : "Auto Oppose")) {
                startAutoOpposeJob(shellState, snapshot, *currentClip);
            }
            ImGui::EndDisabled();
            if (ImGui::BeginTable(
                    "GradeKeys",
                    7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Bright");
                ImGui::TableSetupColumn("Contrast");
                ImGui::TableSetupColumn("Sat");
                ImGui::TableSetupColumn("Opacity");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorGradingKeyframe& keyframe : currentClip->gradingKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "grading-frame-" + std::to_string(keyframe.frame));
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Grading,
                            keyframe.frame);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.brightness);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.contrast);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.saturation);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.opacity);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "grading-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Grading,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorGradingKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertGradingKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Grading,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
}

void drawInspectorTab01(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip* currentClip,
    const jcut::EditorTrack* currentTrack,
    std::int64_t currentClipLocalFrame,
    std::int64_t currentClipLastFrame,
    std::int64_t fadeEndFrame,
    const std::string& requestedInspectorTab,
    bool focusOutput,
    bool focusProjects)
{
    const auto inspectorTabFlags = [&requestedInspectorTab](const char* label) {
        return requestedInspectorTab == label
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    };
        if (ImGui::BeginTabItem("Opacity")) {
            drawInspectorHeading("Opacity", snapshot, currentClip);
            float opacity = currentClip ? static_cast<float>(currentClip->opacity) : 1.0f;
            ImGui::BeginDisabled(!currentClip);
            const bool opacityChanged = ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            if (opacityChanged && currentClip) {
                applyCommand(shellState, jcut::SetClipOpacityCommand{currentClip->id, opacity});
            }
            const jcut::EditorOpacityKeyframe opacityInitial{
                currentClipLocalFrame, opacity, true};
            drawKeyframeDraftEditor<
                jcut::EditorOpacityKeyframe,
                jcut::UpsertOpacityKeyframeCommand>(
                    shellState,
                    currentClip,
                    jcut::EditorKeyframeChannel::Opacity,
                    currentClipLastFrame,
                    "OpacityKeyframeEditor",
                    opacityInitial,
                    [](jcut::EditorOpacityKeyframe* draft) {
                        ImGui::InputDouble(
                            "Key Opacity", &draft->opacity, 0.01, 0.1, "%.3f");
                    });
            if (ImGui::Button("Fade In From Playhead") && currentClip) {
                beginRuntimeHistoryTransaction(shellState);
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {currentClipLocalFrame, 0.0, true}});
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {fadeEndFrame, opacity, true}});
                endRuntimeHistoryTransaction(shellState);
            }
            ImGui::SameLine();
            if (ImGui::Button("Fade Out From Playhead") && currentClip) {
                beginRuntimeHistoryTransaction(shellState);
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {currentClipLocalFrame, opacity, true}});
                applyCommand(shellState, jcut::UpsertOpacityKeyframeCommand{
                    currentClip->id, {fadeEndFrame, 0.0, true}});
                endRuntimeHistoryTransaction(shellState);
            }
            if (ImGui::BeginTable(
                    "OpacityKeys",
                    4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Opacity");
                ImGui::TableSetupColumn("Interp");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                if (currentClip) {
                    for (const jcut::EditorOpacityKeyframe& keyframe : currentClip->opacityKeyframes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        drawFrameSeekCell(
                            shellState,
                            keyframe.frame,
                            static_cast<std::int64_t>(currentClip->startFrame) + keyframe.frame,
                            "opacity-frame-" + std::to_string(keyframe.frame));
                        markInspectorDeleteTargetForLastItem(
                            shellState,
                            InspectorDeleteTargetKind::ClipKeyframe,
                            currentClip->id,
                            jcut::EditorKeyframeChannel::Opacity,
                            keyframe.frame);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", keyframe.opacity);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(keyframe.linearInterpolation ? "Linear" : "Hold");
                        ImGui::TableNextColumn();
                        const std::string keyId = "opacity-" +
                            std::to_string(keyframe.frame);
                        ImGui::PushID(keyId.c_str());
                        if (ImGui::SmallButton("Load/Edit")) {
                            loadKeyframeDraft(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Opacity,
                                keyframe);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                                keyframe.linearInterpolation
                                    ? "Set Hold"
                                    : "Set Linear")) {
                            jcut::EditorOpacityKeyframe updated = keyframe;
                            updated.linearInterpolation =
                                !updated.linearInterpolation;
                            applyCommand(shellState,
                                         jcut::UpsertOpacityKeyframeCommand{
                                             currentClip->id,
                                             std::move(updated)});
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            removeInspectorKeyframe(
                                shellState,
                                currentClip->id,
                                jcut::EditorKeyframeChannel::Opacity,
                                keyframe.frame);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
}

void drawInspectorTab02(
    ShellState* shellState,
    const jcut::EditorDocumentCore& snapshot,
    const jcut::EditorClip* currentClip,
    const jcut::EditorTrack* currentTrack,
    std::int64_t currentClipLocalFrame,
    std::int64_t currentClipLastFrame,
    std::int64_t fadeEndFrame,
    const std::string& requestedInspectorTab,
    bool focusOutput,
    bool focusProjects)
{
    const auto inspectorTabFlags = [&requestedInspectorTab](const char* label) {
        return requestedInspectorTab == label
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
    };
        if (ImGui::BeginTabItem("Effects")) {
            drawInspectorHeading("Effects", snapshot, currentClip);
            std::string presetId = currentClip ? currentClip->effectPreset : "none";
            const std::string presetLabel = effectPresetDisplayName(presetId);
            bool alternate = currentClip ? currentClip->effectAlternateDirection : true;
            const std::string effectMediaKind = currentClip
                ? lowerAscii(currentClip->mediaKind)
                : std::string();
            const bool imagePresetCapable = currentClip &&
                (effectMediaKind == "image" || effectMediaKind == "video");
            ImGui::BeginDisabled(!imagePresetCapable);
            bool effectChanged = false;
            std::string edgeFillEffect =
                currentClip ? currentClip->edgeFillEffect : "none";
            int edgeFillPixels = currentClip ? currentClip->edgeFillPixels : 1;
            float edgeFillPower = currentClip
                ? static_cast<float>(currentClip->edgeFillPower) : 2.0f;
            float edgeFillOpacity = currentClip
                ? static_cast<float>(currentClip->edgeFillOpacity) : 1.0f;
            float edgeFillBrightness = currentClip
                ? static_cast<float>(currentClip->edgeFillBrightness) : 0.0f;
            float edgeFillSaturation = currentClip
                ? static_cast<float>(currentClip->edgeFillSaturation) : 1.0f;
            if (ImGui::CollapsingHeader(
                    "Edge Fill", ImGuiTreeNodeFlags_DefaultOpen)) {
                static constexpr std::array<std::pair<std::string_view, std::string_view>, 7>
                    edgeFillOptions{{
                        {"none", "None"},
                        {"edge_stretch", "Edge Stretch"},
                        {"progressive_edge_stretch", "Progressive Edge Stretch"},
                        {"progressive_bidirectional_edge_stretch",
                         "Progressive Bidirectional Edge Stretch"},
                        {"tile", "Tile"},
                        {"mirror", "Mirror"},
                        {"blur_cover", "Blur Cover"},
                    }};
                std::string_view edgeFillLabel = "None";
                for (const auto& [id, label] : edgeFillOptions) {
                    if (edgeFillEffect == id) {
                        edgeFillLabel = label;
                        break;
                    }
                }
                if (ImGui::BeginCombo("Effect", edgeFillLabel.data())) {
                    for (const auto& [id, label] : edgeFillOptions) {
                        const bool selected = edgeFillEffect == id;
                        if (ImGui::Selectable(label.data(), selected)) {
                            edgeFillEffect = id;
                            effectChanged = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                const bool edgeFillEnabled = edgeFillEffect != "none";
                const bool progressiveEdgeFill =
                    edgeFillEffect == "progressive_edge_stretch" ||
                    edgeFillEffect == "progressive_bidirectional_edge_stretch";
                ImGui::BeginDisabled(!edgeFillEnabled);
                ImGui::BeginDisabled(
                    !progressiveEdgeFill);
                effectChanged |= ImGui::SliderInt(
                    "Edge Width", &edgeFillPixels, 1, 512);
                effectChanged |= ImGui::SliderFloat(
                    "Curve Power", &edgeFillPower, 0.25f, 8.0f, "%.2f");
                ImGui::EndDisabled();
                effectChanged |= ImGui::SliderFloat(
                    "Edge Opacity", &edgeFillOpacity, 0.0f, 1.0f, "%.2f");
                effectChanged |= ImGui::SliderFloat(
                    "Edge Brightness", &edgeFillBrightness, -1.0f, 1.0f, "%.2f");
                effectChanged |= ImGui::SliderFloat(
                    "Edge Saturation", &edgeFillSaturation, 0.0f, 3.0f, "%.2f");
                ImGui::EndDisabled();
            }
            if (ImGui::BeginCombo("Preset", presetLabel.c_str())) {
                for (const std::string_view optionId : jcut::kEditorEffectPresetIds) {
                    const bool selected = presetId == optionId;
                    const std::string optionLabel = effectPresetDisplayName(optionId);
                    if (ImGui::Selectable(optionLabel.c_str(), selected)) {
                        presetId = optionId;
                        effectChanged = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            const bool edge = presetId == "sobel_edges";
            const bool neon = presetId == "neon_glow";
            const bool speakerMask = effectPresetIsSpeakerMask(presetId);
            const bool mirrorGeometry = effectPresetIsMirrorGeometry(presetId);
            const bool sectorEffect =
                presetId == "mirror_ring" || presetId == "kaleidoscope";
            const bool recursionEffect =
                presetId == "droste" || presetId == "infinite_mirror";
            const bool commonParameters =
                effectPresetUsesCommonNeutralParameters(presetId);
            const int contextualMaxRows = edge || neon
                ? 4
                : (speakerMask
                       ? 8
                       : jcut::editorEffectMaxRowsForPreset(presetId));
            int rows = std::clamp(
                currentClip ? currentClip->effectRows : 32,
                jcut::kEditorEffectMinRows,
                contextualMaxRows);
            float speed = std::clamp(
                currentClip ? static_cast<float>(currentClip->effectSpeed) : 1.0f,
                static_cast<float>(jcut::kEditorEffectMinSpeed),
                static_cast<float>(jcut::kEditorEffectMaxSpeed));
            const float contextualMaxScale = speakerMask
                ? 1.0f
                : static_cast<float>(jcut::kEditorEffectMaxScale);
            float scale = std::clamp(
                currentClip ? static_cast<float>(currentClip->effectScale) : 1.0f,
                static_cast<float>(jcut::kEditorEffectMinScale),
                contextualMaxScale);
            bool speechSync = currentClip ? currentClip->effectSkipAwareTiming : true;
            int differenceReference = currentClip ? currentClip->differenceReferenceFrames : 1;
            float differenceThreshold = currentClip
                ? static_cast<float>(currentClip->differenceThreshold) : 0.10f;
            float differenceSoftness = currentClip
                ? static_cast<float>(currentClip->differenceSoftness) : 0.05f;
            int echoCount = currentClip ? currentClip->temporalEchoCount : 4;
            int echoSpacing = currentClip ? currentClip->temporalEchoSpacingFrames : 2;
            float echoDecay = currentClip
                ? static_cast<float>(currentClip->temporalEchoDecay) : 0.65f;
            std::string tilingPattern = currentClip ? currentClip->tilingPattern : "grid";
            float tilingSpacing = currentClip
                ? static_cast<float>(currentClip->tilingSpacing) : 1.0f;
            bool tilingWrap = currentClip ? currentClip->tilingWrap : true;
            bool effectEnabled =
                currentClip ? currentClip->effectEnabled : true;
            std::string effectModulationMode = currentClip
                ? currentClip->effectModulationMode : "none";
            std::string effectModulationTarget = currentClip
                ? currentClip->effectModulationTarget : "scale";
            float effectModulationAmount = currentClip
                ? static_cast<float>(currentClip->effectModulationAmount) : 0.0f;
            float effectModulationRate = currentClip
                ? static_cast<float>(currentClip->effectModulationRate) : 1.0f;
            float effectModulationPhase = currentClip
                ? static_cast<float>(
                      currentClip->effectModulationPhaseDegrees) : 0.0f;

            if (commonParameters) {
                const char* rowsLabel = edge
                    ? "Sample Radius"
                    : (neon
                           ? "Glow Radius"
                           : (speakerMask
                                  ? "Dilation Radius"
                                  : (sectorEffect
                                         ? "Mirror Sectors"
                                         : (recursionEffect
                                                ? "Recursion Density"
                                                : (mirrorGeometry
                                                       ? "Cells Across"
                                                       : "Copies")))));
                effectChanged |= ImGui::SliderInt(
                    rowsLabel,
                    &rows,
                    jcut::kEditorEffectMinRows,
                    contextualMaxRows);
                beginRuntimeHistoryTransactionForLastItem(shellState);

                if (!edge) {
                    const char* speedLabel = neon
                        ? "Hue Speed"
                        : (speakerMask
                               ? "Color Cycle Speed"
                               : (mirrorGeometry ? "Rotation Speed" : "Speed"));
                    effectChanged |= ImGui::SliderFloat(
                        speedLabel,
                        &speed,
                        static_cast<float>(jcut::kEditorEffectMinSpeed),
                        static_cast<float>(jcut::kEditorEffectMaxSpeed),
                        "%.2f");
                    beginRuntimeHistoryTransactionForLastItem(shellState);
                }

                const char* scaleLabel = edge
                    ? "Edge Strength"
                    : (neon
                           ? "Glow Intensity"
                           : (speakerMask
                                  ? "Opacity"
                                  : (mirrorGeometry
                                         ? "Output Grain Size"
                                         : "Scale")));
                effectChanged |= ImGui::SliderFloat(
                    scaleLabel,
                    &scale,
                    static_cast<float>(jcut::kEditorEffectMinScale),
                    contextualMaxScale,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);

                if (!edge && !neon && !speakerMask && !mirrorGeometry) {
                    effectChanged |= ImGui::Checkbox(
                        "Alternate Direction", &alternate);
                }
            }
            if (presetId == "difference_matte") {
                effectChanged |= ImGui::SliderInt(
                    "Difference Reference Frames", &differenceReference, 1, 300);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Difference Threshold", &differenceThreshold, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Difference Softness", &differenceSoftness, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
            } else if (presetId == "temporal_echo") {
                effectChanged |= ImGui::SliderInt("Echo Count", &echoCount, 1, 12);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderInt("Echo Spacing", &echoSpacing, 1, 120);
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::SliderFloat(
                    "Echo Decay", &echoDecay, 0.0f, 1.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
            }
            if (effectPresetUsesTilingParameters(presetId)) {
                static constexpr std::array<std::pair<std::string_view, const char*>, 6>
                    kTilingPatterns = {{
                        {"grid", "Grid"},
                        {"encircle", "Encircle"},
                        {"spiral_xy", "Spiral XY"},
                        {"spiral_xz", "Spiral XZ"},
                        {"spiral_yz", "Spiral YZ"},
                        {"diamond", "Diamond"},
                    }};
                const char* patternLabel = "Grid";
                for (const auto& [id, label] : kTilingPatterns) {
                    if (tilingPattern == id) {
                        patternLabel = label;
                    }
                }
                if (ImGui::BeginCombo("Tiling Pattern", patternLabel)) {
                    for (const auto& [id, label] : kTilingPatterns) {
                        const bool selected = tilingPattern == id;
                        if (ImGui::Selectable(label, selected)) {
                            tilingPattern = id;
                            effectChanged = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                effectChanged |= ImGui::SliderFloat(
                    "Tiling Spacing", &tilingSpacing, 0.1f, 8.0f, "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
                effectChanged |= ImGui::Checkbox("Wrap Across Bounds", &tilingWrap);
            }
            if (effectPresetUsesSpacingParameter(presetId) &&
                !effectPresetUsesTilingParameters(presetId)) {
                effectChanged |= ImGui::SliderFloat(
                    recursionEffect
                        ? "Recursion Spacing"
                        : (mirrorGeometry ? "Geometry Amount" : "Color Spacing"),
                    &tilingSpacing,
                    0.1f,
                    8.0f,
                    "%.2f");
                beginRuntimeHistoryTransactionForLastItem(shellState);
            }
            ImGui::SeparatorText("Effect Animation");
            effectChanged |= ImGui::Checkbox(
                "Enabled before first key", &effectEnabled);
            bool enabledAtPlayhead = effectEnabled;
            bool keyAtPlayhead = false;
            if (currentClip) {
                for (const jcut::EditorBoolKeyframe& keyframe :
                     currentClip->effectEnabledKeyframes) {
                    if (keyframe.frame <= currentClipLocalFrame) {
                        enabledAtPlayhead = keyframe.enabled;
                    }
                    keyAtPlayhead |=
                        keyframe.frame == currentClipLocalFrame;
                }
            }
            ImGui::TextDisabled(
                "At frame %lld: %s",
                static_cast<long long>(currentClipLocalFrame),
                enabledAtPlayhead ? "On" : "Off");
            if (ImGui::Button("Key On") && currentClip) {
                applyCommand(
                    shellState,
                    jcut::UpsertEffectEnabledKeyframeCommand{
                        currentClip->id,
                        {currentClipLocalFrame, true}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Key Off") && currentClip) {
                applyCommand(
                    shellState,
                    jcut::UpsertEffectEnabledKeyframeCommand{
                        currentClip->id,
                        {currentClipLocalFrame, false}});
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!keyAtPlayhead);
            if (ImGui::Button("Remove Enable Key") && currentClip) {
                applyCommand(
                    shellState,
                    jcut::RemoveClipKeyframeCommand{
                        currentClip->id,
                        jcut::EditorKeyframeChannel::EffectEnabled,
                        currentClipLocalFrame});
            }
            ImGui::EndDisabled();
            static constexpr std::array<
                std::pair<std::string_view, const char*>, 3>
                kModulationModes{{
                    {"none", "None"},
                    {"lfo", "LFO"},
                    {"steady_increase", "Steady Increase"},
                }};
            const char* modulationModeLabel = "None";
            for (const auto& [id, label] : kModulationModes) {
                if (effectModulationMode == id) modulationModeLabel = label;
            }
            if (ImGui::BeginCombo(
                    "Dynamic Control", modulationModeLabel)) {
                for (const auto& [id, label] : kModulationModes) {
                    const bool selected = effectModulationMode == id;
                    if (ImGui::Selectable(label, selected)) {
                        effectModulationMode = id;
                        effectChanged = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            const bool movingPresetTranscriptTiming =
                commonParameters && !edge && !neon && !speakerMask &&
                true;
            if (effectModulationMode == "steady_increase" ||
                movingPresetTranscriptTiming) {
                effectChanged |= ImGui::Checkbox(
                    effectModulationMode == "steady_increase"
                        ? "Transcript-aware steady increase"
                        : "Synchronize motion with Speech Filter",
                    &speechSync);
                if (effectModulationMode == "steady_increase" &&
                    ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Pause the steady-increase clock across transcript "
                        "ranges removed by the Speech Filter.");
                }
            }
            static constexpr std::array<
                std::pair<std::string_view, const char*>, 4>
                kModulationTargets{{
                    {"rows", "Copies / Radius"},
                    {"speed", "Speed"},
                    {"scale", "Amount / Strength"},
                    {"spacing", "Spacing"},
                }};
            const char* modulationTargetLabel = "Amount / Strength";
            for (const auto& [id, label] : kModulationTargets) {
                if (effectModulationTarget == id) {
                    modulationTargetLabel = label;
                }
            }
            const bool modulationEnabled =
                effectModulationMode != "none";
            ImGui::BeginDisabled(!modulationEnabled);
            if (ImGui::BeginCombo(
                    "Dynamic Target", modulationTargetLabel)) {
                for (const auto& [id, label] : kModulationTargets) {
                    const bool selected = effectModulationTarget == id;
                    if (ImGui::Selectable(label, selected)) {
                        effectModulationTarget = id;
                        effectChanged = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            effectChanged |= ImGui::SliderFloat(
                effectModulationMode == "lfo"
                    ? "Amplitude"
                    : "Increase Per Second",
                &effectModulationAmount,
                -512.0f,
                512.0f,
                "%.3f");
            ImGui::BeginDisabled(effectModulationMode != "lfo");
            effectChanged |= ImGui::SliderFloat(
                "LFO Frequency (Hz)",
                &effectModulationRate,
                0.0f,
                20.0f,
                "%.3f");
            effectChanged |= ImGui::SliderFloat(
                "LFO Phase",
                &effectModulationPhase,
                -360.0f,
                360.0f,
                "%.1f deg");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (effectChanged && currentClip) {
                jcut::SetClipMaskEffectCommand command;
                command.clipId = currentClip->id;
                command.maskEnabled = currentClip->maskEnabled;
                command.feather = currentClip->maskFeather;
                command.featherGamma = currentClip->maskFeatherGamma;
                command.featherFalloff = currentClip->maskFeatherFalloff;
                command.foregroundLayerEnabled = currentClip->maskForegroundLayerEnabled;
                command.repeatEnabled = currentClip->maskRepeatEnabled;
                command.repeatDeltaX = currentClip->maskRepeatDeltaX;
                command.repeatDeltaY = currentClip->maskRepeatDeltaY;
                command.edgeFillEffect = edgeFillEffect;
                command.edgeFillPixels = edgeFillPixels;
                command.edgeFillPower = edgeFillPower;
                command.edgeFillOpacity = edgeFillOpacity;
                command.edgeFillBrightness = edgeFillBrightness;
                command.edgeFillSaturation = edgeFillSaturation;
                command.effectEnabled = effectEnabled;
                command.effectModulationMode = effectModulationMode;
                command.effectModulationTarget = effectModulationTarget;
                command.effectModulationAmount = effectModulationAmount;
                command.effectModulationRate = effectModulationRate;
                command.effectModulationPhaseDegrees =
                    effectModulationPhase;
                command.effectPreset = presetId;
                command.effectRows = rows;
                command.effectSpeed = speed;
                command.effectScale = scale;
                command.alternateDirection = alternate;
                command.skipAwareTiming = speechSync;
                command.differenceReferenceFrames = differenceReference;
                command.differenceThreshold = differenceThreshold;
                command.differenceSoftness = differenceSoftness;
                command.temporalEchoCount = echoCount;
                command.temporalEchoSpacingFrames = echoSpacing;
                command.temporalEchoDecay = echoDecay;
                command.tilingPattern = tilingPattern;
                command.tilingSpacing = tilingSpacing;
                command.tilingWrap = tilingWrap;
                applyCommand(shellState, std::move(command));
            }
            ImGui::EndDisabled();
            if (currentClip && !imagePresetCapable) {
                ImGui::TextWrapped(
                    "Effect presets require an image or video clip.");
            }
            ImGui::EndTabItem();
        }
}
