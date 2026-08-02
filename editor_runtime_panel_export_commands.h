#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchPanelExportCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, SetWaveformVisibleCommand>) {
                    m_document.panels.showWaveform = typedCommand.visible;
                    return CommandResult{true, "waveform visibility updated"};
                } else if constexpr (std::is_same_v<T, SetTranscriptVisibleCommand>) {
                    m_document.panels.showTranscript = typedCommand.visible;
                    return CommandResult{true, "transcript visibility updated"};
                } else if constexpr (
                    std::is_same_v<T, SeedTranscriptHistoryDocumentCommand> ||
                    std::is_same_v<T, SetTranscriptHistoryDocumentCommand>) {
                    if (typedCommand.path.empty() || typedCommand.jsonPayload.empty()) {
                        return CommandResult{false, "transcript history document requires path and payload"};
                    }
                    auto document = std::find_if(
                        m_document.transcriptHistoryDocuments.begin(),
                        m_document.transcriptHistoryDocuments.end(),
                        [&](const EditorDocumentCore::TranscriptHistoryDocument& candidate) {
                            return candidate.path == typedCommand.path;
                        });
                    if (document == m_document.transcriptHistoryDocuments.end()) {
                        m_document.transcriptHistoryDocuments.push_back(
                            {typedCommand.path, typedCommand.jsonPayload});
                        return CommandResult{true, "transcript history document seeded"};
                    }
                    if (document->jsonPayload == typedCommand.jsonPayload) {
                        return CommandResult{false, "transcript history document unchanged"};
                    }
                    document->jsonPayload = typedCommand.jsonPayload;
                    return CommandResult{true, "transcript history document updated"};
                } else if constexpr (std::is_same_v<T, SetScopesVisibleCommand>) {
                    m_document.panels.showScopes = typedCommand.visible;
                    return CommandResult{true, "scopes visibility updated"};
                } else if constexpr (std::is_same_v<T, SetExportSizeCommand>) {
                    m_document.exportRequest.outputSize = {
                        std::max(16, typedCommand.width),
                        std::max(16, typedCommand.height)};
                    return CommandResult{true, "export size updated"};
                } else if constexpr (std::is_same_v<T, SetExportFpsCommand>) {
                    m_document.exportRequest.outputFps = std::max(1.0, typedCommand.fps);
                    return CommandResult{true, "export fps updated"};
                } else if constexpr (std::is_same_v<T, SetExportOutputPathCommand>) {
                    m_document.exportRequest.outputPath = typedCommand.path;
                    return CommandResult{true, "export output path updated"};
                } else if constexpr (std::is_same_v<T, SetExportFormatCommand>) {
                    m_document.exportRequest.outputFormat = typedCommand.format.empty()
                        ? std::string("mp4")
                        : typedCommand.format;
                    return CommandResult{true, "export format updated"};
                } else if constexpr (std::is_same_v<T, SetExportImageSequenceFormatCommand>) {
                    m_document.exportRequest.imageSequenceFormat = typedCommand.format.empty()
                        ? std::string("jpeg")
                        : typedCommand.format;
                    return CommandResult{true, "image sequence format updated"};
                } else if constexpr (std::is_same_v<T, SetExportUseProxyMediaCommand>) {
                    m_document.exportRequest.useProxyMedia = typedCommand.enabled;
                    return CommandResult{true, "proxy export flag updated"};
                } else if constexpr (std::is_same_v<T, SetExportImageSequenceCommand>) {
                    m_document.exportRequest.createVideoFromImageSequence = typedCommand.enabled;
                    m_document.exportRequest.outputMode = typedCommand.enabled
                        ? render::RenderOutputMode::EncodedFileAndImageSequence
                        : render::RenderOutputMode::EncodedFile;
                    if (typedCommand.enabled && m_document.exportRequest.imageSequenceFormat.empty()) {
                        m_document.exportRequest.imageSequenceFormat = "jpeg";
                    }
                    return CommandResult{true, "image sequence mode updated"};
                }
    return std::nullopt;
}
