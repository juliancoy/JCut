#include "editor_effect_presets.h"

#include "timeline_fps.h"

#include <QHash>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

bool clipRangesOverlap(const TimelineClip& a, const TimelineClip& b)
{
    const int64_t aStart = a.startFrame;
    const int64_t aEnd = a.startFrame + qMax<int64_t>(1, a.durationFrames);
    const int64_t bStart = b.startFrame;
    const int64_t bEnd = b.startFrame + qMax<int64_t>(1, b.durationFrames);
    return aStart < bEnd && bStart < aEnd;
}

bool clipIdExists(const QVector<TimelineClip>& clips, const QString& id)
{
    for (const TimelineClip& clip : clips) {
        if (clip.id == id) {
            return true;
        }
    }
    return false;
}

bool trackNameStartsWith(const QVector<TimelineTrack>& tracks, int trackIndex, const QString& prefix)
{
    return trackIndex >= 0 &&
           trackIndex < tracks.size() &&
           tracks.at(trackIndex).name.trimmed().startsWith(prefix, Qt::CaseInsensitive);
}

QString nextGeneratedTrackName(const QVector<TimelineTrack>& tracks, const QString& baseName)
{
    int count = 0;
    for (const TimelineTrack& track : tracks) {
        if (track.name.trimmed().startsWith(baseName, Qt::CaseInsensitive)) {
            ++count;
        }
    }
    return count <= 0 ? baseName : QStringLiteral("%1 %2").arg(baseName).arg(count + 1);
}

bool wouldClipConflictWithTrack(const QVector<TimelineClip>& clips, const TimelineClip& clip, int trackIndex)
{
    for (const TimelineClip& existing : clips) {
        if (existing.trackIndex == trackIndex && clipRangesOverlap(existing, clip)) {
            return true;
        }
    }
    return false;
}

bool transformKeyframesEqual(const QVector<TimelineClip::TransformKeyframe>& a,
                             const QVector<TimelineClip::TransformKeyframe>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        const TimelineClip::TransformKeyframe& left = a.at(i);
        const TimelineClip::TransformKeyframe& right = b.at(i);
        if (left.frame != right.frame ||
            left.title != right.title ||
            left.translationX != right.translationX ||
            left.translationY != right.translationY ||
            left.rotation != right.rotation ||
            left.scaleX != right.scaleX ||
            left.scaleY != right.scaleY ||
            left.linearInterpolation != right.linearInterpolation ||
            left.maskRepeatDeltaX != right.maskRepeatDeltaX ||
            left.maskRepeatDeltaY != right.maskRepeatDeltaY) {
            return false;
        }
    }
    return true;
}

int appendGeneratedTrack(QVector<TimelineTrack>& tracks, const QString& trackBaseName)
{
    TimelineTrack track;
    track.name = nextGeneratedTrackName(tracks, trackBaseName);
    track.audioEnabled = false;
    track.audioWaveformVisible = false;
    tracks.push_back(track);
    return tracks.size() - 1;
}

bool isSupportedMaskMatteParent(const TimelineClip& clip)
{
    return clip.clipRole == ClipRole::Media &&
           clip.mediaType == ClipMediaType::Video &&
           !clip.id.trimmed().isEmpty() &&
           !clip.filePath.trimmed().isEmpty();
}

QString effectiveMaskSidecarId(const TimelineClip& clip)
{
    const QString directory = clip.maskFramesDir.trimmed();
    const QString persistedId = clip.generatedFromMaskId.trimmed();
    if (!persistedId.isEmpty() && persistedId != directory) {
        return persistedId;
    }
    return directory.isEmpty()
        ? QString()
        : editor::masks::stableMaskSidecarId(directory);
}

} // namespace

QVector<EffectPresetUiOption> effectPresetUiOptions()
{
    return {
        {QStringLiteral("Off"), ClipEffectPreset::None, QStringLiteral("General")},

        {QStringLiteral("Mirror ring"), ClipEffectPreset::MirrorRing, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Kaleidoscope"), ClipEffectPreset::Kaleidoscope, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Quad mirror"), ClipEffectPreset::QuadMirror, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Infinite mirror"), ClipEffectPreset::InfiniteMirror, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Tessellation"), ClipEffectPreset::Tessellation, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Hexagonal prism"), ClipEffectPreset::HexagonalPrism, QStringLiteral("Mirror & Geometry")},
        {QStringLiteral("Droste"), ClipEffectPreset::Droste, QStringLiteral("Mirror & Geometry")},

        {QStringLiteral("Polar tunnel"), ClipEffectPreset::PolarTunnel, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Tiny planet"), ClipEffectPreset::TinyPlanet, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Twirl / vortex"), ClipEffectPreset::TwirlVortex, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Ripple / shockwave"), ClipEffectPreset::RippleShockwave, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Displacement map"), ClipEffectPreset::DisplacementMap, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Glass / refraction"), ClipEffectPreset::GlassRefraction, QStringLiteral("Warp & Distort")},
        {QStringLiteral("Progressive edge stretch"), ClipEffectPreset::ProgressiveEdgeStretch, QStringLiteral("Warp & Distort")},

        {QStringLiteral("Temporal echo"), ClipEffectPreset::TemporalEcho, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Slit scan"), ClipEffectPreset::SlitScan, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Freeze pattern"), ClipEffectPreset::FreezePattern, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Step repeater"), ClipEffectPreset::StepRepeat, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Source image tiling"), ClipEffectPreset::SourceTile, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Logo ticker rows"), ClipEffectPreset::NewsLogoTicker, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Directional trim ticker"), ClipEffectPreset::DirectionalTrimTicker, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Alternating motion background"), ClipEffectPreset::AlternatingMotionBackground, QStringLiteral("Time & Repeat")},
        {QStringLiteral("Encircle person"), ClipEffectPreset::PersonOrbit, QStringLiteral("Time & Repeat")},

        {QStringLiteral("Pixel sorting"), ClipEffectPreset::PixelSorting, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("Datamosh / glitch blocks"), ClipEffectPreset::DatamoshGlitch, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("RGB split / chromatic aberration"), ClipEffectPreset::RgbSplit, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("Halftone mosaic"), ClipEffectPreset::HalftoneMosaic, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("Vulkan 3D Synth"), ClipEffectPreset::Vulkan3DSynth, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("Sobel edge filter"), ClipEffectPreset::SobelEdges, QStringLiteral("Glitch & Stylize")},
        {QStringLiteral("Neon glow"), ClipEffectPreset::NeonGlow, QStringLiteral("Glitch & Stylize")},

        {QStringLiteral("Speaker mask dilation"), ClipEffectPreset::SpeakerMaskDilation, QStringLiteral("Speaker Mask")},
        {QStringLiteral("Speaker mask dilation pulse"), ClipEffectPreset::SpeakerMaskDilationPulse, QStringLiteral("Speaker Mask")},
        {QStringLiteral("Speaker mask dilation rings"), ClipEffectPreset::SpeakerMaskDilationRings, QStringLiteral("Speaker Mask")},

        {QStringLiteral("Difference matte"), ClipEffectPreset::DifferenceMatte, QStringLiteral("Keying & Matte")},
    };
}

QVector<TilingPatternUiOption> tilingPatternUiOptions()
{
    return {
        {QStringLiteral("Grid"), ClipTilingPattern::Grid},
        {QStringLiteral("Encircle"), ClipTilingPattern::Encircle},
        {QStringLiteral("Spiral XY"), ClipTilingPattern::SpiralXY},
        {QStringLiteral("Spiral XZ"), ClipTilingPattern::SpiralXZ},
        {QStringLiteral("Spiral YZ"), ClipTilingPattern::SpiralYZ},
        {QStringLiteral("Diamond"), ClipTilingPattern::Diamond},
    };
}

