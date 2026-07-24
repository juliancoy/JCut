#include "editor_document_render_bridge.h"

#include "clip_serialization.h"
#include "editor_shared_media.h"
#include "editor_shared_timing.h"
#include "editor_timeline_types.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace {

ClipMediaType clipMediaTypeFromKind(const std::string& kind)
{
    if (kind == "image") {
        return ClipMediaType::Image;
    }
    if (kind == "video") {
        return ClipMediaType::Video;
    }
    if (kind == "audio") {
        return ClipMediaType::Audio;
    }
    if (kind == "title" || kind == "graphics") {
        return ClipMediaType::Title;
    }
    return ClipMediaType::Unknown;
}

ClipMediaType inferClipMediaType(const std::string& kind, const std::string& sourcePath)
{
    const ClipMediaType explicitType = clipMediaTypeFromKind(kind);
    if (explicitType != ClipMediaType::Unknown) {
        return explicitType;
    }

    std::string suffix = std::filesystem::path(sourcePath).extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (!suffix.empty() && suffix.front() == '.') {
        suffix.erase(suffix.begin());
    }
    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg" ||
        suffix == "webp" || suffix == "tga" || suffix == "tif" ||
        suffix == "tiff" || suffix == "bmp" || suffix == "exr") {
        return ClipMediaType::Image;
    }
    if (suffix == "wav" || suffix == "mp3" || suffix == "aac" ||
        suffix == "m4a" || suffix == "flac" || suffix == "ogg") {
        return ClipMediaType::Audio;
    }
    if (!sourcePath.empty()) {
        return ClipMediaType::Video;
    }
    return ClipMediaType::Unknown;
}

ClipRole clipRoleFromCore(const std::string& role)
{
    const std::string canonical = jcut::canonicalEditorClipRole(role);
    if (canonical == "mask_matte") {
        return ClipRole::MaskMatte;
    }
    if (canonical == "effect_synth") {
        return ClipRole::EffectSynth;
    }
    if (canonical == "speaker_title") {
        return ClipRole::SpeakerTitle;
    }
    return ClipRole::Media;
}

} // namespace

