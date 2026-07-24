#include "timeline_widget.h"
#include "editor_effect_presets.h"
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

std::vector<jcut::export_range::Range> sharedExportRanges(
    const QVector<ExportRangeSegment>& ranges)
{
    std::vector<jcut::export_range::Range> result;
    result.reserve(static_cast<std::size_t>(ranges.size()));
    for (const ExportRangeSegment& range : ranges) {
        result.push_back({range.startFrame, range.endFrame});
    }
    return result;
}

QVector<ExportRangeSegment> qtExportRanges(
    const std::vector<jcut::export_range::Range>& ranges)
{
    QVector<ExportRangeSegment> result;
    result.reserve(static_cast<qsizetype>(ranges.size()));
    for (const jcut::export_range::Range& range : ranges) {
        result.push_back(
            {range.startFrame, range.endFrame});
    }
    return result;
}

void upsertTransformKeyframe(QVector<TimelineClip::TransformKeyframe>& keyframes,
                             const TimelineClip::TransformKeyframe& keyframe) {
    for (TimelineClip::TransformKeyframe& existing : keyframes) {
        if (existing.frame == keyframe.frame) {
            existing = keyframe;
            return;
        }
    }
    keyframes.push_back(keyframe);
}

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

void splitTransformKeyframes(const TimelineClip& source,
                             int64_t splitLocalFrame,
                             QVector<TimelineClip::TransformKeyframe>* left,
                             QVector<TimelineClip::TransformKeyframe>* right) {
    left->clear();
    right->clear();
    bool hasLeftKey = false;
    bool hasRightKey = false;
    for (const TimelineClip::TransformKeyframe& keyframe : source.transformKeyframes) {
        if (keyframe.frame < splitLocalFrame) {
            left->push_back(keyframe);
            hasLeftKey = true;
        } else {
            TimelineClip::TransformKeyframe shifted = keyframe;
            shifted.frame -= splitLocalFrame;
            right->push_back(shifted);
            hasRightKey = true;
        }
    }
    if (hasRightKey && splitLocalFrame > 0) {
        TimelineClip::TransformKeyframe leftBoundary =
            evaluateClipKeyframeOffsetAtFrame(source, source.startFrame + splitLocalFrame - 1);
        leftBoundary.frame = splitLocalFrame - 1;
        upsertTransformKeyframe(*left, leftBoundary);
    }
    if (hasLeftKey) {
        TimelineClip::TransformKeyframe rightBoundary =
            evaluateClipKeyframeOffsetAtFrame(source, source.startFrame + splitLocalFrame);
        rightBoundary.frame = 0;
        upsertTransformKeyframe(*right, rightBoundary);
    }
}

void splitGradingKeyframes(const TimelineClip& source,
                           int64_t splitLocalFrame,
                           QVector<TimelineClip::GradingKeyframe>* left,
                           QVector<TimelineClip::GradingKeyframe>* right) {
    left->clear();
    right->clear();
    bool hasLeftKey = false;
    bool hasRightKey = false;
    for (const TimelineClip::GradingKeyframe& keyframe : source.gradingKeyframes) {
        if (keyframe.frame < splitLocalFrame) {
            left->push_back(keyframe);
            hasLeftKey = true;
        } else {
            TimelineClip::GradingKeyframe shifted = keyframe;
            shifted.frame -= splitLocalFrame;
            right->push_back(shifted);
            hasRightKey = true;
        }
    }
    if (hasRightKey && splitLocalFrame > 0) {
        TimelineClip::GradingKeyframe leftBoundary = evaluateClipGradingAtPosition(
            source, static_cast<qreal>(source.startFrame + splitLocalFrame - 1));
        leftBoundary.frame = splitLocalFrame - 1;
        upsertGradingKeyframe(*left, leftBoundary);
    }
    if (hasLeftKey) {
        TimelineClip::GradingKeyframe rightBoundary = evaluateClipGradingAtPosition(
            source, static_cast<qreal>(source.startFrame + splitLocalFrame));
        rightBoundary.frame = 0;
        upsertGradingKeyframe(*right, rightBoundary);
    }
}

void splitOpacityKeyframes(const TimelineClip& source,
                           int64_t splitLocalFrame,
                           QVector<TimelineClip::OpacityKeyframe>* left,
                           QVector<TimelineClip::OpacityKeyframe>* right) {
    left->clear();
    right->clear();
    bool hasLeftKey = false;
    bool hasRightKey = false;
    for (const TimelineClip::OpacityKeyframe& keyframe : source.opacityKeyframes) {
        if (keyframe.frame < splitLocalFrame) {
            left->push_back(keyframe);
            hasLeftKey = true;
        } else {
            TimelineClip::OpacityKeyframe shifted = keyframe;
            shifted.frame -= splitLocalFrame;
            right->push_back(shifted);
            hasRightKey = true;
        }
    }
    if (hasRightKey && splitLocalFrame > 0) {
        TimelineClip::OpacityKeyframe leftBoundary;
        leftBoundary.frame = splitLocalFrame - 1;
        leftBoundary.opacity = evaluateClipOpacityAtPosition(
            source, static_cast<qreal>(source.startFrame + splitLocalFrame - 1));
        upsertOpacityKeyframe(*left, leftBoundary);
    }
    if (hasLeftKey) {
        TimelineClip::OpacityKeyframe rightBoundary;
        rightBoundary.frame = 0;
        rightBoundary.opacity = evaluateClipOpacityAtPosition(
            source, static_cast<qreal>(source.startFrame + splitLocalFrame));
        upsertOpacityKeyframe(*right, rightBoundary);
    }
}

template <typename Keyframe>
void splitSimpleKeyframes(const QVector<Keyframe>& source,
                          int64_t splitLocalFrame,
                          QVector<Keyframe>* left,
                          QVector<Keyframe>* right) {
    left->clear();
    right->clear();
    for (const Keyframe& keyframe : source) {
        if (keyframe.frame < splitLocalFrame) {
            left->push_back(keyframe);
        } else {
            Keyframe shifted = keyframe;
            shifted.frame -= splitLocalFrame;
            right->push_back(std::move(shifted));
        }
    }
}

template <typename Keyframe>
void trimSimpleKeyframesFromStart(
    QVector<Keyframe>* keyframes, int64_t trimFrames) {
    QVector<Keyframe> shifted;
    shifted.reserve(keyframes->size());
    for (const Keyframe& keyframe : std::as_const(*keyframes)) {
        if (keyframe.frame < trimFrames) {
            continue;
        }
        Keyframe value = keyframe;
        value.frame -= trimFrames;
        shifted.push_back(std::move(value));
    }
    *keyframes = std::move(shifted);
}

