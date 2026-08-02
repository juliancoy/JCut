#pragma once

void handleKeyboardShortcuts(ShellState* shellState,
                             const jcut::EditorDocumentCore& snapshot)
{
    const auto repeatingShortcut = [](ImGuiKey key, ImGuiKeyChord modifiers) {
        return ImGui::GetIO().KeyMods == modifiers && ImGui::IsKeyPressed(key, true);
    };

    const bool increaseFont =
        repeatingShortcut(ImGuiKey_Equal, ImGuiMod_Ctrl) ||
        repeatingShortcut(ImGuiKey_Equal, ImGuiMod_Ctrl | ImGuiMod_Shift) ||
        repeatingShortcut(ImGuiKey_KeypadAdd, ImGuiMod_Ctrl);
    const bool decreaseFont =
        repeatingShortcut(ImGuiKey_Minus, ImGuiMod_Ctrl) ||
        repeatingShortcut(ImGuiKey_Minus, ImGuiMod_Ctrl | ImGuiMod_Shift) ||
        repeatingShortcut(ImGuiKey_KeypadSubtract, ImGuiMod_Ctrl);
    if (increaseFont) {
        changeUiFontSize(shellState, 1.0f);
    } else if (decreaseFont) {
        changeUiFontSize(shellState, -1.0f);
    }

    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
        return;
    }

    if (ImGui::IsKeyChordPressed(
            ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        requestProjectLifecycleAction(
            shellState, ProjectLifecycleAction::SaveAs, snapshot);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        saveCurrentDocument(shellState);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
        requestProjectLifecycleAction(
            shellState, ProjectLifecycleAction::NewProject, snapshot);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) {
        reloadCurrentDocument(shellState);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        applyCommand(shellState, jcut::UndoCommand{});
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
               ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        applyCommand(shellState, jcut::RedoCommand{});
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        applyCommand(shellState, jcut::SelectAllClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
        applyCommand(shellState, jcut::CopySelectedClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) {
        applyCommand(shellState, jcut::CutSelectedClipsCommand{});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        applyCommand(shellState, jcut::PasteClipsCommand{
            snapshot.transport.currentFrame, selectedTrackId(snapshot)});
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
        if (selectedClipsCanSplitAtFrame(
                snapshot, snapshot.transport.currentFrame)) {
            applyCommand(
                shellState,
                jcut::SplitSelectedClipsCommand{
                snapshot.transport.currentFrame});
        } else {
            shellState->statusMessage =
                "split unavailable: no selected clip intersects playhead";
        }
    }
    if (ImGui::GetIO().KeyMods == ImGuiMod_None &&
        ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        shellState->timelineToolMode =
            shellState->timelineToolMode == TimelineToolMode::Razor
                ? TimelineToolMode::Select
                : TimelineToolMode::Razor;
        shellState->statusMessage = shellState->timelineToolMode == TimelineToolMode::Razor
            ? "razor tool enabled"
            : "select tool enabled";
    }
    if (ImGui::GetIO().KeyMods == ImGuiMod_None &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        shellState->timelineToolMode != TimelineToolMode::Select) {
        shellState->timelineToolMode = TimelineToolMode::Select;
        shellState->statusMessage = "select tool enabled";
    }
    const bool deletePressed =
        ImGui::IsKeyChordPressed(ImGuiKey_Delete);
    const bool rowBackspacePressed =
        ImGui::IsKeyChordPressed(ImGuiKey_Backspace);
    if (deletePressed || rowBackspacePressed) {
        InspectorDeleteTarget& target =
            shellState->inspectorDeleteTarget;
        const bool focusedInspectorTarget =
            target.kind != InspectorDeleteTargetKind::None &&
            target.documentGeneration == shellState->documentGeneration &&
            target.focusedUiFrame + 1 == shellState->uiFrameCounter;
        if (focusedInspectorTarget) {
            if (target.kind ==
                InspectorDeleteTargetKind::TitleKeyframe) {
                applyCommand(
                    shellState,
                    jcut::RemoveTitleKeyframeCommand{
                        target.clipId, target.frame});
            } else if (
                target.kind ==
                InspectorDeleteTargetKind::SyncMarker) {
                applyCommand(
                    shellState,
                    jcut::RemoveRenderSyncMarkerCommand{
                        target.markerClipId,
                        target.frame,
                        target.markerSkipFrame});
            } else if (
                target.kind ==
                InspectorDeleteTargetKind::TranscriptWord) {
                const TranscriptInspectorCache& cache =
                    shellState->transcriptCache;
                if (cache.clipId == target.clipId &&
                    cache.session.activeCutMutable &&
                    cache.session.activePath ==
                        target.transcriptPath &&
                    cache.selectionDraftValid &&
                    cache.selectedWord.segmentIndex ==
                        target.transcriptSegmentIndex &&
                    cache.selectedWord.wordIndex ==
                        target.transcriptWordIndex) {
                    shellState->transcriptDeletePopupRequested =
                        true;
                } else {
                    shellState->statusMessage =
                        "transcript delete canceled after selection change";
                }
            } else {
                removeInspectorKeyframe(
                    shellState,
                    target.clipId,
                    target.channel,
                    target.frame);
            }
            target = {};
        } else if (deletePressed) {
            // Qt reserves unmodified Backspace for focused row/table removal;
            // only Delete falls through to timeline clip deletion.
            deleteSelectedClips(shellState);
        }
    }
    if (repeatingShortcut(ImGuiKey_LeftArrow, ImGuiMod_Alt)) {
        applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
    }
    if (repeatingShortcut(ImGuiKey_RightArrow, ImGuiMod_Alt)) {
        applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
    }
    if (ImGui::IsKeyChordPressed(ImGuiKey_Space)) {
        applyCommand(shellState, jcut::TogglePlaybackCommand{});
    }
    if (repeatingShortcut(ImGuiKey_LeftArrow, ImGuiMod_None)) {
        applyCommand(shellState, jcut::StepFrameCommand{-1});
    }
    if (repeatingShortcut(ImGuiKey_RightArrow, ImGuiMod_None)) {
        applyCommand(shellState, jcut::StepFrameCommand{1});
    }
}

