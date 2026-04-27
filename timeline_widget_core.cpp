#include "timeline_widget.h"
#include "timeline_layout.h"
#include "timeline_renderer.h"
#include "titles.h"
#include "waveform_service.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHash>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

void upsertGradingKeyframe(QVector<TimelineClip::GradingKeyframe>& keyframes,
                           const TimelineClip::GradingKeyframe& keyframe) {
    for (TimelineClip::GradingKeyframe& existing : keyframes) {
        if (existing.frame == keyframe.frame) {
            existing = keyframe;
            return;
        }
    }
    keyframes.push_back(keyframe);
}

void upsertOpacityKeyframe(QVector<TimelineClip::OpacityKeyframe>& keyframes,
                           const TimelineClip::OpacityKeyframe& keyframe) {
    for (TimelineClip::OpacityKeyframe& existing : keyframes) {
        if (existing.frame == keyframe.frame) {
            existing = keyframe;
            return;
        }
    }
    keyframes.push_back(keyframe);
}

void applyVisualCrossfade(TimelineClip& clip, bool fadeIn, int64_t fadeFrames) {
    if (!clipHasVisuals(clip) || clip.durationFrames <= 1 || fadeFrames <= 0) {
        return;
    }

    const int64_t localStartFrame = fadeIn ? 0 : qMax<int64_t>(0, clip.durationFrames - fadeFrames);
    const int64_t localEndFrame = fadeIn ? qMin<int64_t>(clip.durationFrames - 1, fadeFrames) : clip.durationFrames - 1;
    if (localStartFrame >= localEndFrame) {
        return;
    }

    const qreal startState = evaluateClipOpacityAtPosition(clip, static_cast<qreal>(clip.startFrame + localStartFrame));
    const qreal endState = evaluateClipOpacityAtPosition(clip, static_cast<qreal>(clip.startFrame + localEndFrame));

    TimelineClip::OpacityKeyframe startKeyframe;
    startKeyframe.frame = localStartFrame;
    startKeyframe.opacity = fadeIn ? 0.0 : qBound(0.0, startState, 1.0);
    startKeyframe.linearInterpolation = true;

    TimelineClip::OpacityKeyframe endKeyframe;
    endKeyframe.frame = localEndFrame;
    endKeyframe.opacity = fadeIn ? qBound(0.0, endState, 1.0) : 0.0;
    endKeyframe.linearInterpolation = true;

    upsertOpacityKeyframe(clip.opacityKeyframes, startKeyframe);
    upsertOpacityKeyframe(clip.opacityKeyframes, endKeyframe);
    normalizeClipOpacityKeyframes(clip);
}

void drawEyeIcon(QPainter& painter, const QRect& rect, bool enabled, bool interactive) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor stroke = interactive
                              ? (enabled ? QColor(QStringLiteral("#eef4fa"))
                                         : QColor(QStringLiteral("#7f8a99")))
                              : QColor(QStringLiteral("#556170"));
    painter.setPen(QPen(stroke, 1.7));
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    path.moveTo(rect.left() + rect.width() * 0.10, rect.center().y());
    path.quadTo(rect.center().x(), rect.top() + rect.height() * 0.08,
                rect.right() - rect.width() * 0.10, rect.center().y());
    path.quadTo(rect.center().x(), rect.bottom() - rect.height() * 0.08,
                rect.left() + rect.width() * 0.10, rect.center().y());
    painter.drawPath(path);
    painter.setBrush(stroke);
    painter.drawEllipse(QRectF(rect.center().x() - rect.width() * 0.10,
                               rect.center().y() - rect.height() * 0.10,
                               rect.width() * 0.20,
                               rect.height() * 0.20));
    if (!enabled) {
        painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
        painter.drawLine(rect.left() + 2, rect.bottom() - 2, rect.right() - 2, rect.top() + 2);
    }
    painter.restore();
}

void drawSpeakerIcon(QPainter& painter, const QRect& rect, bool enabled, bool interactive) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor stroke = interactive
                              ? (enabled ? QColor(QStringLiteral("#eef4fa"))
                                         : QColor(QStringLiteral("#7f8a99")))
                              : QColor(QStringLiteral("#556170"));
    painter.setPen(QPen(stroke, 1.7));
    painter.setBrush(Qt::NoBrush);

    QPainterPath speaker;
    speaker.moveTo(rect.left() + rect.width() * 0.18, rect.center().y() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.55, rect.top() + rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.55, rect.bottom() - rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.36, rect.center().y() + rect.height() * 0.18);
    speaker.lineTo(rect.left() + rect.width() * 0.18, rect.center().y() + rect.height() * 0.18);
    speaker.closeSubpath();
    painter.drawPath(speaker);
    if (enabled) {
        painter.drawArc(QRect(rect.left() + rect.width() * 0.45,
                              rect.top() + rect.height() * 0.18,
                              rect.width() * 0.28,
                              rect.height() * 0.64),
                        -40 * 16,
                        80 * 16);
        painter.drawArc(QRect(rect.left() + rect.width() * 0.52,
                              rect.top() + rect.height() * 0.06,
                              rect.width() * 0.34,
                              rect.height() * 0.88),
                        -40 * 16,
                        80 * 16);
    } else {
        painter.setPen(QPen(QColor(QStringLiteral("#ff8c82")), 2.0));
        painter.drawLine(rect.left() + rect.width() * 0.62,
                         rect.top() + rect.height() * 0.24,
                         rect.right() - 2,
                         rect.bottom() - rect.height() * 0.22);
        painter.drawLine(rect.right() - 2,
                         rect.top() + rect.height() * 0.24,
                         rect.left() + rect.width() * 0.62,
                         rect.bottom() - rect.height() * 0.22);
    }
    painter.restore();
}

}

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setAcceptDrops(true);
    setMinimumHeight(150);
    setMouseTracking(true);
    setAutoFillBackground(true);

    m_layout = std::make_unique<TimelineLayout>(this);
    m_renderer = std::make_unique<TimelineRenderer>(this);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(QStringLiteral("#0f1216")));
    setPalette(pal);
    ensureTrackCount(1);
    updateMinimumTimelineHeight();

    QPointer<TimelineWidget> self(this);
    editor::WaveformService::instance().setReadyCallback([self]() {
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self, [self]() {
            if (self) {
                self->update();
            }
        }, Qt::QueuedConnection);
    });
}

