#pragma once

void drawInspectorTab08(
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
                "Transcript", nullptr, inspectorTabFlags("Transcript"))) {
            drawInspectorHeading("Transcript", snapshot, currentClip);
            const jcut::jobs::TranscriptionJobSnapshotCore
                transcriptionJob =
                    shellState->transcriptionJob.snapshot();
            ImGui::BeginDisabled(
                !currentClip || !currentClip->hasAudio ||
                transcriptionJob.active());
            if (ImGui::Button("Transcribe with WhisperX") &&
                currentClip) {
                startTranscriptionJob(shellState, *currentClip);
            }
            ImGui::EndDisabled();
            if (!transcriptionJob.status.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "%s", transcriptionJob.status.c_str());
            }
            jcut::EditorTranscriptOverlayState overlay = currentClip
                ? currentClip->transcriptOverlay
                : jcut::EditorTranscriptOverlayState{};
            float width = static_cast<float>(overlay.boxWidth);
            float height = static_cast<float>(overlay.boxHeight);
            float translationX = static_cast<float>(overlay.translationX);
            float translationY = static_cast<float>(overlay.translationY);
            float textOpacity = static_cast<float>(overlay.textOpacity);
            float backgroundOpacity = static_cast<float>(overlay.backgroundOpacity);
            float backgroundCornerRadius = static_cast<float>(overlay.backgroundCornerRadius);
            float backgroundPadding = static_cast<float>(overlay.backgroundPadding);
            float backgroundFrameOpacity = static_cast<float>(overlay.backgroundFrameOpacity);
            float backgroundFrameWidth = static_cast<float>(overlay.backgroundFrameWidth);
            float backgroundFrameGap = static_cast<float>(overlay.backgroundFrameGap);
            float shadowOpacity = static_cast<float>(overlay.shadowOpacity);
            float shadowOffsetX = static_cast<float>(overlay.shadowOffsetX);
            float shadowOffsetY = static_cast<float>(overlay.shadowOffsetY);
            float textOutlineWidth = static_cast<float>(overlay.textOutlineWidth);
            float textOutlineOpacity = static_cast<float>(overlay.textOutlineOpacity);
            float textExtrudeDepth = static_cast<float>(overlay.textExtrudeDepth);
            float textExtrudeBevelScale = static_cast<float>(overlay.textExtrudeBevelScale);
            int fontPointSize = overlay.fontPointSize;
            std::array<char, 128> fontFamily{};
            std::snprintf(fontFamily.data(), fontFamily.size(), "%s",
                          overlay.fontFamily.c_str());
            ImGui::BeginDisabled(!currentClip);
            bool overlayChanged = false;
            overlayChanged |= ImGui::Checkbox("Enable Overlay", &overlay.enabled);
            overlayChanged |= ImGui::Checkbox("Manual Placement", &overlay.useManualPlacement);
            ImGui::BeginDisabled(!overlay.useManualPlacement);
            overlayChanged |= ImGui::SliderFloat(
                "Center X", &translationX, -1.0f, 1.0f, "%.3f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Center Y", &translationY, -1.0f, 1.0f, "%.3f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::InputInt("Max Lines", &overlay.maxLines);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::InputInt("Max Chars", &overlay.maxCharsPerLine);
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::DragFloat("Width", &width, 4.0f, 160.0f, 3840.0f, "%.0f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::DragFloat("Height", &height, 4.0f, 80.0f, 2160.0f, "%.0f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat("Text Opacity", &textOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            const bool fontFamilyChanged = ImGui::InputText(
                "Font Family", fontFamily.data(), fontFamily.size());
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= fontFamilyChanged;
            overlayChanged |= ImGui::SliderInt(
                "Font Size", &fontPointSize, 12, 256, "%d pt");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Bold", &overlay.bold);
            overlayChanged |= ImGui::Checkbox("Italic", &overlay.italic);
            overlayChanged |= editHexRgbColor(
                "Text Color", &overlay.textColor, "#ffffff");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Show Background", &overlay.showBackground);
            overlayChanged |= ImGui::SliderFloat("Background Opacity", &backgroundOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= editHexRgbColor(
                "Background Color", &overlay.backgroundColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Background Radius", &backgroundCornerRadius, 0.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Content Padding", &backgroundPadding, 0.0f, 400.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox(
                "Background Frame", &overlay.backgroundFrameEnabled);
            ImGui::BeginDisabled(!overlay.backgroundFrameEnabled);
            overlayChanged |= editHexRgbColor(
                "Frame Color", &overlay.backgroundFrameColor, "#ffffff");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Opacity", &backgroundFrameOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Width", &backgroundFrameWidth, 0.0f, 120.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Frame Gap", &backgroundFrameGap, 0.0f, 200.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Show Shadow", &overlay.showShadow);
            ImGui::BeginDisabled(!overlay.showShadow);
            overlayChanged |= editHexRgbColor(
                "Shadow Color", &overlay.shadowColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow Opacity", &shadowOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow X", &shadowOffsetX, -128.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Shadow Y", &shadowOffsetY, -128.0f, 128.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Text Outline", &overlay.textOutlineEnabled);
            ImGui::BeginDisabled(!overlay.textOutlineEnabled);
            overlayChanged |= editHexRgbColor(
                "Outline Color", &overlay.textOutlineColor, "#000000");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Outline Width", &textOutlineWidth, 0.0f, 24.0f, "%.1f px");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Outline Opacity", &textOutlineOpacity, 0.0f, 1.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            const char* extrudeLabel = overlay.textExtrudeMode == "stacked_copies"
                ? "Stacked Copies" : (overlay.textExtrudeMode == "eroded_solid"
                    ? "Eroded Solid" : "None");
            if (ImGui::BeginCombo("Text Extrusion", extrudeLabel)) {
                const std::array<std::pair<const char*, const char*>, 3> modes{{
                    {"None", "none"},
                    {"Stacked Copies", "stacked_copies"},
                    {"Eroded Solid", "eroded_solid"},
                }};
                for (const auto& [label, value] : modes) {
                    const bool selected = overlay.textExtrudeMode == value;
                    if (ImGui::Selectable(label, selected)) {
                        overlay.textExtrudeMode = value;
                        overlayChanged = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(overlay.textExtrudeMode == "none");
            overlayChanged |= ImGui::SliderFloat(
                "Extrude Depth", &textExtrudeDepth, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::SliderFloat(
                "Extrude Bevel", &textExtrudeBevelScale, 0.0f, 2.0f, "%.2f");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            ImGui::EndDisabled();
            overlayChanged |= ImGui::Checkbox("Show Speaker Title", &overlay.showSpeakerTitle);
            overlayChanged |= ImGui::Checkbox("Highlight Current Word", &overlay.highlightCurrentWord);
            overlayChanged |= editHexRgbColor(
                "Highlight Color", &overlay.highlightColor, "#fff2a8");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= editHexRgbColor(
                "Highlight Text Color", &overlay.highlightTextColor, "#181818");
            beginRuntimeHistoryTransactionForLastItem(shellState);
            overlayChanged |= ImGui::Checkbox("Auto Scroll", &overlay.autoScroll);
            if (overlayChanged && currentClip) {
                overlay.translationX = translationX;
                overlay.translationY = translationY;
                overlay.boxWidth = width;
                overlay.boxHeight = height;
                overlay.textOpacity = textOpacity;
                overlay.backgroundOpacity = backgroundOpacity;
                overlay.backgroundCornerRadius = backgroundCornerRadius;
                overlay.backgroundPadding = backgroundPadding;
                overlay.backgroundFrameOpacity = backgroundFrameOpacity;
                overlay.backgroundFrameWidth = backgroundFrameWidth;
                overlay.backgroundFrameGap = backgroundFrameGap;
                overlay.shadowOpacity = shadowOpacity;
                overlay.shadowOffsetX = shadowOffsetX;
                overlay.shadowOffsetY = shadowOffsetY;
                overlay.textOutlineWidth = textOutlineWidth;
                overlay.textOutlineOpacity = textOutlineOpacity;
                overlay.textExtrudeDepth = textExtrudeDepth;
                overlay.textExtrudeBevelScale = textExtrudeBevelScale;
                overlay.fontPointSize = fontPointSize;
                if (fontFamilyChanged) {
                    overlay.fontFamily = fontFamily.data();
                }
                applyCommand(shellState, jcut::SetClipTranscriptOverlayCommand{
                    currentClip->id, std::move(overlay)});
            }
            ImGui::EndDisabled();
            ImGui::SeparatorText("Transcript Document");
            if (!currentClip) {
                ImGui::TextWrapped("Select an audio clip to inspect its transcript.");
            } else if (currentClip->mediaKind != "audio" && !currentClip->hasAudio) {
                ImGui::TextWrapped("The selected clip has no detected audio stream.");
            } else {
                ensureTranscriptInspectorCache(shellState, snapshot, *currentClip);
                TranscriptInspectorCache& cache = shellState->transcriptCache;
                const jcut::TranscriptCutSession& transcript = cache.session;

                std::string activeLabel = "No transcript";
                for (const jcut::TranscriptCutEntry& cut : transcript.catalog.cuts) {
                    if (cut.path == transcript.activePath) {
                        activeLabel = cut.label;
                        break;
                    }
                }
                ImGui::BeginDisabled(transcript.catalog.cuts.empty());
                if (ImGui::BeginCombo("Cut", activeLabel.c_str())) {
                    for (const jcut::TranscriptCutEntry& cut : transcript.catalog.cuts) {
                        const bool selected = cut.path == transcript.activePath;
                        if (ImGui::Selectable(cut.label.c_str(), selected)) {
                            applyCommand(
                                shellState,
                                jcut::SetClipTranscriptActiveCutCommand{
                                    currentClip->id, cut.path});
                            cache.requestedPath = cut.path;
                            cache.refreshRequested = true;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (cache.cutLabelPath != transcript.activePath) {
                    cache.cutLabelPath = transcript.activePath;
                    cache.cutLabelDraft = activeLabel;
                }
                ImGui::BeginDisabled(!transcript.ok());
                if (ImGui::Button("New Cut")) {
                    std::string error;
                    const std::optional<std::string> newPath =
                        jcut::createTranscriptCutVersion(transcript, &error);
                    if (newPath) {
                        applyCommand(
                            shellState,
                            jcut::SetClipTranscriptActiveCutCommand{
                                currentClip->id, *newPath});
                        cache.requestedPath = *newPath;
                        cache.refreshRequested = true;
                    } else {
                        cache.mutationError = std::move(error);
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    std::max(120.0f, ImGui::GetContentRegionAvail().x - 190.0f));
                ImGui::InputText("Cut Label", &cache.cutLabelDraft);
                ImGui::SameLine();
                ImGui::BeginDisabled(!transcript.activeCutMutable);
                if (ImGui::Button("Rename")) {
                    std::string error;
                    if (jcut::renameTranscriptCut(
                            transcript, cache.cutLabelDraft, &error)) {
                        cache.refreshRequested = true;
                    } else if (!error.empty()) {
                        cache.mutationError = std::move(error);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Cut")) {
                    ImGui::OpenPopup("Confirm Delete Transcript Cut");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal(
                        "Confirm Delete Transcript Cut", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped(
                        "Permanently delete \"%s\"? This cannot be undone.",
                        activeLabel.c_str());
                    if (ImGui::Button("Delete")) {
                        std::string fallback;
                        std::string error;
                        if (jcut::deleteTranscriptCut(
                                transcript, &fallback, &error)) {
                            applyCommand(
                                shellState,
                                jcut::SetClipTranscriptActiveCutCommand{
                                    currentClip->id, fallback});
                            cache.requestedPath = fallback;
                            cache.refreshRequested = true;
                            ImGui::CloseCurrentPopup();
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                bool showOutsideCut = legacyBoolValue(
                    *shellState, "transcriptShowExcludedLines", false);
                if (ImGui::Checkbox("Show Outside Cut", &showOutsideCut)) {
                    setLegacyStateOverride(
                        shellState,
                        "transcriptShowExcludedLines",
                        showOutsideCut);
                    cache.refreshRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh Transcript")) {
                    cache.refreshRequested = true;
                }
                ImGui::TextDisabled(transcript.activeCutMutable
                    ? "Select a row to edit its text, source timing, or skip state."
                    : "Original transcript is immutable; select an editable cut to edit rows.");
                if (!transcript.activePath.empty()) {
                    ImGui::TextWrapped("Active: %s", transcript.activePath.c_str());
                }
                if (!transcript.warning.empty()) {
                    ImGui::TextColored(
                        ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                        "%s",
                        transcript.warning.c_str());
                }
                if (!transcript.error.empty()) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                        "%s",
                        transcript.error.c_str());
                } else {
                    std::vector<std::string> transcriptSpeakers;
                    for (const jcut::TranscriptRow& row : transcript.rows) {
                        if (!row.gap && !row.speakerId.empty()) {
                            transcriptSpeakers.push_back(row.speakerId);
                        }
                    }
                    std::sort(transcriptSpeakers.begin(), transcriptSpeakers.end());
                    transcriptSpeakers.erase(
                        std::unique(transcriptSpeakers.begin(), transcriptSpeakers.end()),
                        transcriptSpeakers.end());
                    const std::string speakerFilterLabel = cache.speakerFilter.empty()
                        ? "All Speakers" : cache.speakerFilter;
                    if (ImGui::BeginCombo("Speaker Filter", speakerFilterLabel.c_str())) {
                        if (ImGui::Selectable("All Speakers", cache.speakerFilter.empty())) {
                            cache.speakerFilter.clear();
                        }
                        for (const std::string& speaker : transcriptSpeakers) {
                            if (ImGui::Selectable(
                                    speaker.c_str(), cache.speakerFilter == speaker)) {
                                cache.speakerFilter = speaker;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::InputTextWithHint(
                        "Search Transcript", "Filter word text...", &cache.searchFilter);
                    const std::string normalizedSearch = lowerAscii(cache.searchFilter);
                    std::vector<const jcut::TranscriptRow*> visibleTranscriptRows;
                    visibleTranscriptRows.reserve(transcript.rows.size());
                    for (const jcut::TranscriptRow& row : transcript.rows) {
                        if (!cache.speakerFilter.empty() && !row.gap &&
                            row.speakerId != cache.speakerFilter) {
                            continue;
                        }
                        if (!normalizedSearch.empty() &&
                            (row.gap ||
                             lowerAscii(row.text).find(normalizedSearch) ==
                                 std::string::npos)) {
                            continue;
                        }
                        visibleTranscriptRows.push_back(&row);
                    }
                    const ImGuiTableFlags transcriptTableFlags =
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                        ImGuiTableFlags_ScrollY;
                    if (ImGui::BeginTable(
                            "TranscriptRows",
                            6,
                            transcriptTableFlags,
                            ImVec2(0.0f, 280.0f))) {
                        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                        ImGui::TableSetupColumn("Render", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                        ImGui::TableSetupColumn("Speaker", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 180.0f);
                        ImGui::TableSetupColumn("Edits", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(visibleTranscriptRows.size()));
                        while (clipper.Step()) {
                            for (int rowIndex = clipper.DisplayStart;
                                 rowIndex < clipper.DisplayEnd;
                                 ++rowIndex) {
                                const jcut::TranscriptRow& row =
                                    *visibleTranscriptRows[static_cast<std::size_t>(rowIndex)];
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                const std::string sourceTime =
                                    jcut::formatTranscriptTime(row.rawStartSeconds);
                                const bool rowSelected = cache.selectionDraftValid &&
                                    cache.selectedWord.segmentIndex == row.word.segmentIndex &&
                                    cache.selectedWord.wordIndex == row.word.wordIndex;
                                ImGui::PushID(rowIndex);
                                if (ImGui::Selectable(sourceTime.c_str(), rowSelected,
                                                      ImGuiSelectableFlags_SpanAllColumns |
                                                          ImGuiSelectableFlags_AllowOverlap) &&
                                    transcript.activeCutMutable) {
                                    selectTranscriptWordDraft(&cache, row);
                                }
                                markTranscriptDeleteTargetForLastItem(
                                    shellState,
                                    currentClip->id,
                                    transcript,
                                    row);
                                ImGui::TableNextColumn();
                                ImGui::Text("%lld-%lld",
                                            static_cast<long long>(row.sourceStartFrame),
                                            static_cast<long long>(row.sourceEndFrame));
                                ImGui::TableNextColumn();
                                if (row.renderStartFrame < 0 || row.renderEndFrame < 0) {
                                    ImGui::TextUnformatted("Outside");
                                } else {
                                    ImGui::Text("%lld-%lld",
                                                static_cast<long long>(row.renderStartFrame),
                                                static_cast<long long>(row.renderEndFrame));
                                }
                                ImGui::TableNextColumn();
                                const std::string& speaker = row.speakerLabel.empty()
                                    ? row.speakerId
                                    : row.speakerLabel;
                                ImGui::TextUnformatted(speaker.c_str());
                                if (!speaker.empty() && ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", speaker.c_str());
                                }
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(row.text.c_str());
                                if (!row.text.empty() && ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", row.text.c_str());
                                }
                                ImGui::TableNextColumn();
                                const std::string edits = transcriptRowEditLabels(row);
                                ImGui::TextUnformatted(edits.c_str());
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndTable();
                    }
                    bool canUndoGlobally = false;
                    bool canRedoGlobally = false;
                    {
                        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
                        canUndoGlobally = shellState->runtime.canUndo();
                        canRedoGlobally = shellState->runtime.canRedo();
                    }
                    ImGui::BeginDisabled(!canUndoGlobally);
                    if (ImGui::Button("Undo Last Edit")) {
                        applyCommand(shellState, jcut::UndoCommand{});
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!canRedoGlobally);
                    if (ImGui::Button("Redo Last Edit")) {
                        applyCommand(shellState, jcut::RedoCommand{});
                    }
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!transcript.activeCutMutable ||
                                         !cache.selectionDraftValid);
                    ImGui::SeparatorText("Selected Word");
                    ImGui::InputDouble("Source Start (s)", &cache.selectedStartSeconds,
                                       0.01, 0.1, "%.3f");
                    ImGui::InputDouble("Source End (s)", &cache.selectedEndSeconds,
                                       0.01, 0.1, "%.3f");
                    ImGui::InputTextMultiline("Text", &cache.selectedText,
                                              ImVec2(-FLT_MIN, 64.0f));
                    ImGui::Checkbox("Skipped", &cache.selectedSkipped);
                    if (ImGui::Button("Save Word") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        jcut::TranscriptWordPatch patch;
                        patch.startSeconds = cache.selectedStartSeconds;
                        patch.endSeconds = cache.selectedEndSeconds;
                        patch.text = cache.selectedText;
                        patch.skipped = cache.selectedSkipped;
                        std::string error;
                        if (jcut::patchTranscriptWord(
                                &root, cache.selectedWord, patch, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Add Above") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::insertTranscriptWord(
                                &root, cache.selectedWord, true, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Add Below") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::insertTranscriptWord(
                                &root, cache.selectedWord, false, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else {
                            cache.mutationError = std::move(error);
                        }
                    }
                    if (ImGui::Button("Expand Timing") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::expandTranscriptWordTiming(
                                &root, cache.selectedWord, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(
                        cache.selectedWord.originalSegmentIndex < 0 ||
                        cache.selectedWord.originalWordIndex < 0 ||
                        !transcript.originalDocument);
                    if (ImGui::Button("Restore Original") &&
                        transcript.activeDocument && transcript.originalDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::restoreTranscriptWord(
                                &root,
                                cache.selectedWord,
                                transcript.originalDocument->root(),
                                &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Move Up") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::moveTranscriptWordRenderOrder(
                                &root, cache.selectedWord, -1, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Move Down") && transcript.activeDocument) {
                        nlohmann::json root = transcript.activeDocument->root();
                        std::string error;
                        if (jcut::moveTranscriptWordRenderOrder(
                                &root, cache.selectedWord, 1, &error)) {
                            saveTranscriptMutation(shellState, &cache, std::move(root));
                        } else if (!error.empty()) {
                            cache.mutationError = std::move(error);
                        }
                    }
                    if (ImGui::Button("Delete Word")) {
                        ImGui::OpenPopup("Confirm Delete Transcript Word");
                    }
                    ImGui::EndDisabled();
                    if (std::exchange(
                            shellState->transcriptDeletePopupRequested,
                            false)) {
                        ImGui::OpenPopup(
                            "Confirm Delete Transcript Word");
                    }
                    if (ImGui::BeginPopupModal("Confirm Delete Transcript Word", nullptr,
                                               ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextWrapped("Delete the selected word from this editable cut?");
                        if (ImGui::Button("Delete") && transcript.activeDocument) {
                            nlohmann::json root = transcript.activeDocument->root();
                            std::string error;
                            if (jcut::deleteTranscriptWord(
                                    &root, cache.selectedWord, &error)) {
                                saveTranscriptMutation(shellState, &cache, std::move(root));
                                ImGui::CloseCurrentPopup();
                            } else {
                                cache.mutationError = std::move(error);
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    if (!cache.mutationError.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.38f, 1.0f),
                                           "%s", cache.mutationError.c_str());
                    }
                }
            }
            ImGui::EndTabItem();
        }
}