void normalizeSpeakerTitleParentBounds(
    QVector<TimelineClip>* clips) {
    if (!clips) {
        return;
    }
    struct ParentRange {
        int64_t startFrame = 0;
        int64_t endFrame = 0;
    };
    QHash<QString, ParentRange> parentRanges;
    for (const TimelineClip& clip : std::as_const(*clips)) {
        if (clip.clipRole == ClipRole::MaskMatte ||
            clip.clipRole == ClipRole::SpeakerTitle) {
            continue;
        }
        const QString clipId = clip.id.trimmed();
        if (!clipId.isEmpty()) {
            parentRanges.insert(
                clipId,
                {clip.startFrame,
                 clip.startFrame +
                     qMax<int64_t>(1, clip.durationFrames)});
        }
    }

    clips->erase(
        std::remove_if(
            clips->begin(), clips->end(),
            [&](TimelineClip& title) {
                if (title.clipRole !=
                    ClipRole::SpeakerTitle) {
                    return false;
                }
                const auto parent = parentRanges.constFind(
                    title.linkedSourceClipId.trimmed());
                if (parent == parentRanges.cend()) {
                    return true;
                }
                const int64_t titleStart = title.startFrame;
                const int64_t titleEnd =
                    title.startFrame +
                    qMax<int64_t>(
                        1, title.durationFrames);
                const int64_t boundedStart = qMax(
                    titleStart, parent->startFrame);
                const int64_t boundedEnd = qMin(
                    titleEnd, parent->endFrame);
                if (boundedEnd <= boundedStart) {
                    return true;
                }

                const int64_t trimmedFrames =
                    boundedStart - titleStart;
                if (trimmedFrames > 0) {
                    trimSimpleKeyframesFromStart(
                        &title.transformKeyframes,
                        trimmedFrames);
                    trimSimpleKeyframesFromStart(
                        &title.gradingKeyframes,
                        trimmedFrames);
                    trimSimpleKeyframesFromStart(
                        &title.opacityKeyframes,
                        trimmedFrames);
                    trimSimpleKeyframesFromStart(
                        &title.titleKeyframes,
                        trimmedFrames);
                }
                title.startFrame = boundedStart;
                title.durationFrames =
                    boundedEnd - boundedStart;
                title.sourceDurationFrames =
                    title.durationFrames;
                normalizeClipTransformKeyframes(title);
                normalizeClipGradingKeyframes(title);
                normalizeClipOpacityKeyframes(title);
                normalizeClipTitleKeyframes(title);
                normalizeClipTiming(title);
                return false;
            }),
        clips->end());
}

void splitCorrectionPolygons(const QVector<TimelineClip::CorrectionPolygon>& source,
                             int64_t splitLocalFrame,
                             QVector<TimelineClip::CorrectionPolygon>* left,
                             QVector<TimelineClip::CorrectionPolygon>* right) {
    left->clear();
    right->clear();
    for (const TimelineClip::CorrectionPolygon& polygon : source) {
        const int64_t polygonEnd = polygon.endFrame < 0
            ? std::numeric_limits<int64_t>::max()
            : polygon.endFrame;
        if (polygon.startFrame < splitLocalFrame) {
            TimelineClip::CorrectionPolygon leftPolygon = polygon;
            if (leftPolygon.endFrame < 0 || leftPolygon.endFrame >= splitLocalFrame) {
                leftPolygon.endFrame = splitLocalFrame - 1;
            }
            left->push_back(std::move(leftPolygon));
        }
        if (polygonEnd >= splitLocalFrame) {
            TimelineClip::CorrectionPolygon rightPolygon = polygon;
            rightPolygon.startFrame = qMax<int64_t>(0, rightPolygon.startFrame - splitLocalFrame);
            if (rightPolygon.endFrame >= 0) {
                rightPolygon.endFrame -= splitLocalFrame;
            }
            right->push_back(std::move(rightPolygon));
        }
    }
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
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(130);
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
    const int64_t boundedFrame = qMax<int64_t>(0, frame);
    if (m_currentFrame == boundedFrame) {
        return;
    }
    m_currentFrame = boundedFrame;
    normalizeExportRange();
    update();
}

void TimelineWidget::setAudioTabWaveformsVisible(bool visible) {
    if (m_audioTabWaveformsVisible == visible) {
        return;
    }
    m_audioTabWaveformsVisible = visible;
    update();
}