bool effectPresetUsesDirectionalControl(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::NewsLogoTicker:
    case ClipEffectPreset::AlternatingMotionBackground:
    case ClipEffectPreset::DirectionalTrimTicker:
    case ClipEffectPreset::SourceTile:
    case ClipEffectPreset::Vulkan3DSynth:
    case ClipEffectPreset::ProgressiveEdgeStretch:
        return true;
    case ClipEffectPreset::None:
    case ClipEffectPreset::PersonOrbit:
    case ClipEffectPreset::FreezePattern:
    case ClipEffectPreset::StepRepeat:
    case ClipEffectPreset::DifferenceMatte:
    case ClipEffectPreset::TemporalEcho:
    case ClipEffectPreset::MirrorRing:
    case ClipEffectPreset::Tessellation:
    case ClipEffectPreset::Kaleidoscope:
    case ClipEffectPreset::HexagonalPrism:
    case ClipEffectPreset::Droste:
    case ClipEffectPreset::PolarTunnel:
    case ClipEffectPreset::TinyPlanet:
    case ClipEffectPreset::InfiniteMirror:
    case ClipEffectPreset::QuadMirror:
    case ClipEffectPreset::SlitScan:
    case ClipEffectPreset::DisplacementMap:
    case ClipEffectPreset::TwirlVortex:
    case ClipEffectPreset::RippleShockwave:
    case ClipEffectPreset::PixelSorting:
    case ClipEffectPreset::DatamoshGlitch:
    case ClipEffectPreset::RgbSplit:
    case ClipEffectPreset::HalftoneMosaic:
    case ClipEffectPreset::GlassRefraction:
    case ClipEffectPreset::SobelEdges:
    case ClipEffectPreset::NeonGlow:
    case ClipEffectPreset::SpeakerMaskDilation:
    case ClipEffectPreset::SpeakerMaskDilationPulse:
    case ClipEffectPreset::SpeakerMaskDilationRings:
    default:
        return false;
    }
}

bool effectPresetUsesTilingControls(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::SourceTile;
}

TimelineClip makeMaskMatteClip(const TimelineClip& sourceClip)
{
    TimelineClip maskClip = sourceClip;
    const TimelineClip defaults;
    const QString sourceId = sourceClip.id.trimmed();
    const QString maskId = sourceClip.maskFramesDir.trimmed();
    maskClip.id = sourceId.isEmpty()
                      ? QStringLiteral("generated-mask-matte")
                      : QStringLiteral("%1-mask-matte").arg(sourceId);
    maskClip.clipRole = ClipRole::MaskMatte;
    maskClip.linkedSourceClipId = sourceId;
    maskClip.generatedFromMaskId = maskId.isEmpty()
        ? QString()
        : editor::masks::stableMaskSidecarId(maskId);
    maskClip.syncLockedToSource = true;
    maskClip.sourceTransformLocked = true;
    maskClip.label = sourceClip.label.trimmed().isEmpty()
                         ? QStringLiteral("Mask Matte")
                         : QStringLiteral("%1 Mask Matte").arg(sourceClip.label.trimmed());
    maskClip.locked = true;
    maskClip.videoEnabled = true;
    maskClip.hasAudio = false;
    maskClip.audioEnabled = false;
    maskClip.audioLinkedToVideo = false;
    maskClip.audioBusId.clear();
    maskClip.audioSourcePath.clear();
    maskClip.audioSourceOriginalPath.clear();
    maskClip.audioSourceStatus = QStringLiteral("generated");
    maskClip.audioStreamIndex = -1;
    maskClip.audioSourceMode = defaults.audioSourceMode;
    maskClip.audioGain = defaults.audioGain;
    maskClip.audioPan = defaults.audioPan;
    maskClip.audioSolo = defaults.audioSolo;
    maskClip.audioSourceLastVerifiedMs = 0;
    maskClip.maskEnabled = sourceClip.maskEnabled && !maskId.isEmpty();
    maskClip.maskFramesDir = maskId;
    maskClip.maskShowOnly = false;
    maskClip.maskForegroundLayerEnabled = false;
    maskClip.effectPreset = ClipEffectPreset::None;
    maskClip.effectRows = defaults.effectRows;
    maskClip.effectSpeed = defaults.effectSpeed;
    maskClip.effectScale = defaults.effectScale;
    maskClip.effectAlternateDirection = defaults.effectAlternateDirection;
    maskClip.effectSkipAwareTiming = defaults.effectSkipAwareTiming;
    maskClip.maskRepeatEnabled = defaults.maskRepeatEnabled;
    maskClip.maskRepeatDeltaX = defaults.maskRepeatDeltaX;
    maskClip.maskRepeatDeltaY = defaults.maskRepeatDeltaY;
    maskClip.differenceReferenceFrames = defaults.differenceReferenceFrames;
    maskClip.differenceThreshold = defaults.differenceThreshold;
    maskClip.differenceSoftness = defaults.differenceSoftness;
    maskClip.temporalEchoCount = defaults.temporalEchoCount;
    maskClip.temporalEchoSpacingFrames = defaults.temporalEchoSpacingFrames;
    maskClip.temporalEchoDecay = defaults.temporalEchoDecay;
    maskClip.tilingPattern = defaults.tilingPattern;
    maskClip.tilingSpacing = defaults.tilingSpacing;
    maskClip.tilingWrap = defaults.tilingWrap;
    maskClip.effectParameterSets = {};
    // Opacity, corrections, effects, and overlays are child-owned. Start them
    // from a neutral state instead of accidentally cloning the parent's
    // evaluated visual treatment into a second layer.
    maskClip.opacity = defaults.opacity;
    maskClip.opacityKeyframes.clear();
    maskClip.correctionPolygons.clear();
    maskClip.titleKeyframes.clear();
    maskClip.speakerTitleEngineActive = false;
    maskClip.transcriptOverlay = TimelineClip::TranscriptOverlaySettings{};
    // A mask matte is independently gradeable. Do not inherit the source
    // clip's grade; migrate the former masked-area grade into its standard
    // grading model instead.
    maskClip.brightness = sourceClip.maskGradeEnabled ? sourceClip.maskGradeBrightness : 0.0;
    maskClip.contrast = sourceClip.maskGradeEnabled ? sourceClip.maskGradeContrast : 1.0;
    maskClip.saturation = sourceClip.maskGradeEnabled ? sourceClip.maskGradeSaturation : 1.0;
    maskClip.gradingKeyframes.clear();
    if (sourceClip.maskGradeEnabled) {
        TimelineClip::GradingKeyframe grade;
        grade.frame = 0;
        grade.brightness = sourceClip.maskGradeBrightness;
        grade.contrast = sourceClip.maskGradeContrast;
        grade.saturation = sourceClip.maskGradeSaturation;
        grade.curvePointsR = sourceClip.maskGradeCurvePointsR;
        grade.curvePointsG = sourceClip.maskGradeCurvePointsG;
        grade.curvePointsB = sourceClip.maskGradeCurvePointsB;
        grade.curvePointsLuma = sourceClip.maskGradeCurvePointsLuma;
        grade.curveSmoothingEnabled = sourceClip.maskGradeCurveSmoothingEnabled;
        maskClip.gradingKeyframes = {grade};
    }
    maskClip.maskGradeEnabled = false;
    maskClip.maskGradeBrightness = defaults.maskGradeBrightness;
    maskClip.maskGradeContrast = defaults.maskGradeContrast;
    maskClip.maskGradeSaturation = defaults.maskGradeSaturation;
    maskClip.maskGradeCurvePointsR.clear();
    maskClip.maskGradeCurvePointsG.clear();
    maskClip.maskGradeCurvePointsB.clear();
    maskClip.maskGradeCurvePointsLuma.clear();
    maskClip.maskGradeCurveSmoothingEnabled = defaults.maskGradeCurveSmoothingEnabled;
    maskClip.trackIndex = sourceClip.trackIndex;
    maskClip.zLevel = effectiveClipZLevel(sourceClip) + 1;
    maskClip.zLevelUserSet = false;
    return maskClip;
}