void TimelineWidget::setCurrentFrame(int64_t frame) {
    m_currentFrame = qMax<int64_t>(0, frame);
    normalizeExportRange();
    update();
}

int64_t TimelineWidget::totalFrames() const {
    int64_t lastFrame = 300;
    for (const TimelineClip& clip : m_clips) {
        lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + 30);
    }
    return lastFrame;
}

QString TimelineWidget::selectedClipId() const {
    return m_clipSelection.primaryId;
}

QSet<QString> TimelineWidget::selectedClipIds() const {
    return m_clipSelection.ids;
}

QString TimelineWidget::resolveSelectionPrimary(const QSet<QString>& selectedIds) const {
    if (selectedIds.isEmpty()) {
        return QString();
    }
    if (!m_clipSelection.primaryId.isEmpty() && selectedIds.contains(m_clipSelection.primaryId)) {
        return m_clipSelection.primaryId;
    }
    for (const TimelineClip& clip : m_clips) {
        if (selectedIds.contains(clip.id)) {
            return clip.id;
        }
    }
    return *selectedIds.constBegin();
}

void TimelineWidget::applyClipSelection(const QSet<QString>& selectedIds,
                                        const QString& primaryIdHint,
                                        bool notify) {
    QSet<QString> existingIds;
    existingIds.reserve(m_clips.size());
    for (const TimelineClip& clip : m_clips) {
        existingIds.insert(clip.id);
    }

    QSet<QString> filteredSelection;
    filteredSelection.reserve(selectedIds.size());
    for (const QString& id : selectedIds) {
        if (existingIds.contains(id)) {
            filteredSelection.insert(id);
        }
    }

    QString nextPrimary = primaryIdHint;
    if (!nextPrimary.isEmpty() && !filteredSelection.contains(nextPrimary)) {
        nextPrimary.clear();
    }
    if (nextPrimary.isEmpty()) {
        nextPrimary = resolveSelectionPrimary(filteredSelection);
    }

    const bool changed = (m_clipSelection.ids != filteredSelection ||
                          m_clipSelection.primaryId != nextPrimary);
    if (!changed) {
        return;
    }

    m_clipSelection.ids = filteredSelection;
    m_clipSelection.primaryId = nextPrimary;

    if (!m_clipSelection.primaryId.isEmpty()) {
        for (const TimelineClip& clip : m_clips) {
            if (clip.id == m_clipSelection.primaryId) {
                m_selectedTrackIndex = clip.trackIndex;
                break;
            }
        }
    }

    if (notify && selectionChanged) {
        selectionChanged();
    }
}

bool TimelineWidget::isClipSelected(const QString& clipId) const {
    return m_clipSelection.ids.contains(clipId);
}

void TimelineWidget::selectClipWithModifiers(const QString& clipId, Qt::KeyboardModifiers modifiers) {
    const bool shiftPressed = modifiers.testFlag(Qt::ShiftModifier);
    const bool togglePressed = modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier);
    if (!shiftPressed && !togglePressed) {
        applyClipSelection(QSet<QString>{clipId}, clipId);
        return;
    }

    const TimelineClip* clickedClip = nullptr;
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == clipId) {
            clickedClip = &clip;
            break;
        }
    }
    if (!clickedClip) {
        return;
    }

    QSet<QString> nextSelection = m_clipSelection.ids;
    if (!shiftPressed && togglePressed && nextSelection.contains(clipId)) {
        nextSelection.remove(clipId);
        applyClipSelection(nextSelection, QString());
        return;
    }

    nextSelection.insert(clipId);

    if (shiftPressed) {
        const int targetTrack = clickedClip->trackIndex;
        const int64_t clickedStartFrame = clickedClip->startFrame;
        for (const TimelineClip& selectedClip : m_clips) {
            if (!m_clipSelection.ids.contains(selectedClip.id) || selectedClip.id == clipId) {
                continue;
            }
            if (selectedClip.trackIndex != targetTrack) {
                continue;
            }
            const int64_t rangeStart = qMin(clickedStartFrame, selectedClip.startFrame);
            const int64_t rangeEnd = qMax(clickedStartFrame, selectedClip.startFrame);
            for (const TimelineClip& candidate : m_clips) {
                if (candidate.trackIndex != targetTrack) {
                    continue;
                }
                if (candidate.startFrame >= rangeStart && candidate.startFrame <= rangeEnd) {
                    nextSelection.insert(candidate.id);
                }
            }
        }
    }

    applyClipSelection(nextSelection, clipId);
}