int64_t TimelineWidget::totalFrames() const {
    int64_t lastFrame = 300;
    for (const TimelineClip& clip : m_clips) {
        lastFrame = qMax(lastFrame, clip.startFrame + clip.durationFrames + kTimelineFps);
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
    invalidateHoveredClipToolTipCache();
    m_clips = clips;
    for (TimelineClip& clip : m_clips) {
        if (clip.zLevel == TimelineClip::kAutomaticZLevel) {
            clip.zLevel = effectiveClipZLevel(clip);
        }
    }
    reconcileMaskMatteChildrenFromDisk(m_clips);
    normalizeSpeakerTitleParentBounds(&m_clips);
    normalizeRenderSyncMarkerOwnership();

    // A persisted/generated child track is identified by its child clip, not
    // by its row index. Older states can contain duplicate or stale rows after
    // repeated mask discovery. Keep the row currently owned by each matte and
    // remove every other generated row before rebuilding the hierarchy.
    QSet<QString> maskMatteIds;
    QHash<QString, int> preferredChildTrackById;
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (clip.clipRole != ClipRole::MaskMatte || clip.id.trimmed().isEmpty()) {
            continue;
        }
        const QString childId = clip.id.trimmed();
        maskMatteIds.insert(childId);
        if (clip.trackIndex >= 0 && clip.trackIndex < m_tracks.size()) {
            const TimelineTrack& track = m_tracks.at(clip.trackIndex);
            if (track.generatedChildTrack && track.childClipId.trimmed() == childId) {
                preferredChildTrackById.insert(childId, clip.trackIndex);
            }
        }
    }
    for (int trackIndex = 0; trackIndex < m_tracks.size(); ++trackIndex) {
        const TimelineTrack& track = m_tracks.at(trackIndex);
        const QString childId = track.childClipId.trimmed();
        if (track.generatedChildTrack && maskMatteIds.contains(childId) &&
            !preferredChildTrackById.contains(childId)) {
            preferredChildTrackById.insert(childId, trackIndex);
        }
    }
    for (int trackIndex = m_tracks.size() - 1; trackIndex >= 0; --trackIndex) {
        const TimelineTrack& track = m_tracks.at(trackIndex);
        if (!track.generatedChildTrack) {
            continue;
        }
        const QString childId = track.childClipId.trimmed();
        bool hasOwnedOccupant = false;
        bool containsOnlyOwnedChildren = true;
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (clip.trackIndex != trackIndex) {
                continue;
            }
            hasOwnedOccupant = true;
            if (clip.clipRole != ClipRole::MaskMatte &&
                clip.clipRole != ClipRole::SpeakerTitle) {
                containsOnlyOwnedChildren = false;
                break;
            }
        }
        const bool keepSharedChildLane =
            hasOwnedOccupant && containsOnlyOwnedChildren;
        const bool keep =
            keepSharedChildLane ||
            (maskMatteIds.contains(childId) &&
             preferredChildTrackById.value(childId, -1) ==
                 trackIndex);
        if (keep) {
            continue;
        }
        const TimelineClip* foreignOccupant = nullptr;
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (clip.trackIndex == trackIndex && clip.id.trimmed() != childId) {
                foreignOccupant = &clip;
                break;
            }
        }
        if (foreignOccupant) {
            // A historical row-reorder bug could strand the source parent on
            // a duplicate generated row. Deleting that row would remap the
            // parent onto the surviving child row and recreate the duplicate
            // on the next reconciliation pass. Recover the occupied row as a
            // neutral base track instead; the generated binding is derived.
            // Preserve the row's user-facing playback state. In real projects
            // the source plate is often intentionally hidden while its Mask
            // Matte children remain visible; resetting visualMode here would
            // expose the full-frame parent after repair. Only the stale
            // relationship fields are derived and therefore safe to clear.
            TimelineTrack recoveredTrack = track;
            recoveredTrack.generatedChildTrack = false;
            recoveredTrack.parentClipId.clear();
            recoveredTrack.childClipId.clear();
            recoveredTrack.name = foreignOccupant->label.trimmed().isEmpty()
                ? defaultTrackName(trackIndex)
                : foreignOccupant->label.trimmed();
            recoveredTrack.height = qMax(kMinTrackHeight, track.height);
            m_tracks[trackIndex] = std::move(recoveredTrack);
            continue;
        }
        m_tracks.removeAt(trackIndex);
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex > trackIndex) {
                --clip.trackIndex;
            } else if (clip.trackIndex == trackIndex) {
                clip.trackIndex = qMax(0, clip.trackIndex - 1);
            }
        }
        if (m_selectedTrackIndex > trackIndex) {
            --m_selectedTrackIndex;
        } else if (m_selectedTrackIndex == trackIndex) {
            m_selectedTrackIndex = -1;
        }
    }

    // A surviving generated row must contain only its designated child. If a
    // malformed project placed ordinary media on that row, give the media a
    // neutral base track before child-track discovery. Guessing another
    // existing row could create overlaps or transfer child visibility/effect
    // state to the parent.
    const int trackCountBeforeRecovery = m_tracks.size();
    for (int trackIndex = 0; trackIndex < trackCountBeforeRecovery; ++trackIndex) {
        if (!m_tracks.at(trackIndex).generatedChildTrack) {
            continue;
        }
        const QString designatedChildId = m_tracks.at(trackIndex).childClipId.trimmed();
        int recoveredTrackIndex = -1;
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex != trackIndex || clip.id.trimmed() == designatedChildId ||
                clip.clipRole == ClipRole::MaskMatte ||
                clip.clipRole == ClipRole::SpeakerTitle) {
                continue;
            }
            if (recoveredTrackIndex < 0) {
                TimelineTrack recoveredTrack;
                recoveredTrack.name = clip.label.trimmed().isEmpty()
                    ? defaultTrackName(m_tracks.size())
                    : clip.label.trimmed();
                recoveredTrack.height = qMax(kMinTrackHeight, m_tracks.at(trackIndex).height);
                recoveredTrackIndex = m_tracks.size();
                m_tracks.push_back(std::move(recoveredTrack));
            }
            clip.trackIndex = recoveredTrackIndex;
        }
    }
    ensureTrackCount(trackCount());

    QHash<int, int> clipCountByTrack;
    QHash<QString, int> sourceTrackById;
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        ++clipCountByTrack[clip.trackIndex];
        if (clip.clipRole != ClipRole::MaskMatte && !clip.id.trimmed().isEmpty()) {
            sourceTrackById.insert(clip.id.trimmed(), clip.trackIndex);
        }
    }
    for (TimelineClip& child : m_clips) {
        if (child.clipRole != ClipRole::MaskMatte) continue;
        const int sourceTrack = sourceTrackById.value(child.linkedSourceClipId.trimmed(), -1);
        if (sourceTrack < 0) continue;
        const bool dedicatedExistingTrack =
            child.trackIndex >= 0 && child.trackIndex < m_tracks.size() &&
            child.trackIndex != sourceTrack &&
            (m_tracks.at(child.trackIndex).generatedChildTrack ||
             clipCountByTrack.value(child.trackIndex) == 1);
        if (!dedicatedExistingTrack) {
            TimelineTrack childTrack;
            childTrack.height = 44;
            childTrack.audioEnabled = false;
            childTrack.audioWaveformVisible = false;
            m_tracks.push_back(childTrack);
            child.trackIndex = m_tracks.size() - 1;
        }
        TimelineTrack& track = m_tracks[child.trackIndex];
        track.generatedChildTrack = true;
        track.parentClipId = child.linkedSourceClipId.trimmed();
        track.childClipId = child.id;
        track.name = QStringLiteral("↳ %1").arg(child.label);
        track.height = qBound(kMinTrackHeight, track.height, 56);
        track.audioEnabled = false;
        track.audioWaveformVisible = false;
    }

    // Keep child tracks adjacent to their source track without using that
    // layout position as compositing order.
    QVector<int> baseTracks;
    QHash<int, QVector<int>> childTracksBySourceTrack;
    QSet<int> placedChildTracks;
    for (int trackIndex = 0; trackIndex < m_tracks.size(); ++trackIndex) {
        const TimelineTrack& track = m_tracks.at(trackIndex);
        if (!track.generatedChildTrack) {
            baseTracks.push_back(trackIndex);
            continue;
        }
        const int sourceTrack = sourceTrackById.value(track.parentClipId.trimmed(), -1);
        if (sourceTrack >= 0) childTracksBySourceTrack[sourceTrack].push_back(trackIndex);
    }
    QVector<int> desiredTrackOrder;
    desiredTrackOrder.reserve(m_tracks.size());
    for (const int baseTrack : std::as_const(baseTracks)) {
        desiredTrackOrder.push_back(baseTrack);
        QVector<int> children = childTracksBySourceTrack.value(baseTrack);
        std::sort(children.begin(), children.end(), [this](int leftTrack, int rightTrack) {
            const auto childForTrack = [this](int trackIndex) -> const TimelineClip* {
                for (const TimelineClip& clip : m_clips) {
                    if (clip.trackIndex == trackIndex && clip.clipRole == ClipRole::MaskMatte) return &clip;
                }
                return nullptr;
            };
            const TimelineClip* left = childForTrack(leftTrack);
            const TimelineClip* right = childForTrack(rightTrack);
            if (!left || !right) return leftTrack < rightTrack;
            const int labelOrder = QString::localeAwareCompare(left->label, right->label);
            return labelOrder == 0 ? left->id < right->id : labelOrder < 0;
        });
        for (const int childTrack : std::as_const(children)) {
            desiredTrackOrder.push_back(childTrack);
            placedChildTracks.insert(childTrack);
        }
    }
    for (int trackIndex = 0; trackIndex < m_tracks.size(); ++trackIndex) {
        if (m_tracks.at(trackIndex).generatedChildTrack && !placedChildTracks.contains(trackIndex)) {
            desiredTrackOrder.push_back(trackIndex);
        }
    }
    if (desiredTrackOrder.size() == m_tracks.size()) {
        QVector<TimelineTrack> reorderedTracks;
        reorderedTracks.reserve(m_tracks.size());
        QHash<int, int> newIndexByOldIndex;
        for (const int oldIndex : std::as_const(desiredTrackOrder)) {
            newIndexByOldIndex.insert(oldIndex, reorderedTracks.size());
            reorderedTracks.push_back(m_tracks.at(oldIndex));
        }
        for (TimelineClip& clip : m_clips) {
            clip.trackIndex = newIndexByOldIndex.value(clip.trackIndex, clip.trackIndex);
        }
        m_selectedTrackIndex = newIndexByOldIndex.value(m_selectedTrackIndex, m_selectedTrackIndex);
        m_tracks = reorderedTracks;
    }

    // Speaker introductions are generated from transcript parameters as one
    // immutable collection per source clip. Migrate legacy ordinary/multi-lane
    // layouts into a single generated child track; overlapping titles remain
    // on that lane because they are regenerated as a unit.
    struct SpeakerTitleGroup {
        QString parentId;
        int parentTrackIndex = -1;
        QVector<int> childClipIndices;
        int childTrackIndex = -1;
    };
    QHash<QString, int> currentSourceTrackById;
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (clip.clipRole != ClipRole::MaskMatte &&
            clip.clipRole != ClipRole::SpeakerTitle &&
            !clip.id.trimmed().isEmpty()) {
            currentSourceTrackById.insert(
                clip.id.trimmed(), clip.trackIndex);
        }
    }
    QVector<SpeakerTitleGroup> speakerTitleGroups;
    QHash<QString, int> speakerTitleGroupByParent;
    for (int clipIndex = 0; clipIndex < m_clips.size();
         ++clipIndex) {
        TimelineClip& title = m_clips[clipIndex];
        if (title.clipRole != ClipRole::SpeakerTitle) {
            continue;
        }
        const QString parentId =
            title.linkedSourceClipId.trimmed();
        const int parentTrackIndex =
            currentSourceTrackById.value(parentId, -1);
        if (parentId.isEmpty() || parentTrackIndex < 0) {
            continue;
        }
        title.linkedSourceClipId = parentId;
        title.syncLockedToSource = true;
        title.locked = true;
        int groupIndex =
            speakerTitleGroupByParent.value(parentId, -1);
        if (groupIndex < 0) {
            groupIndex = speakerTitleGroups.size();
            speakerTitleGroupByParent.insert(
                parentId, groupIndex);
            SpeakerTitleGroup group;
            group.parentId = parentId;
            group.parentTrackIndex = parentTrackIndex;
            speakerTitleGroups.push_back(std::move(group));
        }
        speakerTitleGroups[groupIndex].childClipIndices.push_back(
            clipIndex);
    }

    QSet<int> claimedSpeakerTitleTracks;
    QSet<int> obsoleteSpeakerTitleTracks;
    for (SpeakerTitleGroup& group : speakerTitleGroups) {
        QSet<int> childIndices(
            group.childClipIndices.cbegin(),
            group.childClipIndices.cend());
        QVector<int> dedicatedTrackIndices;
        for (int trackIndex = 0; trackIndex < m_tracks.size();
             ++trackIndex) {
            if (trackIndex == group.parentTrackIndex) {
                continue;
            }
            QVector<int> occupants;
            for (int clipIndex = 0;
                 clipIndex < m_clips.size(); ++clipIndex) {
                if (m_clips.at(clipIndex).trackIndex ==
                    trackIndex) {
                    occupants.push_back(clipIndex);
                }
            }
            if (occupants.isEmpty()) {
                continue;
            }
            const bool dedicated = std::all_of(
                occupants.cbegin(), occupants.cend(),
                [&](int occupantIndex) {
                    return childIndices.contains(occupantIndex);
                });
            if (dedicated) {
                dedicatedTrackIndices.push_back(trackIndex);
            }
        }

        const int currentSharedTrack =
            m_clips.at(group.childClipIndices.constFirst())
                .trackIndex;
        const bool groupAlreadySharesGeneratedTrack =
            currentSharedTrack >= 0 &&
            currentSharedTrack < m_tracks.size() &&
            currentSharedTrack != group.parentTrackIndex &&
            m_tracks.at(currentSharedTrack)
                .generatedChildTrack &&
            std::all_of(
                group.childClipIndices.cbegin(),
                group.childClipIndices.cend(),
                [&](int childIndex) {
                    return m_clips.at(childIndex).trackIndex ==
                        currentSharedTrack;
                }) &&
            std::all_of(
                m_clips.cbegin(), m_clips.cend(),
                [&](const TimelineClip& occupant) {
                    return occupant.trackIndex !=
                            currentSharedTrack ||
                        occupant.clipRole ==
                            ClipRole::SpeakerTitle;
                });
        if (groupAlreadySharesGeneratedTrack) {
            group.childTrackIndex = currentSharedTrack;
        } else {
            for (int trackIndex = 0;
                 trackIndex < m_tracks.size();
                 ++trackIndex) {
                const TimelineTrack& track =
                    m_tracks.at(trackIndex);
                if (track.generatedChildTrack &&
                    track.parentClipId.trimmed() ==
                        group.parentId &&
                    !claimedSpeakerTitleTracks.contains(
                        trackIndex)) {
                    group.childTrackIndex = trackIndex;
                    break;
                }
            }
        }
        if (group.childTrackIndex < 0) {
            std::sort(dedicatedTrackIndices.begin(),
                      dedicatedTrackIndices.end());
            for (const int trackIndex :
                 std::as_const(dedicatedTrackIndices)) {
                if (!claimedSpeakerTitleTracks.contains(
                        trackIndex)) {
                    group.childTrackIndex = trackIndex;
                    break;
                }
            }
        }
        if (group.childTrackIndex < 0) {
            TimelineTrack childTrack;
            childTrack.height = 44;
            childTrack.audioEnabled = false;
            childTrack.audioWaveformVisible = false;
            m_tracks.push_back(std::move(childTrack));
            group.childTrackIndex = m_tracks.size() - 1;
        }
        claimedSpeakerTitleTracks.insert(
            group.childTrackIndex);
        for (const int trackIndex :
             std::as_const(dedicatedTrackIndices)) {
            if (trackIndex != group.childTrackIndex) {
                obsoleteSpeakerTitleTracks.insert(trackIndex);
            }
        }
        for (const int childIndex :
             std::as_const(group.childClipIndices)) {
            m_clips[childIndex].trackIndex =
                group.childTrackIndex;
        }
        TimelineTrack& childTrack =
            m_tracks[group.childTrackIndex];
        childTrack.generatedChildTrack = true;
        childTrack.parentClipId = group.parentId;
        childTrack.childClipId =
            m_clips.at(group.childClipIndices.constFirst())
                .id.trimmed();
        childTrack.name = QStringLiteral(
            "↳ Transcript • Speaker Introductions");
        childTrack.height = qBound(
            kMinTrackHeight, childTrack.height, 56);
        childTrack.audioEnabled = false;
        childTrack.audioWaveformVisible = false;
    }

    QList<int> obsoleteTrackIndices =
        obsoleteSpeakerTitleTracks.values();
    std::sort(obsoleteTrackIndices.begin(),
              obsoleteTrackIndices.end(), std::greater<int>());
    for (const int trackIndex :
         std::as_const(obsoleteTrackIndices)) {
        const bool occupied = std::any_of(
            m_clips.cbegin(), m_clips.cend(),
            [trackIndex](const TimelineClip& clip) {
                return clip.trackIndex == trackIndex;
            });
        if (occupied || trackIndex < 0 ||
            trackIndex >= m_tracks.size()) {
            continue;
        }
        m_tracks.removeAt(trackIndex);
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex > trackIndex) {
                --clip.trackIndex;
            }
        }
        if (m_selectedTrackIndex > trackIndex) {
            --m_selectedTrackIndex;
        } else if (m_selectedTrackIndex == trackIndex) {
            m_selectedTrackIndex = -1;
        }
    }

    currentSourceTrackById.clear();
    for (const TimelineClip& clip : std::as_const(m_clips)) {
        if (clip.clipRole != ClipRole::MaskMatte &&
            clip.clipRole != ClipRole::SpeakerTitle &&
            !clip.id.trimmed().isEmpty()) {
            currentSourceTrackById.insert(
                clip.id.trimmed(), clip.trackIndex);
        }
    }
    QVector<int> allBaseTracks;
    QHash<int, QVector<int>> allChildrenBySourceTrack;
    QSet<int> allPlacedChildTracks;
    for (int trackIndex = 0; trackIndex < m_tracks.size();
         ++trackIndex) {
        const TimelineTrack& track = m_tracks.at(trackIndex);
        if (!track.generatedChildTrack) {
            allBaseTracks.push_back(trackIndex);
            continue;
        }
        const int parentTrackIndex =
            currentSourceTrackById.value(
                track.parentClipId.trimmed(), -1);
        if (parentTrackIndex >= 0) {
            allChildrenBySourceTrack[parentTrackIndex]
                .push_back(trackIndex);
        }
    }
    QVector<int> allDesiredTrackOrder;
    allDesiredTrackOrder.reserve(m_tracks.size());
    for (const int baseTrack :
         std::as_const(allBaseTracks)) {
        allDesiredTrackOrder.push_back(baseTrack);
        for (const int childTrack :
             allChildrenBySourceTrack.value(baseTrack)) {
            allDesiredTrackOrder.push_back(childTrack);
            allPlacedChildTracks.insert(childTrack);
        }
    }
    for (int trackIndex = 0; trackIndex < m_tracks.size();
         ++trackIndex) {
        if (m_tracks.at(trackIndex).generatedChildTrack &&
            !allPlacedChildTracks.contains(trackIndex)) {
            allDesiredTrackOrder.push_back(trackIndex);
        }
    }
    if (allDesiredTrackOrder.size() == m_tracks.size()) {
        QVector<TimelineTrack> reorderedTracks;
        reorderedTracks.reserve(m_tracks.size());
        QHash<int, int> newIndexByOldIndex;
        for (const int oldIndex :
             std::as_const(allDesiredTrackOrder)) {
            newIndexByOldIndex.insert(
                oldIndex, reorderedTracks.size());
            reorderedTracks.push_back(
                m_tracks.at(oldIndex));
        }
        for (TimelineClip& clip : m_clips) {
            clip.trackIndex = newIndexByOldIndex.value(
                clip.trackIndex, clip.trackIndex);
        }
        m_selectedTrackIndex = newIndexByOldIndex.value(
            m_selectedTrackIndex, m_selectedTrackIndex);
        m_tracks = std::move(reorderedTracks);
    }

    // Version-1 mask mattes occupied synthetic top-level tracks. Once their
    // clips are nested back into the source track, remove only those known
    // empty compatibility tracks and remap the remaining indices.
    for (int trackIndex = m_tracks.size() - 1; trackIndex >= 0; --trackIndex) {
        const bool legacyMaskTrack =
            m_tracks.at(trackIndex).name.trimmed().startsWith(
                QStringLiteral("Mask Mattes"), Qt::CaseInsensitive);
        const bool occupied = std::any_of(
            m_clips.cbegin(), m_clips.cend(), [trackIndex](const TimelineClip& clip) {
                return clip.trackIndex == trackIndex;
            });
        if (!legacyMaskTrack || occupied) {
            continue;
        }
        m_tracks.removeAt(trackIndex);
        for (TimelineClip& clip : m_clips) {
            if (clip.trackIndex > trackIndex) --clip.trackIndex;
        }
        if (m_selectedTrackIndex > trackIndex) --m_selectedTrackIndex;
        else if (m_selectedTrackIndex == trackIndex) m_selectedTrackIndex = -1;
    }
    // setClips establishes the hierarchy first; restore the user's alternate
    // row view after child-track discovery and compatibility cleanup.
    reorderTracksForView();
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
    invalidateHoveredClipToolTipCache();
    m_tracks = tracks;
    ensureTrackCount(trackCount());
    reorderTracksForView();
    normalizeExportRange();
    updateMinimumTimelineHeight();
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    update();
}

