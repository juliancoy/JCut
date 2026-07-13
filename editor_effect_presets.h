#pragma once

#include "editor_shared.h"

struct EffectPresetUiOption {
    QString label;
    ClipEffectPreset preset = ClipEffectPreset::None;
    QString group;
};

struct TilingPatternUiOption {
    QString label;
    ClipTilingPattern pattern = ClipTilingPattern::Grid;
};

QVector<EffectPresetUiOption> effectPresetUiOptions();
QVector<TilingPatternUiOption> tilingPatternUiOptions();
bool effectPresetUsesDirectionalControl(ClipEffectPreset preset);
bool effectPresetUsesTilingControls(ClipEffectPreset preset);

TimelineClip makeSamMaskMatteClip(const TimelineClip& sourceClip);
bool migrateLegacyMaskGradingToMattes(QVector<TimelineClip>& clips);
bool normalizeSamMaskMatteClips(QVector<TimelineClip>& clips);
TimelineClip makeAlternatingMotionBackgroundClip(const TimelineClip& sourceClip, int trackIndex);
TimelineClip makeSourceTilingClip(const TimelineClip& sourceClip, int trackIndex);

enum class SpeakerTitleFlyInStyle {
    SlideFromLeft = 0,
    SlideFromRight = 1,
    RiseFromBottom = 2,
    DropFromTop = 3,
    WrapAroundSpeaker = 4,
};

struct SpeakerTitleFlyInSettings {
    SpeakerTitleFlyInStyle style = SpeakerTitleFlyInStyle::SlideFromLeft;
    int64_t titleDurationFrames = kTimelineFps * 3;
    int64_t titleStartDelayFrames = (kTimelineFps * 35 + 50) / 100;
    int64_t flyInFrames = (kTimelineFps * 35 + 50) / 100;
    int64_t flyOutFrames = (kTimelineFps * 45 + 50) / 100;
    qreal wrapRadius = 1.05;
    qreal wrapDepth = 0.70;
    qreal wrapStartAngleDegrees = -110.0;
    qreal wrapEndAngleDegrees = 110.0;
    qreal wrapPitchDegrees = 8.0;
    qreal wrapRollDegrees = 0.0;
    qreal rotationStartXDegrees = 0.0;
    qreal rotationStartYDegrees = 0.0;
    qreal rotationStartZDegrees = 0.0;
    qreal titleFontSize = 48.0;
    bool titleAutoFitToOutput = true;
    qreal titleBoxWidth = 720.0;
    bool titleBackgroundEnabled = true;
    bool showSpeakerName = true;
    bool showSpeakerOrganization = true;
    TimelineClip::TitleKeyframe::MaterialStyle titleTextMaterialStyle =
        TimelineClip::TitleKeyframe::MaterialStyle::Solid;
    TimelineClip::TitleKeyframe::MaterialStyle titleBorderMaterialStyle =
        TimelineClip::TitleKeyframe::MaterialStyle::Solid;
    QString titleTextPatternImagePath;
    QString titleBorderPatternImagePath;
    qreal titlePatternScale = 1.0;
    bool titleExtrude3D = false;
    TimelineClip::TitleKeyframe::TextExtrudeMode titleExtrudeMode =
        TimelineClip::TitleKeyframe::TextExtrudeMode::ErodedSolid;
    qreal titleExtrudeDepth = 0.16;
    qreal titleBevelScale = 0.7;
};

QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    int64_t titleDurationFrames = kTimelineFps * 3,
    int64_t titleStartDelayFrames = (kTimelineFps * 35 + 50) / 100);
QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    const SpeakerTitleFlyInSettings& settings);
int applySpeakerTitleFlyInsToSourceClip(TimelineClip& sourceClip,
                                        const QString& transcriptPath,
                                        const QVector<TranscriptSection>& sections,
                                        const SpeakerTitleFlyInSettings& settings);
bool applyNewsLowerThirdFlyInPreset(TimelineClip& clip,
                                    const SpeakerTitleFlyInSettings& settings = SpeakerTitleFlyInSettings{});

struct GeneratedClipPlacementResult {
    bool changed = false;
    int removedCount = 0;
    int insertedCount = 0;
    QString firstInsertedClipId;
};

GeneratedClipPlacementResult replaceGeneratedClipsForSource(
    QVector<TimelineClip>& timelineClips,
    QVector<TimelineTrack>& timelineTracks,
    const QString& sourceClipId,
    ClipRole generatedRole,
    QVector<TimelineClip> generatedClips,
    const QString& trackBaseName);
