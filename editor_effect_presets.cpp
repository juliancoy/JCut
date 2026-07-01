#include "editor_effect_presets.h"

#include "timeline_fps.h"

#include <QUuid>

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

int appendGeneratedTrack(QVector<TimelineTrack>& tracks, const QString& trackBaseName)
{
    TimelineTrack track;
    track.name = nextGeneratedTrackName(tracks, trackBaseName);
    track.audioEnabled = false;
    track.audioWaveformVisible = false;
    tracks.push_back(track);
    return tracks.size() - 1;
}

} // namespace

TimelineClip makeSamMaskMatteClip(const TimelineClip& sourceClip)
{
    TimelineClip maskClip = sourceClip;
    const QString sourceId = sourceClip.id.trimmed();
    const QString maskId = sourceClip.maskFramesDir.trimmed();
    maskClip.id = sourceId.isEmpty()
                      ? QStringLiteral("generated-mask-matte")
                      : QStringLiteral("%1-mask-matte").arg(sourceId);
    maskClip.clipRole = ClipRole::MaskMatte;
    maskClip.linkedSourceClipId = sourceId;
    maskClip.generatedFromMaskId = maskId;
    maskClip.syncLockedToSource = true;
    maskClip.label = sourceClip.label.trimmed().isEmpty()
                         ? QStringLiteral("SAM Mask Matte")
                         : QStringLiteral("%1 Mask Matte").arg(sourceClip.label.trimmed());
    maskClip.hasAudio = false;
    maskClip.audioEnabled = false;
    maskClip.audioLinkedToVideo = false;
    maskClip.audioBusId.clear();
    maskClip.audioSourcePath.clear();
    maskClip.audioSourceOriginalPath.clear();
    maskClip.audioSourceStatus = QStringLiteral("generated");
    maskClip.audioStreamIndex = -1;
    maskClip.maskEnabled = sourceClip.maskEnabled && !maskId.isEmpty();
    maskClip.maskFramesDir = maskId;
    maskClip.maskShowOnly = true;
    maskClip.maskForegroundLayerEnabled = false;
    maskClip.effectPreset = ClipEffectPreset::None;
    return maskClip;
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
    return effectClip;
}

QVector<TimelineClip> makeSpeakerTitleClipsForTranscriptIntroductions(
    const TimelineClip& sourceClip,
    const QString& transcriptPath,
    const QVector<TranscriptSection>& sections,
    int trackIndex,
    int64_t titleDurationFrames)
{
    QVector<TimelineClip> clips;
    if (sections.isEmpty() || sourceClip.durationFrames <= 0) {
        return clips;
    }

    const int64_t boundedDuration = qMax<int64_t>(kTimelineFps, titleDurationFrames);
    const int64_t sourceStart = sourceClip.sourceInFrame;
    const int64_t sourceEndExclusive =
        sourceStart + qMax<int64_t>(1, qRound64(sourceClip.durationFrames * qMax<qreal>(0.001, sourceClip.playbackRate)));
    const int64_t clipTimelineEnd = sourceClip.startFrame + sourceClip.durationFrames;
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
            const int64_t duration = qMin<int64_t>(boundedDuration, qMax<int64_t>(1, clipTimelineEnd - startFrame));
            const int64_t titleLookupFrame = qMax<int64_t>(
                word.startFrame,
                word.startFrame + ((qMax<int64_t>(word.startFrame, word.endFrame) - word.startFrame) / 2));
            QString title = transcriptSpeakerTitleForSourceFrame(
                transcriptPath,
                sections,
                titleLookupFrame,
                TranscriptOverlayTiming{0, 0}).trimmed();
            if (title.isEmpty() || title == speakerId) {
                const SpeakerProfile profile = transcriptSpeakerProfileForSourceFrame(
                    transcriptPath,
                    sections,
                    titleLookupFrame,
                    TranscriptOverlayTiming{0, 0});
                const QString name = profile.name.trimmed();
                const QString organization = profile.organization.trimmed();
                if (!name.isEmpty()) {
                    title = name;
                    if (!organization.isEmpty()) {
                        title += QStringLiteral(" - ") + organization;
                    }
                }
            }
            if (title.isEmpty()) {
                title = speakerId;
            }

            TimelineClip titleClip;
            titleClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            titleClip.clipRole = ClipRole::SpeakerTitle;
            titleClip.linkedSourceClipId = sourceClip.id.trimmed();
            titleClip.syncLockedToSource = true;
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
            base.text = title;
            base.translationY = 0.68;
            base.fontSize = 48.0;
            base.windowEnabled = true;
            base.windowOpacity = 0.62;
            base.windowPadding = 24.0;
            base.windowFrameEnabled = true;
            base.windowFrameOpacity = 0.85;
            base.windowFrameWidth = 2.0;
            titleClip.titleKeyframes = {base};
            applyNewsLowerThirdFlyInPreset(titleClip);
            clips.push_back(titleClip);
        }
    }

    return clips;
}

bool applyNewsLowerThirdFlyInPreset(TimelineClip& clip)
{
    if (clip.mediaType != ClipMediaType::Title) {
        return false;
    }

    const int64_t duration = qMax<int64_t>(static_cast<int64_t>(kTimelineFps * 3), clip.durationFrames);
    const int64_t inEnd = qMin<int64_t>(duration - 1, qRound64(kTimelineFps * 0.35));
    const int64_t holdEnd =
        qMin<int64_t>(duration - 1, qMax<int64_t>(inEnd + 1, duration - qRound64(kTimelineFps * 0.45)));
    const int64_t outEnd = duration - 1;

    TimelineClip::TitleKeyframe base =
        clip.titleKeyframes.isEmpty() ? TimelineClip::TitleKeyframe{} : clip.titleKeyframes.constFirst();
    if (base.text.trimmed().isEmpty()) {
        base.text = clip.label.trimmed().isEmpty() ? QStringLiteral("Speaker Name") : clip.label;
    }
    base.translationY = qBound<qreal>(-0.95, base.translationY == 0.0 ? 0.68 : base.translationY, 0.95);
    base.windowEnabled = true;
    base.windowOpacity = qMax<qreal>(base.windowOpacity, 0.55);
    base.windowPadding = qMax<qreal>(base.windowPadding, 22.0);
    base.dropShadowEnabled = true;
    base.linearInterpolation = true;

    TimelineClip::TitleKeyframe before = base;
    before.frame = 0;
    before.translationX = -1.28;
    before.opacity = 0.0;

    TimelineClip::TitleKeyframe arrived = base;
    arrived.frame = inEnd;
    arrived.translationX = -0.34;
    arrived.opacity = 1.0;

    TimelineClip::TitleKeyframe hold = arrived;
    hold.frame = holdEnd;

    TimelineClip::TitleKeyframe after = arrived;
    after.frame = outEnd;
    after.translationX = 1.28;
    after.opacity = 0.0;

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

    for (int i = timelineClips.size() - 1; i >= 0; --i) {
        if (timelineClips.at(i).clipRole == generatedRole &&
            timelineClips.at(i).linkedSourceClipId == sourceId) {
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
            if (!trackNameStartsWith(timelineTracks, trackIndex, baseName)) {
                continue;
            }
            clip.trackIndex = trackIndex;
            if (!wouldClipConflictWithTrack(timelineClips, clip, trackIndex)) {
                targetTrack = trackIndex;
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
