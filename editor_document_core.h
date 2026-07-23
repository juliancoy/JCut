#pragma once

#include "audio_dynamics_core.h"
#include "render_contract_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace jcut {

enum class EditorAudioTreatment {
    Disabled,
    Varispeed,
    PreservePitch,
    RubberBand,
    HarmonicSpeechIsolation,
};

inline constexpr std::string_view editorAudioTreatmentId(
    EditorAudioTreatment treatment)
{
    switch (treatment) {
    case EditorAudioTreatment::Varispeed:
        return "varispeed";
    case EditorAudioTreatment::PreservePitch:
        return "time_stretch";
    case EditorAudioTreatment::RubberBand:
        return "rubber_band";
    case EditorAudioTreatment::HarmonicSpeechIsolation:
        return "rubber_band_pass_through_frequency";
    case EditorAudioTreatment::Disabled:
    default:
        return "disabled";
    }
}

inline EditorAudioTreatment editorAudioTreatmentFromId(
    std::string_view value)
{
    if (value == "varispeed") return EditorAudioTreatment::Varispeed;
    if (value == "time_stretch" || value == "time-stretch") {
        return EditorAudioTreatment::PreservePitch;
    }
    if (value == "rubber_band" || value == "rubber-band" ||
        value == "rubberband" || value == "rubber_band_100" ||
        value == "rubberband_100" || value == "rubber-band-100") {
        return EditorAudioTreatment::RubberBand;
    }
    if (value == "rubber_band_pass_through_frequency" ||
        value == "rubber-band-pass-through-frequency" ||
        value == "rubberband_pass_through_frequency" ||
        value == "rubber_band_50" || value == "rubber-band-50" ||
        value == "rubberband_50") {
        return EditorAudioTreatment::HarmonicSpeechIsolation;
    }
    return EditorAudioTreatment::Disabled;
}

// Canonical persisted IDs shared with the Qt preset serializer. Commands still
// preserve unknown IDs so newer projects remain forward-compatible.
inline constexpr std::array<std::string_view, 35> kEditorEffectPresetIds = {
    "none",
    "mirror_ring",
    "kaleidoscope",
    "quad_mirror",
    "infinite_mirror",
    "tessellation",
    "hexagonal_prism",
    "droste",
    "polar_tunnel",
    "tiny_planet",
    "twirl_vortex",
    "ripple_shockwave",
    "displacement_map",
    "glass_refraction",
    "progressive_edge_stretch",
    "temporal_echo",
    "slit_scan",
    "freeze_pattern",
    "step_repeat",
    "source_tile",
    "news_logo_ticker",
    "directional_trim_ticker",
    "alternating_motion_background",
    "person_orbit",
    "pixel_sorting",
    "datamosh_glitch",
    "rgb_split",
    "halftone_mosaic",
    "vulkan_3d_synth",
    "sobel_edges",
    "neon_glow",
    "speaker_mask_dilation",
    "speaker_mask_dilation_pulse",
    "speaker_mask_dilation_rings",
    "difference_matte",
};

inline constexpr int kEditorEffectMinRows = 1;
inline constexpr int kEditorEffectDefaultMaxRows = 96;
inline constexpr int kEditorEffectProgressiveEdgeMaxRows = 512;
inline constexpr double kEditorEffectMinSpeed = -8.0;
inline constexpr double kEditorEffectMaxSpeed = 8.0;
inline constexpr double kEditorEffectMinScale = 0.1;
inline constexpr double kEditorEffectMaxScale = 8.0;

inline constexpr int editorEffectMaxRowsForPreset(std::string_view presetId)
{
    return presetId == "progressive_edge_stretch"
        ? kEditorEffectProgressiveEdgeMaxRows
        : kEditorEffectDefaultMaxRows;
}

