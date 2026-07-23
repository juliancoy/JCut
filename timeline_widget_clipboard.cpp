#include "timeline_widget.h"

#include <QUuid>

#include <algorithm>

bool TimelineWidget::copySelectedClips()
{
    m_clipClipboard.clear();
    m_renderSyncMarkerClipboard.clear();
    // Structural clipboard operations work on the ownership aggregate.  A
    // generated child cannot be meaningfully pasted without its parent, and a
    // copied parent must not silently lose its mask mattes.
    const QSet<QString> copiedIds = ownershipClosure(m_clipSelection.ids, true);
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (copiedIds.contains(clip.id)) {
            m_clipClipboard.push_back(clip);
        }
    }
    for (const RenderSyncMarker& marker : std::as_const(m_renderSyncMarkers)) {
        if (copiedIds.contains(marker.clipId)) {
            m_renderSyncMarkerClipboard.push_back(marker);
        }
    }
    std::sort(m_clipClipboard.begin(), m_clipClipboard.end(), [](const TimelineClip& a, const TimelineClip& b) {
        return a.startFrame != b.startFrame ? a.startFrame < b.startFrame : a.trackIndex < b.trackIndex;
    });
    return !m_clipClipboard.isEmpty();
}

bool TimelineWidget::cutSelectedClips()
{
    if (m_clipSelection.ids.isEmpty()) {
        return false;
    }
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (!m_clipSelection.ids.contains(clip.id)) {
            continue;
        }
        if (clip.clipRole == ClipRole::MaskMatte) {
            // A selected child is valid only as an explicitly selected
            // descendant of its selected parent. Copy/cut still expands the
            // complete ownership closure; a child alone remains non-cuttable.
            if (!m_clipSelection.ids.contains(clip.linkedSourceClipId.trimmed())) {
                return false;
            }
            continue;
        }
        if (clip.locked) {
            return false;
        }
    }
    if (!copySelectedClips()) {
        return false;
    }
    QSet<QString> cutIds;
    for (const TimelineClip& clip : std::as_const(m_clipClipboard)) {
        cutIds.insert(clip.id);
    }
    QVector<TimelineClip> remaining;
    remaining.reserve(m_clips.size() - cutIds.size());
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (!cutIds.contains(clip.id)) {
            remaining.push_back(clip);
        }
    }
    const qsizetype markerCountBefore = m_renderSyncMarkers.size();
    m_renderSyncMarkers.erase(
        std::remove_if(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
                       [&cutIds](const RenderSyncMarker& marker) {
                           return cutIds.contains(marker.clipId);
                       }),
        m_renderSyncMarkers.end());
    rebuildRenderSyncMarkerIndex();
    const bool removedMarkers = m_renderSyncMarkers.size() != markerCountBefore;
    setClips(remaining);
    applyClipSelection({}, QString(), false);
    if (selectionChanged) selectionChanged();
    if (clipsChanged) clipsChanged();
    if (removedMarkers && renderSyncMarkersChanged) renderSyncMarkersChanged();
    update();
    return true;
}

bool TimelineWidget::pasteClipsAtCurrentFrame()
{
    if (m_clipClipboard.isEmpty()) {
        return false;
    }
    int64_t anchorFrame = m_clipClipboard.constFirst().startFrame;
    int minimumTrack = m_clipClipboard.constFirst().trackIndex;
    for (const TimelineClip& clip : std::as_const(m_clipClipboard)) {
        anchorFrame = qMin(anchorFrame, clip.startFrame);
        minimumTrack = qMin(minimumTrack, clip.trackIndex);
    }
    const int targetBaseTrack = m_selectedTrackIndex >= 0 ? m_selectedTrackIndex : minimumTrack;
    QHash<QString, QString> pastedIds;
    for (const TimelineClip& clip : std::as_const(m_clipClipboard)) {
        pastedIds.insert(clip.id, QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    QSet<QString> selection;
    QString primary;
    for (TimelineClip clip : std::as_const(m_clipClipboard)) {
        const QString oldId = clip.id;
        clip.id = pastedIds.value(oldId);
        clip.startFrame = qMax<int64_t>(0, m_currentFrame + (clip.startFrame - anchorFrame));
        clip.trackIndex = qMax(0, targetBaseTrack + (clip.trackIndex - minimumTrack));
        if (pastedIds.contains(clip.linkedSourceClipId)) {
            clip.linkedSourceClipId = pastedIds.value(clip.linkedSourceClipId);
        }
        ensureTrackCount(clip.trackIndex + 1);
        selection.insert(clip.id);
        if (oldId == m_clipSelection.primaryId || primary.isEmpty()) primary = clip.id;
        m_clips.push_back(std::move(clip));
    }
    bool pastedMarkers = false;
    for (RenderSyncMarker marker : std::as_const(m_renderSyncMarkerClipboard)) {
        if (!pastedIds.contains(marker.clipId)) {
            continue;
        }
        marker.clipId = pastedIds.value(marker.clipId);
        marker.frame = qMax<int64_t>(0, m_currentFrame + (marker.frame - anchorFrame));
        m_renderSyncMarkers.push_back(marker);
        pastedMarkers = true;
    }
    sortRenderSyncMarkers();
    rebuildRenderSyncMarkerIndex();
    const QVector<TimelineClip> pastedModel = m_clips;
    setClips(pastedModel);
    applyClipSelection(selection, primary, false);
    if (selectionChanged) selectionChanged();
    if (clipsChanged) clipsChanged();
    if (pastedMarkers && renderSyncMarkersChanged) renderSyncMarkersChanged();
    update();
    return true;
}

bool TimelineWidget::duplicateSelectedClips()
{
    if (!copySelectedClips()) {
        return false;
    }
    int64_t endFrame = m_clipClipboard.constFirst().startFrame;
    for (const TimelineClip& clip : std::as_const(m_clipClipboard)) {
        endFrame = qMax(endFrame, clip.startFrame + clip.durationFrames);
    }
    const int64_t savedCurrentFrame = m_currentFrame;
    m_currentFrame = endFrame;
    const bool pasted = pasteClipsAtCurrentFrame();
    m_currentFrame = savedCurrentFrame;
    return pasted;
}

bool TimelineWidget::selectAllClips()
{
    QSet<QString> ids;
    for (const TimelineClip& clip : std::as_const(m_clips)) ids.insert(clip.id);
    if (ids.isEmpty()) return false;
    applyClipSelection(ids, m_clipSelection.primaryId);
    return true;
}