bool setMaskSidecarAssociation(TimelineClip& clip, const QString& directory)
{
    const QString normalizedDirectory = directory.trimmed();
    const QString stableId = normalizedDirectory.isEmpty()
        ? QString()
        : editor::masks::stableMaskSidecarId(normalizedDirectory);
    if (clip.maskFramesDir == normalizedDirectory &&
        clip.generatedFromMaskId == stableId) {
        return false;
    }
    clip.maskFramesDir = normalizedDirectory;
    clip.generatedFromMaskId = stableId;
    return true;
}

QString maskMatteChildIdForSidecar(const QVector<TimelineClip>& clips,
                                   const QString& sourceClipId,
                                   const QString& stableSidecarId)
{
    const QString normalizedSourceId = sourceClipId.trimmed();
    const QString normalizedSidecarId = stableSidecarId.trimmed();
    if (normalizedSourceId.isEmpty() || normalizedSidecarId.isEmpty()) {
        return {};
    }
    for (const TimelineClip& clip : clips) {
        if (clip.clipRole == ClipRole::MaskMatte &&
            clip.linkedSourceClipId.trimmed() == normalizedSourceId &&
            effectiveMaskSidecarId(clip) == normalizedSidecarId) {
            return clip.id;
        }
    }
    return {};
}

TimelineClip makeMaskMatteClip(const TimelineClip& sourceClip,
                               const editor::masks::MaskSidecar& sidecar,
                               int zLevel)
{
    TimelineClip maskClip = makeMaskMatteClip(sourceClip);
    const QString sourceId = sourceClip.id.trimmed();
    maskClip.id = QStringLiteral("%1-mask-%2").arg(
        sourceId.isEmpty() ? QStringLiteral("generated") : sourceId,
        sidecar.id);
    maskClip.generatedFromMaskId = sidecar.id;
    maskClip.maskFramesDir = sidecar.directory;
    maskClip.maskEnabled = true;
    maskClip.maskSidecarAvailable = sidecar.isReadyForTimeline();
    maskClip.maskSidecarAvailabilityIssue = sidecar.readinessIssue;
    maskClip.trackIndex = sourceClip.trackIndex;
    maskClip.zLevel = zLevel;
    const QString sourceLabel = sourceClip.label.trimmed().isEmpty()
        ? QFileInfo(sourceClip.filePath).completeBaseName()
        : sourceClip.label.trimmed();
    maskClip.label = QStringLiteral("%1 · %2 Mask").arg(
        sourceLabel,
        sidecar.displayName.trimmed().isEmpty()
            ? QStringLiteral("Generated")
            : sidecar.displayName.trimmed());
    return maskClip;
}

bool normalizeMaskMatteClips(QVector<TimelineClip>& clips)
{
    QHash<QString, QString> sourceMaskDirectoryById;
    for (const TimelineClip& clip : std::as_const(clips)) {
        if (isSupportedMaskMatteParent(clip)) {
            sourceMaskDirectoryById.insert(clip.id.trimmed(), clip.maskFramesDir.trimmed());
        }
    }

    // The first child in persisted timeline order owns the association. This
    // deterministic rule removes malformed duplicates without merging or
    // overwriting any child-owned visual state on the retained clip.
    QSet<QString> seenAssociations;
    bool changed = false;
    const qsizetype originalSize = clips.size();
    clips.erase(std::remove_if(clips.begin(), clips.end(), [&](TimelineClip& clip) {
        if (clip.clipRole != ClipRole::MaskMatte) {
            return false;
        }

        const QString sourceId = clip.linkedSourceClipId.trimmed();
        if (!sourceMaskDirectoryById.contains(sourceId)) {
            return true;
        }

        QString directory = clip.maskFramesDir.trimmed();
        if (directory.isEmpty()) {
            directory = sourceMaskDirectoryById.value(sourceId);
            if (!directory.isEmpty() && clip.maskFramesDir != directory) {
                clip.maskFramesDir = directory;
                changed = true;
            }
        }

        const QString stableId = effectiveMaskSidecarId(clip);
        if (stableId.isEmpty()) {
            return true;
        }
        if (clip.generatedFromMaskId != stableId) {
            clip.generatedFromMaskId = stableId;
            changed = true;
        }

        const QString associationKey = sourceId + QChar(0x1f) + stableId;
        if (seenAssociations.contains(associationKey)) {
            return true;
        }
        seenAssociations.insert(associationKey);
        return false;
    }), clips.end());

    QHash<QString, int> sourceIndexById;
    for (int i = 0; i < clips.size(); ++i) {
        const QString id = clips.at(i).id.trimmed();
        if (isSupportedMaskMatteParent(clips.at(i))) {
            sourceIndexById.insert(id, i);
        }
    }

    changed = clips.size() != originalSize || changed;
    auto assignIfChanged = [&changed](auto& field, const auto& value) {
        if (field == value) {
            return;
        }
        field = value;
        changed = true;
    };

    for (TimelineClip& clip : clips) {
        if (clip.clipRole != ClipRole::MaskMatte) {
            continue;
        }

        const QString sourceId = clip.linkedSourceClipId.trimmed();
        assignIfChanged(clip.syncLockedToSource, true);
        assignIfChanged(clip.sourceTransformLocked, true);
        assignIfChanged(clip.locked, true);
        assignIfChanged(clip.videoEnabled, true);
        assignIfChanged(clip.hasAudio, false);
        assignIfChanged(clip.audioEnabled, false);
        assignIfChanged(clip.audioLinkedToVideo, false);
        assignIfChanged(clip.audioBusId, QString());
        assignIfChanged(clip.audioSourcePath, QString());
        assignIfChanged(clip.audioSourceOriginalPath, QString());
        assignIfChanged(clip.audioSourceStatus, QStringLiteral("generated"));
        assignIfChanged(clip.audioStreamIndex, -1);
        assignIfChanged(clip.maskForegroundLayerEnabled, false);
        assignIfChanged(clip.speakerTitleEngineActive, false);
        assignIfChanged(clip.transcriptOverlay.enabled, false);
        assignIfChanged(clip.transcriptOverlay.showSpeakerTitle, false);
        if (!clip.titleKeyframes.isEmpty()) {
            clip.titleKeyframes.clear();
            changed = true;
        }

        if (!sourceIndexById.contains(sourceId)) {
            continue;
        }

        TimelineClip& source = clips[sourceIndexById.value(sourceId)];
        // A generated matte is an independent child layer. The source remains
        // an ordinary full-frame layer and supplies decoded media only.
        assignIfChanged(source.maskForegroundLayerEnabled, false);
        assignIfChanged(source.maskShowOnly, false);

        assignIfChanged(clip.filePath, source.filePath);
        assignIfChanged(clip.proxyPath, source.proxyPath);
        assignIfChanged(clip.useProxy, source.useProxy);
        assignIfChanged(clip.mediaType, source.mediaType);
        assignIfChanged(clip.sourceKind, source.sourceKind);
        assignIfChanged(clip.sourceFps, source.sourceFps);
        assignIfChanged(clip.sourceDurationFrames, source.sourceDurationFrames);
        assignIfChanged(clip.sourceFrameSize, source.sourceFrameSize);
        assignIfChanged(clip.sourceInFrame, source.sourceInFrame);
        assignIfChanged(clip.sourceInSubframeSamples, source.sourceInSubframeSamples);
        assignIfChanged(clip.startFrame, source.startFrame);
        assignIfChanged(clip.startSubframeSamples, source.startSubframeSamples);
        assignIfChanged(clip.durationFrames, source.durationFrames);
        assignIfChanged(clip.durationSubframeSamples, source.durationSubframeSamples);
        assignIfChanged(clip.playbackRate, source.playbackRate);
        assignIfChanged(clip.baseTranslationX, source.baseTranslationX);
        assignIfChanged(clip.baseTranslationY, source.baseTranslationY);
        assignIfChanged(clip.baseRotation, source.baseRotation);
        assignIfChanged(clip.baseScaleX, source.baseScaleX);
        assignIfChanged(clip.baseScaleY, source.baseScaleY);
        assignIfChanged(clip.transformSkipAwareTiming, source.transformSkipAwareTiming);
        if (!transformKeyframesEqual(clip.transformKeyframes, source.transformKeyframes)) {
            clip.transformKeyframes = source.transformKeyframes;
            changed = true;
        }
        if (clip.maskFramesDir.trimmed().isEmpty()) {
            assignIfChanged(clip.maskFramesDir, source.maskFramesDir);
        }
        if (clip.generatedFromMaskId.trimmed().isEmpty() &&
            !clip.maskFramesDir.trimmed().isEmpty()) {
            assignIfChanged(clip.generatedFromMaskId,
                            editor::masks::stableMaskSidecarId(clip.maskFramesDir));
        }
        if (clip.zLevel == TimelineClip::kAutomaticZLevel) {
            assignIfChanged(clip.zLevel, effectiveClipZLevel(source) + 1);
        }
        const bool migrateLegacyMaskGrade = source.maskGradeEnabled || clip.maskGradeEnabled;
        if (migrateLegacyMaskGrade) {
            const TimelineClip& legacy = source.maskGradeEnabled ? source : clip;
            TimelineClip::GradingKeyframe grade;
            grade.frame = 0;
            grade.brightness = legacy.maskGradeBrightness;
            grade.contrast = legacy.maskGradeContrast;
            grade.saturation = legacy.maskGradeSaturation;
            grade.curvePointsR = legacy.maskGradeCurvePointsR;
            grade.curvePointsG = legacy.maskGradeCurvePointsG;
            grade.curvePointsB = legacy.maskGradeCurvePointsB;
            grade.curvePointsLuma = legacy.maskGradeCurvePointsLuma;
            grade.curveSmoothingEnabled = legacy.maskGradeCurveSmoothingEnabled;
            clip.brightness = grade.brightness;
            clip.contrast = grade.contrast;
            clip.saturation = grade.saturation;
            clip.gradingKeyframes = {grade};
            source.maskGradeEnabled = false;
            clip.maskGradeEnabled = false;
            changed = true;
        }
        assignIfChanged(clip.maskGradeEnabled, false);
    }

    return changed;
}