namespace jcut::render {

TimelineRenderData buildTimelineRenderData(const EditorDocumentCore& document,
                                           bool probeMedia)
{
    TimelineRenderData timelineData;

    timelineData.tracks.reserve(document.tracks.size());
    std::unordered_map<int, int> trackIndexById;
    for (std::size_t index = 0; index < document.tracks.size(); ++index) {
        const EditorTrack& track = document.tracks[index];
        TimelineTrack timelineTrack;
        timelineTrack.name = QString::fromStdString(track.label);
        timelineTrack.height = std::max(24, track.height);
        timelineTrack.visualMode = static_cast<TrackVisualMode>(std::clamp(track.visualMode, 0, 2));
        timelineTrack.gradingPreviewEnabled = track.gradingPreviewEnabled;
        timelineTrack.audioEnabled = track.audioEnabled;
        timelineTrack.audioBusId = QString::fromStdString(track.audioBusId);
        timelineTrack.audioGain = track.audioGain;
        timelineTrack.audioMuted = track.audioMuted;
        timelineTrack.audioSolo = track.audioSolo;
        timelineTrack.audioWaveformVisible = track.audioWaveformVisible;
        timelineTrack.generatedChildTrack = track.generatedChildTrack;
        timelineTrack.parentClipId =
            QString::fromStdString(track.parentClipId).trimmed();
        timelineTrack.childClipId =
            QString::fromStdString(track.childClipId).trimmed();
        timelineData.tracks.push_back(timelineTrack);
        trackIndexById.emplace(track.id, static_cast<int>(index));
    }

    std::unordered_map<std::string, std::string> mediaKindById;
    for (const EditorMediaItem& mediaItem : document.mediaItems) {
        mediaKindById.emplace(mediaItem.id, mediaItem.kind);
    }

    timelineData.clips.reserve(document.clips.size());
    for (const EditorClip& clip : document.clips) {
        auto trackIt = trackIndexById.find(clip.trackId);
        if (trackIt == trackIndexById.end()) {
            continue;
        }
        TimelineClip timelineClip;
        timelineClip.id = clip.persistentId.empty()
            ? QStringLiteral("imgui-clip-%1").arg(clip.id)
            : QString::fromStdString(clip.persistentId);
        timelineClip.clipRole = clipRoleFromCore(clip.clipRole);
        timelineClip.linkedSourceClipId =
            QString::fromStdString(clip.linkedSourceClipId).trimmed();
        timelineClip.generatedFromMaskId =
            QString::fromStdString(clip.generatedFromMaskId).trimmed();
        timelineClip.syncLockedToSource = clip.syncLockedToSource;
        timelineClip.sourceTransformLocked = clip.sourceTransformLocked;
        timelineClip.speakerFramingEnabled =
            clip.speakerFramingEnabled;
        timelineClip.speakerFramingBakedTargetXNorm =
            clip.speakerFramingBakedTargetXNorm;
        timelineClip.speakerFramingBakedTargetYNorm =
            clip.speakerFramingBakedTargetYNorm;
        timelineClip.speakerFramingBakedTargetBoxNorm =
            clip.speakerFramingBakedTargetBoxNorm;
        timelineClip.speakerFramingMinConfidence =
            clip.speakerFramingMinConfidence;
        timelineClip.speakerFramingManualTrackId =
            clip.speakerFramingManualTrackId;
        timelineClip.speakerFramingManualStreamId =
            QString::fromStdString(
                clip.speakerFramingManualStreamId);
        timelineClip.speakerFramingCenterSmoothingFrames =
            clip.speakerFramingCenterSmoothingFrames;
        timelineClip.speakerFramingZoomSmoothingFrames =
            clip.speakerFramingZoomSmoothingFrames;
        timelineClip.speakerFramingSmoothingMode =
            clip.speakerFramingSmoothingMode;
        timelineClip.speakerFramingCenterSmoothingStrength =
            clip.speakerFramingCenterSmoothingStrength;
        timelineClip.speakerFramingZoomSmoothingStrength =
            clip.speakerFramingZoomSmoothingStrength;
        timelineClip.speakerFramingGapHoldFrames =
            clip.speakerFramingGapHoldFrames;
        timelineClip.speakerSectionMinimumWords =
            std::clamp(clip.speakerSectionMinimumWords, 0, 1000);
        timelineClip.zLevel = clip.zLevel;
        timelineClip.zLevelUserSet = clip.zLevelUserSet;
        timelineClip.filePath = QString::fromStdString(clip.sourcePath);
        timelineClip.proxyPath = QString::fromStdString(clip.proxyPath);
        timelineClip.useProxy = clip.useProxy;
        timelineClip.label = QString::fromStdString(clip.label);
        timelineClip.trackIndex = trackIt->second;
        timelineClip.startFrame = clip.startFrame;
        timelineClip.startSubframeSamples = clip.startSubframeSamples;
        timelineClip.durationFrames = std::max(1, clip.durationFrames);
        timelineClip.durationSubframeSamples = clip.durationSubframeSamples;
        timelineClip.sourceDurationFrames = clip.sourceDurationFrames;
        timelineClip.sourceInFrame = clip.sourceInFrame;
        timelineClip.sourceInSubframeSamples = clip.sourceInSubframeSamples;
        timelineClip.sourceFps = clip.sourceFps;
        timelineClip.playbackRate = clip.playbackRate;
        timelineClip.videoEnabled = clip.videoEnabled;
        timelineClip.audioEnabled = clip.audioEnabled;
        timelineClip.hasAudio = clip.hasAudio;
        timelineClip.audioSourceMode = QString::fromStdString(clip.audioSourceMode);
        timelineClip.audioSourcePath = QString::fromStdString(clip.audioSourcePath);
        timelineClip.audioSourceStatus = QString::fromStdString(clip.audioSourceStatus);
        timelineClip.audioStreamIndex = clip.audioStreamIndex;
        timelineClip.transcriptActiveCutPath =
            QString::fromStdString(clip.transcriptActiveCutPath);
        timelineClip.audioBusId = QString::fromStdString(clip.audioBusId);
        timelineClip.audioGain = clip.audioGain;
        timelineClip.audioPan = clip.audioPan;
        timelineClip.audioSolo = clip.audioSolo;
        timelineClip.audioLinkedToVideo = clip.audioLinkedToVideo;
        timelineClip.fadeSamples = std::max(0, clip.fadeSamples);
        timelineClip.brightness = clip.brightness;
        timelineClip.contrast = clip.contrast;
        timelineClip.saturation = clip.saturation;
        timelineClip.opacity = clip.opacity;
        timelineClip.baseTranslationX = clip.baseTranslationX;
        timelineClip.baseTranslationY = clip.baseTranslationY;
        timelineClip.baseRotation = clip.baseRotation;
        timelineClip.baseScaleX = clip.baseScaleX;
        timelineClip.baseScaleY = clip.baseScaleY;
        timelineClip.gradingPreviewEnabled = clip.gradingPreviewEnabled;
        timelineClip.locked = clip.locked;
        timelineClip.maskEnabled = clip.maskEnabled;
        timelineClip.maskFramesDir = QString::fromStdString(clip.maskFramesDir);
        timelineClip.maskFeather = clip.maskFeather;
        timelineClip.maskFeatherGamma = clip.maskFeatherGamma;
        timelineClip.maskFeatherFalloff = std::clamp(clip.maskFeatherFalloff, 0, 5);
        timelineClip.maskDilate = clip.maskDilate;
        timelineClip.maskErode = clip.maskErode;
        timelineClip.maskBlur = clip.maskBlur;
        timelineClip.maskInvert = clip.maskInvert;
        timelineClip.maskShowOnly = clip.maskShowOnly;
        timelineClip.maskOpacity = clip.maskOpacity;
        timelineClip.maskGradeEnabled = clip.maskGradeEnabled;
        timelineClip.maskGradeBrightness = clip.maskGradeBrightness;
        timelineClip.maskGradeContrast = clip.maskGradeContrast;
        timelineClip.maskGradeSaturation = clip.maskGradeSaturation;
        const auto copyMaskCurveToQt = [](const std::vector<EditorPoint>& source) {
            QVector<QPointF> result;
            result.reserve(static_cast<qsizetype>(source.size()));
            for (const EditorPoint& point : source) {
                result.push_back(QPointF(point.x, point.y));
            }
            return result;
        };
        timelineClip.maskGradeCurvePointsR = copyMaskCurveToQt(clip.maskGradeCurvePointsR);
        timelineClip.maskGradeCurvePointsG = copyMaskCurveToQt(clip.maskGradeCurvePointsG);
        timelineClip.maskGradeCurvePointsB = copyMaskCurveToQt(clip.maskGradeCurvePointsB);
        timelineClip.maskGradeCurvePointsLuma = copyMaskCurveToQt(clip.maskGradeCurvePointsLuma);
        timelineClip.maskGradeCurveSmoothingEnabled = clip.maskGradeCurveSmoothingEnabled;
        timelineClip.maskDropShadowEnabled = clip.maskDropShadowEnabled;
        timelineClip.maskDropShadowRadius = clip.maskDropShadowRadius;
        timelineClip.maskDropShadowOffsetX = clip.maskDropShadowOffsetX;
        timelineClip.maskDropShadowOffsetY = clip.maskDropShadowOffsetY;
        timelineClip.maskDropShadowOpacity = clip.maskDropShadowOpacity;
        timelineClip.maskForegroundLayerEnabled = clip.maskForegroundLayerEnabled;
        timelineClip.maskRepeatEnabled = clip.maskRepeatEnabled;
        timelineClip.maskRepeatDeltaX = clip.maskRepeatDeltaX;
        timelineClip.maskRepeatDeltaY = clip.maskRepeatDeltaY;
        timelineClip.edgeFillEnabled = clip.edgeFillEnabled;
        timelineClip.edgeFillProgressive = clip.edgeFillProgressive;
        timelineClip.edgeFillPixels = clip.edgeFillPixels;
        timelineClip.edgeFillPower = clip.edgeFillPower;
        timelineClip.edgeFillOpacity = clip.edgeFillOpacity;
        timelineClip.edgeFillBrightness = clip.edgeFillBrightness;
        timelineClip.edgeFillSaturation = clip.edgeFillSaturation;
        timelineClip.effectPreset = editor::effectPresetFromJson(QString::fromStdString(clip.effectPreset));
        timelineClip.effectRows = clip.effectRows;
        timelineClip.effectSpeed = clip.effectSpeed;
        timelineClip.effectScale = clip.effectScale;
        timelineClip.effectAlternateDirection = clip.effectAlternateDirection;
        timelineClip.effectSkipAwareTiming = clip.effectSkipAwareTiming;
        timelineClip.differenceReferenceFrames = clip.differenceReferenceFrames;
        timelineClip.differenceThreshold = clip.differenceThreshold;
        timelineClip.differenceSoftness = clip.differenceSoftness;
        timelineClip.temporalEchoCount = clip.temporalEchoCount;
        timelineClip.temporalEchoSpacingFrames = clip.temporalEchoSpacingFrames;
        timelineClip.temporalEchoDecay = clip.temporalEchoDecay;
        timelineClip.tilingPattern = editor::tilingPatternFromJson(
            QString::fromStdString(clip.tilingPattern));
        timelineClip.tilingSpacing = clip.tilingSpacing;
        timelineClip.tilingWrap = clip.tilingWrap;
        for (const EditorCorrectionPolygon& polygon : clip.correctionPolygons) {
            TimelineClip::CorrectionPolygon value;
            value.enabled = polygon.enabled;
            value.startFrame = polygon.startFrame;
            value.endFrame = polygon.endFrame;
            for (const EditorPoint& point : polygon.pointsNormalized) {
                value.pointsNormalized.push_back(QPointF(point.x, point.y));
            }
            timelineClip.correctionPolygons.push_back(std::move(value));
        }
        timelineClip.transcriptOverlay.enabled = clip.transcriptOverlay.enabled;
        timelineClip.transcriptOverlay.showBackground = clip.transcriptOverlay.showBackground;
        timelineClip.transcriptOverlay.backgroundOpacity = clip.transcriptOverlay.backgroundOpacity;
        timelineClip.transcriptOverlay.backgroundCornerRadius = clip.transcriptOverlay.backgroundCornerRadius;
        timelineClip.transcriptOverlay.backgroundPadding = clip.transcriptOverlay.backgroundPadding;
        timelineClip.transcriptOverlay.backgroundFrameEnabled = clip.transcriptOverlay.backgroundFrameEnabled;
        timelineClip.transcriptOverlay.backgroundFrameColor = QColor(QString::fromStdString(clip.transcriptOverlay.backgroundFrameColor));
        timelineClip.transcriptOverlay.backgroundFrameOpacity = clip.transcriptOverlay.backgroundFrameOpacity;
        timelineClip.transcriptOverlay.backgroundFrameWidth = clip.transcriptOverlay.backgroundFrameWidth;
        timelineClip.transcriptOverlay.backgroundFrameGap = clip.transcriptOverlay.backgroundFrameGap;
        timelineClip.transcriptOverlay.showShadow = clip.transcriptOverlay.showShadow;
        timelineClip.transcriptOverlay.shadowColor = QColor(QString::fromStdString(clip.transcriptOverlay.shadowColor));
        timelineClip.transcriptOverlay.shadowOpacity = clip.transcriptOverlay.shadowOpacity;
        timelineClip.transcriptOverlay.shadowOffsetX = clip.transcriptOverlay.shadowOffsetX;
        timelineClip.transcriptOverlay.shadowOffsetY = clip.transcriptOverlay.shadowOffsetY;
        timelineClip.transcriptOverlay.textOutlineEnabled = clip.transcriptOverlay.textOutlineEnabled;
        timelineClip.transcriptOverlay.textOutlineWidth = clip.transcriptOverlay.textOutlineWidth;
        timelineClip.transcriptOverlay.textOutlineColor = QColor(QString::fromStdString(clip.transcriptOverlay.textOutlineColor));
        timelineClip.transcriptOverlay.textOutlineOpacity = clip.transcriptOverlay.textOutlineOpacity;
        using TextExtrudeMode = TimelineClip::TitleKeyframe::TextExtrudeMode;
        timelineClip.transcriptOverlay.textExtrudeMode =
            clip.transcriptOverlay.textExtrudeMode == "stacked_copies"
                ? TextExtrudeMode::StackedCopies
                : (clip.transcriptOverlay.textExtrudeMode == "eroded_solid"
                    ? TextExtrudeMode::ErodedSolid : TextExtrudeMode::None);
        timelineClip.transcriptOverlay.textExtrudeDepth = clip.transcriptOverlay.textExtrudeDepth;
        timelineClip.transcriptOverlay.textExtrudeBevelScale = clip.transcriptOverlay.textExtrudeBevelScale;
        timelineClip.transcriptOverlay.showSpeakerTitle = clip.transcriptOverlay.showSpeakerTitle;
        timelineClip.transcriptOverlay.highlightCurrentWord = clip.transcriptOverlay.highlightCurrentWord;
        timelineClip.transcriptOverlay.autoScroll = clip.transcriptOverlay.autoScroll;
        timelineClip.transcriptOverlay.useManualPlacement = clip.transcriptOverlay.useManualPlacement;
        timelineClip.transcriptOverlay.translationX = clip.transcriptOverlay.translationX;
        timelineClip.transcriptOverlay.translationY = clip.transcriptOverlay.translationY;
        timelineClip.transcriptOverlay.boxWidth = clip.transcriptOverlay.boxWidth;
        timelineClip.transcriptOverlay.boxHeight = clip.transcriptOverlay.boxHeight;
        timelineClip.transcriptOverlay.maxLines = std::max(1, clip.transcriptOverlay.maxLines);
        timelineClip.transcriptOverlay.maxCharsPerLine = std::max(1, clip.transcriptOverlay.maxCharsPerLine);
        timelineClip.transcriptOverlay.fontFamily = QString::fromStdString(clip.transcriptOverlay.fontFamily);
        timelineClip.transcriptOverlay.fontPointSize = std::max(8, clip.transcriptOverlay.fontPointSize);
        timelineClip.transcriptOverlay.bold = clip.transcriptOverlay.bold;
        timelineClip.transcriptOverlay.italic = clip.transcriptOverlay.italic;
        timelineClip.transcriptOverlay.textColor = QColor(QString::fromStdString(clip.transcriptOverlay.textColor));
        timelineClip.transcriptOverlay.textOpacity = clip.transcriptOverlay.textOpacity;
        timelineClip.transcriptOverlay.backgroundColor = QColor(QString::fromStdString(clip.transcriptOverlay.backgroundColor));
        timelineClip.transcriptOverlay.highlightColor = QColor(QString::fromStdString(clip.transcriptOverlay.highlightColor));
        timelineClip.transcriptOverlay.highlightTextColor = QColor(QString::fromStdString(clip.transcriptOverlay.highlightTextColor));
        for (const EditorBoolKeyframe& keyframe :
             clip.speakerFramingEnabledKeyframes) {
            TimelineClip::BoolKeyframe value;
            value.frame = keyframe.frame;
            value.enabled = keyframe.enabled;
            timelineClip.speakerFramingEnabledKeyframes.push_back(
                value);
        }
        const auto copySpeakerFramingKeyframes =
            [](const std::vector<EditorTransformKeyframe>& source) {
                QVector<TimelineClip::TransformKeyframe> result;
                result.reserve(
                    static_cast<qsizetype>(source.size()));
                for (const EditorTransformKeyframe& keyframe :
                     source) {
                    TimelineClip::TransformKeyframe value;
                    value.frame = keyframe.frame;
                    value.title =
                        QString::fromStdString(keyframe.title);
                    value.translationX = keyframe.translationX;
                    value.translationY = keyframe.translationY;
                    value.rotation = keyframe.rotation;
                    value.scaleX = keyframe.scaleX;
                    value.scaleY = keyframe.scaleY;
                    value.linearInterpolation =
                        keyframe.linearInterpolation;
                    result.push_back(std::move(value));
                }
                return result;
            };
        timelineClip.speakerFramingKeyframes =
            copySpeakerFramingKeyframes(
                clip.speakerFramingKeyframes);
        timelineClip.speakerFramingTargetKeyframes =
            copySpeakerFramingKeyframes(
                clip.speakerFramingTargetKeyframes);
        for (const EditorTransformKeyframe& keyframe : clip.transformKeyframes) {
            TimelineClip::TransformKeyframe value;
            value.frame = keyframe.frame;
            value.title = QString::fromStdString(keyframe.title);
            value.translationX = keyframe.translationX;
            value.translationY = keyframe.translationY;
            value.rotation = keyframe.rotation;
            value.scaleX = keyframe.scaleX;
            value.scaleY = keyframe.scaleY;
            value.linearInterpolation = keyframe.linearInterpolation;
            timelineClip.transformKeyframes.push_back(std::move(value));
        }
        for (const EditorGradingKeyframe& keyframe : clip.gradingKeyframes) {
            TimelineClip::GradingKeyframe value;
            value.frame = keyframe.frame;
            value.brightness = keyframe.brightness;
            value.contrast = keyframe.contrast;
            value.saturation = keyframe.saturation;
            value.opacity = keyframe.opacity;
            value.linearInterpolation = keyframe.linearInterpolation;
            value.shadowsR = keyframe.shadowsR;
            value.shadowsG = keyframe.shadowsG;
            value.shadowsB = keyframe.shadowsB;
            value.midtonesR = keyframe.midtonesR;
            value.midtonesG = keyframe.midtonesG;
            value.midtonesB = keyframe.midtonesB;
            value.highlightsR = keyframe.highlightsR;
            value.highlightsG = keyframe.highlightsG;
            value.highlightsB = keyframe.highlightsB;
            const auto copyPoints = [](const std::vector<EditorPoint>& points) {
                QVector<QPointF> result;
                result.reserve(static_cast<qsizetype>(points.size()));
                for (const EditorPoint& point : points) {
                    result.push_back(QPointF(point.x, point.y));
                }
                if (result.isEmpty()) {
                    result = {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
                }
                return result;
            };
            value.curvePointsR = copyPoints(keyframe.curvePointsR);
            value.curvePointsG = copyPoints(keyframe.curvePointsG);
            value.curvePointsB = copyPoints(keyframe.curvePointsB);
            value.curvePointsLuma = copyPoints(keyframe.curvePointsLuma);
            value.curveThreePointLock = keyframe.curveThreePointLock;
            value.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
            timelineClip.gradingKeyframes.push_back(std::move(value));
        }
        for (const EditorOpacityKeyframe& keyframe : clip.opacityKeyframes) {
            timelineClip.opacityKeyframes.push_back({
                keyframe.frame,
                keyframe.opacity,
                keyframe.linearInterpolation
            });
        }
        for (const EditorTitleKeyframe& keyframe : clip.titleKeyframes) {
            const auto materialStyle = [](const std::string& name) {
                using Style = TimelineClip::TitleKeyframe::MaterialStyle;
                if (name == "neon") return Style::Neon;
                if (name == "diagonal_stripes") return Style::DiagonalStripes;
                if (name == "grid") return Style::Grid;
                if (name == "image_pattern") return Style::ImagePattern;
                return Style::Solid;
            };
            const auto extrudeMode = [](const std::string& name) {
                using Mode = TimelineClip::TitleKeyframe::TextExtrudeMode;
                if (name == "stacked_copies") return Mode::StackedCopies;
                if (name == "eroded_solid") return Mode::ErodedSolid;
                return Mode::None;
            };
            TimelineClip::TitleKeyframe value;
            value.frame = keyframe.frame;
            value.text = QString::fromStdString(keyframe.text);
            value.translationX = keyframe.translationX;
            value.translationY = keyframe.translationY;
            value.fontSize = keyframe.fontSize;
            value.opacity = keyframe.opacity;
            value.fontFamily = QString::fromStdString(keyframe.fontFamily);
            value.bold = keyframe.bold;
            value.italic = keyframe.italic;
            value.color = QColor(QString::fromStdString(keyframe.color));
            value.linearInterpolation = keyframe.linearInterpolation;
            value.autoFitToOutput = keyframe.autoFitToOutput;
            value.logoPath = QString::fromStdString(keyframe.logoPath);
            value.textMaterialStyle = materialStyle(keyframe.textMaterialStyle);
            value.textPatternImagePath = QString::fromStdString(keyframe.textPatternImagePath);
            value.textPatternScale = keyframe.textPatternScale;
            value.dropShadowEnabled = keyframe.dropShadowEnabled;
            value.dropShadowColor = QColor(QString::fromStdString(keyframe.dropShadowColor));
            value.dropShadowOpacity = keyframe.dropShadowOpacity;
            value.dropShadowOffsetX = keyframe.dropShadowOffsetX;
            value.dropShadowOffsetY = keyframe.dropShadowOffsetY;
            value.windowEnabled = keyframe.windowEnabled;
            value.windowColor = QColor(QString::fromStdString(keyframe.windowColor));
            value.windowOpacity = keyframe.windowOpacity;
            value.windowPadding = keyframe.windowPadding;
            value.windowWidth = keyframe.windowWidth;
            value.windowFrameEnabled = keyframe.windowFrameEnabled;
            value.windowFrameColor = QColor(QString::fromStdString(keyframe.windowFrameColor));
            value.windowFrameOpacity = keyframe.windowFrameOpacity;
            value.windowFrameWidth = keyframe.windowFrameWidth;
            value.windowFrameGap = keyframe.windowFrameGap;
            value.windowFrameMaterialStyle = materialStyle(keyframe.windowFrameMaterialStyle);
            value.windowFramePatternImagePath = QString::fromStdString(keyframe.windowFramePatternImagePath);
            value.windowFramePatternScale = keyframe.windowFramePatternScale;
            value.vulkan3DEnabled = keyframe.vulkan3DEnabled;
            value.vulkan3DExtrudeEnabled = keyframe.vulkan3DExtrudeEnabled;
            value.textExtrudeMode = extrudeMode(keyframe.textExtrudeMode);
            value.vulkan3DExtrudeDepth = keyframe.vulkan3DExtrudeDepth;
            value.vulkan3DBevelScale = keyframe.vulkan3DBevelScale;
            value.vulkan3DYawDegrees = keyframe.vulkan3DYawDegrees;
            value.vulkan3DPitchDegrees = keyframe.vulkan3DPitchDegrees;
            value.vulkan3DRollDegrees = keyframe.vulkan3DRollDegrees;
            value.vulkan3DDepth = keyframe.vulkan3DDepth;
            value.vulkan3DScale = keyframe.vulkan3DScale;
            timelineClip.titleKeyframes.push_back(std::move(value));
        }
        const auto mediaIt = mediaKindById.find(clip.sourcePath);
        timelineClip.mediaType = inferClipMediaType(
            !clip.mediaKind.empty()
                ? clip.mediaKind
                : (mediaIt == mediaKindById.end() ? std::string{} : mediaIt->second),
            clip.sourcePath);
        timelineClip.sourceKind = isImageSequencePath(timelineClip.filePath)
            ? MediaSourceKind::ImageSequence
            : MediaSourceKind::File;
        std::error_code pathError;
        const bool sourceExists = !clip.sourcePath.empty() &&
            std::filesystem::exists(std::filesystem::path(clip.sourcePath), pathError) &&
            !pathError;
        if (probeMedia && sourceExists && timelineClip.mediaType != ClipMediaType::Title) {
            const MediaProbeResult probe = probeMediaFile(
                timelineClip.filePath,
                static_cast<qreal>(timelineClip.durationFrames) /
                    static_cast<qreal>(kTimelineFps));
            const bool probeHasAuthoritativeMedia = probe.hasVideo || probe.hasAudio ||
                probe.mediaType != ClipMediaType::Unknown;
            if (probeHasAuthoritativeMedia && probe.hasVideo && probe.fps > 0.001) {
                timelineClip.sourceFps = probe.fps;
            }
            if (probeHasAuthoritativeMedia &&
                probe.mediaType != ClipMediaType::Image && probe.durationFrames > 0) {
                timelineClip.sourceDurationFrames = probe.durationFrames;
            }
            if (probe.frameSize.isValid()) {
                timelineClip.sourceFrameSize = probe.frameSize;
            }
            if (probeHasAuthoritativeMedia && probe.mediaType != ClipMediaType::Unknown) {
                timelineClip.mediaType = probe.mediaType;
            }
            if (probeHasAuthoritativeMedia) {
                timelineClip.sourceKind = probe.sourceKind;
                timelineClip.hasAudio = probe.hasAudio;
            }
        }
        // Resolve the audio source only after probing so the selected path and
        // status reflect the authoritative hasAudio value when one is
        // available. This remains filesystem-only when probing is disabled.
        refreshClipAudioSource(timelineClip);
        if (timelineClip.sourceDurationFrames <= 0) {
            timelineClip.sourceDurationFrames =
                timelineClip.sourceInFrame + timelineClip.durationFrames;
        }
        timelineData.clips.push_back(std::move(timelineClip));
    }

    timelineData.renderSyncMarkers.reserve(document.renderSyncMarkers.size());
    for (const EditorRenderSyncMarker& marker : document.renderSyncMarkers) {
        RenderSyncMarker value;
        value.clipId = QString::fromStdString(marker.clipId);
        value.frame = marker.frame;
        value.action = marker.skipFrame
            ? RenderSyncAction::SkipFrame
            : RenderSyncAction::DuplicateFrame;
        value.count = std::max(1, marker.count);
        timelineData.renderSyncMarkers.push_back(std::move(value));
    }

    timelineData.exportRanges.reserve(document.exportRanges.size());
    for (const EditorExportRange& range : document.exportRanges) {
        if (range.endFrame > range.startFrame) {
            timelineData.exportRanges.push_back({range.startFrame, range.endFrame});
        }
    }

    const int exportEndFrame = document.exportRequest.exportEndFrame > document.exportRequest.exportStartFrame
        ? static_cast<int>(document.exportRequest.exportEndFrame)
        : [&document]() {
              int maxFrame = 0;
              for (const EditorClip& clip : document.clips) {
                  maxFrame = std::max(maxFrame, clip.startFrame + clip.durationFrames);
              }
              return maxFrame;
          }();

    if (timelineData.exportRanges.empty() &&
        exportEndFrame > document.exportRequest.exportStartFrame) {
        ExportRangeSegment segment;
        segment.startFrame = document.exportRequest.exportStartFrame;
        segment.endFrame = exportEndFrame;
        timelineData.exportRanges.push_back(segment);
    }

    return timelineData;
}

} // namespace jcut::render