inline bool mediaKindMayContainAudio(const std::string& mediaKind,
                                     const std::string& sourcePath = {})
{
    const auto equalsAsciiCaseInsensitive = [&mediaKind](const char* expected) {
        std::size_t index = 0;
        for (; expected[index] != '\0'; ++index) {
            if (index >= mediaKind.size()) {
                return false;
            }
            const char value = mediaKind[index] >= 'A' && mediaKind[index] <= 'Z'
                ? static_cast<char>(mediaKind[index] - 'A' + 'a')
                : mediaKind[index];
            if (value != expected[index]) {
                return false;
            }
        }
        return index == mediaKind.size();
    };
    if (equalsAsciiCaseInsensitive("audio") ||
        equalsAsciiCaseInsensitive("video")) {
        return true;
    }
    if (equalsAsciiCaseInsensitive("image") ||
        equalsAsciiCaseInsensitive("title") ||
        equalsAsciiCaseInsensitive("graphics")) {
        return false;
    }

    const std::size_t dot = sourcePath.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = sourcePath.substr(dot);
    for (char& value : extension) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return extension == ".wav" || extension == ".mp3" ||
        extension == ".flac" || extension == ".aac" ||
        extension == ".m4a" || extension == ".ogg" ||
        extension == ".mp4" || extension == ".mov" ||
        extension == ".mkv" || extension == ".webm" ||
        extension == ".avi" || extension == ".m4v";
}

struct EditorMediaItem {
    std::string id;
    std::string label;
    std::string kind;
    bool audioPresenceKnown = false;
    bool hasAudio = false;
};

struct EditorTransformKeyframe {
    std::int64_t frame = 0;
    std::string title;
    double translationX = 0.0;
    double translationY = 0.0;
    double rotation = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    bool linearInterpolation = true;
};

struct EditorBoolKeyframe {
    std::int64_t frame = 0;
    bool enabled = false;
};

struct EditorPoint {
    double x = 0.0;
    double y = 0.0;
};

struct EditorGradingKeyframe {
    std::int64_t frame = 0;
    double brightness = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    double opacity = 1.0;
    bool linearInterpolation = true;
    // Keep the original six aggregate fields above stable. These fields mirror
    // every grading-key property currently persisted by the Qt editor so a
    // neutral edit can distinguish preservation from an intentional reset.
    double shadowsR = 0.0;
    double shadowsG = 0.0;
    double shadowsB = 0.0;
    double midtonesR = 0.0;
    double midtonesG = 0.0;
    double midtonesB = 0.0;
    double highlightsR = 0.0;
    double highlightsG = 0.0;
    double highlightsB = 0.0;
    std::vector<EditorPoint> curvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> curvePointsG = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> curvePointsB = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> curvePointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    bool curveThreePointLock = false;
    bool curveSmoothingEnabled = true;
};

struct EditorOpacityKeyframe {
    std::int64_t frame = 0;
    double opacity = 1.0;
    bool linearInterpolation = true;
};

struct EditorTitleKeyframe {
    std::int64_t frame = 0;
    std::string text;
    double translationX = 0.0;
    double translationY = 0.0;
    double fontSize = 48.0;
    double opacity = 1.0;
    std::string fontFamily = "DejaVu Sans";
    bool bold = true;
    bool italic = false;
    std::string color = "#ffffff";
    bool linearInterpolation = true;
    bool autoFitToOutput = false;
    std::string logoPath;
    // Canonical material values: solid, neon, diagonal_stripes, grid,
    // and image_pattern.
    std::string textMaterialStyle = "solid";
    std::string textPatternImagePath;
    double textPatternScale = 1.0;
    bool dropShadowEnabled = true;
    std::string dropShadowColor = "#ff000000";
    double dropShadowOpacity = 0.6;
    double dropShadowOffsetX = 2.0;
    double dropShadowOffsetY = 2.0;
    bool windowEnabled = false;
    std::string windowColor = "#ff000000";
    double windowOpacity = 0.35;
    double windowPadding = 16.0;
    double windowWidth = 0.0;
    bool windowFrameEnabled = false;
    std::string windowFrameColor = "#ffffffff";
    double windowFrameOpacity = 1.0;
    double windowFrameWidth = 2.0;
    double windowFrameGap = 4.0;
    std::string windowFrameMaterialStyle = "solid";
    std::string windowFramePatternImagePath;
    double windowFramePatternScale = 1.0;
    bool vulkan3DEnabled = false;
    bool vulkan3DExtrudeEnabled = false;
    // Canonical extrusion values: none, stacked_copies, eroded_solid.
    std::string textExtrudeMode = "none";
    double vulkan3DExtrudeDepth = 0.0;
    double vulkan3DBevelScale = 0.0;
    double vulkan3DYawDegrees = 0.0;
    double vulkan3DPitchDegrees = 0.0;
    double vulkan3DRollDegrees = 0.0;
    double vulkan3DDepth = 0.0;
    double vulkan3DScale = 1.0;
};