bool reconcileMaskMatteChildrenFromDisk(QVector<TimelineClip>& clips,
                                        bool* runtimeAvailabilityChanged)
{
    bool persistentChanged = normalizeMaskMatteClips(clips);
    bool availabilityChanged = false;
    auto setAvailability = [&availabilityChanged](TimelineClip& clip,
                                                  bool available,
                                                  const QString& issue) {
        if (clip.maskSidecarAvailable != available) {
            clip.maskSidecarAvailable = available;
            availabilityChanged = true;
        }
        if (clip.maskSidecarAvailabilityIssue != issue) {
            clip.maskSidecarAvailabilityIssue = issue;
            availabilityChanged = true;
        }
    };
    const QVector<TimelineClip> snapshot = clips;
    for (const TimelineClip& source : snapshot) {
        if (!isSupportedMaskMatteParent(source)) {
            continue;
        }
        const QVector<editor::masks::MaskSidecar> sidecars =
            editor::masks::discoverMaskSidecars(source);
        QSet<QString> discoveredIds;
        for (const editor::masks::MaskSidecar& sidecar : sidecars) {
            discoveredIds.insert(sidecar.id);
        }
        for (TimelineClip& existing : clips) {
            if (existing.clipRole != ClipRole::MaskMatte ||
                existing.linkedSourceClipId.trimmed() != source.id.trimmed()) {
                continue;
            }
            const QString sidecarId = effectiveMaskSidecarId(existing);
            if (!sidecarId.isEmpty() && discoveredIds.contains(sidecarId)) {
                continue;
            }
            const QFileInfo directory(existing.maskFramesDir.trimmed());
            setAvailability(
                existing,
                false,
                directory.isDir()
                    ? QStringLiteral("No mask frames")
                    : QStringLiteral("Sidecar missing"));
        }
        int nextZ = effectiveClipZLevel(source) + 1;
        for (const editor::masks::MaskSidecar& sidecar : sidecars) {
            auto existing = std::find_if(clips.begin(), clips.end(), [&](const TimelineClip& clip) {
                if (clip.clipRole != ClipRole::MaskMatte ||
                    clip.linkedSourceClipId.trimmed() != source.id.trimmed()) {
                    return false;
                }
                return clip.generatedFromMaskId.trimmed() == sidecar.id ||
                       editor::masks::stableMaskSidecarId(clip.maskFramesDir) == sidecar.id;
            });
            if (!sidecar.isReadyForTimeline()) {
                // Preserve a previously materialized child and its edits, but
                // never turn transient artifact availability into a durable
                // change to the user's maskEnabled intent.
                if (existing != clips.end()) {
                    setAvailability(*existing, false, sidecar.readinessIssue);
                    ++nextZ;
                }
                continue;
            }
            if (existing == clips.end()) {
                clips.push_back(makeMaskMatteClip(source, sidecar, nextZ));
                persistentChanged = true;
            } else {
                setAvailability(*existing, true, QString());
                if (existing->maskFramesDir != sidecar.directory) {
                    existing->maskFramesDir = sidecar.directory;
                    persistentChanged = true;
                }
                if (existing->generatedFromMaskId != sidecar.id) {
                    existing->generatedFromMaskId = sidecar.id;
                    persistentChanged = true;
                }
                if (!existing->zLevelUserSet && existing->zLevel != nextZ) {
                    existing->zLevel = nextZ;
                    persistentChanged = true;
                }
                // Replace only the old generated label. A user-supplied label
                // remains untouched, while migrated children become
                // distinguishable when more than one disk sidecar exists.
                const bool hasLegacyGeneratedLabel =
                    existing->id.endsWith(QStringLiteral("-mask-matte")) ||
                    existing->label.trimmed().endsWith(QStringLiteral(" Mask Matte"));
                if (hasLegacyGeneratedLabel) {
                    const QString sourceLabel = source.label.trimmed().isEmpty()
                        ? QFileInfo(source.filePath).completeBaseName()
                        : source.label.trimmed();
                    const QString desiredLabel = QStringLiteral("%1 · %2 Mask").arg(
                        sourceLabel,
                        sidecar.displayName.trimmed().isEmpty()
                            ? QStringLiteral("Generated")
                            : sidecar.displayName.trimmed());
                    if (existing->label != desiredLabel) {
                        existing->label = desiredLabel;
                        persistentChanged = true;
                    }
                }
            }
            ++nextZ;
        }
    }
    persistentChanged = normalizeMaskMatteClips(clips) || persistentChanged;
    if (runtimeAvailabilityChanged) {
        *runtimeAvailabilityChanged = availabilityChanged;
    }
    return persistentChanged;
}

bool migrateLegacyMaskGradingToMattes(QVector<TimelineClip>& clips)
{
    bool hasLegacyGrade = false;
    for (const TimelineClip& clip : std::as_const(clips)) {
        hasLegacyGrade = hasLegacyGrade || clip.maskGradeEnabled;
    }
    if (!hasLegacyGrade) return false;
    return normalizeMaskMatteClips(clips);
}

