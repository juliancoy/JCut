#include "editor_runtime_qt_bridge.h"

#include "clip_serialization.h"
#include "editor_timeline_types.h"

#include <algorithm>
#include <unordered_set>

namespace {

std::string clipKind(ClipMediaType type)
{
    switch (type) {
    case ClipMediaType::Image:
        return "image";
    case ClipMediaType::Video:
        return "video";
    case ClipMediaType::Audio:
        return "audio";
    case ClipMediaType::Title:
        return "title";
    case ClipMediaType::Unknown:
    default:
        return "unknown";
    }
}

std::string clipLabel(const TimelineClip& clip)
{
    if (!clip.label.trimmed().isEmpty()) {
        return clip.label.toStdString();
    }
    if (!clip.filePath.trimmed().isEmpty()) {
        return clip.filePath.section(QLatin1Char('/'), -1).toStdString();
    }
    return std::string("clip");
}

std::string clipRoleName(ClipRole role)
{
    switch (role) {
    case ClipRole::MaskMatte:
        return "mask_matte";
    case ClipRole::EffectSynth:
        return "effect_synth";
    case ClipRole::SpeakerTitle:
        return "speaker_title";
    case ClipRole::Media:
    default:
        return "media";
    }
}

} // namespace

namespace jcut {

EditorDocumentCore buildEditorDocumentCore(const QString& projectName,
                                           const QVector<TimelineClip>& clips,
                                           const QVector<TimelineTrack>& tracks)
{
    EditorDocumentCore document;
    document.projectName = projectName.toStdString();

    document.tracks.reserve(static_cast<std::size_t>(tracks.size()));
    for (int i = 0; i < tracks.size(); ++i) {
        const TimelineTrack& track = tracks.at(i);
        EditorTrack coreTrack;
        coreTrack.id = i + 1;
        coreTrack.label = track.name.trimmed().isEmpty()
            ? ("Track " + std::to_string(i + 1))
            : track.name.toStdString();
        coreTrack.selected = i == 0;
        coreTrack.height = track.height;
        coreTrack.visualMode = static_cast<int>(track.visualMode);
        coreTrack.gradingPreviewEnabled = track.gradingPreviewEnabled;
        coreTrack.audioEnabled = track.audioEnabled;
        coreTrack.audioBusId = track.audioBusId.toStdString();
        coreTrack.audioGain = track.audioGain;
        coreTrack.audioMuted = track.audioMuted;
        coreTrack.audioSolo = track.audioSolo;
        coreTrack.audioWaveformVisible = track.audioWaveformVisible;
        coreTrack.generatedChildTrack = track.generatedChildTrack;
        coreTrack.parentClipId = track.parentClipId.trimmed().toStdString();
        coreTrack.childClipId = track.childClipId.trimmed().toStdString();
        document.tracks.push_back(std::move(coreTrack));
    }

    std::unordered_set<std::string> seenMediaIds;
    document.clips.reserve(static_cast<std::size_t>(clips.size()));
    int nextClipId = 1;
    for (const TimelineClip& clip : clips) {
        const int trackId = clip.trackIndex + 1;
        const std::string sourcePath = clip.filePath.toStdString();
        if (!sourcePath.empty() && seenMediaIds.insert(sourcePath).second) {
            document.mediaItems.push_back({
                sourcePath,
                clipLabel(clip),
                clipKind(clip.mediaType),
                true,
                clip.hasAudio
            });
        }
        EditorClip coreClip;
        coreClip.id = nextClipId++;
        coreClip.trackId = trackId;
        coreClip.label = clipLabel(clip);
        coreClip.startFrame = static_cast<int>(clip.startFrame);
        coreClip.durationFrames = static_cast<int>(clip.durationFrames);
        coreClip.selected = document.clips.empty();
        coreClip.sourcePath = sourcePath;
        coreClip.persistentId = clip.id.toStdString();
        coreClip.clipRole = clipRoleName(clip.clipRole);
        coreClip.linkedSourceClipId =
            clip.linkedSourceClipId.trimmed().toStdString();
        coreClip.proxyPath = clip.proxyPath.toStdString();
        coreClip.useProxy = clip.useProxy;
        coreClip.mediaKind = clipKind(clip.mediaType);
        coreClip.sourceDurationFrames = clip.sourceDurationFrames;
        coreClip.sourceInFrame = clip.sourceInFrame;
        coreClip.sourceInSubframeSamples = clip.sourceInSubframeSamples;
        coreClip.startSubframeSamples = clip.startSubframeSamples;
        coreClip.durationSubframeSamples = clip.durationSubframeSamples;
        coreClip.sourceFps = clip.sourceFps;
        coreClip.playbackRate = clip.playbackRate;
        coreClip.videoEnabled = clip.videoEnabled;
        coreClip.audioEnabled = clip.audioEnabled;
        coreClip.hasAudio = clip.hasAudio;
        coreClip.audioPresenceKnown = true;
        coreClip.audioSourceMode = clip.audioSourceMode.toStdString();
        coreClip.audioSourcePath = clip.audioSourcePath.toStdString();
        coreClip.audioSourceStatus = clip.audioSourceStatus.toStdString();
        coreClip.audioStreamIndex = clip.audioStreamIndex;
        coreClip.transcriptActiveCutPath = clip.transcriptActiveCutPath.toStdString();
        coreClip.audioBusId = clip.audioBusId.toStdString();
        coreClip.audioGain = clip.audioGain;
        coreClip.audioPan = clip.audioPan;
        coreClip.audioSolo = clip.audioSolo;
        coreClip.audioLinkedToVideo = clip.audioLinkedToVideo;
        coreClip.fadeSamples = std::max(0, clip.fadeSamples);
        coreClip.brightness = clip.brightness;
        coreClip.contrast = clip.contrast;
        coreClip.saturation = clip.saturation;
        coreClip.opacity = clip.opacity;
        coreClip.baseTranslationX = clip.baseTranslationX;
        coreClip.baseTranslationY = clip.baseTranslationY;
        coreClip.baseRotation = clip.baseRotation;
        coreClip.baseScaleX = clip.baseScaleX;
        coreClip.baseScaleY = clip.baseScaleY;
        coreClip.gradingPreviewEnabled = clip.gradingPreviewEnabled;
        coreClip.locked = clip.locked;
        coreClip.maskEnabled = clip.maskEnabled;
        coreClip.maskFramesDir = clip.maskFramesDir.toStdString();
        coreClip.maskFeather = clip.maskFeather;
        coreClip.maskFeatherGamma = clip.maskFeatherGamma;
        coreClip.maskFeatherFalloff = clip.maskFeatherFalloff;
        coreClip.maskDilate = clip.maskDilate;
        coreClip.maskErode = clip.maskErode;
        coreClip.maskBlur = clip.maskBlur;
        coreClip.maskInvert = clip.maskInvert;
        coreClip.maskShowOnly = clip.maskShowOnly;
        coreClip.maskOpacity = clip.maskOpacity;
        coreClip.maskForegroundLayerEnabled = clip.maskForegroundLayerEnabled;
        coreClip.maskRepeatEnabled = clip.maskRepeatEnabled;
        coreClip.maskRepeatDeltaX = clip.maskRepeatDeltaX;
        coreClip.maskRepeatDeltaY = clip.maskRepeatDeltaY;
        coreClip.effectPreset = editor::effectPresetToJson(clip.effectPreset).toStdString();
        coreClip.effectRows = clip.effectRows;
        coreClip.effectSpeed = clip.effectSpeed;
        coreClip.effectScale = clip.effectScale;
        coreClip.effectAlternateDirection = clip.effectAlternateDirection;
        for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
            EditorCorrectionPolygon value;
            value.enabled = polygon.enabled;
            value.startFrame = polygon.startFrame;
            value.endFrame = polygon.endFrame;
            for (const QPointF& point : polygon.pointsNormalized) {
                value.pointsNormalized.push_back({point.x(), point.y()});
            }
            coreClip.correctionPolygons.push_back(std::move(value));
        }
        coreClip.transcriptOverlay.enabled = clip.transcriptOverlay.enabled;
        coreClip.transcriptOverlay.showBackground = clip.transcriptOverlay.showBackground;
        coreClip.transcriptOverlay.backgroundOpacity = clip.transcriptOverlay.backgroundOpacity;
        coreClip.transcriptOverlay.showShadow = clip.transcriptOverlay.showShadow;
        coreClip.transcriptOverlay.highlightCurrentWord = clip.transcriptOverlay.highlightCurrentWord;
        coreClip.transcriptOverlay.autoScroll = clip.transcriptOverlay.autoScroll;
        coreClip.transcriptOverlay.useManualPlacement = clip.transcriptOverlay.useManualPlacement;
        coreClip.transcriptOverlay.translationX = clip.transcriptOverlay.translationX;
        coreClip.transcriptOverlay.translationY = clip.transcriptOverlay.translationY;
        coreClip.transcriptOverlay.boxWidth = clip.transcriptOverlay.boxWidth;
        coreClip.transcriptOverlay.boxHeight = clip.transcriptOverlay.boxHeight;
        coreClip.transcriptOverlay.maxLines = clip.transcriptOverlay.maxLines;
        coreClip.transcriptOverlay.maxCharsPerLine = clip.transcriptOverlay.maxCharsPerLine;
        coreClip.transcriptOverlay.fontFamily = clip.transcriptOverlay.fontFamily.toStdString();
        coreClip.transcriptOverlay.fontPointSize = clip.transcriptOverlay.fontPointSize;
        coreClip.transcriptOverlay.bold = clip.transcriptOverlay.bold;
        coreClip.transcriptOverlay.italic = clip.transcriptOverlay.italic;
        coreClip.transcriptOverlay.textColor = clip.transcriptOverlay.textColor.name(QColor::HexArgb).toStdString();
        coreClip.transcriptOverlay.textOpacity = clip.transcriptOverlay.textOpacity;
        coreClip.transcriptOverlay.backgroundColor = clip.transcriptOverlay.backgroundColor.name(QColor::HexArgb).toStdString();
        coreClip.transcriptOverlay.highlightColor = clip.transcriptOverlay.highlightColor.name(QColor::HexArgb).toStdString();
        coreClip.transcriptOverlay.highlightTextColor = clip.transcriptOverlay.highlightTextColor.name(QColor::HexArgb).toStdString();
        for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
            coreClip.transformKeyframes.push_back({
                keyframe.frame,
                keyframe.title.toStdString(),
                keyframe.translationX,
                keyframe.translationY,
                keyframe.rotation,
                keyframe.scaleX,
                keyframe.scaleY,
                keyframe.linearInterpolation
            });
        }
        for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
            EditorGradingKeyframe value;
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
            const auto copyPoints = [](const QVector<QPointF>& points) {
                std::vector<EditorPoint> result;
                result.reserve(static_cast<std::size_t>(points.size()));
                for (const QPointF& point : points) {
                    result.push_back({point.x(), point.y()});
                }
                if (result.empty()) {
                    result = {{0.0, 0.0}, {1.0, 1.0}};
                }
                return result;
            };
            value.curvePointsR = copyPoints(keyframe.curvePointsR);
            value.curvePointsG = copyPoints(keyframe.curvePointsG);
            value.curvePointsB = copyPoints(keyframe.curvePointsB);
            value.curvePointsLuma = copyPoints(keyframe.curvePointsLuma);
            value.curveThreePointLock = keyframe.curveThreePointLock;
            value.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
            coreClip.gradingKeyframes.push_back(std::move(value));
        }
        for (const TimelineClip::OpacityKeyframe& keyframe : clip.opacityKeyframes) {
            coreClip.opacityKeyframes.push_back({
                keyframe.frame,
                keyframe.opacity,
                keyframe.linearInterpolation
            });
        }
        for (const TimelineClip::TitleKeyframe& keyframe : clip.titleKeyframes) {
            coreClip.titleKeyframes.push_back({
                keyframe.frame,
                keyframe.text.toStdString(),
                keyframe.translationX,
                keyframe.translationY,
                keyframe.fontSize,
                keyframe.opacity,
                keyframe.fontFamily.toStdString(),
                keyframe.bold,
                keyframe.italic,
                keyframe.color.name(QColor::HexArgb).toStdString(),
                keyframe.linearInterpolation
            });
        }
        document.clips.push_back(std::move(coreClip));
    }

    return document;
}

} // namespace jcut