struct EditorCorrectionPolygon {
    std::vector<EditorPoint> pointsNormalized;
    bool enabled = true;
    std::int64_t startFrame = 0;
    std::int64_t endFrame = -1;
};

struct EditorTranscriptOverlayState {
    bool enabled = false;
    bool showBackground = true;
    double backgroundOpacity = 120.0 / 255.0;
    double backgroundCornerRadius = 14.0;
    double backgroundPadding = 16.0;
    bool backgroundFrameEnabled = false;
    std::string backgroundFrameColor = "#ffffffff";
    double backgroundFrameOpacity = 1.0;
    double backgroundFrameWidth = 2.0;
    double backgroundFrameGap = 4.0;
    bool showShadow = true;
    std::string shadowColor = "#ff000000";
    double shadowOpacity = 0.78;
    double shadowOffsetX = 5.0;
    double shadowOffsetY = 5.0;
    bool textOutlineEnabled = false;
    double textOutlineWidth = 0.0;
    std::string textOutlineColor = "#ff000000";
    double textOutlineOpacity = 0.80;
    // Canonical values mirror the Qt TextExtrudeMode storage names:
    // none, stacked_copies, and eroded_solid.
    std::string textExtrudeMode = "none";
    double textExtrudeDepth = 0.16;
    double textExtrudeBevelScale = 0.7;
    bool showSpeakerTitle = false;
    bool highlightCurrentWord = true;
    bool autoScroll = false;
    bool useManualPlacement = false;
    double translationX = 0.0;
    double translationY = 0.0;
    double boxWidth = 900.0;
    double boxHeight = 220.0;
    int maxLines = 2;
    int maxCharsPerLine = 28;
    std::string fontFamily = "DejaVu Sans";
    int fontPointSize = 42;
    bool bold = true;
    bool italic = false;
    std::string textColor = "#ffffff";
    double textOpacity = 1.0;
    std::string backgroundColor = "#000000";
    std::string highlightColor = "#fff2a8";
    std::string highlightTextColor = "#181818";
};

inline constexpr int kEditorTrackDefaultHeight = 72;
inline constexpr int kEditorTrackMinHeight = 28;
inline constexpr int kEditorTrackMaxHeight = 480;
inline constexpr int kEditorRenderSyncMinCount = 1;
inline constexpr int kEditorRenderSyncMaxCount = 120;

struct EditorTrack {
    int id = 0;
    std::string label;
    bool selected = false;
    int height = kEditorTrackDefaultHeight;
    int visualMode = 0;
    bool gradingPreviewEnabled = true;
    bool audioEnabled = true;
    std::string audioBusId;
    double audioGain = 1.0;
    bool audioMuted = false;
    bool audioSolo = false;
    bool audioWaveformVisible = true;
    // Generated Mask Matte lanes are derived presentation bindings. Their
    // visibility, grading-preview state, and bounded height are persisted,
    // while their identity, order, label, audio state, and lifecycle follow
    // the child clip relationship.
    bool generatedChildTrack = false;
    std::string parentClipId;
    std::string childClipId;
};

inline bool isGeneratedEditorChildTrack(const EditorTrack& track)
{
    return track.generatedChildTrack;
}

