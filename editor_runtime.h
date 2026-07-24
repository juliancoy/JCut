#pragma once

#include "editor_document_core.h"
#include "export_range_core.h"
#include "playback_range_core.h"
#include "speaker_framing_core.h"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace jcut {

struct CommandResult {
    bool applied = false;
    std::string message;
};

struct TickParams {
    double deltaSeconds = 0.0;
};

struct TogglePlaybackCommand {};
struct UndoCommand {};
struct RedoCommand {};
struct SetPlaybackActiveCommand { bool active = false; };
struct SetPlaybackSpeedCommand { float speed = 1.0f; };
struct SetPlaybackLoopEnabledCommand { bool enabled = false; };
struct SetPreviewViewModeCommand { std::string mode = "video"; };
struct SetTransportAudioCommand {
    bool muted = false;
    float volume = 0.8f;
};
struct SetPreviewZoomCommand { float zoom = 1.0f; };
struct SeekToFrameCommand { int frame = 0; };
struct StepFrameCommand { int delta = 0; };
struct SetProjectNameCommand { std::string name; };
struct ImportMediaCommand {
    std::string sourcePath;
    std::string label;
    std::string mediaKind;
    bool audioPresenceKnown = false;
    bool hasAudio = false;
};
// Removes only the project-bin entry. Timeline clips are never cascaded: a
// referenced media item must first be removed from every clip that uses it.
struct RemoveMediaCommand { std::string mediaId; };
struct AddTrackCommand {
    std::string label;
    // Negative appends. A non-negative value inserts directly at the clamped
    // final row position, which lets a file drop create an ordinary lane in
    // front of a generated child without targeting that derived lane.
    int insertionIndex = -1;
};
struct DeleteTrackCommand { int trackId = 0; };
// targetIndex is the final zero-based position. Values outside the current
// track range are clamped to the first or last position.
struct ReorderTrackCommand { int trackId = 0; int targetIndex = 0; };
// Applies Qt's per-clip edge fades to consecutive clips on an ordinary track.
// When moveClips is true, later clips are cascaded into an exact sample-domain
// overlap; false preserves their existing timeline positions.
struct CrossfadeTrackCommand {
    int trackId = 0;
    double seconds = 0.5;
    bool moveClips = true;
};
struct SelectTrackCommand { int trackId = 0; };
struct SelectClipCommand {
    int clipId = 0;
    bool additive = false;
    bool toggle = false;
};
struct CopySelectedClipsCommand {};
struct CutSelectedClipsCommand {};
struct PasteClipsCommand {
    int targetFrame = 0;
    int targetTrackId = 0;
};
struct DuplicateSelectedClipsCommand {};
struct SelectAllClipsCommand {};
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
    bool audioPresenceKnown = false;
    bool hasAudio = false;
};
inline constexpr int kEditorDefaultTitleDurationFrames = 90;
// Creates a visual-only title asset without adding a synthetic media-bin
// entry. Like the Qt timeline action, creation always targets an exact
// unnumbered Titles track first, reuses another free Titles* lane on overlap,
// or creates the next numbered Titles lane as part of the same undoable edit.
struct CreateTitleClipCommand {
    int startFrame = 0;
    int durationFrames = kEditorDefaultTitleDurationFrames;
};
struct ReplaceSpeakerTitleClipsCommand {
    int sourceClipId = 0;
    std::vector<EditorClip> generatedClips;
};
struct DeleteClipCommand { int clipId = 0; };
struct DeleteSelectedClipsCommand {};
struct SplitClipCommand {
    int clipId = 0;
    int frame = 0;
};
// Razor-style split: every selected, unlocked clip intersecting frame is split
// in one undoable edit. Ineligible selected clips are skipped.
struct SplitSelectedClipsCommand { int frame = 0; };
struct TrimClipStartCommand {
    int clipId = 0;
    int startFrame = 0;
};
struct TrimClipEndCommand {
    int clipId = 0;
    int endFrame = 0;
};
struct SetClipLabelCommand { int clipId = 0; std::string label; };
struct SetClipProxyCommand {
    int clipId = 0;
    std::string proxyPath;
    bool useProxy = true;
};
struct EditorClipMetadataUpdate {
    int clipId = 0;
    std::string mediaKind;
    bool hasAudio = false;
    double sourceFps = 30.0;
    std::int64_t sourceDurationFrames = 0;
    int durationFrames = 1;
};
struct RefreshClipMetadataCommand {
    std::vector<EditorClipMetadataUpdate> updates;
};
struct SetClipLockedCommand { int clipId = 0; bool locked = false; };
struct SetSelectedClipsLockedCommand { bool locked = false; };
// Matches the Qt timeline's clip-speed action: changing rate rescales the
// clip's timeline duration and ripples later clips on the same track.
struct SetClipPlaybackRateCommand {
    int clipId = 0;
    double playbackRate = 1.0;
};
struct MoveClipCommand { int clipId = 0; int trackId = 0; int startFrame = 0; };
struct MoveSelectedClipsCommand {
    int anchorClipId = 0;
    int targetTrackId = 0;
    int startFrame = 0;
};
struct ResizeClipCommand { int clipId = 0; int durationFrames = 0; };
struct NudgeSelectedClipCommand { int deltaFrames = 0; };
struct SetClipGradingCommand {
    int clipId = 0;
    double brightness = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    bool previewEnabled = true;
};
// Matches the Qt timeline context action: restore neutral base grade and
// opacity, discard both animation channels, and preserve every unrelated clip
// property (including the grading-preview toggle).
struct ResetClipGradingCommand { int clipId = 0; };
struct UpsertGradingKeyframeCommand {
    int clipId = 0;
    EditorGradingKeyframe keyframe;
};
struct SetClipOpacityCommand { int clipId = 0; double opacity = 1.0; };
struct UpsertOpacityKeyframeCommand {
    int clipId = 0;
    EditorOpacityKeyframe keyframe;
};
enum class EditorKeyframeChannel {
    Grading,
    Opacity,
    Transform,
    SpeakerFramingEnabled,
    SpeakerFraming,
    SpeakerFramingTarget,
};
struct RemoveClipKeyframeCommand {
    int clipId = 0;
    EditorKeyframeChannel channel = EditorKeyframeChannel::Transform;
    std::int64_t frame = 0;
};
struct SetClipTransformCommand {
    int clipId = 0;
    double translationX = 0.0;
    double translationY = 0.0;
    double rotation = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};