void TimelineWidget::setClips(const QVector<TimelineClip>& clips) {
    m_clips = clips;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    static constexpr qint64 kAudioSourceVerifyReuseWindowMs = 10000;
    for (TimelineClip& clip : m_clips) {
        normalizeClipTiming(clip);
        const QString normalizedPath = QFileInfo(clip.filePath).absoluteFilePath();
        const bool recentlyVerified =
            clip.audioSourceLastVerifiedMs > 0 &&
            nowMs >= clip.audioSourceLastVerifiedMs &&
            (nowMs - clip.audioSourceLastVerifiedMs) <= kAudioSourceVerifyReuseWindowMs;
        const bool pathUnchanged =
            !normalizedPath.isEmpty() &&
            clip.audioSourceOriginalPath == normalizedPath;
        if (!(recentlyVerified && pathUnchanged)) {
            refreshClipAudioSource(clip);
        }
    }
    normalizeTrackIndices();
    sortClips();
    ensureTrackCount(trackCount());
    applyClipSelection(m_clipSelection.ids, m_clipSelection.primaryId, false);
    if (m_selectedTrackIndex >= trackCount()) {
        m_selectedTrackIndex = -1;
    }
    bool hoveredClipStillExists = m_hoveredClipId.isEmpty();
    if (!hoveredClipStillExists) {
        for (const TimelineClip& clip : m_clips) {
            if (clip.id == m_hoveredClipId) {
                hoveredClipStillExists = true;
                break;
            }
        }
    }
    if (!hoveredClipStillExists) {
        m_hoveredClipId.clear();
    }
    normalizeExportRange();
    updateMinimumTimelineHeight();
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
}

void TimelineWidget::setTracks(const QVector<TimelineTrack>& tracks) {
    m_tracks = tracks;
    ensureTrackCount(trackCount());
    normalizeExportRange();
    updateMinimumTimelineHeight();
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
}

const TimelineClip* TimelineWidget::selectedClip() const {
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == m_clipSelection.primaryId) {
            return &clip;
        }
    }
    return nullptr;
}

const TimelineTrack* TimelineWidget::selectedTrack() const {
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= m_tracks.size()) {
        return nullptr;
    }
    return &m_tracks[m_selectedTrackIndex];
}

void TimelineWidget::setSelectedClipId(const QString& clipId) {
    if (clipId.isEmpty()) {
        applyClipSelection({}, QString());
        update();
        return;
    }
    applyClipSelection(QSet<QString>{clipId}, clipId);
    update();
}

void TimelineWidget::setSelectedTrackIndex(int trackIndex) {
    const int normalizedTrackIndex = (trackIndex >= 0 && trackIndex < trackCount()) ? trackIndex : -1;
    if (m_selectedTrackIndex == normalizedTrackIndex && m_clipSelection.ids.isEmpty()) {
        return;
    }
    m_selectedTrackIndex = normalizedTrackIndex;
    applyClipSelection({}, QString());
    update();
}

bool TimelineWidget::updateClipById(const QString& clipId, const std::function<void(TimelineClip&)>& updater) {
    for (TimelineClip& clip : m_clips) {
        if (clip.id == clipId) {
            updater(clip);
            normalizeClipTiming(clip);
            refreshClipAudioSource(clip);
            update();
            return true;
        }
    }
    return false;
}

bool TimelineWidget::updateTrackByIndex(int trackIndex, const std::function<void(TimelineTrack&)>& updater) {
    if (trackIndex < 0) {
        return false;
    }
    ensureTrackCount(trackIndex + 1);
    updater(m_tracks[trackIndex]);
    if (clipsChanged) {
        clipsChanged();
    }
    updateMinimumTimelineHeight();
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
    return true;
}

bool TimelineWidget::updateTrackVisualMode(int trackIndex, TrackVisualMode mode) {
    return setTrackVisualMode(trackIndex, mode);
}

bool TimelineWidget::updateTrackAudioEnabled(int trackIndex, bool enabled) {
    return setTrackAudioEnabled(trackIndex, enabled);
}

bool TimelineWidget::crossfadeTrack(int trackIndex, double seconds) {
    return applyCrossfadeToTrack(trackIndex, seconds);
}

bool TimelineWidget::moveTrackUp(int trackIndex) {
    if (trackIndex <= 0 || trackIndex >= m_tracks.size()) {
        return false;
    }
    
    ensureTrackCount(trackCount());
    
    // Swap track indices
    std::swap(m_tracks[trackIndex], m_tracks[trackIndex - 1]);
    
    // Update clip track indices
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex) {
            clip.trackIndex = trackIndex - 1;
        } else if (clip.trackIndex == trackIndex - 1) {
            clip.trackIndex = trackIndex;
        }
    }
    
    normalizeTrackIndices();
    sortClips();
    
    // Update selected track
    if (m_selectedTrackIndex == trackIndex) {
        m_selectedTrackIndex = trackIndex - 1;
    } else if (m_selectedTrackIndex == trackIndex - 1) {
        m_selectedTrackIndex = trackIndex;
    }
    
    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
    return true;
}

bool TimelineWidget::moveTrackDown(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_tracks.size() - 1) {
        return false;
    }
    
    ensureTrackCount(trackCount());
    
    // Swap track indices
    std::swap(m_tracks[trackIndex], m_tracks[trackIndex + 1]);
    
    // Update clip track indices
    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex) {
            clip.trackIndex = trackIndex + 1;
        } else if (clip.trackIndex == trackIndex + 1) {
            clip.trackIndex = trackIndex;
        }
    }
    
    normalizeTrackIndices();
    sortClips();
    
    // Update selected track
    if (m_selectedTrackIndex == trackIndex) {
        m_selectedTrackIndex = trackIndex + 1;
    } else if (m_selectedTrackIndex == trackIndex + 1) {
        m_selectedTrackIndex = trackIndex;
    }
    
    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
    return true;
}

bool TimelineWidget::moveTrack(int fromTrack, int toTrack) {
    if (fromTrack < 0 || toTrack < 0 || fromTrack >= m_tracks.size() || toTrack >= m_tracks.size()) {
        return false;
    }
    if (fromTrack == toTrack) {
        return false;
    }

    ensureTrackCount(trackCount());

    for (TimelineClip& clip : m_clips) {
        if (clip.trackIndex == fromTrack) {
            clip.trackIndex = toTrack;
        } else if (fromTrack < toTrack && clip.trackIndex > fromTrack && clip.trackIndex <= toTrack) {
            clip.trackIndex -= 1;
        } else if (fromTrack > toTrack && clip.trackIndex >= toTrack && clip.trackIndex < fromTrack) {
            clip.trackIndex += 1;
        }
    }

    TimelineTrack movedTrack = m_tracks.takeAt(fromTrack);
    m_tracks.insert(toTrack, movedTrack);

    normalizeTrackIndices();
    sortClips();

    if (m_selectedTrackIndex == fromTrack) {
        m_selectedTrackIndex = toTrack;
    } else if (fromTrack < toTrack && m_selectedTrackIndex > fromTrack && m_selectedTrackIndex <= toTrack) {
        m_selectedTrackIndex -= 1;
    } else if (fromTrack > toTrack && m_selectedTrackIndex >= toTrack && m_selectedTrackIndex < fromTrack) {
        m_selectedTrackIndex += 1;
    }

    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
    return true;
}

