#include "facestream_tracking.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace jcut::facestream {

namespace {

QString trackStateString(ContinuityTrackState state)
{
    switch (state) {
    case ContinuityTrackState::Tentative:
        return QStringLiteral("tentative");
    case ContinuityTrackState::Confirmed:
        return QStringLiteral("confirmed");
    case ContinuityTrackState::Lost:
        return QStringLiteral("lost");
    case ContinuityTrackState::Removed:
        return QStringLiteral("removed");
    }
    return QStringLiteral("unknown");
}

double safeAspect(const QRectF& box)
{
    return box.height() > 0.0 ? box.width() / box.height() : 1.0;
}

QRectF clampPredictedBox(const QRectF& box, const QSize& frameSize)
{
    const double maxWidth = qMax(1, frameSize.width());
    const double maxHeight = qMax(1, frameSize.height());
    const double width = qBound(1.0, box.width(), maxWidth);
    const double height = qBound(1.0, box.height(), maxHeight);
    const double x = qBound(0.0, box.x(), maxWidth - width);
    const double y = qBound(0.0, box.y(), maxHeight - height);
    return QRectF(x, y, width, height);
}

QRectF predictTrackBox(const ContinuityTrack& track, int frameDelta, const QSize& frameSize)
{
    if (frameDelta <= 0) {
        return clampPredictedBox(track.box, frameSize);
    }
    const QPointF center = track.box.center() + (track.centerVelocity * frameDelta);
    const QSizeF size(
        qMax(1.0, track.box.width() + (track.sizeVelocity.width() * frameDelta)),
        qMax(1.0, track.box.height() + (track.sizeVelocity.height() * frameDelta)));
    const QRectF predicted(center.x() - (size.width() * 0.5),
                           center.y() - (size.height() * 0.5),
                           size.width(),
                           size.height());
    return clampPredictedBox(predicted, frameSize);
}

double normalizedCenterDistance(const QRectF& a, const QRectF& b)
{
    const QPointF delta = a.center() - b.center();
    const double distance =
        std::hypot(delta.x(), delta.y());
    const double scale =
        qMax(1.0, 0.5 * (qMax(a.width(), a.height()) + qMax(b.width(), b.height())));
    return distance / scale;
}

double areaRatio(const QRectF& a, const QRectF& b)
{
    const double areaA = qMax(1.0, a.width() * a.height());
    const double areaB = qMax(1.0, b.width() * b.height());
    return qMax(areaA, areaB) / qMin(areaA, areaB);
}

double aspectDelta(const QRectF& a, const QRectF& b)
{
    return std::abs(safeAspect(a) - safeAspect(b));
}

bool trackCanReceiveDetection(const ContinuityTrack& track,
                              const Detection& detection,
                              int frameDelta,
                              const QSize& frameSize,
                              const ContinuityTrackingTuning& tuning,
                              QRectF* predictedBoxOut,
                              double* scoreOut)
{
    if (track.state == ContinuityTrackState::Removed) {
        return false;
    }
    if (frameDelta <= 0 || frameDelta > tuning.staleTrackFrameWindow) {
        return false;
    }

    const QRectF predictedBox = predictTrackBox(track, frameDelta, frameSize);
    const double iou = continuityIou(predictedBox, detection.box);
    const double centerDistance = normalizedCenterDistance(predictedBox, detection.box);
    const double scaleRatio = areaRatio(predictedBox, detection.box);
    const double shapeDelta = aspectDelta(predictedBox, detection.box);

    if (centerDistance > tuning.maxCenterDistanceRatio ||
        scaleRatio > tuning.maxAreaRatio ||
        shapeDelta > tuning.maxAspectDelta) {
        return false;
    }

    const double score =
        (0.60 * iou) +
        (0.25 * qBound(0.0, 1.0 - (centerDistance / qMax(0.01f, tuning.maxCenterDistanceRatio)), 1.0)) +
        (0.10 * qBound(0.0, 1.0 - ((scaleRatio - 1.0) / qMax(0.01f, tuning.maxAreaRatio - 1.0f)), 1.0)) +
        (0.05 * qBound(0.0, 1.0 - (shapeDelta / qMax(0.01f, tuning.maxAspectDelta)), 1.0));

    const bool passesIouFloor =
        iou >= qMax(0.10f, tuning.trackMatchIouThreshold * 0.5f);
    if (!passesIouFloor || score < tuning.matchScoreThreshold) {
        return false;
    }

    if (predictedBoxOut) {
        *predictedBoxOut = predictedBox;
    }
    if (scoreOut) {
        *scoreOut = score;
    }
    return true;
}

struct MatchCandidate {
    int trackIndex = -1;
    int detectionIndex = -1;
    double score = 0.0;
    QRectF predictedBox;
};

void pruneRemovedTracks(QVector<ContinuityTrack>* tracks)
{
    if (!tracks) {
        return;
    }
    tracks->erase(
        std::remove_if(
            tracks->begin(),
            tracks->end(),
            [](const ContinuityTrack& track) {
                return track.state == ContinuityTrackState::Removed;
            }),
        tracks->end());
}

int nextTrackId(const QVector<ContinuityTrack>& tracks)
{
    int maxId = -1;
    for (const ContinuityTrack& track : tracks) {
        maxId = qMax(maxId, track.id);
    }
    return maxId + 1;
}

void updateMatchedTrack(ContinuityTrack* track,
                        const Detection& detection,
                        const QRectF& predictedBox,
                        int frameNumber,
                        const QSize& frameSize,
                        const ContinuityTrackingTuning& tuning)
{
    if (!track) {
        return;
    }
    const int frameDelta = qMax(1, frameNumber - track->lastFrame);
    const QPointF measuredCenter = detection.box.center();
    const QPointF previousCenter = track->box.center();
    const QSizeF measuredSize = detection.box.size();
    const QSizeF previousSize = track->box.size();

    const QPointF measuredCenterVelocity =
        (measuredCenter - previousCenter) / frameDelta;
    const QSizeF measuredSizeVelocity(
        (measuredSize.width() - previousSize.width()) / frameDelta,
        (measuredSize.height() - previousSize.height()) / frameDelta);
    track->centerVelocity =
        (track->centerVelocity * 0.55) + (measuredCenterVelocity * 0.45);
    track->sizeVelocity = QSizeF(
        (track->sizeVelocity.width() * 0.55) + (measuredSizeVelocity.width() * 0.45),
        (track->sizeVelocity.height() * 0.55) + (measuredSizeVelocity.height() * 0.45));

    const QPointF blendedTopLeft(
        (predictedBox.x() * tuning.positionSmoothing) +
            (detection.box.x() * (1.0f - tuning.positionSmoothing)),
        (predictedBox.y() * tuning.positionSmoothing) +
            (detection.box.y() * (1.0f - tuning.positionSmoothing)));
    const QSizeF blendedSize(
        (predictedBox.width() * tuning.sizeSmoothing) +
            (detection.box.width() * (1.0f - tuning.sizeSmoothing)),
        (predictedBox.height() * tuning.sizeSmoothing) +
            (detection.box.height() * (1.0f - tuning.sizeSmoothing)));

    track->box = clampPredictedBox(
        QRectF(blendedTopLeft.x(), blendedTopLeft.y(), blendedSize.width(), blendedSize.height()),
        frameSize);
    track->predictedBox = track->box;
    track->lastFrame = frameNumber;
    track->hits += 1;
    track->misses = 0;
    track->state = track->hits >= tuning.tentativeTrackHitCount
        ? ContinuityTrackState::Confirmed
        : ContinuityTrackState::Tentative;
    track->detections.append(compactTrackDetectionJson(detection, frameNumber, frameSize));
}

void markMissedTrack(ContinuityTrack* track,
                     int frameNumber,
                     const QSize& frameSize,
                     const ContinuityTrackingTuning& tuning)
{
    if (!track || track->state == ContinuityTrackState::Removed) {
        return;
    }
    const int frameDelta = qMax(1, frameNumber - track->lastFrame);
    track->predictedBox = predictTrackBox(*track, frameDelta, frameSize);
    track->misses += frameDelta;
    if (track->state == ContinuityTrackState::Tentative &&
        track->misses > tuning.tentativeTrackMaxMisses) {
        track->state = ContinuityTrackState::Removed;
        return;
    }
    if (track->misses > tuning.staleTrackFrameWindow) {
        track->state = ContinuityTrackState::Removed;
        return;
    }
    track->state = ContinuityTrackState::Lost;
}

} // namespace