struct SetClipSourceTransformLockedCommand {
    int clipId = 0;
    bool locked = false;
};
struct SetClipSpeakerFramingCommand {
    int clipId = 0;
    bool enabled = false;
    double bakedTargetXNorm = 0.5;
    double bakedTargetYNorm = 0.35;
    double bakedTargetBoxNorm = -1.0;
    double minConfidence = 0.08;
    int manualTrackId = -1;
    std::string manualStreamId;
    int centerSmoothingFrames = 0;
    int zoomSmoothingFrames = 0;
    int smoothingMode = 0;
    double centerSmoothingStrength = 1.0;
    double zoomSmoothingStrength = 1.0;
    int gapHoldFrames = 0;
};
struct SetClipSpeakerSectionMinimumWordsCommand {
    int clipId = 0;
    int minimumWords = 10;
};
struct UpsertSpeakerFramingEnabledKeyframeCommand {
    int clipId = 0;
    EditorBoolKeyframe keyframe;
};
struct UpsertSpeakerFramingKeyframeCommand {
    int clipId = 0;
    EditorTransformKeyframe keyframe;
};
struct UpsertSpeakerFramingTargetKeyframeCommand {
    int clipId = 0;
    EditorTransformKeyframe keyframe;
};
struct UpsertTransformKeyframeCommand {
    int clipId = 0;
    EditorTransformKeyframe keyframe;
};
// Commits a Program-monitor transform expressed in evaluated/output space.
// The runtime converts it to the stored base-relative keyframe representation,
// matching the Qt preview edit helper.
struct CommitPreviewTransformCommand {
    int clipId = 0;
    std::int64_t localFrame = 0;
    double translationX = 0.0;
    double translationY = 0.0;
    double rotation = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};