bool TimelineWidget::deleteClipById(const QString& clipId) {
    if (clipId.isEmpty()) {
        return false;
    }
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id != clipId) {
            continue;
        }
        if (m_clips[i].locked) {
            return false;
        }
        m_clips.removeAt(i);
        QSet<QString> nextSelection = m_clipSelection.ids;
        nextSelection.remove(clipId);
        applyClipSelection(nextSelection, m_clipSelection.primaryId, false);
        sortClips();
        if (clipsChanged) {
            clipsChanged();
        }
        update();
        return true;
    }
    return false;
}

bool TimelineWidget::deleteSelectedClip() {
    if (m_clipSelection.primaryId.isEmpty()) {
        return false;
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id != m_clipSelection.primaryId) {
            continue;
        }

        if (m_clips[i].locked) {
            return false;
        }

        m_clips.removeAt(i);
        QSet<QString> nextSelection = m_clipSelection.ids;
        nextSelection.remove(m_clipSelection.primaryId);
        applyClipSelection(nextSelection, QString(), false);
        normalizeTrackIndices();
        sortClips();
        if (selectionChanged) {
            selectionChanged();
        }
        if (clipsChanged) {
            clipsChanged();
        }
        update();
        return true;
    }

    applyClipSelection({}, QString(), false);
    if (selectionChanged) {
        selectionChanged();
    }
    update();
    return false;
}

bool TimelineWidget::splitSelectedClipAtFrame(int64_t frame) {
    QVector<QString> targetClipIds;
    targetClipIds.reserve(m_clipSelection.ids.size() + 1);
    if (!m_clipSelection.ids.isEmpty()) {
        for (const TimelineClip& clip : m_clips) {
            if (m_clipSelection.ids.contains(clip.id)) {
                targetClipIds.push_back(clip.id);
            }
        }
    } else if (!m_clipSelection.primaryId.isEmpty()) {
        targetClipIds.push_back(m_clipSelection.primaryId);
    }
    if (targetClipIds.isEmpty()) {
        return false;
    }

    QSet<QString> newSelection;
    newSelection.reserve(targetClipIds.size());
    bool splitAny = false;
    const QString previousPrimarySelection = m_clipSelection.primaryId;
    QString nextPrimarySelection;

    for (const QString& targetClipId : std::as_const(targetClipIds)) {
        for (int i = 0; i < m_clips.size(); ++i) {
            TimelineClip& clip = m_clips[i];
            if (clip.id != targetClipId) {
                continue;
            }
            if (clip.locked || frame <= clip.startFrame || frame >= clip.startFrame + clip.durationFrames) {
                break;
            }

            const int64_t leftDuration = frame - clip.startFrame;
            const int64_t rightDuration = clip.durationFrames - leftDuration;
            if (leftDuration <= 0 || rightDuration <= 0) {
                break;
            }
            const int64_t splitTimelineSample = frameToSamples(frame) + clip.startSubframeSamples;
            const int64_t splitSourceSample =
                sourceSampleForClipAtTimelineSample(clip, splitTimelineSample, m_renderSyncMarkers);

            const bool isImage = clip.mediaType == ClipMediaType::Image;
            TimelineClip rightClip = clip;
            rightClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            rightClip.startFrame = frame;
            rightClip.startSubframeSamples = clip.startSubframeSamples;
            rightClip.durationFrames = rightDuration;
            if (isImage) {
                rightClip.sourceInFrame = 0;
                rightClip.sourceInSubframeSamples = 0;
                rightClip.sourceDurationFrames = rightDuration;
            } else {
                rightClip.sourceInFrame = splitSourceSample / kSamplesPerFrame;
                rightClip.sourceInSubframeSamples = splitSourceSample % kSamplesPerFrame;
            }
            rightClip.transformKeyframes.clear();
            for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
                if (keyframe.frame >= leftDuration) {
                    TimelineClip::TransformKeyframe shifted = keyframe;
                    shifted.frame -= leftDuration;
                    rightClip.transformKeyframes.push_back(shifted);
                }
            }

            QVector<TimelineClip::TransformKeyframe> leftKeyframes;
            for (const TimelineClip::TransformKeyframe& keyframe : clip.transformKeyframes) {
                if (keyframe.frame < leftDuration) {
                    leftKeyframes.push_back(keyframe);
                }
            }
            clip.transformKeyframes = leftKeyframes;

            rightClip.gradingKeyframes.clear();
            for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                if (keyframe.frame >= leftDuration) {
                    TimelineClip::GradingKeyframe shifted = keyframe;
                    shifted.frame -= leftDuration;
                    rightClip.gradingKeyframes.push_back(shifted);
                }
            }
            QVector<TimelineClip::GradingKeyframe> leftGradingKeyframes;
            for (const TimelineClip::GradingKeyframe& keyframe : clip.gradingKeyframes) {
                if (keyframe.frame < leftDuration) {
                    leftGradingKeyframes.push_back(keyframe);
                }
            }
            clip.gradingKeyframes = leftGradingKeyframes;

            rightClip.opacityKeyframes.clear();
            for (const TimelineClip::OpacityKeyframe& keyframe : clip.opacityKeyframes) {
                if (keyframe.frame >= leftDuration) {
                    TimelineClip::OpacityKeyframe shifted = keyframe;
                    shifted.frame -= leftDuration;
                    rightClip.opacityKeyframes.push_back(shifted);
                }
            }
            QVector<TimelineClip::OpacityKeyframe> leftOpacityKeyframes;
            for (const TimelineClip::OpacityKeyframe& keyframe : clip.opacityKeyframes) {
                if (keyframe.frame < leftDuration) {
                    leftOpacityKeyframes.push_back(keyframe);
                }
            }
            clip.opacityKeyframes = leftOpacityKeyframes;

            clip.durationFrames = leftDuration;
            if (isImage) {
                clip.sourceInFrame = 0;
                clip.sourceInSubframeSamples = 0;
                clip.sourceDurationFrames = leftDuration;
            }
            normalizeClipTransformKeyframes(clip);
            normalizeClipTransformKeyframes(rightClip);
            normalizeClipGradingKeyframes(clip);
            normalizeClipGradingKeyframes(rightClip);
            normalizeClipOpacityKeyframes(clip);
            normalizeClipOpacityKeyframes(rightClip);
            normalizeClipTiming(clip);
            normalizeClipTiming(rightClip);

            bool movedMarkers = false;
            for (RenderSyncMarker& marker : m_renderSyncMarkers) {
                if (marker.clipId == clip.id && marker.frame >= frame) {
                    marker.clipId = rightClip.id;
                    movedMarkers = true;
                }
            }
            m_clips.insert(i + 1, rightClip);
            newSelection.insert(rightClip.id);
            if (nextPrimarySelection.isEmpty() || targetClipId == previousPrimarySelection) {
                nextPrimarySelection = rightClip.id;
            }
            splitAny = true;
            if (movedMarkers) {
                sortRenderSyncMarkers();
                rebuildRenderSyncMarkerIndex();
            }
            break;
        }
    }

    if (!splitAny) {
        return false;
    }

    sortClips();
    applyClipSelection(newSelection, nextPrimarySelection, false);
    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    update();
    return true;
}