void TimelineWidget::setTrackViewMode(TrackViewMode mode) {
    if (m_trackViewMode == mode) {
        return;
    }
    m_trackViewMode = mode;
    reorderTracksForView();
    sortClips();
    updateMinimumTimelineHeight();
    if (selectionChanged) selectionChanged();
    if (clipsChanged) clipsChanged();
    if (trackLayoutChanged) trackLayoutChanged();
    update();
}

void TimelineWidget::reorderTracksForView() {
    if (m_tracks.size() < 2) {
        return;
    }

    QVector<int> order;
    order.reserve(m_tracks.size());

    if (m_trackViewMode == TrackViewMode::Precedence) {
        QVector<int> highestZByTrack(m_tracks.size(), std::numeric_limits<int>::min());
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (clip.trackIndex >= 0 && clip.trackIndex < highestZByTrack.size()) {
                highestZByTrack[clip.trackIndex] =
                    qMax(highestZByTrack.at(clip.trackIndex), effectiveClipZLevel(clip));
            }
        }
        for (int i = 0; i < m_tracks.size(); ++i) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&highestZByTrack](int left, int right) {
            return highestZByTrack.at(left) > highestZByTrack.at(right);
        });
    } else {
        QHash<QString, int> sourceTrackById;
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (clip.clipRole != ClipRole::MaskMatte && !clip.id.trimmed().isEmpty()) {
                sourceTrackById.insert(clip.id.trimmed(), clip.trackIndex);
            }
        }
        QHash<int, QVector<int>> childrenBySourceTrack;
        QSet<int> placedChildren;
        for (int i = 0; i < m_tracks.size(); ++i) {
            const TimelineTrack& track = m_tracks.at(i);
            if (!track.generatedChildTrack) continue;
            const int sourceTrack = sourceTrackById.value(track.parentClipId.trimmed(), -1);
            if (sourceTrack >= 0) childrenBySourceTrack[sourceTrack].push_back(i);
        }
        for (int i = 0; i < m_tracks.size(); ++i) {
            if (m_tracks.at(i).generatedChildTrack) continue;
            order.push_back(i);
            const QVector<int> children = childrenBySourceTrack.value(i);
            for (const int childTrack : children) {
                order.push_back(childTrack);
                placedChildren.insert(childTrack);
            }
        }
        for (int i = 0; i < m_tracks.size(); ++i) {
            if (m_tracks.at(i).generatedChildTrack && !placedChildren.contains(i)) order.push_back(i);
        }
    }

    if (order.size() != m_tracks.size()) {
        return;
    }
    bool alreadyOrdered = true;
    for (int i = 0; i < order.size(); ++i) alreadyOrdered = alreadyOrdered && order.at(i) == i;
    if (alreadyOrdered) {
        return;
    }

    QVector<TimelineTrack> reorderedTracks;
    reorderedTracks.reserve(m_tracks.size());
    QHash<int, int> newIndexByOldIndex;
    for (const int oldIndex : std::as_const(order)) {
        newIndexByOldIndex.insert(oldIndex, reorderedTracks.size());
        reorderedTracks.push_back(m_tracks.at(oldIndex));
    }
    for (TimelineClip& clip : m_clips) {
        clip.trackIndex = newIndexByOldIndex.value(clip.trackIndex, clip.trackIndex);
    }
    m_selectedTrackIndex = newIndexByOldIndex.value(m_selectedTrackIndex, m_selectedTrackIndex);
    m_tracks = reorderedTracks;
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
            if (clip.clipRole == ClipRole::SpeakerTitle) {
                return false;
            }
            invalidateHoveredClipToolTipCache();
            updater(clip);
            normalizeClipTiming(clip);
            refreshClipAudioSource(clip);
            // updateClipById is the common programmatic mutation boundary used
            // by Properties and inspector tabs.  Followers must be coherent
            // before callers snapshot the model for preview, save, or export.
            normalizeMaskMatteClips(m_clips);
            normalizeSpeakerTitleParentBounds(&m_clips);
            update();
            return true;
        }
    }
    return false;
}