struct SetClipMaskEffectCommand {
    int clipId = 0;
    bool maskEnabled = false;
    double feather = 0.0;
    double featherGamma = 1.0;
    int featherFalloff = 0;
    bool foregroundLayerEnabled = false;
    bool repeatEnabled = false;
    double repeatDeltaX = 160.0;
    double repeatDeltaY = 0.0;
    std::string effectPreset = "none";
    int effectRows = 32;
    double effectSpeed = 1.0;
    double effectScale = 1.0;
    bool alternateDirection = true;
    bool skipAwareTiming = true;
    int differenceReferenceFrames = 1;
    double differenceThreshold = 0.10;
    double differenceSoftness = 0.05;
    int temporalEchoCount = 4;
    int temporalEchoSpacingFrames = 2;
    double temporalEchoDecay = 0.65;
    std::string tilingPattern = "grid";
    double tilingSpacing = 1.0;
    bool tilingWrap = true;
    bool edgeFillEnabled = false;
    bool edgeFillProgressive = false;
    int edgeFillPixels = 1;
    double edgeFillPower = 2.0;
    double edgeFillOpacity = 1.0;
    double edgeFillBrightness = 0.0;
    double edgeFillSaturation = 1.0;
};
// Updates only mask presentation fields. Keeping this separate from effect
// normalization prevents a mask-only edit from rewriting a forward-compatible
// effect preset that this build does not understand yet.
struct SetClipMaskCommand {
    int clipId = 0;
    bool maskEnabled = false;
    double feather = 0.0;
    double featherGamma = 1.0;
    int featherFalloff = 0;
    bool foregroundLayerEnabled = false;
    bool repeatEnabled = false;
    double repeatDeltaX = 160.0;
    double repeatDeltaY = 0.0;
    double dilate = 0.0;
    double erode = 0.0;
    double blur = 0.0;
    bool invert = false;
    bool showOnly = false;
    double opacity = 1.0;
    bool gradeEnabled = false;
    double gradeBrightness = 0.0;
    double gradeContrast = 1.0;
    double gradeSaturation = 1.0;
    std::vector<EditorPoint> gradeCurvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> gradeCurvePointsG = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> gradeCurvePointsB = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> gradeCurvePointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    bool gradeCurveSmoothingEnabled = true;
    bool dropShadowEnabled = false;
    double dropShadowRadius = 12.0;
    double dropShadowOffsetX = 0.0;
    double dropShadowOffsetY = 4.0;
    double dropShadowOpacity = 0.45;
};
struct MaterializeMaskMatteCommand {
    int sourceClipId = 0;
    std::string sidecarDirectory;
    std::string sidecarId;
    std::string sidecarLabel;
};
struct SetClipZLevelCommand {
    int clipId = 0;
    int zLevel = 0;
    bool automatic = false;
};
struct SetClipTranscriptOverlayCommand {
    int clipId = 0;
    EditorTranscriptOverlayState overlay;
};
struct SetClipTranscriptActiveCutCommand {
    int clipId = 0;
    std::string transcriptPath;
};
struct UpsertTitleKeyframeCommand {
    int clipId = 0;
    EditorTitleKeyframe keyframe;
};
struct RemoveTitleKeyframeCommand {
    int clipId = 0;
    std::int64_t frame = 0;
};
struct SetClipCorrectionPolygonsCommand {
    int clipId = 0;
    std::vector<EditorCorrectionPolygon> polygons;
};
struct ClearCorrectionPolygonsCommand { int clipId = 0; };
struct SetCorrectionsEnabledCommand { bool enabled = true; };
struct SetClipAudioCommand {
    int clipId = 0;
    bool enabled = true;
    double gain = 1.0;
    double pan = 0.0;
    bool solo = false;
};
struct SetAudioDynamicsCommand {
    audio::DynamicsSettingsCore settings;
};
struct SetAudioTreatmentCommand {
    EditorAudioTreatment treatment = EditorAudioTreatment::PreservePitch;
};
struct SetTrackPropertiesCommand {
    int trackId = 0;
    std::string label;
    int height = kEditorTrackDefaultHeight;
};
struct SetTrackStateCommand {
    int trackId = 0;
    int visualMode = 0;
    bool audioEnabled = true;
    double audioGain = 1.0;
    bool audioMuted = false;
    bool audioSolo = false;
    bool gradingPreviewEnabled = true;
};
struct AddRenderSyncMarkerCommand {
    int clipId = 0;
    std::int64_t frame = 0;
    bool skipFrame = false;
    int count = 1;
};
struct RemoveRenderSyncMarkerCommand {
    std::string clipId;
    std::int64_t frame = 0;
    bool skipFrame = false;
};
struct ClearRenderSyncMarkersCommand { int clipId = 0; };
struct SetExportRangeCommand {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
};
struct SetExportRangesCommand {
    std::vector<EditorExportRange> ranges;
};
using ExportRangeEdit = export_range::Edit;
struct EditExportRangesCommand {
    ExportRangeEdit edit = ExportRangeEdit::Reset;
    std::int64_t frame = 0;
};
struct SetWaveformVisibleCommand { bool visible = true; };
struct SetTranscriptVisibleCommand { bool visible = true; };
struct SeedTranscriptHistoryDocumentCommand {
    std::string path;
    std::string jsonPayload;
};
struct SetTranscriptHistoryDocumentCommand {
    std::string path;
    std::string jsonPayload;
};
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
    UndoCommand,
    RedoCommand,
    SetPlaybackActiveCommand,
    SetPlaybackSpeedCommand,
    SetPlaybackLoopEnabledCommand,
    SetPreviewViewModeCommand,
    SetTransportAudioCommand,
    SetPreviewZoomCommand,
    SeekToFrameCommand,
    StepFrameCommand,
    SetProjectNameCommand,
    ImportMediaCommand,
    RemoveMediaCommand,
    AddTrackCommand,
    DeleteTrackCommand,
    ReorderTrackCommand,
    CrossfadeTrackCommand,
    SelectTrackCommand,
    SelectClipCommand,
    CopySelectedClipsCommand,
    CutSelectedClipsCommand,
    PasteClipsCommand,
    DuplicateSelectedClipsCommand,
    SelectAllClipsCommand,
    InsertClipFromMediaCommand,
    AddClipCommand,
    CreateTitleClipCommand,
    ReplaceSpeakerTitleClipsCommand,
    DeleteClipCommand,
    DeleteSelectedClipsCommand,
    SplitClipCommand,
    SplitSelectedClipsCommand,
    TrimClipStartCommand,
    TrimClipEndCommand,
    SetClipLabelCommand,
    SetClipProxyCommand,
    RefreshClipMetadataCommand,
    SetClipLockedCommand,
    SetSelectedClipsLockedCommand,
    SetClipPlaybackRateCommand,
    MoveClipCommand,
    MoveSelectedClipsCommand,
    ResizeClipCommand,
    NudgeSelectedClipCommand,
    SetClipGradingCommand,
    ResetClipGradingCommand,
    UpsertGradingKeyframeCommand,
    SetClipOpacityCommand,
    UpsertOpacityKeyframeCommand,
    RemoveClipKeyframeCommand,
    SetClipTransformCommand,
    SetClipSourceTransformLockedCommand,
    SetClipSpeakerFramingCommand,
    SetClipSpeakerSectionMinimumWordsCommand,
    UpsertSpeakerFramingEnabledKeyframeCommand,
    UpsertSpeakerFramingKeyframeCommand,
    UpsertSpeakerFramingTargetKeyframeCommand,
    UpsertTransformKeyframeCommand,
    CommitPreviewTransformCommand,
    SetClipMaskEffectCommand,
    SetClipMaskCommand,
    MaterializeMaskMatteCommand,
    SetClipZLevelCommand,
    SetClipTranscriptOverlayCommand,
    SetClipTranscriptActiveCutCommand,
    UpsertTitleKeyframeCommand,
    RemoveTitleKeyframeCommand,
    SetClipCorrectionPolygonsCommand,
    ClearCorrectionPolygonsCommand,
    SetCorrectionsEnabledCommand,
    SetClipAudioCommand,
    SetAudioDynamicsCommand,
    SetAudioTreatmentCommand,
    SetTrackPropertiesCommand,
    SetTrackStateCommand,
    AddRenderSyncMarkerCommand,
    RemoveRenderSyncMarkerCommand,
    ClearRenderSyncMarkersCommand,
    SetExportRangeCommand,
    SetExportRangesCommand,
    EditExportRangesCommand,
    SetWaveformVisibleCommand,
    SetTranscriptVisibleCommand,
    SeedTranscriptHistoryDocumentCommand,
    SetTranscriptHistoryDocumentCommand,
    SetScopesVisibleCommand,
    SetExportSizeCommand,
    SetExportFpsCommand,
    SetExportOutputPathCommand,
    SetExportFormatCommand,
    SetExportImageSequenceFormatCommand,
    SetExportUseProxyMediaCommand,
    SetExportImageSequenceCommand>;