bool TimelineWidget::splitClipAtFrame(const QString& clipId, int64_t frame) {
    const ClipSelectionState previousSelection = m_clipSelection;
    applyClipSelection(QSet<QString>{clipId}, clipId, false);
    const bool result = splitSelectedClipAtFrame(frame);
    if (!result) {
        m_clipSelection = previousSelection;
    }
    return result;
}

void TimelineWidget::setToolMode(ToolMode mode) {
    if (m_toolMode == mode) return;
    m_toolMode = mode;
    m_razorHoverFrame = -1;
    setCursor(mode == ToolMode::Razor ? Qt::CrossCursor : Qt::ArrowCursor);
    if (toolModeChanged) toolModeChanged();
    update();
}

void TimelineWidget::sortClips() {
    std::sort(m_clips.begin(), m_clips.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            const int64_t aStartSamples = clipTimelineStartSamples(a);
            const int64_t bStartSamples = clipTimelineStartSamples(b);
            if (aStartSamples == bStartSamples) {
                return a.label < b.label;
            }
            return aStartSamples < bStartSamples;
        }
        return a.trackIndex > b.trackIndex;
    });
}

void TimelineWidget::sortRenderSyncMarkers() {
    std::sort(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
              [](const RenderSyncMarker& a, const RenderSyncMarker& b) {
                  if (a.clipId != b.clipId) {
                      return a.clipId < b.clipId;
                  }
                  if (a.frame == b.frame) {
                      return static_cast<int>(a.action) < static_cast<int>(b.action);
                  }
                  return a.frame < b.frame;
              });
}

void TimelineWidget::rebuildRenderSyncMarkerIndex() {
    m_renderSyncMarkersByClip.clear();
    if (m_renderSyncMarkers.isEmpty()) {
        return;
    }
    for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
        m_renderSyncMarkersByClip[marker.clipId].push_back(marker);
    }
}

const QVector<RenderSyncMarker>* TimelineWidget::renderSyncMarkersForClipId(const QString& clipId) const {
    const auto it = m_renderSyncMarkersByClip.constFind(clipId);
    if (it == m_renderSyncMarkersByClip.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

void TimelineWidget::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
    sortRenderSyncMarkers();
    rebuildRenderSyncMarkerIndex();
    update();
}

const RenderSyncMarker* TimelineWidget::renderSyncMarkerAtFrame(const QString& clipId, int64_t frame) const {
    const QVector<RenderSyncMarker>* markers = renderSyncMarkersForClipId(clipId);
    if (!markers || markers->isEmpty()) {
        return nullptr;
    }
    const auto it = std::lower_bound(markers->begin(), markers->end(), frame,
                                     [](const RenderSyncMarker& marker, int64_t value) {
                                         return marker.frame < value;
                                     });
    if (it != markers->end() && it->frame == frame) {
        return &(*it);
    }
    return nullptr;
}

bool TimelineWidget::setRenderSyncMarkerAtCurrentFrame(const QString& clipId, RenderSyncAction action, int count) {
    if (clipId.isEmpty()) {
        return false;
    }
    count = qMax(1, count);
    for (RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.clipId == clipId && marker.frame == m_currentFrame) {
            if (marker.action == action && marker.count == count) {
                return false;
            }
            marker.action = action;
            marker.count = count;
            sortRenderSyncMarkers();
            rebuildRenderSyncMarkerIndex();
            if (renderSyncMarkersChanged) {
                renderSyncMarkersChanged();
            }
            update();
            return true;
        }
    }

    RenderSyncMarker marker;
    marker.clipId = clipId;
    marker.frame = m_currentFrame;
    marker.action = action;
    marker.count = count;
    m_renderSyncMarkers.push_back(marker);
    sortRenderSyncMarkers();
    rebuildRenderSyncMarkerIndex();
    if (renderSyncMarkersChanged) {
        renderSyncMarkersChanged();
    }
    update();
    return true;
}

