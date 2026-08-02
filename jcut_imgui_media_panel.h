#pragma once

void drawMediaPanel(ShellState* shellState, const jcut::EditorDocumentCore& snapshot)
{
    shellState->mediaHoveredPath.clear();
    const bool focusFiles = std::exchange(shellState->focusMediaFilesRequested, false);
    const ShellLayout layout = computeShellLayout();
    const ImGuiCond layoutCondition = shellState->resetLayoutRequested
        ? ImGuiCond_Always
        : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(layout.media.pos, layoutCondition);
    ImGui::SetNextWindowSize(layout.media.size, layoutCondition);
    if (focusFiles) {
        ImGui::SetNextWindowFocus();
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Media", nullptr, flags);
    if (ImGui::BeginTabBar("MediaTabs")) {
        if (ImGui::BeginTabItem("Project")) {
            ImGui::InputText("Path", shellState->importMediaPath.data(), shellState->importMediaPath.size());
            ImGui::InputText("Label", shellState->importMediaLabel.data(), shellState->importMediaLabel.size());
            ImGui::InputText("Kind", shellState->importMediaKind.data(), shellState->importMediaKind.size());
            if (ImGui::Button("Import")) {
                applyCommand(shellState, importMediaCommandForPath(
                    shellState->importMediaPath.data(),
                    shellState->importMediaLabel.data(),
                    shellState->importMediaKind.data()));
            }
            ImGui::Separator();
            const int trackId = selectedTrackId(snapshot);
            for (const jcut::EditorMediaItem& item : snapshot.mediaItems) {
                ImGui::PushID(item.id.c_str());
                const bool selected = shellState->mediaSelectedPath == item.id;
                if (ImGui::Selectable(item.label.c_str(), selected)) {
                    shellState->mediaSelectedPath = item.id;
                }
                const bool itemHovered = ImGui::IsItemHovered();
                if (itemHovered) {
                    shellState->mediaHoveredPath = item.id;
                }
                if (itemHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (trackId > 0) {
                        std::int64_t probedDurationFrames = 0;
                        importMediaCommandForPath(
                            item.id,
                            item.label,
                            item.kind,
                            &probedDurationFrames);
                        applyCommand(shellState, jcut::InsertClipFromMediaCommand{
                            item.id,
                            trackId,
                            snapshot.transport.currentFrame,
                            resolvedMediaDurationFrames(
                                0, probedDurationFrames)});
                    } else {
                        shellState->statusMessage = "select a track before inserting media";
                    }
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(
                        kProjectMediaDragPayload,
                        item.id.c_str(),
                        item.id.size() + 1);
                    ImGui::TextUnformatted(item.label.c_str());
                    ImGui::TextDisabled("Drop on a timeline track");
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("ProjectMediaContext")) {
                    if (ImGui::MenuItem("Remove from Project")) {
                        const jcut::CommandResult result = applyCommand(
                            shellState, jcut::RemoveMediaCommand{item.id});
                        if (result.applied &&
                            shellState->mediaSelectedPath == item.id) {
                            shellState->mediaSelectedPath.clear();
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", item.kind.c_str());
                ImGui::PopID();
            }
            if (!shellState->mediaSelectedPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", shellState->mediaSelectedPath.c_str());
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Files",
                                nullptr,
                                focusFiles ? ImGuiTabItemFlags_SetSelected
                                           : ImGuiTabItemFlags_None)) {
            if (shellState->mediaRootPath[0] == '\0') {
                std::snprintf(shellState->mediaRootPath.data(),
                              shellState->mediaRootPath.size(),
                              "%s",
                              shellState->mediaRootDirectory.empty()
                                  ? (shellState->projectRootPath.empty()
                                      ? fs::current_path().string().c_str()
                                      : shellState->projectRootPath.c_str())
                                  : shellState->mediaRootDirectory.c_str());
            }
            const bool rootSubmitted = ImGui::InputText(
                "Root",
                shellState->mediaRootPath.data(),
                shellState->mediaRootPath.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (rootSubmitted || ImGui::IsItemDeactivatedAfterEdit()) {
                if (!applyMediaRootPath(shellState, shellState->mediaRootPath.data())) {
                    std::snprintf(shellState->mediaRootPath.data(),
                                  shellState->mediaRootPath.size(),
                                  "%s",
                                  shellState->mediaRootDirectory.c_str());
                }
            }
            ImGui::InputText("Filter", shellState->mediaBrowserFilter.data(), shellState->mediaBrowserFilter.size());
            if (ImGui::Button("Use Project Root")) {
                applyMediaRootPath(shellState, shellState->projectRootPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Up")) {
                fs::path current = shellState->mediaGalleryPath.empty()
                    ? fs::path(shellState->mediaRootPath.data())
                    : fs::path(shellState->mediaGalleryPath);
                if (current.has_parent_path()) {
                    const std::string parent = pathString(current.parent_path());
                    applyMediaRootPath(shellState, parent);
                }
            }
            const fs::path activeRoot = shellState->mediaGalleryPath.empty()
                ? fs::path(shellState->mediaRootPath.data())
                : fs::path(shellState->mediaGalleryPath);
            ImGui::Separator();
            ImGui::TextWrapped("%s", pathString(activeRoot).c_str());
            const float browserHeight = std::max(160.0f, ImGui::GetContentRegionAvail().y - 92.0f);
            if (ImGui::BeginChild("FileSystemBrowser", ImVec2(0.0f, browserHeight), true)) {
                for (const fs::directory_entry& entry :
                     sortedDirectoryEntries(activeRoot, shellState->mediaBrowserFilter.data())) {
                    std::error_code ec;
                    const bool isDir = entry.is_directory(ec);
                    const fs::path entryPath = entry.path();
                    const bool isSequence = isDir &&
                        jcut::isImageSequenceDirectory(entryPath);
                    const bool importable = isSequence ||
                        (!isDir && isMediaFilePath(entryPath));
                    const bool selectable = isDir || importable;
                    const std::string entryPathText = pathString(entryPath);
                    ImGui::PushID(entryPathText.c_str());
                    std::string label = isSequence
                        ? "[sequence] " + entryPath.filename().string()
                        : isDir
                        ? "[dir] " + entryPath.filename().string()
                        : "[" + mediaKindForPath(entryPath) + "] " + entryPath.filename().string();
                    if (!selectable) {
                        ImGui::BeginDisabled();
                    }
                    const bool selected = shellState->mediaSelectedPath == pathString(entryPath);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        shellState->mediaSelectedPath = entryPathText;
                    }
                    if (ImGui::IsItemHovered()) {
                        shellState->mediaHoveredPath = entryPathText;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (isDir && !isSequence) {
                            shellState->mediaGalleryPath = pathString(entryPath);
                        } else if (importable) {
                            importFilesystemMedia(shellState, snapshot, entryPath, true);
                        }
                    }
                    if (importable && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(
                            kFilesystemMediaDragPayload,
                            entryPathText.c_str(),
                            entryPathText.size() + 1);
                        ImGui::TextUnformatted(entryPath.filename().string().c_str());
                        ImGui::TextDisabled("Drop on a timeline track");
                        ImGui::EndDragDropSource();
                    }
                    if (!selectable) {
                        ImGui::EndDisabled();
                    }
                    if (ImGui::BeginPopupContextItem(
                            "FilesystemMediaContext")) {
                        const fs::path desktopPath =
                            isDir && !isSequence
                                ? entryPath
                                : entryPath.parent_path();
                        if (ImGui::MenuItem(
                                isDir && !isSequence
                                    ? "Open Folder"
                                    : "Open Containing Folder")) {
                            std::string openError;
                            if (openDesktopPath(
                                    desktopPath, &openError)) {
                                shellState->statusMessage =
                                    "opened " + pathString(desktopPath);
                            } else {
                                shellState->statusMessage =
                                    std::move(openError);
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Copy Absolute Path")) {
                            ImGui::SetClipboardText(
                                entryPathText.c_str());
                            shellState->statusMessage =
                                "absolute media path copied";
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            const std::string previewPath = !shellState->mediaHoveredPath.empty()
                ? shellState->mediaHoveredPath
                : shellState->mediaSelectedPath;
            if (!previewPath.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", previewPath.c_str());
                if (shellState->mediaThumbnailTextureId != 0 &&
                    shellState->mediaThumbnailSize.valid() &&
                    shellState->mediaThumbnailLoadedPath ==
                        previewPath) {
                    const float availableWidth =
                        std::max(
                            1.0f,
                            ImGui::GetContentRegionAvail().x);
                    const float scale = std::min(
                        1.0f,
                        std::min(
                            availableWidth /
                                static_cast<float>(
                                    shellState
                                        ->mediaThumbnailSize.width),
                            180.0f /
                                static_cast<float>(
                                    shellState
                                        ->mediaThumbnailSize.height)));
                    ImGui::Image(
                        shellState->mediaThumbnailTextureId,
                        ImVec2(
                            shellState->mediaThumbnailSize.width *
                                scale,
                            shellState->mediaThumbnailSize.height *
                                scale));
                } else if (
                    shellState->mediaThumbnailRunning &&
                    shellState->mediaThumbnailPendingPath ==
                        previewPath) {
                    ImGui::TextDisabled(
                        "Loading thumbnail...");
                } else if (
                    !shellState->mediaThumbnailError.empty() &&
                    shellState->mediaThumbnailPendingPath ==
                        previewPath) {
                    ImGui::TextDisabled(
                        "%s",
                        shellState->mediaThumbnailError.c_str());
                }
                const bool previewImportable = isImportableMediaPath(previewPath);
                if (previewImportable && ImGui::Button("Import Selected")) {
                    importFilesystemMedia(shellState, snapshot, previewPath, false);
                }
                ImGui::SameLine();
                if (previewImportable && ImGui::Button("Insert Selected")) {
                    importFilesystemMedia(shellState, snapshot, previewPath, true);
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