// Rebuilds the derived one-child-per-lane Mask Matte presentation while
// preserving the independently mutable state of valid existing child lanes.
// Malformed foreign occupants are recovered onto neutral base lanes rather
// than discarded.
void reconcileEditorGeneratedChildTracks(EditorDocumentCore* document);

// Returns the preferred track index when the proposed clip does not overlap a
// clip in the same visual/audio lane category, otherwise the first compatible
// existing track. Returns -1 when a new track is required.
[[nodiscard]] int firstNonConflictingTrackIndex(
    const EditorDocumentCore& document,
    int preferredTrackIndex,
    const std::string& mediaKind,
    int startFrame,
    int durationFrames);

// Evaluates the complete neutral grade at a clip-local frame. Opacity follows
// its independently keyed channel, matching the render-time Qt policy.
[[nodiscard]] EditorGradingKeyframe evaluateEditorClipGradingAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame);

// Evaluates the complete neutral clip transform at a local frame. Keyframes
// are offsets composed with the clip's base transform, matching the Qt model.
[[nodiscard]] EditorTransformKeyframe evaluateEditorClipTransformAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame);

// Resolves a source-transform chain by persistent clip identity and evaluates
// the terminal source at the same timeline position. Missing links and cycles
// fail safely to the requested clip's own transform.
[[nodiscard]] EditorTransformKeyframe
evaluateEditorClipRenderTransformAtTimelineFrame(
    const EditorDocumentCore& document,
    const EditorClip& clip,
    std::int64_t timelineFrame);

