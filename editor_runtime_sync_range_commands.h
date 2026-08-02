#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchSyncRangeCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, AddRenderSyncMarkerCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    EditorRenderSyncMarker marker;
                    const std::string clipPersistentId = clip->persistentId.empty()
                        ? persistentClipIdForNumericId(clip->id)
                        : clip->persistentId;
                    marker.clipId = editorRenderSyncOwnerClipId(
                        m_document, clipPersistentId);
                    if (marker.clipId.empty()) {
                        return CommandResult{false, "render sync marker owner not found"};
                    }
                    marker.frame = std::max<std::int64_t>(0, typedCommand.frame);
                    marker.skipFrame = typedCommand.skipFrame;
                    marker.count = std::clamp(
                        typedCommand.count,
                        kEditorRenderSyncMinCount,
                        kEditorRenderSyncMaxCount);
                    const auto existing = std::find_if(
                        m_document.renderSyncMarkers.begin(), m_document.renderSyncMarkers.end(),
                        [&](const EditorRenderSyncMarker& value) {
                            return value.clipId == marker.clipId &&
                                   value.frame == marker.frame;
                        });
                    if (existing == m_document.renderSyncMarkers.end()) {
                        m_document.renderSyncMarkers.push_back(std::move(marker));
                    } else {
                        *existing = std::move(marker);
                    }
                    std::sort(m_document.renderSyncMarkers.begin(),
                              m_document.renderSyncMarkers.end(),
                              renderSyncMarkerLess);
                    m_document.exportRequest.renderSyncMarkerCount =
                        m_document.renderSyncMarkers.size();
                    return CommandResult{true, "render sync marker updated"};
                } else if constexpr (std::is_same_v<T, RemoveRenderSyncMarkerCommand>) {
                    const std::string ownerClipId = editorRenderSyncOwnerClipId(
                        m_document, typedCommand.clipId);
                    const auto marker = std::find_if(
                        m_document.renderSyncMarkers.begin(),
                        m_document.renderSyncMarkers.end(),
                        [&](const EditorRenderSyncMarker& value) {
                            return value.clipId == ownerClipId &&
                                value.frame == typedCommand.frame &&
                                value.skipFrame == typedCommand.skipFrame;
                        });
                    if (marker == m_document.renderSyncMarkers.end()) {
                        return CommandResult{false, "render sync marker not found"};
                    }
                    m_document.renderSyncMarkers.erase(marker);
                    m_document.exportRequest.renderSyncMarkerCount =
                        m_document.renderSyncMarkers.size();
                    return CommandResult{true, "render sync marker removed"};
                } else if constexpr (std::is_same_v<T, ClearRenderSyncMarkersCommand>) {
                    const auto oldSize = m_document.renderSyncMarkers.size();
                    if (typedCommand.clipId == 0) {
                        m_document.renderSyncMarkers.clear();
                    } else {
                        const EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                        if (!clip) {
                            return CommandResult{false, "clip not found"};
                        }
                        const std::string clipPersistentId = clip->persistentId.empty()
                            ? persistentClipIdForNumericId(clip->id)
                            : clip->persistentId;
                        const std::string persistentId = editorRenderSyncOwnerClipId(
                            m_document, clipPersistentId);
                        m_document.renderSyncMarkers.erase(
                            std::remove_if(m_document.renderSyncMarkers.begin(),
                                           m_document.renderSyncMarkers.end(),
                                           [&](const EditorRenderSyncMarker& marker) {
                                               return marker.clipId == persistentId;
                                           }),
                            m_document.renderSyncMarkers.end());
                    }
                    if (m_document.renderSyncMarkers.size() == oldSize) {
                        return CommandResult{false, "no render sync markers"};
                    }
                    m_document.exportRequest.renderSyncMarkerCount =
                        m_document.renderSyncMarkers.size();
                    return CommandResult{true, "render sync markers cleared"};
                } else if constexpr (std::is_same_v<T, SetExportRangeCommand>) {
                    if (typedCommand.startFrame < 0 ||
                        typedCommand.endFrame < typedCommand.startFrame) {
                        return CommandResult{false, "invalid export range"};
                    }
                    m_document.exportRanges = {{typedCommand.startFrame, typedCommand.endFrame}};
                    synchronizeExportRequestRanges(&m_document);
                    return CommandResult{true, "export range updated"};
                } else if constexpr (
                    std::is_same_v<T, SetExportRangesCommand>) {
                    if (typedCommand.ranges.empty()) {
                        return CommandResult{false, "export ranges are empty"};
                    }
                    std::vector<export_range::Range> ranges =
                        sharedExportRanges(typedCommand.ranges);
                    export_range::normalize(
                        &ranges,
                        qtTimelineExtentFrame(m_document));
                    std::vector<EditorExportRange> next;
                    storeSharedExportRanges(ranges, &next);
                    if (next == m_document.exportRanges) {
                        return CommandResult{false, "export ranges unchanged"};
                    }
                    m_document.exportRanges = std::move(next);
                    synchronizeExportRequestRanges(&m_document);
                    return CommandResult{true, "export ranges updated"};
                } else if constexpr (std::is_same_v<T, EditExportRangesCommand>) {
                    const std::int64_t extent =
                        qtTimelineExtentFrame(m_document);
                    std::vector<export_range::Range> ranges =
                        sharedExportRanges(m_document.exportRanges);
                    if (!export_range::apply(
                            &ranges,
                            extent,
                            typedCommand.edit,
                            typedCommand.frame)) {
                        return CommandResult{
                            false,
                            "export range cannot split at playhead"};
                    }
                    storeSharedExportRanges(
                        ranges, &m_document.exportRanges);
                    synchronizeExportRequestRanges(&m_document);
                    return CommandResult{true, "export ranges updated"};
                } 
    return std::nullopt;
}
