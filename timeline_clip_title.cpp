#include "timeline_clip_title.h"

#include "editor_effect_presets.h"
#include "editor_shared_media.h"
#include "editor_shared_render_sync.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace {

QString cleanDisplayText(QString text)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return text.simplified();
}

QString clipIdentity(const TimelineClip& clip)
{
    if ((clip.mediaType == ClipMediaType::Title ||
         clip.clipRole == ClipRole::SpeakerTitle) &&
        !clip.titleKeyframes.isEmpty()) {
        const QString title = cleanDisplayText(clip.titleKeyframes.constFirst().text);
        if (!title.isEmpty()) {
            return title;
        }
    }

    const QString label = cleanDisplayText(clip.label);
    if (!label.isEmpty()) {
        return label;
    }

    const QString fileName = QFileInfo(clip.filePath).fileName().trimmed();
    return fileName.isEmpty() ? QStringLiteral("Untitled clip") : fileName;
}

QString maskIdentity(const TimelineClip& clip, const TimelineClip* parent)
{
    QString identity = clipIdentity(clip);
    const QString parentIdentity = parent ? clipIdentity(*parent) : QString();
    if (!parentIdentity.isEmpty()) {
        const QString relationshipPrefix = parentIdentity + QStringLiteral(" · ");
        if (identity.startsWith(relationshipPrefix, Qt::CaseInsensitive)) {
            identity.remove(0, relationshipPrefix.size());
        } else if (identity.compare(parentIdentity + QStringLiteral(" Mask"),
                                    Qt::CaseInsensitive) == 0 ||
                   identity.compare(parentIdentity + QStringLiteral(" Mask Matte"),
                                    Qt::CaseInsensitive) == 0) {
            identity = QStringLiteral("Generated matte");
        }
    }
    for (const QString& suffix : {QStringLiteral(" Mask Matte"),
                                  QStringLiteral(" Mask")}) {
        if (identity.endsWith(suffix, Qt::CaseInsensitive) &&
            identity.size() > suffix.size()) {
            identity.chop(suffix.size());
            break;
        }
    }
    identity = cleanDisplayText(identity);
    return identity.isEmpty() ? QStringLiteral("Generated matte") : identity;
}

QString presetLabel(ClipEffectPreset preset)
{
    if (preset == ClipEffectPreset::None) {
        return {};
    }
    static const QVector<EffectPresetUiOption> options = effectPresetUiOptions();
    for (const EffectPresetUiOption& option : options) {
        if (option.preset == preset) {
            return option.label;
        }
    }
    return QStringLiteral("Custom effect");
}

QString badgeFor(const TimelineClip& clip, int maskChildCount)
{
    switch (clip.clipRole) {
    case ClipRole::MaskMatte:
        return QStringLiteral("MASK");
    case ClipRole::EffectSynth:
        return QStringLiteral("FX");
    case ClipRole::SpeakerTitle:
        return QStringLiteral("TITLE");
    case ClipRole::Media:
        break;
    }
    if (maskChildCount > 0) {
        return QStringLiteral("SOURCE");
    }
    if (clip.sourceKind == MediaSourceKind::ImageSequence) {
        return QStringLiteral("SEQ");
    }
    switch (clip.mediaType) {
    case ClipMediaType::Audio:
        return QStringLiteral("AUDIO");
    case ClipMediaType::Image:
        return QStringLiteral("IMAGE");
    case ClipMediaType::Title:
        return QStringLiteral("TITLE");
    case ClipMediaType::Video:
        return QStringLiteral("VIDEO");
    case ClipMediaType::Unknown:
        return QStringLiteral("CLIP");
    }
    return QStringLiteral("CLIP");
}

QString rateLabel(qreal rate)
{
    return QStringLiteral("%1×").arg(rate, 0, 'g', 3);
}