struct EditorClip {
    int id = 0;
    int trackId = 0;
    std::string label;
    int startFrame = 0;
    int durationFrames = 0;
    bool selected = false;
    std::string sourcePath;
    std::string persistentId;
    std::string proxyPath;
    bool useProxy = true;
    std::string mediaKind;
    std::int64_t sourceDurationFrames = 0;
    std::int64_t sourceInFrame = 0;
    std::int64_t sourceInSubframeSamples = 0;
    std::int64_t startSubframeSamples = 0;
    std::int64_t durationSubframeSamples = 0;
    double sourceFps = 30.0;
    double playbackRate = 1.0;
    bool videoEnabled = true;
    bool audioEnabled = true;
    bool hasAudio = false;
    bool audioPresenceKnown = false;
    std::string audioSourceMode = "embedded";
    std::string audioSourcePath;
    std::string audioSourceStatus = "unknown";
    int audioStreamIndex = -1;
    // Neutral, per-clip active transcript selection. The legacy project root
    // still mirrors the selected clip for compatibility with the Qt shell.
    std::string transcriptActiveCutPath;
    std::string audioBusId;
    double audioGain = 1.0;
    double audioPan = 0.0;
    bool audioSolo = false;
    bool audioLinkedToVideo = true;
    double brightness = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    double opacity = 1.0;
    double baseTranslationX = 0.0;
    double baseTranslationY = 0.0;
    double baseRotation = 0.0;
    double baseScaleX = 1.0;
    double baseScaleY = 1.0;
    bool gradingPreviewEnabled = true;
    std::vector<EditorTransformKeyframe> transformKeyframes;
    std::vector<EditorGradingKeyframe> gradingKeyframes;
    std::vector<EditorOpacityKeyframe> opacityKeyframes;
    std::vector<EditorTitleKeyframe> titleKeyframes;
    EditorTranscriptOverlayState transcriptOverlay;
    bool locked = false;
    bool maskEnabled = false;
    std::string maskFramesDir;
    double maskFeather = 0.0;
    double maskFeatherGamma = 1.0;
    int maskFeatherFalloff = 0;
    double maskDilate = 0.0;
    double maskErode = 0.0;
    double maskBlur = 0.0;
    bool maskInvert = false;
    bool maskShowOnly = false;
    double maskOpacity = 1.0;
    bool maskGradeEnabled = false;
    double maskGradeBrightness = 0.0;
    double maskGradeContrast = 1.0;
    double maskGradeSaturation = 1.0;
    std::vector<EditorPoint> maskGradeCurvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> maskGradeCurvePointsG = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> maskGradeCurvePointsB = {{0.0, 0.0}, {1.0, 1.0}};
    std::vector<EditorPoint> maskGradeCurvePointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    bool maskGradeCurveSmoothingEnabled = true;
    bool maskDropShadowEnabled = false;
    double maskDropShadowRadius = 12.0;
    double maskDropShadowOffsetX = 0.0;
    double maskDropShadowOffsetY = 4.0;
    double maskDropShadowOpacity = 0.45;
    bool maskForegroundLayerEnabled = false;
    bool maskRepeatEnabled = false;
    double maskRepeatDeltaX = 160.0;
    double maskRepeatDeltaY = 0.0;
    std::string effectPreset = "none";
    int effectRows = 32;
    double effectSpeed = 1.0;
    double effectScale = 1.0;
    bool effectAlternateDirection = true;
    bool effectSkipAwareTiming = true;
    int differenceReferenceFrames = 1;
    double differenceThreshold = 0.10;
    double differenceSoftness = 0.05;
    int temporalEchoCount = 4;
    int temporalEchoSpacingFrames = 2;
    double temporalEchoDecay = 0.65;
    std::string tilingPattern = "grid";
    double tilingSpacing = 1.0;
    bool tilingWrap = true;
    std::vector<EditorCorrectionPolygon> correctionPolygons;
    // Known values match clip_serialization.cpp: media, mask_matte,
    // effect_synth, and speaker_title. Unknown future values remain opaque.
    // Keep these at the end so existing aggregate initializers stay compatible.
    std::string clipRole = "media";
    std::string linkedSourceClipId;
    std::string generatedFromMaskId;
    bool syncLockedToSource = false;
    bool sourceTransformLocked = false;
    int zLevel = std::numeric_limits<int>::min();
    bool zLevelUserSet = false;
    // Audio edge-fade length at 48 kHz. Qt persists this per clip and uses
    // 250 samples as its click-suppression default when no crossfade has been
    // applied.
    int fadeSamples = 250;
    // Automatic speaker/face framing. These mirror the durable Qt clip
    // contract so baked framing can render identically in the Qt-free path;
    // manual/runtime tracking selectors remain available for artifact-backed
    // evaluation.
    bool speakerFramingEnabled = false;
    double speakerFramingBakedTargetXNorm = 0.5;
    double speakerFramingBakedTargetYNorm = 0.35;
    double speakerFramingBakedTargetBoxNorm = -1.0;
    double speakerFramingMinConfidence = 0.08;
    int speakerFramingManualTrackId = -1;
    std::string speakerFramingManualStreamId;
    int speakerFramingCenterSmoothingFrames = 0;
    int speakerFramingZoomSmoothingFrames = 0;
    int speakerFramingSmoothingMode = 0;
    double speakerFramingCenterSmoothingStrength = 1.0;
    double speakerFramingZoomSmoothingStrength = 1.0;
    int speakerFramingGapHoldFrames = 0;
    int speakerSectionMinimumWords = 10;
    std::vector<EditorBoolKeyframe>
        speakerFramingEnabledKeyframes;
    std::vector<EditorTransformKeyframe>
        speakerFramingKeyframes;
    std::vector<EditorTransformKeyframe>
        speakerFramingTargetKeyframes;
};