double continuityIou(const QRectF& a, const QRectF& b)
{
    const QRectF intersection = a.intersected(b);
    const double interArea = intersection.width() * intersection.height();
    const double unionArea =
        a.width() * a.height() + b.width() * b.height() - interArea;
    return unionArea > 0.0 ? interArea / unionArea : 0.0;
}

QJsonObject compactDetectionJson(const Detection& detection, const QSize& frameSize)
{
    const qreal safeWidth = qMax(1, frameSize.width());
    const qreal safeHeight = qMax(1, frameSize.height());
    return QJsonObject{
        {QStringLiteral("x"), detection.box.x()},
        {QStringLiteral("y"), detection.box.y()},
        {QStringLiteral("w"), detection.box.width()},
        {QStringLiteral("h"), detection.box.height()},
        {QStringLiteral("confidence"), detection.confidence},
        {QStringLiteral("x_norm"), detection.box.x() / safeWidth},
        {QStringLiteral("y_norm"), detection.box.y() / safeHeight},
        {QStringLiteral("w_norm"), detection.box.width() / safeWidth},
        {QStringLiteral("h_norm"), detection.box.height() / safeHeight},
        {QStringLiteral("frame_width"), frameSize.width()},
        {QStringLiteral("frame_height"), frameSize.height()}
    };
}