TimelineClip makeAlternatingMotionBackgroundClip(const TimelineClip& sourceClip, int trackIndex)
{
    TimelineClip effectClip = sourceClip;
    const QString sourceId = sourceClip.id.trimmed();
    effectClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    effectClip.clipRole = ClipRole::EffectSynth;
    effectClip.linkedSourceClipId = sourceId;
    effectClip.generatedFromMaskId.clear();
    effectClip.syncLockedToSource = false;
    effectClip.sourceTransformLocked = false;
    effectClip.label = sourceClip.label.trimmed().isEmpty()
                           ? QStringLiteral("Alternating Motion Background")
                           : QStringLiteral("%1 Motion Background").arg(sourceClip.label.trimmed());
    effectClip.trackIndex = qMax(0, trackIndex);
    effectClip.hasAudio = false;
    effectClip.audioEnabled = false;
    effectClip.audioLinkedToVideo = false;
    effectClip.audioBusId.clear();
    effectClip.audioSourcePath.clear();
    effectClip.audioSourceOriginalPath.clear();
    effectClip.audioSourceStatus = QStringLiteral("generated");
    effectClip.audioStreamIndex = -1;
    effectClip.maskEnabled = false;
    effectClip.maskFramesDir.clear();
    effectClip.maskShowOnly = false;
    effectClip.maskForegroundLayerEnabled = false;
    effectClip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;
    effectClip.effectRows = qBound(1, sourceClip.effectRows == 32 ? 8 : sourceClip.effectRows, 96);
    effectClip.effectSpeed = qBound<qreal>(-8.0, sourceClip.effectSpeed, 8.0);
    effectClip.effectScale = qBound<qreal>(0.1, sourceClip.effectScale, 8.0);
    effectClip.effectAlternateDirection = true;
    return effectClip;
}

TimelineClip makeSourceTilingClip(const TimelineClip& sourceClip, int trackIndex)
{
    TimelineClip effectClip = sourceClip;
    const QString sourceId = sourceClip.id.trimmed();
    effectClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    effectClip.clipRole = ClipRole::EffectSynth;
    effectClip.linkedSourceClipId = sourceId;
    effectClip.generatedFromMaskId.clear();
    effectClip.syncLockedToSource = false;
    effectClip.sourceTransformLocked = false;
    effectClip.label = sourceClip.label.trimmed().isEmpty()
                           ? QStringLiteral("Source Tiling")
                           : QStringLiteral("%1 Tiling").arg(sourceClip.label.trimmed());
    effectClip.trackIndex = qMax(0, trackIndex);
    effectClip.hasAudio = false;
    effectClip.audioEnabled = false;
    effectClip.audioLinkedToVideo = false;
    effectClip.audioBusId.clear();
    effectClip.audioSourcePath.clear();
    effectClip.audioSourceOriginalPath.clear();
    effectClip.audioSourceStatus = QStringLiteral("generated");
    effectClip.audioStreamIndex = -1;
    effectClip.maskEnabled = false;
    effectClip.maskFramesDir.clear();
    effectClip.maskShowOnly = false;
    effectClip.maskForegroundLayerEnabled = false;
    effectClip.effectPreset = ClipEffectPreset::SourceTile;
    effectClip.effectRows = qBound(1, sourceClip.effectRows == 32 ? 6 : sourceClip.effectRows, 96);
    effectClip.effectSpeed = 0.0;
    effectClip.effectScale = qBound<qreal>(0.1, sourceClip.effectScale, 8.0);
    effectClip.effectAlternateDirection = false;
    effectClip.tilingPattern = ClipTilingPattern::Grid;
    effectClip.tilingSpacing = 1.0;
    effectClip.tilingWrap = true;
    return effectClip;
}

QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    int64_t titleDurationFrames,
    int64_t titleStartDelayFrames)
{
    SpeakerTitleFlyInSettings settings;
    settings.titleDurationFrames = titleDurationFrames;
    settings.titleStartDelayFrames = titleStartDelayFrames;
    return makeSpeakerTitleClipsForTranscriptIntroductions(
        sourceClip,
        transcriptPath,
        sections,
        trackIndex,
        settings);
}

QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    const SpeakerTitleFlyInSettings& settings)
{
    QVector<TimelineClip> clips;
    if (sections.isEmpty() || sourceClip.durationFrames <= 0) {
        return clips;
    }

    const int64_t boundedDuration = qMax<int64_t>(kTimelineFps, settings.titleDurationFrames);
    const int64_t startDelay = qMax<int64_t>(0, settings.titleStartDelayFrames);
    const int64_t sourceStart = sourceClip.sourceInFrame;
    const int64_t sourceEndExclusive =
        sourceStart + qMax<int64_t>(1, qRound64(sourceClip.durationFrames * qMax<qreal>(0.001, sourceClip.playbackRate)));
    const int64_t clipTimelineEnd = sourceClip.startFrame + sourceClip.durationFrames;
    QVector<ExportRangeSegment> keptSourceRanges;
    for (const TranscriptSection& transcriptSection : sections) {
        for (const TranscriptWord& transcriptWord : transcriptSection.words) {
            if (!transcriptWord.skipped && !transcriptWord.text.trimmed().isEmpty()) {
                keptSourceRanges.push_back({transcriptWord.startFrame, transcriptWord.endFrame});
            }
        }
    }
    std::sort(keptSourceRanges.begin(), keptSourceRanges.end(), [](const auto& a, const auto& b) {
        return a.startFrame < b.startFrame;
    });
    QVector<ExportRangeSegment> mergedKeptSourceRanges;
    for (const ExportRangeSegment& range : std::as_const(keptSourceRanges)) {
        if (mergedKeptSourceRanges.isEmpty() ||
            range.startFrame > mergedKeptSourceRanges.constLast().endFrame + 1) {
            mergedKeptSourceRanges.push_back(range);
        } else {
            mergedKeptSourceRanges.last().endFrame =
                qMax(mergedKeptSourceRanges.constLast().endFrame, range.endFrame);
        }
    }
    const auto rawEndForPlaybackFrames = [&](int64_t introductionSourceFrame,
                                             int64_t playbackFrames) -> int64_t {
        int64_t remaining = qMax<int64_t>(1, playbackFrames);
        int64_t lastMappedEnd = qMin<int64_t>(clipTimelineEnd, sourceClip.startFrame + 1);
        for (const ExportRangeSegment& range : std::as_const(mergedKeptSourceRanges)) {
            const int64_t rangeStart = qMax(range.startFrame, introductionSourceFrame);
            if (range.endFrame < rangeStart) continue;
            lastMappedEnd = qMin<int64_t>(
                clipTimelineEnd,
                sourceClip.startFrame + qCeil(
                    (range.endFrame - sourceClip.sourceInFrame + 1) /
                    qMax<qreal>(0.001, sourceClip.playbackRate)));
            const int64_t availableTimelineFrames = qMax<int64_t>(
                1, qCeil((range.endFrame - rangeStart + 1) /
                         qMax<qreal>(0.001, sourceClip.playbackRate)));
            if (remaining <= availableTimelineFrames) {
                return qMin<int64_t>(
                    clipTimelineEnd,
                    sourceClip.startFrame + qRound64(
                        (rangeStart - sourceClip.sourceInFrame) /
                        qMax<qreal>(0.001, sourceClip.playbackRate)) + remaining);
            }
            remaining -= availableTimelineFrames;
        }
        return lastMappedEnd;
    };
    QString previousSpeaker;

    for (const TranscriptSection& section : sections) {
        for (const TranscriptWord& word : section.words) {
            const QString speakerId = word.speaker.trimmed();
            if (speakerId.isEmpty() || word.skipped) {
                continue;
            }
            if (word.startFrame < sourceStart || word.startFrame >= sourceEndExclusive) {
                continue;
            }
            if (speakerId == previousSpeaker) {
                continue;
            }
            previousSpeaker = speakerId;

            const qreal localTimelineFrames =
                static_cast<qreal>(word.startFrame - sourceClip.sourceInFrame) /
                qMax<qreal>(0.001, sourceClip.playbackRate);
            const int64_t startFrame =
                qBound<int64_t>(sourceClip.startFrame,
                                sourceClip.startFrame + qRound64(localTimelineFrames),
                                qMax<int64_t>(sourceClip.startFrame, clipTimelineEnd - 1));
            const int64_t rawEndFrame = rawEndForPlaybackFrames(
                word.startFrame, startDelay + boundedDuration);
            const int64_t duration = qMax<int64_t>(1, rawEndFrame - startFrame);
            const int64_t titleLookupFrame = qMax<int64_t>(
                word.startFrame,
                word.startFrame + ((qMax<int64_t>(word.startFrame, word.endFrame) - word.startFrame) / 2));
            const SpeakerProfile speakerProfile = transcriptSpeakerProfileForSourceFrame(
                transcriptPath,
                sections,
                titleLookupFrame,
                TranscriptOverlayTiming{0, 0});
            QStringList titleLines;
            if (settings.showSpeakerName) {
                const QString name = speakerProfile.name.trimmed();
                titleLines.push_back(name.isEmpty() ? speakerId : name);
            }
            if (settings.showSpeakerOrganization &&
                !speakerProfile.organization.trimmed().isEmpty()) {
                titleLines.push_back(speakerProfile.organization.trimmed());
            }
            if (titleLines.isEmpty()) {
                continue;
            }
            const QString title = titleLines.join(QLatin1Char('\n'));

            TimelineClip titleClip;
            titleClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            titleClip.clipRole = ClipRole::SpeakerTitle;
            titleClip.linkedSourceClipId = sourceClip.id.trimmed();
            titleClip.syncLockedToSource = true;
            titleClip.sourceTransformLocked = false;
            titleClip.mediaType = ClipMediaType::Title;
            titleClip.label = QStringLiteral("Speaker: %1").arg(title);
            titleClip.startFrame = startFrame;
            titleClip.durationFrames = duration;
            titleClip.sourceDurationFrames = duration;
            titleClip.trackIndex = qMax(0, trackIndex);
            titleClip.videoEnabled = true;
            titleClip.audioEnabled = false;
            titleClip.hasAudio = false;
            titleClip.color = QColor(QStringLiteral("#255f85"));

            TimelineClip::TitleKeyframe base;
            base.text = title.trimmed();
            base.translationY = 0.68;
            base.fontSize = qBound<qreal>(12.0, settings.titleFontSize, 220.0);
            base.autoFitToOutput = settings.titleAutoFitToOutput;
            base.color = QColor(QStringLiteral("#f7fbff"));
            if (speakerProfile.primaryColor.isValid()) {
                base.color = speakerProfile.primaryColor;
            }
            base.logoPath = speakerProfile.logoPath;
            base.textMaterialStyle = settings.titleTextMaterialStyle;
            base.textPatternImagePath = settings.titleTextPatternImagePath;
            base.textPatternScale = qBound<qreal>(0.10, settings.titlePatternScale, 8.0);
            base.windowEnabled = settings.titleBackgroundEnabled;
            base.windowColor = QColor(QStringLiteral("#07111d"));
            if (speakerProfile.secondaryColor.isValid()) {
                base.windowColor = speakerProfile.secondaryColor;
            }
            base.windowOpacity = 0.72;
            base.windowPadding = 24.0;
            base.windowWidth = qMax<qreal>(0.0, settings.titleBoxWidth);
            base.windowFrameEnabled = settings.titleBackgroundEnabled;
            base.windowFrameColor = QColor(QStringLiteral("#56c7ff"));
            if (speakerProfile.accentColor.isValid()) {
                base.windowFrameColor = speakerProfile.accentColor;
            }
            base.windowFrameOpacity = 0.85;
            base.windowFrameWidth = 2.0;
            base.windowFrameMaterialStyle = settings.titleBorderMaterialStyle;
            base.windowFramePatternImagePath = settings.titleBorderPatternImagePath;
            base.windowFramePatternScale = qBound<qreal>(0.10, settings.titlePatternScale, 8.0);
            base.dropShadowEnabled = true;
            base.dropShadowOpacity = 0.82;
            base.dropShadowOffsetX = 3.0;
            base.dropShadowOffsetY = 5.0;
            base.vulkan3DExtrudeEnabled = settings.titleExtrude3D;
            base.textExtrudeMode = settings.titleExtrude3D
                ? settings.titleExtrudeMode
                : TimelineClip::TitleKeyframe::TextExtrudeMode::None;
            base.vulkan3DEnabled = settings.titleExtrude3D;
            base.vulkan3DExtrudeDepth = qBound<qreal>(0.0, settings.titleExtrudeDepth, 2.0);
            base.vulkan3DBevelScale = qBound<qreal>(0.0, settings.titleBevelScale, 2.0);
            // The solid extrusion is the depth cue. Keeping a second offset
            // shadow makes the title look duplicated rather than dimensional.
            base.dropShadowEnabled = !settings.titleExtrude3D;
            if (settings.titleExtrude3D) {
                base.dropShadowOpacity = 0.0;
            }
            titleClip.titleKeyframes = {base};
            TimelineClip animationClip = titleClip;
            animationClip.durationFrames = qMin<int64_t>(
                boundedDuration, qMax<int64_t>(1, duration - startDelay));
            applyNewsLowerThirdFlyInPreset(animationClip, settings);
            titleClip.titleKeyframes = animationClip.titleKeyframes;
            if (startDelay > 0 && !titleClip.titleKeyframes.isEmpty()) {
                TimelineClip::TitleKeyframe waiting = titleClip.titleKeyframes.constFirst();
                waiting.frame = 0;
                waiting.opacity = 0.0;
                waiting.windowOpacity = 0.0;
                waiting.windowFrameOpacity = 0.0;
                waiting.dropShadowOpacity = 0.0;
                for (TimelineClip::TitleKeyframe& keyframe : titleClip.titleKeyframes) {
                    keyframe.frame = qMin<int64_t>(duration - 1, keyframe.frame + startDelay);
                }
                titleClip.titleKeyframes.prepend(waiting);
            }
            clips.push_back(titleClip);
        }
    }

    return clips;
}

int applySpeakerTitleFlyInsToSourceClip(TimelineClip& sourceClip,
                                        const QString& transcriptPath,
                                        const QVector<TranscriptSection>& sections,
                                        const SpeakerTitleFlyInSettings& settings)
{
    QVector<TimelineClip> titleClips = makeSpeakerTitleClipsForTranscriptIntroductions(
        sourceClip,
        transcriptPath,
        sections,
        sourceClip.trackIndex,
        settings);

    if (!titleClips.isEmpty()) {
        // A generated lower-third owns speaker identification. Showing the
        // transcript overlay's speaker label at the same time produces two
        // competing speaker titles; keep transcript captions, but suppress
        // their redundant identity label.
        sourceClip.transcriptOverlay.showSpeakerTitle = false;
    }

    QVector<TimelineClip::TitleKeyframe> sourceKeyframes;
    for (const TimelineClip& titleClip : titleClips) {
        const int64_t titleStartLocalFrame =
            qMax<int64_t>(0, titleClip.startFrame - sourceClip.startFrame);
        for (TimelineClip::TitleKeyframe keyframe : titleClip.titleKeyframes) {
            keyframe.frame = qBound<int64_t>(
                0,
                titleStartLocalFrame + qMax<int64_t>(0, keyframe.frame),
                qMax<int64_t>(0, sourceClip.durationFrames - 1));
            sourceKeyframes.push_back(keyframe);
        }
    }

    std::sort(sourceKeyframes.begin(),
              sourceKeyframes.end(),
              [](const TimelineClip::TitleKeyframe& a, const TimelineClip::TitleKeyframe& b) {
                  return a.frame < b.frame;
              });
    sourceClip.titleKeyframes = sourceKeyframes;
    sourceClip.speakerTitleEngineActive = !titleClips.isEmpty();
    return titleClips.size();
}

