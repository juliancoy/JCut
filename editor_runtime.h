#pragma once

#include "editor_document_core.h"

#include <string>
#include <variant>

namespace jcut {

struct CommandResult {
    bool applied = false;
    std::string message;
};

struct TickParams {
    double deltaSeconds = 0.0;
};

struct TogglePlaybackCommand {};
struct SetPlaybackActiveCommand { bool active = false; };
struct SetPlaybackSpeedCommand { float speed = 1.0f; };
struct SetPreviewZoomCommand { float zoom = 1.0f; };
struct SeekToFrameCommand { int frame = 0; };
struct StepFrameCommand { int delta = 0; };
struct SetProjectNameCommand { std::string name; };
struct ImportMediaCommand {
    std::string sourcePath;
    std::string label;
    std::string mediaKind;
};
struct AddTrackCommand { std::string label; };
struct DeleteTrackCommand { int trackId = 0; };
struct SelectTrackCommand { int trackId = 0; };
struct SelectClipCommand { int clipId = 0; };
struct InsertClipFromMediaCommand {
    std::string mediaId;
    int trackId = 0;
    int startFrame = 0;
    int durationFrames = 90;
};
struct AddClipCommand {
    int trackId = 0;
    std::string label;
    int startFrame = 0;
    int durationFrames = 90;
    std::string sourcePath;
    std::string mediaKind;
};
struct DeleteClipCommand { int clipId = 0; };
struct SplitClipCommand {
    int clipId = 0;
    int frame = 0;
};
struct TrimClipStartCommand {
    int clipId = 0;
    int startFrame = 0;
};
struct TrimClipEndCommand {
    int clipId = 0;
    int endFrame = 0;
};
struct SetClipLabelCommand { int clipId = 0; std::string label; };
struct MoveClipCommand { int clipId = 0; int trackId = 0; int startFrame = 0; };
struct ResizeClipCommand { int clipId = 0; int durationFrames = 0; };
struct SetWaveformVisibleCommand { bool visible = true; };
struct SetTranscriptVisibleCommand { bool visible = true; };
struct SetScopesVisibleCommand { bool visible = true; };
struct SetExportSizeCommand { int width = 1080; int height = 1920; };
struct SetExportFpsCommand { double fps = 30.0; };
struct SetExportOutputPathCommand { std::string path; };
struct SetExportFormatCommand { std::string format; };
struct SetExportImageSequenceFormatCommand { std::string format; };
struct SetExportUseProxyMediaCommand { bool enabled = false; };
struct SetExportImageSequenceCommand { bool enabled = false; };

using EditorCommand = std::variant<
    TogglePlaybackCommand,
    SetPlaybackActiveCommand,
    SetPlaybackSpeedCommand,
    SetPreviewZoomCommand,
    SeekToFrameCommand,
    StepFrameCommand,
    SetProjectNameCommand,
    ImportMediaCommand,
    AddTrackCommand,
    DeleteTrackCommand,
    SelectTrackCommand,
    SelectClipCommand,
    InsertClipFromMediaCommand,
    AddClipCommand,
    DeleteClipCommand,
    SplitClipCommand,
    TrimClipStartCommand,
    TrimClipEndCommand,
    SetClipLabelCommand,
    MoveClipCommand,
    ResizeClipCommand,
    SetWaveformVisibleCommand,
    SetTranscriptVisibleCommand,
    SetScopesVisibleCommand,
    SetExportSizeCommand,
    SetExportFpsCommand,
    SetExportOutputPathCommand,
    SetExportFormatCommand,
    SetExportImageSequenceFormatCommand,
    SetExportUseProxyMediaCommand,
    SetExportImageSequenceCommand>;

class EditorRuntime {
public:
    static EditorRuntime createDemo();
    static EditorRuntime fromDocument(EditorDocumentCore document);

    [[nodiscard]] EditorDocumentCore snapshot() const;
    [[nodiscard]] CommandResult execute(const EditorCommand& command);
    void tick(const TickParams& params);

private:
    [[nodiscard]] int timelineEndFrame() const;

    EditorDocumentCore m_document;
    double m_frameAccumulator = 0.0;
};

} // namespace jcut