// Evaluates durable/baked Qt speaker framing without transcript or UI
// dependencies. `applied` is true only when framing is enabled at the frame
// and baked transform keys are available.
[[nodiscard]] EditorTransformKeyframe
evaluateEditorClipBakedSpeakerFramingAtLocalFrame(
    const EditorClip& clip,
    double localFrame,
    int sourceWidth,
    int sourceHeight,
    int outputWidth,
    int outputHeight,
    bool* applied = nullptr);
EditorTransformKeyframe
evaluateEditorClipSpeakerFramingForFaceBoxAtLocalFrame(
    const EditorClip& clip,
    double localFrame,
    double locationXNorm,
    double locationYNorm,
    double boxSizeNorm,
    double rotationDegrees,
    int sourceWidth,
    int sourceHeight,
    int outputWidth,
    int outputHeight,
    bool* applied = nullptr);

// Evaluates title keyframes without depending on either UI toolkit. Discrete
// text/font/style fields follow the preceding keyframe; numeric fields honor
// Linear/Hold interpolation.
[[nodiscard]] EditorTitleKeyframe evaluateEditorClipTitleAtLocalFrame(
    const EditorClip& clip,
    std::int64_t localFrame);

class EditorRuntime {
public:
    static EditorRuntime createDemo();
    static EditorRuntime fromDocument(EditorDocumentCore document);

    [[nodiscard]] EditorDocumentCore snapshot() const;
    [[nodiscard]] CommandResult execute(const EditorCommand& command);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] std::size_t undoDepth() const;
    [[nodiscard]] std::size_t redoDepth() const;
    void clearHistory();
    void beginHistoryTransaction();
    void endHistoryTransaction();
    void tick(const TickParams& params);

private:
    static constexpr std::size_t kMaxHistoryEntries = 200;

    struct ClipboardClip {
        EditorClip clip;
        std::size_t trackOffset = 0;
    };

    [[nodiscard]] int timelineEndFrame() const;
    [[nodiscard]] bool copySelectedClipsToClipboard();
    [[nodiscard]] CommandResult pasteClipboardAt(int targetFrame,
                                                 int targetTrackId);
    void recordUndoSnapshot(EditorDocumentCore document);

    EditorDocumentCore m_document;
    std::vector<ClipboardClip> m_clipClipboard;
    std::vector<EditorRenderSyncMarker> m_renderSyncMarkerClipboard;
    int m_clipboardBaseTrackId = 0;
    std::vector<EditorDocumentCore> m_undoStack;
    std::vector<EditorDocumentCore> m_redoStack;
    EditorDocumentCore m_historyTransactionSnapshot;
    bool m_historyTransactionActive = false;
    bool m_historyTransactionHasChanges = false;
    double m_frameAccumulator = 0.0;
};

} // namespace jcut