bool TimelineWidget::setClipZLevel(const QString& clipId, int zLevel, bool automatic)
{
    const auto targetIt = std::find_if(
        m_clips.cbegin(), m_clips.cend(),
        [&clipId](const TimelineClip& clip) { return clip.id == clipId; });
    if (targetIt == m_clips.cend() || !clipHasVisuals(*targetIt)) {
        return false;
    }

    invalidateHoveredClipToolTipCache();
    const int explicitZ = qBound(-100000, zLevel, 100000);
    const auto applyZ = [automatic, explicitZ](TimelineClip& clip) {
        clip.zLevel = automatic
            ? -qMax(0, clip.trackIndex) * 100
            : explicitZ;
        clip.zLevelUserSet = !automatic;
    };
    if (targetIt->clipRole == ClipRole::SpeakerTitle) {
        const QString sourceId = targetIt->linkedSourceClipId.trimmed();
        if (sourceId.isEmpty()) {
            return false;
        }
        for (TimelineClip& clip : m_clips) {
            if (clip.clipRole == ClipRole::SpeakerTitle &&
                clip.linkedSourceClipId.trimmed() == sourceId) {
                applyZ(clip);
            }
        }
    } else {
        TimelineClip& clip = m_clips[
            static_cast<int>(std::distance(m_clips.cbegin(), targetIt))];
        applyZ(clip);
    }
    update();
    return true;
}