bool applyNewsLowerThirdFlyInPreset(TimelineClip& clip, const SpeakerTitleFlyInSettings& settings)
{
    if (clip.mediaType != ClipMediaType::Title) {
        return false;
    }

    const int64_t duration = qMax<int64_t>(static_cast<int64_t>(kTimelineFps), clip.durationFrames);
    const int64_t inEnd = qMin<int64_t>(duration - 1, qMax<int64_t>(1, settings.flyInFrames));
    const int64_t holdEnd =
        qMin<int64_t>(duration - 1, qMax<int64_t>(inEnd + 1, duration - qMax<int64_t>(1, settings.flyOutFrames)));
    const int64_t outEnd = duration - 1;

    TimelineClip::TitleKeyframe base =
        clip.titleKeyframes.isEmpty() ? TimelineClip::TitleKeyframe{} : clip.titleKeyframes.constFirst();
    if (base.text.trimmed().isEmpty()) {
        base.text = clip.label.trimmed().isEmpty() ? QStringLiteral("Speaker Name") : clip.label;
    }
    constexpr qreal kDefaultLowerThirdX = 0.0;
    constexpr qreal kDefaultLowerThirdY = 112.0;
    constexpr qreal kOffscreenX = 520.0;
    constexpr qreal kVerticalFlyOffset = 190.0;
    if (std::abs(base.translationY) < 4.0) {
        base.translationY = kDefaultLowerThirdY;
    }
    base.windowEnabled = settings.titleBackgroundEnabled;
    base.windowOpacity = qMax<qreal>(base.windowOpacity, 0.55);
    base.windowPadding = qMax<qreal>(base.windowPadding, 22.0);
    base.windowFrameEnabled = settings.titleBackgroundEnabled;
    base.windowFrameOpacity = qMax<qreal>(base.windowFrameOpacity, 0.78);
    base.windowFrameWidth = qMax<qreal>(base.windowFrameWidth, 2.0);
    base.dropShadowEnabled = true;
    base.dropShadowOpacity = qMax<qreal>(base.dropShadowOpacity, 0.72);
    base.textMaterialStyle = settings.titleTextMaterialStyle;
    base.textPatternImagePath = settings.titleTextPatternImagePath;
    base.textPatternScale = qBound<qreal>(0.10, settings.titlePatternScale, 8.0);
    base.windowFrameMaterialStyle = settings.titleBorderMaterialStyle;
    base.windowFramePatternImagePath = settings.titleBorderPatternImagePath;
    base.windowFramePatternScale = qBound<qreal>(0.10, settings.titlePatternScale, 8.0);
    base.vulkan3DExtrudeEnabled = settings.titleExtrude3D;
    base.textExtrudeMode = settings.titleExtrude3D
        ? settings.titleExtrudeMode
        : TimelineClip::TitleKeyframe::TextExtrudeMode::None;
    const bool animated3DRotation =
        std::abs(settings.rotationStartXDegrees) > 0.001 ||
        std::abs(settings.rotationStartYDegrees) > 0.001 ||
        std::abs(settings.rotationStartZDegrees) > 0.001;
    base.vulkan3DEnabled = settings.titleExtrude3D || animated3DRotation;
    base.vulkan3DExtrudeDepth = qBound<qreal>(0.0, settings.titleExtrudeDepth, 2.0);
    base.vulkan3DBevelScale = qBound<qreal>(0.0, settings.titleBevelScale, 2.0);
    base.dropShadowEnabled = !settings.titleExtrude3D;
    if (settings.titleExtrude3D) {
        base.dropShadowOpacity = 0.0;
    }
    base.linearInterpolation = true;

    if (settings.style == SpeakerTitleFlyInStyle::WrapAroundSpeaker) {
        base.translationY = std::abs(base.translationY) < 4.0 ? kDefaultLowerThirdY : base.translationY;
        base.windowPadding = qMin<qreal>(base.windowPadding, 14.0);
        const int64_t flyFrames = qBound<int64_t>(4, settings.flyInFrames, qMax<int64_t>(4, duration - 1));
        const int64_t arriveFrame = qBound<int64_t>(flyFrames + 1, flyFrames + qMax<int64_t>(2, flyFrames / 5), duration - 1);
        const int64_t holdFrame =
            qMin<int64_t>(duration - 1, qMax<int64_t>(arriveFrame + 1, duration - qMax<int64_t>(1, settings.flyOutFrames)));
        const int64_t outFrame = duration - 1;
        const qreal radius = qBound<qreal>(0.24, settings.wrapRadius, 1.80);
        const qreal depth = qBound<qreal>(0.0, settings.wrapDepth, 1.0);
        const qreal baseFontSize = base.fontSize;
        const qreal orbitFontSize = qMin<qreal>(baseFontSize, 30.0);
        const QString orbitText = base.text.trimmed();
        const qreal startAngle = settings.wrapStartAngleDegrees * M_PI / 180.0;
        const qreal endAngle = settings.wrapEndAngleDegrees * M_PI / 180.0;
        const qreal pitch = qBound<qreal>(-80.0, settings.wrapPitchDegrees, 80.0) * M_PI / 180.0;
        const qreal roll = settings.wrapRollDegrees * M_PI / 180.0;
        const qreal cosPitch = std::cos(pitch);
        const qreal sinPitch = std::sin(pitch);
        const qreal cosRoll = std::cos(roll);
        const qreal sinRoll = std::sin(roll);

        auto smoothStep = [](qreal t) {
            const qreal x = qBound<qreal>(0.0, t, 1.0);
            return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
        };

        auto orbitKeyframe = [&](qreal frameT) {
            TimelineClip::TitleKeyframe keyframe = base;
            keyframe.text = orbitText;
            keyframe.windowPadding = qMin<qreal>(keyframe.windowPadding, 10.0);
            keyframe.frame = qBound<int64_t>(
                0,
                static_cast<int64_t>(qRound64(frameT * static_cast<qreal>(flyFrames))),
                duration - 1);
            const qreal t = smoothStep(frameT);
            const qreal angle = startAngle + (endAngle - startAngle) * t;
            const qreal angleDegrees = angle * 180.0 / M_PI;
            qreal x = std::sin(angle) * radius;
            qreal y = 0.0;
            qreal z = -std::cos(angle);
            const qreal pitchedY = y * cosPitch - z * sinPitch;
            const qreal pitchedZ = y * sinPitch + z * cosPitch;
            y = pitchedY;
            z = pitchedZ;
            const qreal rolledX = x * cosRoll - y * sinRoll;
            const qreal rolledY = x * sinRoll + y * cosRoll;
            x = rolledX;
            y = rolledY;

            const qreal depthScale = qBound<qreal>(0.48, 1.0 + z * depth * 0.26, 1.22);
            const bool behindMask = z < -0.42 && std::abs(x) < radius * 0.42;
            const qreal visibility = behindMask
                ? qBound<qreal>(0.0, 0.12 - depth * 0.12, 0.12)
                : qBound<qreal>(0.18, 0.72 + z * depth * 0.28, 1.0);

            keyframe.translationX = qBound<qreal>(-620.0, x * 360.0, 620.0);
            keyframe.translationY = qBound<qreal>(-220.0, y * 160.0 - z * 34.0, 220.0);
            keyframe.fontSize = orbitFontSize * depthScale;
            keyframe.opacity = visibility;
            keyframe.vulkan3DEnabled = true;
            keyframe.vulkan3DYawDegrees =
                qBound<qreal>(-720.0,
                              qBound<qreal>(-62.0, -angleDegrees * 0.52, 62.0) +
                                  settings.rotationStartYDegrees * (1.0 - t),
                              720.0);
            keyframe.vulkan3DPitchDegrees = qBound<qreal>(
                -720.0,
                settings.wrapPitchDegrees + settings.rotationStartXDegrees * (1.0 - t),
                720.0);
            keyframe.vulkan3DRollDegrees = qBound<qreal>(
                -720.0,
                settings.wrapRollDegrees + settings.rotationStartZDegrees * (1.0 - t),
                720.0);
            keyframe.vulkan3DDepth = z * depth;
            keyframe.vulkan3DScale = depthScale;
            if (behindMask) {
                keyframe.windowOpacity = 0.0;
                keyframe.windowFrameOpacity = 0.0;
                keyframe.dropShadowOpacity = 0.0;
            } else {
                keyframe.windowOpacity = base.windowOpacity * qBound<qreal>(0.38, visibility, 1.0);
                keyframe.windowFrameOpacity = base.windowFrameOpacity * qBound<qreal>(0.42, visibility, 1.0);
                keyframe.dropShadowOpacity = base.dropShadowOpacity * qBound<qreal>(0.35, visibility, 1.0);
            }
            return keyframe;
        };

        QVector<TimelineClip::TitleKeyframe> keyframes;
        constexpr int kOrbitSamples = 19;
        keyframes.reserve(kOrbitSamples + 3);
        for (int sample = 0; sample < kOrbitSamples; ++sample) {
            const qreal t = static_cast<qreal>(sample) / static_cast<qreal>(kOrbitSamples - 1);
            keyframes.push_back(orbitKeyframe(t));
        }
        keyframes.first().opacity = 0.0;
        keyframes.first().windowOpacity = 0.0;
        keyframes.first().windowFrameOpacity = 0.0;
        keyframes.first().dropShadowOpacity = 0.0;

        TimelineClip::TitleKeyframe arrived = base;
        arrived.frame = arriveFrame;
        arrived.translationX = kDefaultLowerThirdX;
        arrived.opacity = 1.0;
        arrived.vulkan3DEnabled = arrived.vulkan3DExtrudeEnabled;
        arrived.vulkan3DYawDegrees = 0.0;
        arrived.vulkan3DPitchDegrees = 0.0;
        arrived.vulkan3DRollDegrees = 0.0;
        arrived.vulkan3DDepth = 0.0;
        arrived.vulkan3DScale = 1.0;

        TimelineClip::TitleKeyframe hold = arrived;
        hold.frame = holdFrame;

        TimelineClip::TitleKeyframe after = arrived;
        after.frame = outFrame;
        after.translationX = kOffscreenX;
        after.translationY = base.translationY - 0.06;
        after.fontSize = baseFontSize * 0.92;
        after.opacity = 0.0;

        keyframes.push_back(arrived);
        keyframes.push_back(hold);
        keyframes.push_back(after);
        std::sort(keyframes.begin(), keyframes.end(), [](const TimelineClip::TitleKeyframe& a,
                                                         const TimelineClip::TitleKeyframe& b) {
            return a.frame < b.frame;
        });
        clip.titleKeyframes = keyframes;
        return true;
    }

    TimelineClip::TitleKeyframe before = base;
    before.frame = 0;
    before.opacity = 0.0;
    before.vulkan3DPitchDegrees = settings.rotationStartXDegrees;
    before.vulkan3DYawDegrees = settings.rotationStartYDegrees;
    before.vulkan3DRollDegrees = settings.rotationStartZDegrees;

    TimelineClip::TitleKeyframe arrived = base;
    arrived.frame = inEnd;
    arrived.translationX = kDefaultLowerThirdX;
    arrived.opacity = 1.0;
    arrived.vulkan3DPitchDegrees = 0.0;
    arrived.vulkan3DYawDegrees = 0.0;
    arrived.vulkan3DRollDegrees = 0.0;

    TimelineClip::TitleKeyframe hold = arrived;
    hold.frame = holdEnd;

    TimelineClip::TitleKeyframe after = arrived;
    after.frame = outEnd;
    after.opacity = 0.0;

    switch (settings.style) {
    case SpeakerTitleFlyInStyle::SlideFromRight:
        before.translationX = kOffscreenX;
        before.translationY = arrived.translationY;
        after.translationX = -kOffscreenX;
        after.translationY = arrived.translationY;
        break;
    case SpeakerTitleFlyInStyle::RiseFromBottom:
        before.translationX = arrived.translationX;
        before.translationY = arrived.translationY + kVerticalFlyOffset;
        after.translationX = arrived.translationX;
        after.translationY = arrived.translationY + kVerticalFlyOffset * 0.65;
        break;
    case SpeakerTitleFlyInStyle::DropFromTop:
        before.translationX = arrived.translationX;
        before.translationY = arrived.translationY - kVerticalFlyOffset;
        after.translationX = arrived.translationX;
        after.translationY = arrived.translationY + kVerticalFlyOffset * 0.65;
        break;
    case SpeakerTitleFlyInStyle::SlideFromLeft:
    default:
        before.translationX = -kOffscreenX;
        before.translationY = arrived.translationY;
        after.translationX = kOffscreenX;
        after.translationY = arrived.translationY;
        break;
    }

    clip.titleKeyframes = {before, arrived, hold, after};
    return true;
}

