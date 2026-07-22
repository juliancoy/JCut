#include "timeline_widget.h"

#include <QUuid>

#include <algorithm>

bool TimelineWidget::copySelectedClips()
{
    m_clipClipboard.clear();
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (m_clipSelection.ids.contains(clip.id)) {
            m_clipClipboard.push_back(clip);
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
        if (m_clipSelection.ids.contains(clip.id) && clip.locked) {
            return false;
        }
    }
    if (!copySelectedClips()) {
        return false;
    }
    m_clips.erase(std::remove_if(m_clips.begin(), m_clips.end(), [this](const TimelineClip& clip) {
        return m_clipSelection.ids.contains(clip.id);
    }), m_clips.end());
    applyClipSelection({}, QString(), false);
    normalizeTrackIndices();
    sortClips();
    if (selectionChanged) selectionChanged();
    if (clipsChanged) clipsChanged();
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
    sortClips();
    applyClipSelection(selection, primary, false);
    if (selectionChanged) selectionChanged();
    if (clipsChanged) clipsChanged();
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
