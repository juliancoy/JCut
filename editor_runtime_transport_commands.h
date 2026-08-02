#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchTransportCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, TogglePlaybackCommand>) {
                    const bool nextActive =
                        !m_document.transport.playbackActive;
                    if (nextActive) {
                        m_document.transport.currentFrame =
                            playbackStartFrame(
                                m_document, timelineEndFrame());
                    }
                    m_document.transport.playbackActive = nextActive;
                    return CommandResult{true, m_document.transport.playbackActive ? "playback started" : "playback paused"};
                } else if constexpr (std::is_same_v<T, UndoCommand>) {
                    if (m_undoStack.empty()) {
                        return CommandResult{false, "nothing to undo"};
                    }

                    const EditorTransportState transport = m_document.transport;
                    const EditorPanelState panels = m_document.panels;
                    m_redoStack.push_back(std::move(m_document));
                    m_document = std::move(m_undoStack.back());
                    m_undoStack.pop_back();
                    m_document.transport = transport;
                    m_document.panels = panels;
                    m_frameAccumulator = 0.0;
                    return CommandResult{true, "edit undone"};
                } else if constexpr (std::is_same_v<T, RedoCommand>) {
                    if (m_redoStack.empty()) {
                        return CommandResult{false, "nothing to redo"};
                    }

                    const EditorTransportState transport = m_document.transport;
                    const EditorPanelState panels = m_document.panels;
                    m_undoStack.push_back(std::move(m_document));
                    m_document = std::move(m_redoStack.back());
                    m_redoStack.pop_back();
                    m_document.transport = transport;
                    m_document.panels = panels;
                    m_frameAccumulator = 0.0;
                    return CommandResult{true, "edit redone"};
                } else if constexpr (std::is_same_v<T, SetPlaybackActiveCommand>) {
                    if (typedCommand.active &&
                        !m_document.transport.playbackActive) {
                        m_document.transport.currentFrame =
                            playbackStartFrame(
                                m_document, timelineEndFrame());
                    }
                    m_document.transport.playbackActive = typedCommand.active;
                    return CommandResult{true, typedCommand.active ? "playback started" : "playback paused"};
                } else if constexpr (std::is_same_v<T, SetPlaybackSpeedCommand>) {
                    m_document.transport.playbackSpeed =
                        std::clamp(typedCommand.speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
                    return CommandResult{true, "playback speed updated"};
                } else if constexpr (
                    std::is_same_v<T, SetPlaybackLoopEnabledCommand>) {
                    m_document.transport.playbackLoopEnabled =
                        typedCommand.enabled;
                    return CommandResult{true, typedCommand.enabled
                        ? "playback loop enabled"
                        : "playback loop disabled"};
                } else if constexpr (
                    std::is_same_v<T, SetPreviewViewModeCommand>) {
                    m_document.transport.previewViewMode =
                        typedCommand.mode == "audio" ? "audio" : "video";
                    return CommandResult{true, "preview view mode updated"};
                } else if constexpr (
                    std::is_same_v<T, SetTransportAudioCommand>) {
                    m_document.transport.audioMuted = typedCommand.muted;
                    m_document.transport.audioVolume =
                        std::clamp(typedCommand.volume, 0.0f, 1.0f);
                    return CommandResult{true, "transport audio updated"};
                } else if constexpr (std::is_same_v<T, SetPreviewZoomCommand>) {
                    m_document.transport.previewZoom =
                        jcut::normalizedEditorPreviewZoom(
                            typedCommand.zoom);
                    return CommandResult{true, "preview zoom updated"};
                } else if constexpr (std::is_same_v<T, SeekToFrameCommand>) {
                    m_document.transport.currentFrame =
                        std::clamp(typedCommand.frame, 0, timelineEndFrame());
                    m_frameAccumulator = 0.0;
                    return CommandResult{true, "playhead moved"};
                } else if constexpr (std::is_same_v<T, StepFrameCommand>) {
                    const int endFrame = timelineEndFrame();
                    const auto ranges = normalizedPlaybackRangesCore(
                        m_document.exportRanges, endFrame);
                    const int direction = typedCommand.delta < 0 ? -1 : 1;
                    for (int step = 0;
                         step < std::abs(typedCommand.delta);
                         ++step) {
                        m_document.transport.currentFrame = static_cast<int>(
                            stepPlaybackFrameCore(
                                ranges,
                                m_document.transport.currentFrame,
                                direction,
                                endFrame));
                    }
                    m_frameAccumulator = 0.0;
                    return CommandResult{true, "playhead stepped"};
                } 
    return std::nullopt;
}