QJsonObject compactTrackDetectionJson(const Detection& detection,
                                      int frameNumber,
                                      const QSize& frameSize)
{
    const double x = qBound(0.0, detection.box.center().x() / qMax(1, frameSize.width()), 1.0);
    const double y = qBound(0.0, detection.box.center().y() / qMax(1, frameSize.height()), 1.0);
    const double box = qBound(
        0.01,
        qMax(detection.box.width(), detection.box.height()) /
            static_cast<double>(qMax(1, qMin(frameSize.width(), frameSize.height()))),
        1.0);
    return QJsonObject{
        {QStringLiteral("frame"), frameNumber},
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("box"), box},
        {QStringLiteral("score"), detection.confidence}
    };
}

void updateContinuityTracks(QVector<ContinuityTrack>* tracks,
                            const QVector<Detection>& detections,
                            int frameNumber,
                            const QSize& frameSize,
                            const ContinuityTrackingTuning& tuning)
{
    if (!tracks) {
        return;
    }
    pruneRemovedTracks(tracks);
    QVector<Detection> filteredDetections = detections;
    if (tuning.primaryFaceOnly && filteredDetections.size() > 1) {
        std::sort(filteredDetections.begin(), filteredDetections.end(), [](const Detection& a, const Detection& b) {
            return a.confidence > b.confidence;
        });
        filteredDetections.resize(1);
    }

    QVector<MatchCandidate> candidates;
    for (int trackIndex = 0; trackIndex < tracks->size(); ++trackIndex) {
        const ContinuityTrack& track = tracks->at(trackIndex);
        for (int detectionIndex = 0; detectionIndex < filteredDetections.size(); ++detectionIndex) {
            QRectF predictedBox;
            double score = 0.0;
            if (!trackCanReceiveDetection(track,
                                          filteredDetections.at(detectionIndex),
                                          frameNumber - track.lastFrame,
                                          frameSize,
                                          tuning,
                                          &predictedBox,
                                          &score)) {
                continue;
            }
            candidates.push_back(MatchCandidate{trackIndex, detectionIndex, score, predictedBox});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const MatchCandidate& a, const MatchCandidate& b) {
        if (a.score == b.score) {
            if (a.trackIndex == b.trackIndex) {
                return a.detectionIndex < b.detectionIndex;
            }
            return a.trackIndex < b.trackIndex;
        }
        return a.score > b.score;
    });

    QVector<bool> trackMatched(tracks->size(), false);
    QVector<bool> detectionMatched(filteredDetections.size(), false);
    QVector<int> matchedDetectionByTrack(tracks->size(), -1);
    QVector<QRectF> matchedPredictions(tracks->size());
    for (const MatchCandidate& candidate : candidates) {
        if (trackMatched.at(candidate.trackIndex) ||
            detectionMatched.at(candidate.detectionIndex)) {
            continue;
        }
        trackMatched[candidate.trackIndex] = true;
        detectionMatched[candidate.detectionIndex] = true;
        matchedDetectionByTrack[candidate.trackIndex] = candidate.detectionIndex;
        matchedPredictions[candidate.trackIndex] = candidate.predictedBox;
    }

    for (int trackIndex = 0; trackIndex < tracks->size(); ++trackIndex) {
        if (!trackMatched.at(trackIndex)) {
            markMissedTrack(&(*tracks)[trackIndex], frameNumber, frameSize, tuning);
            continue;
        }
        const int matchedDetectionIndex = matchedDetectionByTrack.at(trackIndex);
        if (matchedDetectionIndex >= 0) {
            updateMatchedTrack(&(*tracks)[trackIndex],
                               filteredDetections.at(matchedDetectionIndex),
                               matchedPredictions.at(trackIndex),
                               frameNumber,
                               frameSize,
                               tuning);
        }
    }

    int trackId = nextTrackId(*tracks);
    for (int detectionIndex = 0; detectionIndex < filteredDetections.size(); ++detectionIndex) {
        if (detectionMatched.at(detectionIndex)) {
            continue;
        }
        const Detection& detection = filteredDetections.at(detectionIndex);
        if (detection.confidence < tuning.newTrackMinConfidence) {
            continue;
        }
        if (tuning.primaryFaceOnly) {
            bool alreadyActive = false;
            for (const ContinuityTrack& track : *tracks) {
                if (track.state != ContinuityTrackState::Removed) {
                    alreadyActive = true;
                    break;
                }
            }
            if (alreadyActive) {
                continue;
            }
        }
        ContinuityTrack track;
        track.id = trackId++;
        track.box = clampPredictedBox(detection.box, frameSize);
        track.predictedBox = track.box;
        track.firstFrame = frameNumber;
        track.lastFrame = frameNumber;
        track.hits = 1;
        track.misses = 0;
        track.state = tuning.tentativeTrackHitCount <= 1
            ? ContinuityTrackState::Confirmed
            : ContinuityTrackState::Tentative;
        track.detections.append(compactTrackDetectionJson(detection, frameNumber, frameSize));
        tracks->push_back(track);
    }

    pruneRemovedTracks(tracks);
}

