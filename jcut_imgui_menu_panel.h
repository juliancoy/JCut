#pragma once

void drawMenuBar(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        const bool projectDocumentDirty = documentIsDirty(*shellState, snapshot);
        const bool canChangeProjectIdentity =
            shellState->usesQtProjectStorage && !projectDocumentDirty;
        if (ImGui::MenuItem("Open Project")) {
            shellState->focusInspectorProjectsRequested = true;
            shellState->statusMessage = "projects panel focused";
        }
        if (ImGui::MenuItem("New Project", "Ctrl+N", false,
                            canChangeProjectIdentity)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::NewProject, snapshot);
        }
        if (ImGui::MenuItem("Import Media")) {
            shellState->focusMediaFilesRequested = true;
            shellState->statusMessage = "media browser focused";
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            saveCurrentDocument(shellState);
        }
        if (ImGui::MenuItem("Save Project As", "Ctrl+Shift+S", false,
                            shellState->usesQtProjectStorage)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::SaveAs, snapshot);
        }
        if (ImGui::MenuItem("Rename Project", nullptr, false,
                            canChangeProjectIdentity)) {
            requestProjectLifecycleAction(
                shellState, ProjectLifecycleAction::Rename, snapshot);
        }
        if (ImGui::MenuItem("Reload", "Ctrl+R", false,
                            !shellState->documentPath.empty() || shellState->usesQtProjectStorage)) {
            reloadCurrentDocument(shellState);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export")) {
            shellState->focusInspectorOutputRequested = true;
            shellState->statusMessage = "output panel focused";
        }
        ImGui::EndMenu();
    }
    bool canUndo = false;
    bool canRedo = false;
    {
        std::lock_guard<std::mutex> lock(shellState->runtimeMutex);
        canUndo = shellState->runtime.canUndo();
        canRedo = shellState->runtime.canRedo();
    }
    const jcut::EditorClip* currentClip = selectedClip(snapshot);
    const std::size_t selectionCount = selectedClipCount(snapshot);
    const bool canSplit = selectedClipsCanSplitAtFrame(
        snapshot, snapshot.transport.currentFrame);
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            applyCommand(shellState, jcut::UndoCommand{});
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, canRedo)) {
            applyCommand(shellState, jcut::RedoCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::CutSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::CopySelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Paste At Playhead", "Ctrl+V")) {
            applyCommand(shellState, jcut::PasteClipsCommand{
                snapshot.transport.currentFrame, selectedTrackId(snapshot)});
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selectionCount > 0)) {
            applyCommand(shellState, jcut::DuplicateSelectedClipsCommand{});
        }
        if (ImGui::MenuItem("Select All Clips", "Ctrl+A", false,
                            !snapshot.clips.empty())) {
            applyCommand(shellState, jcut::SelectAllClipsCommand{});
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                selectionCount > 1
                    ? "Split Selected At Playhead"
                    : "Split At Playhead",
                "Ctrl+B",
                false,
                canSplit)) {
            applyCommand(
                shellState,
                jcut::SplitSelectedClipsCommand{
                snapshot.transport.currentFrame});
        }
        if (ImGui::MenuItem(selectionCount == 1
                                ? "Delete Selected Clip"
                                : "Delete Selected Clips",
                            "Delete", false, selectionCount > 0)) {
            deleteSelectedClips(shellState);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Nudge Left", "Alt+Left", false,
                            currentClip && currentClip->startFrame > 0)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{-1});
        }
        if (ImGui::MenuItem("Nudge Right", "Alt+Right", false, currentClip != nullptr)) {
            applyCommand(shellState, jcut::NudgeSelectedClipCommand{1});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Transport")) {
        bool playing = snapshot.transport.playbackActive;
        if (ImGui::MenuItem(playing ? "Pause" : "Play", "Space", playing, true)) {
            applyCommand(shellState, jcut::TogglePlaybackCommand{});
        }
        if (ImGui::MenuItem("Previous Frame", "Left")) {
            applyCommand(shellState, jcut::StepFrameCommand{-1});
        }
        if (ImGui::MenuItem("Next Frame", "Right")) {
            applyCommand(shellState, jcut::StepFrameCommand{1});
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        bool waveform = snapshot.panels.showWaveform;
        bool transcript = snapshot.panels.showTranscript;
        bool scopes = snapshot.panels.showScopes;
        if (ImGui::MenuItem("Waveform", nullptr, waveform, true)) {
            applyCommand(shellState, jcut::SetWaveformVisibleCommand{!waveform});
        }
        if (ImGui::MenuItem("Transcript", nullptr, transcript, true)) {
            applyCommand(shellState, jcut::SetTranscriptVisibleCommand{!transcript});
        }
        if (ImGui::MenuItem("Scopes", nullptr, scopes, true)) {
            applyCommand(shellState, jcut::SetScopesVisibleCommand{!scopes});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Panel Layout")) {
            shellState->resetLayoutRequested = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const bool dirty = documentIsDirty(*shellState, snapshot);
    ImGui::Text("%s%s", snapshot.projectName.c_str(), dirty ? " *" : "");
    ImGui::EndMainMenuBar();
}
