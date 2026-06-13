#pragma once

#include "render_contract_types.h"

#include <string>
#include <vector>

namespace jcut {

struct EditorMediaItem {
    std::string id;
    std::string label;
    std::string kind;
};

struct EditorTrack {
    int id = 0;
    std::string label;
    bool selected = false;
};

struct EditorClip {
    int id = 0;
    int trackId = 0;
    std::string label;
    int startFrame = 0;
    int durationFrames = 0;
    bool selected = false;
    std::string sourcePath;
};

struct EditorTransportState {
    bool playbackActive = false;
    float playbackSpeed = 1.0f;
    float previewZoom = 1.0f;
    int currentFrame = 0;
};

struct EditorPanelState {
    bool showWaveform = true;
    bool showTranscript = true;
    bool showScopes = false;
};

struct EditorDocumentCore {
    std::string projectName;
    std::vector<EditorMediaItem> mediaItems;
    std::vector<EditorTrack> tracks;
    std::vector<EditorClip> clips;
    EditorTransportState transport;
    EditorPanelState panels;
    render::RenderRequestCore exportRequest;
};

} // namespace jcut