bool TimelineWidget::clearRenderSyncMarkerAtCurrentFrame(const QString& clipId) {
    if (clipId.isEmpty()) {
        return false;
    }
    for (int i = 0; i < m_renderSyncMarkers.size(); ++i) {
        if (m_renderSyncMarkers[i].clipId == clipId && m_renderSyncMarkers[i].frame == m_currentFrame) {
            m_renderSyncMarkers.removeAt(i);
            rebuildRenderSyncMarkerIndex();
            if (renderSyncMarkersChanged) {
                renderSyncMarkersChanged();
            }
            update();
            return true;
        }
    }
    return false;
}

void TimelineWidget::normalizeExportRange() {
    normalizeExportRanges();
}

void TimelineWidget::normalizeExportRanges() {
    const int64_t total = totalFrames();
    if (m_exportRanges.isEmpty()) {
        m_exportRanges = {ExportRangeSegment{0, total}};
    } else {
        for (ExportRangeSegment& segment : m_exportRanges) {
            segment.startFrame = qBound<int64_t>(0, segment.startFrame, total);
            segment.endFrame = qBound<int64_t>(0, segment.endFrame, total);
            if (segment.endFrame < segment.startFrame) {
                std::swap(segment.startFrame, segment.endFrame);
            }
        }
    }
    std::sort(m_exportRanges.begin(), m_exportRanges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame == b.startFrame) {
            return a.endFrame < b.endFrame;
        }
        return a.startFrame < b.startFrame;
    });
    QVector<ExportRangeSegment> normalized;
    normalized.reserve(m_exportRanges.size());
    for (const ExportRangeSegment& segment : std::as_const(m_exportRanges)) {
        if (!normalized.isEmpty() &&
            normalized.constLast().startFrame == segment.startFrame &&
            normalized.constLast().endFrame == segment.endFrame) {
            continue;
        }
        normalized.push_back(segment);
    }
    m_exportRanges = normalized;
}

int TimelineWidget::exportSegmentIndexAtFrame(int64_t frame) const {
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        if (frame >= m_exportRanges[i].startFrame && frame <= m_exportRanges[i].endFrame) {
            return i;
        }
    }
    return -1;
}

QRect TimelineWidget::exportHandleHitRect(int segmentIndex, bool startHandle) const {
    const QRect visual = exportHandleRect(segmentIndex, startHandle);
    if (visual.isEmpty()) {
        return QRect();
    }
    // Keep the visual handle slim, but make pointer hit-testing forgiving.
    return visual.adjusted(-6, -5, 6, 5);
}

int TimelineWidget::exportHandleAtPos(const QPoint& pos, bool* startHandleOut) const {
    struct Candidate {
        int segmentIndex = -1;
        bool startHandle = false;
        int distance = std::numeric_limits<int>::max();
    };

    Candidate best;
    for (int i = 0; i < m_exportRanges.size(); ++i) {
        for (const bool startHandle : {true, false}) {
            const QRect hitRect = exportHandleHitRect(i, startHandle);
            if (!hitRect.contains(pos)) {
                continue;
            }

            const int64_t frame = startHandle ? m_exportRanges[i].startFrame : m_exportRanges[i].endFrame;
            const int handleX = xFromFrame(frame);
            const int distance = std::abs(pos.x() - handleX);
            if (distance < best.distance) {
                best.segmentIndex = i;
                best.startHandle = startHandle;
                best.distance = distance;
            }
        }
    }

    if (best.segmentIndex >= 0) {
        if (startHandleOut) {
            *startHandleOut = best.startHandle;
        }
        return best.segmentIndex;
    }
    return -1;
}

int64_t TimelineWidget::snapThresholdFrames() const {
    static constexpr qreal kSnapThresholdPixels = 10.0;
    return qMax<int64_t>(1, qRound64(kSnapThresholdPixels / qMax<qreal>(0.25, m_pixelsPerFrame)));
}

int64_t TimelineWidget::nearestClipBoundaryFrame(const QString& excludedClipId, int64_t frame, bool* snapped) const {
    const int64_t threshold = snapThresholdFrames();
    int64_t bestFrame = frame;
    int64_t bestDistance = threshold + 1;

    auto consider = [&](int64_t candidate) {
        const int64_t distance = qAbs(candidate - frame);
        if (distance <= threshold && distance < bestDistance) {
            bestDistance = distance;
            bestFrame = candidate;
        }
    };

    consider(0);
    for (const TimelineClip& clip : m_clips) {
        if (clip.id == excludedClipId) {
            continue;
        }
        consider(clip.startFrame);
        consider(clip.startFrame + clip.durationFrames);
    }

    if (snapped) {
        *snapped = bestDistance <= threshold;
    }
    return bestFrame;
}

int64_t TimelineWidget::snapMoveStartFrame(const TimelineClip& clip,
                                           int64_t proposedStartFrame,
                                           bool* snapped,
                                           int64_t* snappedBoundaryFrame) const {
    const int64_t proposedEndFrame = proposedStartFrame + clip.durationFrames;
    const int64_t threshold = snapThresholdFrames();
    int64_t bestStartFrame = proposedStartFrame;
    int64_t bestBoundary = -1;
    int64_t bestDistance = threshold + 1;

    auto consider = [&](int64_t boundaryFrame) {
        const int64_t startDistance = qAbs(boundaryFrame - proposedStartFrame);
        if (startDistance <= threshold && startDistance < bestDistance) {
            bestDistance = startDistance;
            bestStartFrame = boundaryFrame;
            bestBoundary = boundaryFrame;
        }

        const int64_t endDistance = qAbs(boundaryFrame - proposedEndFrame);
        if (endDistance <= threshold && endDistance < bestDistance) {
            bestDistance = endDistance;
            bestStartFrame = qMax<int64_t>(0, boundaryFrame - clip.durationFrames);
            bestBoundary = boundaryFrame;
        }
    };

    consider(0);
    for (const TimelineClip& other : m_clips) {
        if (other.id == clip.id) {
            continue;
        }
        consider(other.startFrame);
        consider(other.startFrame + other.durationFrames);
    }

    if (snapped) {
        *snapped = bestDistance <= threshold;
    }
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = bestBoundary;
    }
    return bestStartFrame;
}