GeneratedClipPlacementResult replaceGeneratedClipsForSource(
    QVector<TimelineClip>& timelineClips,
    QVector<TimelineTrack>& timelineTracks,
    const QString& sourceClipId,
    ClipRole generatedRole,
    QVector<TimelineClip> generatedClips,
    const QString& trackBaseName)
{
    GeneratedClipPlacementResult result;
    const QString sourceId = sourceClipId.trimmed();
    const QString baseName = trackBaseName.trimmed().isEmpty()
                                 ? QStringLiteral("Generated")
                                 : trackBaseName.trimmed();
    QSet<int> reusableChildTracks;

    for (int i = timelineClips.size() - 1; i >= 0; --i) {
        if (timelineClips.at(i).clipRole == generatedRole &&
            timelineClips.at(i).linkedSourceClipId == sourceId) {
            reusableChildTracks.insert(timelineClips.at(i).trackIndex);
            timelineClips.removeAt(i);
            ++result.removedCount;
        }
    }

    for (TimelineClip clip : generatedClips) {
        if (clip.id.trimmed().isEmpty() || clipIdExists(timelineClips, clip.id)) {
            clip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        clip.clipRole = generatedRole;
        clip.linkedSourceClipId = sourceId;

        int targetTrack = -1;
        for (int trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
            if (!trackNameStartsWith(timelineTracks, trackIndex, baseName) &&
                !reusableChildTracks.contains(trackIndex)) {
                continue;
            }
            clip.trackIndex = trackIndex;
            if (!wouldClipConflictWithTrack(timelineClips, clip, trackIndex)) {
                targetTrack = trackIndex;
                if (reusableChildTracks.contains(trackIndex)) {
                    timelineTracks[trackIndex].name = baseName;
                }
                break;
            }
        }
        if (targetTrack < 0) {
            targetTrack = appendGeneratedTrack(timelineTracks, baseName);
        }

        clip.trackIndex = targetTrack;
        if (clip.durationFrames <= 0) {
            clip.durationFrames = 1;
        }
        timelineClips.push_back(clip);
        if (result.firstInsertedClipId.isEmpty()) {
            result.firstInsertedClipId = clip.id;
        }
        ++result.insertedCount;
    }

    result.changed = result.removedCount > 0 || result.insertedCount > 0;
    return result;
}
