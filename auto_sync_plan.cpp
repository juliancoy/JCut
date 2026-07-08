#include "auto_sync_plan.h"

namespace {

bool clipHasAudioForSync(const TimelineClip& clip)
{
    return clip.hasAudio || clip.mediaType == ClipMediaType::Audio;
}

} // namespace

AutoSyncSelectionPlan buildAutoSyncSelectionPlan(
    const QVector<TimelineClip>& clips,
    const QSet<QString>& selectedClipIds,
    const std::function<bool(const TimelineClip&)>& clipHasVisuals,
    const std::function<QString(const TimelineClip&)>& syncAudioPathForClip,
    const std::function<QString(const TimelineClip&)>& playbackMediaPathForClip)
{
    AutoSyncSelectionPlan plan;
    if (selectedClipIds.isEmpty()) {
        plan.message = QStringLiteral("No selected clips.");
        return plan;
    }

    QVector<TimelineClip> selected;
    selected.reserve(selectedClipIds.size());
    for (const TimelineClip& clip : clips) {
        if (selectedClipIds.contains(clip.id)) {
            selected.push_back(clip);
        }
    }
    if (selected.isEmpty()) {
        plan.message = QStringLiteral("No selected clips.");
        return plan;
    }

    QVector<TimelineClip> visualClips;
    QVector<TimelineClip> audioClips;
    for (const TimelineClip& clip : selected) {
        if (clipHasVisuals && clipHasVisuals(clip)) {
            visualClips.push_back(clip);
        }
        if (clipHasAudioForSync(clip)) {
            audioClips.push_back(clip);
        }
    }

    if (audioClips.isEmpty()) {
        plan.message = QStringLiteral("Select at least one clip with audio to use as the sync anchor.");
        return plan;
    }

    for (const TimelineClip& clip : audioClips) {
        if (!clip.locked) {
            continue;
        }
        const QString audioPath = syncAudioPathForClip ? syncAudioPathForClip(clip) : QString();
        if (!audioPath.trimmed().isEmpty()) {
            plan.lockedAudioAnchors.push_back(AutoSyncAnchorPlan{clip, audioPath});
        }
    }

    if (!plan.lockedAudioAnchors.isEmpty()) {
        plan.primaryAudioAnchor = plan.lockedAudioAnchors.constFirst();
    } else {
        QString audioAnchorId;
        for (const TimelineClip& clip : audioClips) {
            if (!clipHasVisuals || !clipHasVisuals(clip)) {
                audioAnchorId = clip.id;
                break;
            }
        }
        if (audioAnchorId.isEmpty() && !audioClips.isEmpty()) {
            audioAnchorId = audioClips.constFirst().id;
        }
        for (const TimelineClip& clip : clips) {
            if (clip.id != audioAnchorId) {
                continue;
            }
            const QString audioPath = syncAudioPathForClip ? syncAudioPathForClip(clip) : QString();
            if (!audioPath.trimmed().isEmpty()) {
                plan.primaryAudioAnchor = AutoSyncAnchorPlan{clip, audioPath};
            }
            break;
        }
    }

    if (plan.primaryAudioAnchor.clip.id.trimmed().isEmpty() ||
        plan.primaryAudioAnchor.audioPath.trimmed().isEmpty()) {
        plan.message = QStringLiteral("Audio source is unavailable for sync detection.");
        return plan;
    }

    for (const TimelineClip& clip : audioClips) {
        if (clip.locked || clip.id == plan.primaryAudioAnchor.clip.id) {
            continue;
        }
        const QString audioPath = syncAudioPathForClip ? syncAudioPathForClip(clip) : QString();
        if (audioPath.trimmed().isEmpty()) {
            continue;
        }
        const QVector<AutoSyncAnchorPlan> anchors =
            plan.lockedAudioAnchors.isEmpty() ? QVector<AutoSyncAnchorPlan>{plan.primaryAudioAnchor}
                                              : plan.lockedAudioAnchors;
        plan.targets.push_back(AutoSyncTargetPlan{
            clip,
            audioPath,
            QStringLiteral("audio"),
            true,
            anchors});
        plan.syncMarkerClipIds.insert(clip.id);
    }

    for (const TimelineClip& clip : visualClips) {
        if (clip.locked || clip.id == plan.primaryAudioAnchor.clip.id ||
            clip.hasAudio || clip.mediaType == ClipMediaType::Audio) {
            continue;
        }
        const QString videoPath = playbackMediaPathForClip ? playbackMediaPathForClip(clip) : QString();
        if (videoPath.trimmed().isEmpty()) {
            continue;
        }
        plan.targets.push_back(AutoSyncTargetPlan{
            clip,
            videoPath,
            QStringLiteral("av"),
            true,
            QVector<AutoSyncAnchorPlan>{plan.primaryAudioAnchor}});
        plan.syncMarkerClipIds.insert(clip.id);
    }

    if (plan.targets.isEmpty()) {
        plan.message = QStringLiteral("Select another clip with audio, or a visual-only clip, alongside the audio anchor.");
        return plan;
    }

    plan.ok = true;
    return plan;
}