QSet<QString> TimelineWidget::ownershipClosure(const QSet<QString>& clipIds,
                                               bool includeAncestors) const
{
    QSet<QString> closure = clipIds;
    if (includeAncestors) {
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const TimelineClip& clip : m_clips) {
                if (!closure.contains(clip.id) ||
                    (clip.clipRole != ClipRole::MaskMatte &&
                     clip.clipRole != ClipRole::SpeakerTitle)) {
                    continue;
                }
                const QString parentId = clip.linkedSourceClipId.trimmed();
                if (!parentId.isEmpty() && !closure.contains(parentId)) {
                    closure.insert(parentId);
                    expanded = true;
                }
            }
        }
    }

    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const TimelineClip& clip : m_clips) {
            if ((clip.clipRole != ClipRole::MaskMatte &&
                 clip.clipRole != ClipRole::SpeakerTitle) ||
                closure.contains(clip.id)) {
                continue;
            }
            if (closure.contains(clip.linkedSourceClipId.trimmed())) {
                closure.insert(clip.id);
                expanded = true;
            }
        }
    }
    return closure;
}

QString TimelineWidget::renderSyncOwnerClipId(const QString& clipId) const
{
    const QString normalizedId = clipId.trimmed();
    for (const TimelineClip& clip : m_clips) {
        if (clip.id.trimmed() != normalizedId) {
            continue;
        }
        if (clip.clipRole == ClipRole::MaskMatte) {
            const QString parentId = clip.linkedSourceClipId.trimmed();
            if (!parentId.isEmpty()) {
                return parentId;
            }
        }
        break;
    }
    return normalizedId;
}

void TimelineWidget::normalizeRenderSyncMarkerOwnership()
{
    QVector<RenderSyncMarker> canonicalMarkers;
    canonicalMarkers.reserve(m_renderSyncMarkers.size());
    QHash<QString, QHash<int64_t, int>> markerIndexByOwnerAndFrame;
    for (RenderSyncMarker marker : std::as_const(m_renderSyncMarkers)) {
        marker.clipId = renderSyncOwnerClipId(marker.clipId);
        if (marker.clipId.isEmpty()) {
            continue;
        }
        QHash<int64_t, int>& indexByFrame = markerIndexByOwnerAndFrame[marker.clipId];
        const auto existing = indexByFrame.constFind(marker.frame);
        if (existing != indexByFrame.constEnd()) {
            // One mapping decision is allowed per source/frame. Last input
            // wins when migrating a legacy child-owned duplicate.
            canonicalMarkers[existing.value()] = marker;
        } else {
            indexByFrame.insert(marker.frame, canonicalMarkers.size());
            canonicalMarkers.push_back(marker);
        }
    }
    m_renderSyncMarkers = std::move(canonicalMarkers);
    sortRenderSyncMarkers();
    rebuildRenderSyncMarkerIndex();
}

bool TimelineWidget::updateTrackByIndex(int trackIndex, const std::function<void(TimelineTrack&)>& updater) {
    if (trackIndex < 0) {
        return false;
    }
    ensureTrackCount(trackIndex + 1);
    updater(m_tracks[trackIndex]);
    invalidateHoveredClipToolTipCache();
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
    if (trackIndex <= 0 || trackIndex >= m_tracks.size() ||
        m_tracks.at(trackIndex).generatedChildTrack) {
        return false;
    }
    for (int target = trackIndex - 1; target >= 0; --target) {
        if (!m_tracks.at(target).generatedChildTrack) {
            return moveTrack(trackIndex, target);
        }
    }
    return false;
}

bool TimelineWidget::moveTrackDown(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= m_tracks.size() - 1 ||
        m_tracks.at(trackIndex).generatedChildTrack) {
        return false;
    }
    for (int target = trackIndex + 1; target < m_tracks.size(); ++target) {
        if (!m_tracks.at(target).generatedChildTrack) {
            return moveTrack(trackIndex, target);
        }
    }
    return false;
}