int64_t TimelineWidget::snapTrimLeftFrame(const TimelineClip& clip,
                                          int64_t proposedStartFrame,
                                          bool* snapped,
                                          int64_t* snappedBoundaryFrame) const {
    Q_UNUSED(clip)
    const int64_t snappedFrame = nearestClipBoundaryFrame(clip.id, proposedStartFrame, snapped);
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = (snapped && *snapped) ? snappedFrame : -1;
    }
    return snappedFrame;
}

int64_t TimelineWidget::snapTrimRightFrame(const TimelineClip& clip,
                                           int64_t proposedEndFrame,
                                           bool* snapped,
                                           int64_t* snappedBoundaryFrame) const {
    Q_UNUSED(clip)
    const int64_t snappedFrame = nearestClipBoundaryFrame(clip.id, proposedEndFrame, snapped);
    if (snappedBoundaryFrame) {
        *snappedBoundaryFrame = (snapped && *snapped) ? snappedFrame : -1;
    }
    return snappedFrame;
}

bool TimelineWidget::nudgeSelectedClip(int direction) {
    if (direction == 0 || m_clipSelection.primaryId.isEmpty()) {
        return false;
    }

    for (TimelineClip& clip : m_clips) {
        if (clip.id != m_clipSelection.primaryId) {
            continue;
        }

        if (clip.locked) {
            return false;
        }

        if (clipIsAudioOnly(clip)) {
            const int64_t nextStartSamples =
                qMax<int64_t>(0, clipTimelineStartSamples(clip) + (direction * kAudioNudgeSamples));
            clip.startFrame = nextStartSamples / kSamplesPerFrame;
            clip.startSubframeSamples = nextStartSamples % kSamplesPerFrame;
        } else {
            clip.startFrame = qMax<int64_t>(0, clip.startFrame + direction);
        }

        normalizeClipTiming(clip);
        sortClips();
        if (clipsChanged) {
            clipsChanged();
        }
        update();
        return true;
    }

    return false;
}

bool TimelineWidget::renameTrack(int trackIndex) {
    if (trackIndex < 0) {
        return false;
    }

    const QString currentName =
        (trackIndex >= 0 && trackIndex < m_tracks.size()) ? m_tracks[trackIndex].name : defaultTrackName(trackIndex);
    bool accepted = false;
    const QString nextName = QInputDialog::getText(this,
                                                   QStringLiteral("Rename Track"),
                                                   QStringLiteral("Track name"),
                                                   QLineEdit::Normal,
                                                   currentName,
                                                   &accepted);
    if (!accepted) {
        return false;
    }

    ensureTrackCount(trackIndex + 1);
    m_tracks[trackIndex].name = nextName.trimmed().isEmpty() ? defaultTrackName(trackIndex) : nextName.trimmed();
    if (clipsChanged) {
        clipsChanged();
    }
    update();
    return true;
}

bool TimelineWidget::deleteTrack(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) {
        return false;
    }

    int clipCountOnTrack = 0;
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex) {
            ++clipCountOnTrack;
        }
    }

    if (clipCountOnTrack > 0) {
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this,
            QStringLiteral("Delete Track"),
            QStringLiteral("Track \"%1\" contains %2 clip%3.\n\nDelete the track and all clips on it?")
                .arg(m_tracks[trackIndex].name.trimmed().isEmpty() ? defaultTrackName(trackIndex) : m_tracks[trackIndex].name)
                .arg(clipCountOnTrack)
                .arg(clipCountOnTrack == 1 ? QString() : QStringLiteral("s")),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (choice != QMessageBox::Yes) {
            return false;
        }
    }

    for (int i = m_clips.size() - 1; i >= 0; --i) {
        if (m_clips[i].trackIndex == trackIndex) {
            m_clips.removeAt(i);
        }
    }
    applyClipSelection(m_clipSelection.ids, m_clipSelection.primaryId, false);

    if (m_tracks.size() > 1) {
        m_tracks.removeAt(trackIndex);
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex > trackIndex) {
                clip.trackIndex -= 1;
            }
        }
    } else {
        TimelineTrack resetTrack;
        resetTrack.name = defaultTrackName(0);
        resetTrack.height = m_tracks[0].height;
        m_tracks[0] = resetTrack;
    }

    if (m_selectedTrackIndex == trackIndex) {
        m_selectedTrackIndex = -1;
    } else if (m_selectedTrackIndex > trackIndex) {
        m_selectedTrackIndex -= 1;
    }

    normalizeTrackIndices();
    sortClips();

    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
    return true;
}

void TimelineWidget::removeEmptyTracks() {
    if (m_tracks.size() <= 1) {
        return;
    }

    QVector<int> emptyTrackIndices;
    for (int i = static_cast<int>(m_tracks.size()) - 1; i >= 0; --i) {
        bool hasClip = false;
        for (const TimelineClip& clip : m_clips) {
            if (clip.trackIndex == i) {
                hasClip = true;
                break;
            }
        }
        if (!hasClip) {
            emptyTrackIndices.push_back(i);
        }
    }

    if (emptyTrackIndices.isEmpty()) {
        return;
    }

    for (int idx : emptyTrackIndices) {
        m_tracks.removeAt(idx);
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex > idx) {
                clip.trackIndex -= 1;
            }
        }
    }

    if (m_selectedTrackIndex >= m_tracks.size()) {
        m_selectedTrackIndex = m_tracks.size() - 1;
    } else if (m_selectedTrackIndex < 0) {
        m_selectedTrackIndex = 0;
    }

    normalizeTrackIndices();
    sortClips();

    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
}

