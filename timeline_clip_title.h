#pragma once

#include "editor_playback_types.h"
#include "editor_timeline_types.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

struct TimelineClipTitlePresentation {
    QString badge;
    QString primary;
    QStringList attributes;
    QString tooltipText;

    QString inlineText() const;
};

// Builds the relationship and track-state indices once, so painting every clip
// does not repeatedly scan the timeline. The model is intentionally presentation
// only: it never performs sidecar discovery or filesystem reads.
class TimelineClipTitleModel {
public:
    TimelineClipTitleModel(const QVector<TimelineClip>& clips,
                           const QVector<TimelineTrack>& tracks);

    TimelineClipTitlePresentation describe(const TimelineClip& clip,
                                           bool includeTooltip = false) const;

private:
    const TimelineTrack* trackFor(const TimelineClip& clip) const;
    const TimelineClip* parentFor(const TimelineClip& clip) const;

    const QVector<TimelineTrack>* m_tracks = nullptr;
    QHash<QString, const TimelineClip*> m_clipsById;
    QHash<QString, int> m_maskChildCountByParentId;
    QHash<QString, int> m_activeMaskChildCountByParentId;
    QHash<QString, int> m_unavailableMaskChildCountByParentId;
    bool m_anyAudioSolo = false;
};

// Canonical source-frame mapping for presentation diagnostics. A Mask Matte
// without a valid Media+Video source returns no value rather than falling back
// to child timing caches.
std::optional<int64_t> timelineClipSourceFrameAtTimelinePosition(
    const TimelineClip& clip,
    const QVector<TimelineClip>& clips,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers);
