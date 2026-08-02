#pragma once

template <typename T>
std::optional<CommandResult> EditorRuntime::dispatchAudioTrackCommand(
    const T& typedCommand)
{
    if constexpr (std::is_same_v<T, ClearCorrectionPolygonsCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    if (clip->correctionPolygons.empty()) {
                        return CommandResult{false, "no correction polygons"};
                    }
                    clip->correctionPolygons.clear();
                    return CommandResult{true, "correction polygons cleared"};
                } else if constexpr (std::is_same_v<T, SetCorrectionsEnabledCommand>) {
                    m_document.exportRequest.correctionsEnabled = typedCommand.enabled;
                    return CommandResult{true, "corrections visibility updated"};
                } else if constexpr (std::is_same_v<T, SetClipAudioCommand>) {
                    EditorClip* clip = findClip(&m_document.clips, typedCommand.clipId);
                    if (!clip) {
                        return CommandResult{false, "clip not found"};
                    }
                    clip->audioEnabled = typedCommand.enabled;
                    clip->audioGain = std::clamp(typedCommand.gain, 0.0, 8.0);
                    clip->audioPan = std::clamp(typedCommand.pan, -1.0, 1.0);
                    clip->audioSolo = typedCommand.solo;
                    return CommandResult{true, "clip audio updated"};
                } else if constexpr (std::is_same_v<T, SetAudioDynamicsCommand>) {
                    const audio::DynamicsSettingsCore normalized =
                        audio::normalizedDynamicsSettingsCore(
                            typedCommand.settings);
                    if (m_document.audioDynamics == normalized) {
                        return CommandResult{false, "audio dynamics unchanged"};
                    }
                    m_document.audioDynamics = normalized;
                    return CommandResult{true, "audio dynamics updated"};
                } else if constexpr (std::is_same_v<T, SetAudioTreatmentCommand>) {
                    if (m_document.audioTreatment == typedCommand.treatment) {
                        return CommandResult{false, "audio treatment unchanged"};
                    }
                    m_document.audioTreatment = typedCommand.treatment;
                    return CommandResult{true, "audio treatment updated"};
                } else if constexpr (std::is_same_v<T, SetTrackPropertiesCommand>) {
                    const std::size_t trackIndex =
                        trackIndexForId(m_document.tracks, typedCommand.trackId);
                    if (trackIndex == m_document.tracks.size()) {
                        return CommandResult{false, "track not found"};
                    }
                    EditorTrack& track = m_document.tracks[trackIndex];
                    if (!isGeneratedEditorChildTrack(track)) {
                        track.label = trimmed(typedCommand.label);
                        if (track.label.empty()) {
                            track.label = std::string("Track ") +
                                std::to_string(trackIndex + 1);
                        }
                    }
                    track.height = std::clamp(
                        typedCommand.height,
                        kEditorTrackMinHeight,
                        isGeneratedEditorChildTrack(track)
                            ? 56
                            : kEditorTrackMaxHeight);
                    return CommandResult{true, "track properties updated"};
                } else if constexpr (std::is_same_v<T, SetTrackStateCommand>) {
                    EditorTrack* track = findTrack(&m_document.tracks, typedCommand.trackId);
                    if (!track) {
                        return CommandResult{false, "track not found"};
                    }
                    track->visualMode = std::clamp(typedCommand.visualMode, 0, 2);
                    track->gradingPreviewEnabled =
                        typedCommand.gradingPreviewEnabled;
                    track->audioGain = std::clamp(typedCommand.audioGain, 0.0, 8.0);
                    if (isGeneratedEditorChildTrack(*track)) {
                        track->audioEnabled = false;
                        track->audioMuted = false;
                        track->audioSolo = false;
                        track->audioWaveformVisible = false;
                    } else {
                        track->audioEnabled = typedCommand.audioEnabled;
                        track->audioMuted = typedCommand.audioMuted;
                        track->audioSolo = typedCommand.audioSolo;
                    }
                    return CommandResult{true, "track state updated"};
                } 
    return std::nullopt;
}