bool TimelineWidget::applyCrossfadeToTrack(int trackIndex, double seconds, bool moveClips) {
    if (trackIndex < 0 || seconds <= 0.0) {
        return false;
    }

    QVector<int> clipIndices;
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].trackIndex == trackIndex) {
            clipIndices.push_back(i);
        }
    }

    if (clipIndices.size() < 2) {
        QMessageBox::information(this,
                                 QStringLiteral("Crossfade Consecutive Clips"),
                                 QStringLiteral("This track needs at least two clips to apply a crossfade."));
        return false;
    }

    for (const int clipIndex : clipIndices) {
        if (m_clips[clipIndex].locked) {
            QMessageBox::warning(this,
                                 QStringLiteral("Crossfade Consecutive Clips"),
                                 QStringLiteral("Unlock all clips on this track before applying a crossfade."));
            return false;
        }
    }

    std::sort(clipIndices.begin(), clipIndices.end(), [&](int a, int b) {
        const int64_t startSamplesA = clipTimelineStartSamples(m_clips[a]);
        const int64_t startSamplesB = clipTimelineStartSamples(m_clips[b]);
        if (startSamplesA == startSamplesB) {
            return m_clips[a].label < m_clips[b].label;
        }
        return startSamplesA < startSamplesB;
    });

    const int fadeSamples = qMax(1, qRound(seconds * static_cast<double>(kAudioSampleRate)));
    const int64_t fadeFrames = qMax<int64_t>(1, qRound64(seconds * static_cast<double>(kTimelineFps)));
    bool changed = false;

    if (moveClips) {
        for (int i = 0; i + 1 < clipIndices.size(); ++i) {
            TimelineClip& leftClip = m_clips[clipIndices[i]];
            TimelineClip& rightClip = m_clips[clipIndices[i + 1]];

            const int64_t leftStartSamples = clipTimelineStartSamples(leftClip);
            const int64_t leftEndSamples = leftStartSamples + (leftClip.durationFrames * kSamplesPerFrame);
            const int64_t targetRightStartSamples = qMax<int64_t>(0, leftEndSamples - fadeSamples);
            if (clipTimelineStartSamples(rightClip) != targetRightStartSamples) {
                rightClip.startFrame = targetRightStartSamples / kSamplesPerFrame;
                rightClip.startSubframeSamples = targetRightStartSamples % kSamplesPerFrame;
                normalizeClipTiming(rightClip);
                changed = true;
            }
        }
    }

    for (int i = 0; i + 1 < clipIndices.size(); ++i) {
        TimelineClip& leftClip = m_clips[clipIndices[i]];
        TimelineClip& rightClip = m_clips[clipIndices[i + 1]];

        if (leftClip.hasAudio || leftClip.mediaType == ClipMediaType::Audio) {
            if (leftClip.fadeSamples != fadeSamples) {
                leftClip.fadeSamples = fadeSamples;
                changed = true;
            }
        }
        if (rightClip.hasAudio || rightClip.mediaType == ClipMediaType::Audio) {
            if (rightClip.fadeSamples != fadeSamples) {
                rightClip.fadeSamples = fadeSamples;
                changed = true;
            }
        }

        const bool leftHasVisuals = clipHasVisuals(leftClip);
        const bool rightHasVisuals = clipHasVisuals(rightClip);
        applyVisualCrossfade(leftClip, false, fadeFrames);
        applyVisualCrossfade(rightClip, true, fadeFrames);
        if (leftHasVisuals || rightHasVisuals) {
            changed = true;
        }
    }

    if (!changed) {
        return false;
    }

    sortClips();
    if (clipsChanged) {
        clipsChanged();
    }
    update();
    return true;
}

void TimelineWidget::setExportRange(int64_t startFrame, int64_t endFrame) {
    setExportRanges({ExportRangeSegment{startFrame, endFrame}});
}

void TimelineWidget::setExportRanges(const QVector<ExportRangeSegment>& ranges) {
    m_exportRanges = ranges;
    normalizeExportRanges();
    update();
}

bool TimelineWidget::isVisualMediaType(ClipMediaType type) const {
    return type == ClipMediaType::Image || type == ClipMediaType::Video || type == ClipMediaType::Title;
}

bool TimelineWidget::isAudioMediaType(ClipMediaType type) const {
    return type == ClipMediaType::Audio;
}

bool TimelineWidget::wouldClipConflictWithTrack(const TimelineClip& clip, int trackIndex, const QString& excludeClipId) const {
    const bool clipIsVisual = isVisualMediaType(clip.mediaType);
    const bool clipIsAudio = isAudioMediaType(clip.mediaType);
    
    // Calculate clip time range
    const int64_t clipStart = clip.startFrame;
    const int64_t clipEnd = clip.startFrame + clip.durationFrames;
    
    for (const TimelineClip& other : m_clips) {
        if (other.id == excludeClipId || other.id == clip.id) {
            continue;
        }
        if (other.trackIndex != trackIndex) {
            continue;
        }
        
        const bool otherIsVisual = isVisualMediaType(other.mediaType);
        const bool otherIsAudio = isAudioMediaType(other.mediaType);
        
        // Only check for conflicts if they're the same media type
        if ((clipIsVisual && otherIsVisual) || (clipIsAudio && otherIsAudio)) {
            // Check for time overlap
            const int64_t otherStart = other.startFrame;
            const int64_t otherEnd = other.startFrame + other.durationFrames;
            
            // Check if the clips overlap in time
            if (clipEnd > otherStart && clipStart < otherEnd) {
                return true;
            }
        }
    }
    return false;
}