bool isValidMaskSourceParent(const TimelineClip* candidate)
{
    return candidate &&
           candidate->clipRole == ClipRole::Media &&
           candidate->mediaType == ClipMediaType::Video &&
           !candidate->id.trimmed().isEmpty() &&
           !candidate->filePath.trimmed().isEmpty() &&
           clipHasVisuals(*candidate);
}

const TimelineClip* validMaskSourceParent(const TimelineClip& clip,
                                          const QVector<TimelineClip>& clips)
{
    if (clip.clipRole != ClipRole::MaskMatte) {
        return nullptr;
    }
    const QString parentId = clip.linkedSourceClipId.trimmed();
    if (parentId.isEmpty()) {
        return nullptr;
    }
    for (const TimelineClip& candidate : clips) {
        if (candidate.id.trimmed() == parentId && isValidMaskSourceParent(&candidate)) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

QString TimelineClipTitlePresentation::inlineText() const
{
    if (attributes.isEmpty()) {
        return primary;
    }
    return QStringLiteral("%1 · %2").arg(primary, attributes.join(QStringLiteral(" · ")));
}

TimelineClipTitleModel::TimelineClipTitleModel(const QVector<TimelineClip>& clips,
                                               const QVector<TimelineTrack>& tracks)
    : m_tracks(&tracks)
{
    m_clipsById.reserve(clips.size());
    for (const TimelineClip& clip : clips) {
        const QString clipId = clip.id.trimmed();
        if (!clipId.isEmpty()) {
            m_clipsById.insert(clipId, &clip);
        }
    }
    for (const TimelineClip& clip : clips) {
        m_anyAudioSolo =
            m_anyAudioSolo || (clipAudioPlaybackEnabled(clip) && clip.audioSolo);
        if (clip.clipRole != ClipRole::MaskMatte) {
            continue;
        }
        const QString parentId = clip.linkedSourceClipId.trimmed();
        if (parentId.isEmpty()) {
            continue;
        }
        const auto parent = m_clipsById.constFind(parentId);
        if (parent == m_clipsById.cend() || !isValidMaskSourceParent(parent.value())) {
            continue;
        }
        ++m_maskChildCountByParentId[parentId];
        if (!clip.maskSidecarAvailable) {
            ++m_unavailableMaskChildCountByParentId[parentId];
        }
        if (clip.maskSidecarAvailable && clipChildPlaybackEnabled(clip, tracks)) {
            ++m_activeMaskChildCountByParentId[parentId];
        }
    }
    for (const TimelineTrack& track : tracks) {
        m_anyAudioSolo = m_anyAudioSolo || track.audioSolo;
    }
}

const TimelineTrack* TimelineClipTitleModel::trackFor(const TimelineClip& clip) const
{
    return m_tracks && clip.trackIndex >= 0 && clip.trackIndex < m_tracks->size()
        ? &m_tracks->at(clip.trackIndex)
        : nullptr;
}

const TimelineClip* TimelineClipTitleModel::parentFor(const TimelineClip& clip) const
{
    if (clip.clipRole == ClipRole::Media) {
        return nullptr;
    }
    const auto found = m_clipsById.constFind(clip.linkedSourceClipId.trimmed());
    if (found == m_clipsById.cend()) {
        return nullptr;
    }
    const TimelineClip* candidate = found.value();
    return clip.clipRole != ClipRole::MaskMatte || isValidMaskSourceParent(candidate)
        ? candidate
        : nullptr;
}

TimelineClipTitlePresentation TimelineClipTitleModel::describe(const TimelineClip& clip,
                                                               bool includeTooltip) const
{
    TimelineClipTitlePresentation result;
    const TimelineTrack* track = trackFor(clip);
    const TimelineClip* parent = parentFor(clip);
    const QString clipId = clip.id.trimmed();
    const int maskChildCount = m_maskChildCountByParentId.value(clipId);
    const int activeMaskChildCount = m_activeMaskChildCountByParentId.value(clipId);
    const int unavailableMaskChildCount =
        m_unavailableMaskChildCountByParentId.value(clipId);
    const TrackVisualMode visualMode = track ? track->visualMode : TrackVisualMode::Enabled;
    const bool hasVisuals = clipHasVisuals(clip);
    const bool trackHidden = hasVisuals && visualMode == TrackVisualMode::Hidden;
    const bool clipHidden = hasVisuals && !clip.videoEnabled;
    const bool visuallyHidden = trackHidden || clipHidden;

    result.badge = badgeFor(clip, maskChildCount);
    result.primary = clip.clipRole == ClipRole::MaskMatte
        ? maskIdentity(clip, parent)
        : clipIdentity(clip);

    if (clip.clipRole == ClipRole::MaskMatte) {
        if (!parent) {
            result.attributes.push_back(QStringLiteral("Missing source"));
            result.attributes.push_back(QStringLiteral("Inactive"));
        } else if (!clip.maskEnabled) {
            result.attributes.push_back(QStringLiteral("Mask off"));
        } else if (!clip.maskSidecarAvailable) {
            result.attributes.push_back(QStringLiteral("Unavailable"));
        } else {
            result.attributes.push_back(visuallyHidden
                ? QStringLiteral("Hidden")
                : QStringLiteral("Enabled"));
        }
        if (!clip.maskSidecarAvailable &&
            !clip.maskSidecarAvailabilityIssue.trimmed().isEmpty()) {
            result.attributes.push_back(
                cleanDisplayText(clip.maskSidecarAvailabilityIssue));
        }
        result.attributes.push_back(
            QStringLiteral("Z %1").arg(effectiveClipZLevel(clip)));
        if (parent) {
            result.attributes.push_back(
                QStringLiteral("↳ %1").arg(clipIdentity(*parent)));
        }
        if (parent && (clip.syncLockedToSource || clip.sourceTransformLocked)) {
            result.attributes.push_back(QStringLiteral("Source-locked"));
        }
        if (hasVisuals && visualMode == TrackVisualMode::ForceOpaque) {
            result.attributes.push_back(QStringLiteral("Opaque"));
        }
        if (clip.maskShowOnly) {
            result.attributes.push_back(QStringLiteral("Matte only"));
        }
        if (clip.maskInvert) {
            result.attributes.push_back(QStringLiteral("Inverted"));
        }
    } else {
        if (visuallyHidden) {
            result.attributes.push_back(maskChildCount > 0 && activeMaskChildCount > 0
                ? QStringLiteral("Provider only")
                : QStringLiteral("Hidden"));
        } else if (hasVisuals && visualMode == TrackVisualMode::ForceOpaque) {
            result.attributes.push_back(QStringLiteral("Opaque"));
        }
        if (maskChildCount > 0) {
            result.attributes.push_back(
                QStringLiteral("%1/%2 masks active")
                    .arg(activeMaskChildCount)
                    .arg(maskChildCount));
            if (unavailableMaskChildCount > 0) {
                result.attributes.push_back(
                    QStringLiteral("%1 unavailable").arg(unavailableMaskChildCount));
            }
        }
    }

    if (clip.hasAudio) {
        const bool clipOrTrackSolo =
            clip.audioSolo || (track && track->audioSolo);
        if (!clip.audioEnabled || clip.audioGain <= 0.0) {
            result.attributes.push_back(QStringLiteral("Audio off"));
        } else if (track &&
                   (!track->audioEnabled || track->audioMuted || track->audioGain <= 0.0)) {
            result.attributes.push_back(QStringLiteral("Track muted"));
        } else if (m_anyAudioSolo && !clipOrTrackSolo) {
            result.attributes.push_back(QStringLiteral("Muted by solo"));
        }
        if (clipOrTrackSolo) {
            result.attributes.push_back(QStringLiteral("Solo"));
        }
    }

    if (clip.clipRole != ClipRole::MaskMatte && clip.locked) {
        result.attributes.push_back(QStringLiteral("Locked"));
    }
    if (!clip.gradingPreviewEnabled || (track && !track->gradingPreviewEnabled)) {
        result.attributes.push_back(QStringLiteral("Grade bypass"));
    }

    const QString clipEffect = presetLabel(clip.effectPreset);
    if (!clipEffect.isEmpty()) {
        result.attributes.push_back(QStringLiteral("FX %1").arg(clipEffect));
    }
    // Mask Matte timing is always presented from the canonical parent. Child
    // timing fields are normalization caches, not a second clock (TIME.md).
    const TimelineClip* timingOwner =
        clip.clipRole == ClipRole::MaskMatte ? parent : &clip;
    if (timingOwner && std::abs(timingOwner->playbackRate - 1.0) > 0.001) {
        result.attributes.push_back(rateLabel(timingOwner->playbackRate));
    }

    if (includeTooltip) {
        QStringList tooltip;
        tooltip.push_back(QStringLiteral("%1 — %2").arg(result.badge, result.primary));
        if (!result.attributes.isEmpty()) {
            tooltip.push_back(QStringLiteral("State: %1").arg(
                result.attributes.join(QStringLiteral(" · "))));
        }
        if (clip.clipRole == ClipRole::MaskMatte) {
            tooltip.push_back(parent
                ? QStringLiteral("Source parent: %1").arg(clipIdentity(*parent))
                : QStringLiteral("Source parent: Missing"));
        } else if (maskChildCount > 0) {
            tooltip.push_back(QStringLiteral("Mask children: %1 active of %2")
                                  .arg(activeMaskChildCount)
                                  .arg(maskChildCount));
        }
        if (timingOwner) {
            tooltip.push_back(QStringLiteral("Timeline: start %1 · duration %2 frames · source in %3 · %4")
                                  .arg(timingOwner->startFrame)
                                  .arg(timingOwner->durationFrames)
                                  .arg(timingOwner->sourceInFrame)
                                  .arg(rateLabel(timingOwner->playbackRate)));
        } else {
            tooltip.push_back(QStringLiteral("Timeline: Unavailable without a valid source parent"));
        }
        tooltip.push_back(QStringLiteral("Track: %1 · %2")
                              .arg(clip.trackIndex + 1)
                              .arg(trackVisualModeLabel(visualMode)));
        if (!clip.filePath.trimmed().isEmpty()) {
            tooltip.push_back(QStringLiteral("Media: %1").arg(
                QDir::toNativeSeparators(clip.filePath.trimmed())));
        }
        if (clip.clipRole == ClipRole::MaskMatte && !clip.maskFramesDir.trimmed().isEmpty()) {
            tooltip.push_back(QStringLiteral("Mask sidecar: %1").arg(
                QDir::toNativeSeparators(clip.maskFramesDir.trimmed())));
            if (!clip.maskSidecarAvailable) {
                tooltip.push_back(QStringLiteral("Mask availability: %1").arg(
                    clip.maskSidecarAvailabilityIssue.trimmed().isEmpty()
                        ? QStringLiteral("Unavailable")
                        : cleanDisplayText(clip.maskSidecarAvailabilityIssue)));
            }
        }
        result.tooltipText = tooltip.join(QLatin1Char('\n'));
    }
    return result;
}

std::optional<int64_t> timelineClipSourceFrameAtTimelinePosition(
    const TimelineClip& clip,
    const QVector<TimelineClip>& clips,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers)
{
    if (clip.clipRole == ClipRole::MaskMatte && !validMaskSourceParent(clip, clips)) {
        return std::nullopt;
    }
    const ClipFrameMapping mapping = clipFrameMappingForClock(
        clip,
        clips,
        renderFrameClockForTimelinePosition(timelineFramePosition),
        markers);
    return mapping.sourceFrame;
}