bool TimelineWidget::moveTrack(int fromTrack, int toTrack) {
    if (fromTrack < 0 || toTrack < 0 || fromTrack >= m_tracks.size() || toTrack >= m_tracks.size()) {
        return false;
    }
    if (fromTrack == toTrack) {
        return false;
    }
    // Generated rows are derived presentation state. Reorder only source
    // tracks; reorderTracksForView() carries every child row with its owner.
    if (m_tracks.at(fromTrack).generatedChildTrack || m_tracks.at(toTrack).generatedChildTrack) {
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

    if (m_selectedTrackIndex == fromTrack) {
        m_selectedTrackIndex = toTrack;
    } else if (fromTrack < toTrack && m_selectedTrackIndex > fromTrack && m_selectedTrackIndex <= toTrack) {
        m_selectedTrackIndex -= 1;
    } else if (fromTrack > toTrack && m_selectedTrackIndex >= toTrack && m_selectedTrackIndex < fromTrack) {
        m_selectedTrackIndex += 1;
    }

    reorderTracksForView();
    normalizeTrackIndices();
    sortClips();
    invalidateHoveredClipToolTipCache();

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
        if (m_clips[i].locked ||
            m_clips[i].clipRole == ClipRole::MaskMatte ||
            m_clips[i].clipRole == ClipRole::SpeakerTitle) {
            return false;
        }
        const QSet<QString> removedIds = ownershipClosure(QSet<QString>{clipId}, false);
        QVector<TimelineClip> remaining;
        remaining.reserve(m_clips.size() - removedIds.size());
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (!removedIds.contains(clip.id)) {
                remaining.push_back(clip);
            }
        }
        const qsizetype markerCountBefore = m_renderSyncMarkers.size();
        m_renderSyncMarkers.erase(
            std::remove_if(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
                           [&removedIds](const RenderSyncMarker& marker) {
                               return removedIds.contains(marker.clipId);
                           }),
            m_renderSyncMarkers.end());
        rebuildRenderSyncMarkerIndex();
        const bool removedMarkers = m_renderSyncMarkers.size() != markerCountBefore;
        QSet<QString> nextSelection = m_clipSelection.ids;
        nextSelection.subtract(removedIds);
        setClips(remaining);
        applyClipSelection(nextSelection, m_clipSelection.primaryId, false);
        if (clipsChanged) {
            clipsChanged();
        }
        if (removedMarkers && renderSyncMarkersChanged) {
            renderSyncMarkersChanged();
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

        if (m_clips[i].locked ||
            m_clips[i].clipRole == ClipRole::MaskMatte ||
            m_clips[i].clipRole == ClipRole::SpeakerTitle) {
            return false;
        }

        const QString selectedId = m_clips[i].id;
        const QSet<QString> removedIds = ownershipClosure(QSet<QString>{selectedId}, false);
        QVector<TimelineClip> remaining;
        remaining.reserve(m_clips.size() - removedIds.size());
        for (const TimelineClip& clip : std::as_const(m_clips)) {
            if (!removedIds.contains(clip.id)) {
                remaining.push_back(clip);
            }
        }
        const qsizetype markerCountBefore = m_renderSyncMarkers.size();
        m_renderSyncMarkers.erase(
            std::remove_if(m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
                           [&removedIds](const RenderSyncMarker& marker) {
                               return removedIds.contains(marker.clipId);
                           }),
            m_renderSyncMarkers.end());
        rebuildRenderSyncMarkerIndex();
        const bool removedMarkers = m_renderSyncMarkers.size() != markerCountBefore;
        QSet<QString> nextSelection = m_clipSelection.ids;
        nextSelection.subtract(removedIds);
        setClips(remaining);
        applyClipSelection(nextSelection, QString(), false);
        if (selectionChanged) {
            selectionChanged();
        }
        if (clipsChanged) {
            clipsChanged();
        }
        if (removedMarkers && renderSyncMarkersChanged) {
            renderSyncMarkersChanged();
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
    bool movedAnyMarkers = false;
    const QString previousPrimarySelection = m_clipSelection.primaryId;
    QString nextPrimarySelection;

    for (const QString& targetClipId : std::as_const(targetClipIds)) {
        for (int i = 0; i < m_clips.size(); ++i) {
            TimelineClip& clip = m_clips[i];
            if (clip.id != targetClipId) {
                continue;
            }
            if (clip.locked ||
                clip.clipRole == ClipRole::MaskMatte ||
                clip.clipRole == ClipRole::SpeakerTitle ||
                frame <= clip.startFrame || frame >= clip.startFrame + clip.durationFrames) {
                break;
            }

            const int64_t leftDuration = frame - clip.startFrame;
            const int64_t rightDuration = clip.durationFrames - leftDuration;
            if (leftDuration <= 0 || rightDuration <= 0) {
                break;
            }

            // Generated layers are owned by their source, so razor the whole
            // aggregate. Capture them before inserting into m_clips, which
            // would invalidate clip references.
            QVector<TimelineClip> ownedChildren;
            for (const TimelineClip& candidate : std::as_const(m_clips)) {
                if ((candidate.clipRole == ClipRole::MaskMatte ||
                     candidate.clipRole == ClipRole::SpeakerTitle) &&
                    candidate.linkedSourceClipId.trimmed() ==
                        clip.id.trimmed()) {
                    ownedChildren.push_back(candidate);
                }
            }
            const int64_t splitTimelineSample = frameToSamples(frame) + clip.startSubframeSamples;
            const int64_t splitSourceSample =
                sourceSampleForClipAtTimelineSample(clip, splitTimelineSample, m_renderSyncMarkers);

            const TimelineClip originalClip = clip;
            const bool isImage = originalClip.mediaType == ClipMediaType::Image;
            TimelineClip rightClip = originalClip;
            rightClip.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            rightClip.startFrame = frame;
            rightClip.startSubframeSamples = clip.startSubframeSamples;
            rightClip.durationFrames = rightDuration;
            if (isImage) {
                rightClip.sourceInFrame = 0;
                rightClip.sourceInSubframeSamples = 0;
                rightClip.sourceDurationFrames = rightDuration;
            } else {
                setClipSourceInSamples(rightClip, splitSourceSample);
            }
            splitTransformKeyframes(originalClip,
                                    leftDuration,
                                    &clip.transformKeyframes,
                                    &rightClip.transformKeyframes);
            splitGradingKeyframes(originalClip,
                                  leftDuration,
                                  &clip.gradingKeyframes,
                                  &rightClip.gradingKeyframes);
            splitOpacityKeyframes(originalClip,
                                  leftDuration,
                                  &clip.opacityKeyframes,
                                  &rightClip.opacityKeyframes);
            splitCorrectionPolygons(originalClip.correctionPolygons,
                                    leftDuration,
                                    &clip.correctionPolygons,
                                    &rightClip.correctionPolygons);

            clip.durationFrames = leftDuration;
            // The split point is frame-aligned. Any fractional duration tail
            // belongs only to the right half; duplicating it lengthens the
            // aggregate and eventually desynchronizes source mapping.
            clip.durationSubframeSamples = 0;
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

            QVector<TimelineClip> rightChildren;
            rightChildren.reserve(ownedChildren.size());
            for (const TimelineClip& originalChild :
                 std::as_const(ownedChildren)) {
                if (originalChild.startFrame >= frame) {
                    for (TimelineClip& candidate : m_clips) {
                        if (candidate.id == originalChild.id) {
                            candidate.linkedSourceClipId =
                                rightClip.id;
                            break;
                        }
                    }
                    continue;
                }
                if (originalChild.startFrame +
                        originalChild.durationFrames <=
                    frame) {
                    continue;
                }

                const int64_t childLeftDuration =
                    frame - originalChild.startFrame;
                const int64_t childRightDuration =
                    originalChild.durationFrames -
                    childLeftDuration;
                TimelineClip leftChild = originalChild;
                TimelineClip rightChild = originalChild;
                rightChild.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                rightChild.linkedSourceClipId = rightClip.id;
                rightChild.startFrame = frame;
                rightChild.durationFrames = childRightDuration;
                leftChild.durationFrames = childLeftDuration;
                leftChild.durationSubframeSamples = 0;

                const bool childIsStill =
                    originalChild.mediaType ==
                        ClipMediaType::Image ||
                    originalChild.mediaType ==
                        ClipMediaType::Title;
                if (childIsStill) {
                    leftChild.sourceInFrame = 0;
                    leftChild.sourceInSubframeSamples = 0;
                    leftChild.sourceDurationFrames =
                        childLeftDuration;
                    rightChild.sourceInFrame = 0;
                    rightChild.sourceInSubframeSamples = 0;
                    rightChild.sourceDurationFrames =
                        childRightDuration;
                } else {
                    const int64_t childSplitTimelineSample =
                        frameToSamples(frame) +
                        originalChild.startSubframeSamples;
                    setClipSourceInSamples(
                        rightChild,
                        sourceSampleForClipAtTimelineSample(
                            originalChild,
                            childSplitTimelineSample,
                            m_renderSyncMarkers));
                }

                splitTransformKeyframes(
                    originalChild,
                    childLeftDuration,
                    &leftChild.transformKeyframes,
                    &rightChild.transformKeyframes);
                splitGradingKeyframes(originalChild,
                                      childLeftDuration,
                                      &leftChild.gradingKeyframes,
                                      &rightChild.gradingKeyframes);
                splitOpacityKeyframes(originalChild,
                                      childLeftDuration,
                                      &leftChild.opacityKeyframes,
                                      &rightChild.opacityKeyframes);
                splitSimpleKeyframes(
                    originalChild.titleKeyframes,
                    childLeftDuration,
                    &leftChild.titleKeyframes,
                    &rightChild.titleKeyframes);
                splitCorrectionPolygons(originalChild.correctionPolygons,
                                        childLeftDuration,
                                        &leftChild.correctionPolygons,
                                        &rightChild.correctionPolygons);
                normalizeClipTransformKeyframes(leftChild);
                normalizeClipTransformKeyframes(rightChild);
                normalizeClipGradingKeyframes(leftChild);
                normalizeClipGradingKeyframes(rightChild);
                normalizeClipOpacityKeyframes(leftChild);
                normalizeClipOpacityKeyframes(rightChild);
                normalizeClipTitleKeyframes(leftChild);
                normalizeClipTitleKeyframes(rightChild);
                normalizeClipTiming(leftChild);
                normalizeClipTiming(rightChild);

                for (TimelineClip& candidate : m_clips) {
                    if (candidate.id == originalChild.id) {
                        candidate = std::move(leftChild);
                        break;
                    }
                }
                rightChildren.push_back(std::move(rightChild));
            }

            m_clips.insert(i + 1, rightClip);
            for (TimelineClip& rightChild : rightChildren) {
                m_clips.push_back(std::move(rightChild));
            }
            newSelection.insert(rightClip.id);
            if (nextPrimarySelection.isEmpty() || targetClipId == previousPrimarySelection) {
                nextPrimarySelection = rightClip.id;
            }
            splitAny = true;
            if (movedMarkers) {
                movedAnyMarkers = true;
                sortRenderSyncMarkers();
                rebuildRenderSyncMarkerIndex();
            }
            break;
        }
    }

    if (!splitAny) {
        return false;
    }

    // Rebuild derived child-track ownership as well as normalizing inherited
    // timing and transforms for both halves of every split aggregate.
    const QVector<TimelineClip> splitModel = m_clips;
    setClips(splitModel);
    applyClipSelection(newSelection, nextPrimarySelection, false);
    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (movedAnyMarkers && renderSyncMarkersChanged) {
        renderSyncMarkersChanged();
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
    // Generated timeline followers are not independent A/V streams.  Keep their
    // edit-domain timing derived from the linked source before publishing or
    // ordering the model (TIME.md, "Generated timeline followers").  Putting
    // this at the common mutation boundary covers moves, trims, rate changes,
    // splits, nudges, pastes, and programmatic edits that finish by sorting.
    normalizeMaskMatteClips(m_clips);
    normalizeSpeakerTitleParentBounds(&m_clips);
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
    invalidateHoveredClipToolTipCache();
    m_renderSyncMarkersByClip.clear();
    if (m_renderSyncMarkers.isEmpty()) {
        return;
    }
    for (const RenderSyncMarker& marker : m_renderSyncMarkers) {
        m_renderSyncMarkersByClip[marker.clipId].push_back(marker);
    }
}

const QVector<RenderSyncMarker>* TimelineWidget::renderSyncMarkersForClipId(const QString& clipId) const {
    const auto it = m_renderSyncMarkersByClip.constFind(renderSyncOwnerClipId(clipId));
    if (it == m_renderSyncMarkersByClip.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

void TimelineWidget::setRenderSyncMarkers(const QVector<RenderSyncMarker>& markers) {
    m_renderSyncMarkers = markers;
    normalizeRenderSyncMarkerOwnership();
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
    const QString ownerClipId = renderSyncOwnerClipId(clipId);
    if (ownerClipId.isEmpty()) {
        return false;
    }
    count = qMax(1, count);
    for (RenderSyncMarker& marker : m_renderSyncMarkers) {
        if (marker.clipId == ownerClipId && marker.frame == m_currentFrame) {
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
    marker.clipId = ownerClipId;
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
    const QString ownerClipId = renderSyncOwnerClipId(clipId);
    if (ownerClipId.isEmpty()) {
        return false;
    }
    for (int i = 0; i < m_renderSyncMarkers.size(); ++i) {
        if (m_renderSyncMarkers[i].clipId == ownerClipId &&
            m_renderSyncMarkers[i].frame == m_currentFrame) {
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
    std::vector<jcut::export_range::Range> ranges =
        sharedExportRanges(m_exportRanges);
    jcut::export_range::normalize(&ranges, totalFrames());
    m_exportRanges = qtExportRanges(ranges);
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

        if (clip.locked ||
            clip.clipRole == ClipRole::MaskMatte ||
            clip.clipRole == ClipRole::SpeakerTitle) {
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
    if (trackIndex < 0 || trackIndex >= m_tracks.size() ||
        m_tracks.at(trackIndex).generatedChildTrack) {
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
    if (trackIndex < 0 || trackIndex >= m_tracks.size() ||
        m_tracks.at(trackIndex).generatedChildTrack) {
        return false;
    }

    QSet<QString> directClipIds;
    for (const TimelineClip& clip : m_clips) {
        if (clip.trackIndex == trackIndex) {
            directClipIds.insert(clip.id);
        }
    }
    const QSet<QString> removedIds = ownershipClosure(directClipIds, false);
    const int clipCountOnTrack = directClipIds.size();
    const int relatedClipCount = removedIds.size() - clipCountOnTrack;

    if (clipCountOnTrack > 0) {
        QString detail = QStringLiteral("Track \"%1\" contains %2 clip%3.")
                             .arg(m_tracks[trackIndex].name.trimmed().isEmpty()
                                      ? defaultTrackName(trackIndex)
                                      : m_tracks[trackIndex].name)
                             .arg(clipCountOnTrack)
                             .arg(clipCountOnTrack == 1 ? QString() : QStringLiteral("s"));
        if (relatedClipCount > 0) {
            detail += QStringLiteral(" Its %1 owned Mask Matte layer%2 will also be removed.")
                          .arg(relatedClipCount)
                          .arg(relatedClipCount == 1 ? QString() : QStringLiteral("s"));
        }
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this,
            QStringLiteral("Delete Track"),
            detail + QStringLiteral("\n\nDelete the track and this complete ownership set?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (choice != QMessageBox::Yes) {
            return false;
        }
    }

    for (int i = m_clips.size() - 1; i >= 0; --i) {
        if (removedIds.contains(m_clips.at(i).id)) {
            m_clips.removeAt(i);
        }
    }
    const auto markerEnd = std::remove_if(
        m_renderSyncMarkers.begin(), m_renderSyncMarkers.end(),
        [&removedIds](const RenderSyncMarker& marker) {
            return removedIds.contains(marker.clipId);
        });
    const bool removedMarkers = markerEnd != m_renderSyncMarkers.end();
    m_renderSyncMarkers.erase(markerEnd, m_renderSyncMarkers.end());
    rebuildRenderSyncMarkerIndex();
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

    // Reconciliation removes stale generated rows and re-derives every
    // surviving child binding after the base-row index remap.
    const QVector<TimelineClip> remaining = m_clips;
    setClips(remaining);

    if (selectionChanged) {
        selectionChanged();
    }
    if (clipsChanged) {
        clipsChanged();
    }
    if (trackLayoutChanged) {
        trackLayoutChanged();
    }
    if (removedMarkers && renderSyncMarkersChanged) {
        renderSyncMarkersChanged();
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
            const int64_t leftEndSamples = leftStartSamples + clipTimelineDurationSamples(leftClip);
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

bool TimelineWidget::editExportRanges(
    jcut::export_range::Edit edit,
    int64_t frame)
{
    std::vector<jcut::export_range::Range> ranges =
        sharedExportRanges(m_exportRanges);
    if (!jcut::export_range::apply(
            &ranges, totalFrames(), edit, frame)) {
        return false;
    }
    m_exportRanges = qtExportRanges(ranges);
    if (exportRangeChanged) {
        exportRangeChanged();
    }
    update();
    return true;
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
    const int64_t clipStart = clipTimelineStartSamples(clip);
    const int64_t clipEnd = clipTimelineEndSamples(clip);
    
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
            const int64_t otherStart = clipTimelineStartSamples(other);
            const int64_t otherEnd = clipTimelineEndSamples(other);
            
            // Check if the clips overlap in time
            if (clipEnd > otherStart && clipStart < otherEnd) {
                return true;
            }
        }
    }
    return false;
}