QJsonArray frameTrackDetections(const QVector<ContinuityTrack>& tracks, int frameNumber)
{
    QJsonArray rows;
    for (const ContinuityTrack& track : tracks) {
        if (track.lastFrame != frameNumber ||
            track.detections.isEmpty() ||
            track.state == ContinuityTrackState::Removed) {
            continue;
        }
        QJsonObject row = track.detections.last().toObject();
        row.insert(QStringLiteral("track_id"), track.id);
        row.insert(QStringLiteral("track_box_x"), track.box.x());
        row.insert(QStringLiteral("track_box_y"), track.box.y());
        row.insert(QStringLiteral("track_box_w"), track.box.width());
        row.insert(QStringLiteral("track_box_h"), track.box.height());
        row.insert(QStringLiteral("track_state"), trackStateString(track.state));
        rows.append(row);
    }
    return rows;
}

QJsonArray buildContinuityTrackRows(const QVector<ContinuityTrack>& tracks)
{
    QJsonArray rows;
    for (const ContinuityTrack& track : tracks) {
        if (track.id < 0 ||
            track.detections.isEmpty() ||
            track.state == ContinuityTrackState::Removed) {
            continue;
        }
        rows.append(QJsonObject{
            {QStringLiteral("track_id"), track.id},
            {QStringLiteral("first_frame"), track.firstFrame},
            {QStringLiteral("last_frame"), track.lastFrame},
            {QStringLiteral("length"), track.detections.size()},
            {QStringLiteral("hits"), track.hits},
            {QStringLiteral("misses"), track.misses},
            {QStringLiteral("state"), trackStateString(track.state)},
            {QStringLiteral("detections"), track.detections}
        });
    }
    return rows;
}

QVector<ContinuityTrack> buildContinuityTracksFromDetectionFrames(
    const QJsonArray& rawDetectionFrames,
    const ContinuityTrackingTuning& tuning)
{
    QVector<ContinuityTrack> tracks;
    for (const QJsonValue& frameValue : rawDetectionFrames) {
        const QJsonObject frameObject = frameValue.toObject();
        const int frameNumber = frameObject.value(QStringLiteral("frame")).toInt(-1);
        if (frameNumber < 0) {
            continue;
        }
        const int frameWidth = qMax(1, frameObject.value(QStringLiteral("frame_width")).toInt());
        const int frameHeight = qMax(1, frameObject.value(QStringLiteral("frame_height")).toInt());
        QVector<Detection> detections;
        const QJsonArray detectionRows = frameObject.value(QStringLiteral("detections")).toArray();
        detections.reserve(detectionRows.size());
        for (const QJsonValue& detectionValue : detectionRows) {
            const QJsonObject detectionObject = detectionValue.toObject();
            Detection detection;
            detection.box = QRectF(
                detectionObject.value(QStringLiteral("x")).toDouble(),
                detectionObject.value(QStringLiteral("y")).toDouble(),
                detectionObject.value(QStringLiteral("w")).toDouble(),
                detectionObject.value(QStringLiteral("h")).toDouble());
            detection.confidence =
                static_cast<float>(detectionObject.value(QStringLiteral("confidence")).toDouble());
            if (detection.box.width() > 0.0 && detection.box.height() > 0.0) {
                detections.push_back(detection);
            }
        }
        updateContinuityTracks(&tracks, detections, frameNumber, QSize(frameWidth, frameHeight), tuning);
    }
    return tracks;
}

} // namespace jcut::facestream