struct EditorTransportState {
    bool playbackActive = false;
    float playbackSpeed = 1.0f;
    bool playbackLoopEnabled = false;
    std::string previewViewMode = "video";
    bool audioMuted = false;
    float audioVolume = 0.8f;
    float previewZoom = 1.0f;
    int currentFrame = 0;
};

struct EditorPanelState {
    bool showWaveform = true;
    bool showTranscript = true;
    bool showScopes = false;
};

struct EditorRenderSyncMarker {
    std::string clipId;
    std::int64_t frame = 0;
    bool skipFrame = false;
    int count = 1;
};

struct EditorExportRange {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;

    friend bool operator==(
        const EditorExportRange&,
        const EditorExportRange&) = default;
};

struct EditorDocumentCore {
    std::string projectName;
    std::vector<EditorMediaItem> mediaItems;
    std::vector<EditorTrack> tracks;
    std::vector<EditorClip> clips;
    std::vector<EditorRenderSyncMarker> renderSyncMarkers;
    std::vector<EditorExportRange> exportRanges;
    EditorTransportState transport;
    EditorPanelState panels;
    EditorAudioTreatment audioTreatment =
        EditorAudioTreatment::PreservePitch;
    audio::DynamicsSettingsCore audioDynamics;
    render::RenderRequestCore exportRequest;
    // Runtime-only transcript payloads participate in globally ordered undo/
    // redo. They are intentionally omitted by every project serializer.
    struct TranscriptHistoryDocument {
        std::string path;
        std::string jsonPayload;
    };
    std::vector<TranscriptHistoryDocument> transcriptHistoryDocuments;
};

inline std::string trimmedEditorClipId(std::string_view value)
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' ||
            value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

inline std::string canonicalEditorClipRole(std::string_view value)
{
    std::string normalized = trimmedEditorClipId(value);
    for (char& character : normalized) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (normalized == "mask_matte" || normalized == "mask" ||
        normalized == "matte") {
        return "mask_matte";
    }
    if (normalized == "effect_synth" || normalized == "effect" ||
        normalized == "synth") {
        return "effect_synth";
    }
    if (normalized == "speaker_title" || normalized == "lower_third" ||
        normalized == "speaker_lower_third") {
        return "speaker_title";
    }
    if (normalized.empty() || normalized == "media") {
        return "media";
    }
    // Unknown roles remain distinct for behavior and forward compatibility.
    return normalized;
}

inline std::string editorClipRoleForStorage(std::string_view value)
{
    const std::string rawRole = trimmedEditorClipId(value);
    const std::string canonicalRole = canonicalEditorClipRole(rawRole);
    if (canonicalRole == "media" || canonicalRole == "mask_matte" ||
        canonicalRole == "effect_synth" || canonicalRole == "speaker_title") {
        return canonicalRole;
    }
    // Do not collapse a role introduced by a newer Qt build into media.
    return rawRole;
}

// Qt assigns render-sync decisions made on a generated mask layer to the
// linked media source. Keep the same rule in one neutral helper so loading,
// commands, and either shell cannot create competing parent/child decisions.
inline std::string editorRenderSyncOwnerClipId(
    const EditorDocumentCore& document,
    std::string_view clipId)
{
    const std::string normalizedId = trimmedEditorClipId(clipId);
    if (normalizedId.empty()) {
        return {};
    }
    for (const EditorClip& clip : document.clips) {
        if (trimmedEditorClipId(clip.persistentId) != normalizedId) {
            continue;
        }
        if (canonicalEditorClipRole(clip.clipRole) == "mask_matte") {
            const std::string linkedSourceId =
                trimmedEditorClipId(clip.linkedSourceClipId);
            if (!linkedSourceId.empty()) {
                return linkedSourceId;
            }
        }
        break;
    }
    return normalizedId;
}

} // namespace jcut
