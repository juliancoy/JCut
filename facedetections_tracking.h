#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QVector>

namespace jcut::facedetections {

struct Detection {
    QRectF box;
    float confidence = 0.0f;
};

enum class ContinuityTrackState {
    Tentative,
    Confirmed,
    Lost,
    Removed
};

struct ContinuityTrack {
    int id = -1;
    QRectF box;
    QRectF predictedBox;
    QPointF centerVelocity;
    QSizeF sizeVelocity;
    int firstFrame = -1;
    int lastFrame = -1;
    int hits = 0;
    int misses = 0;
    ContinuityTrackState state = ContinuityTrackState::Tentative;
    QJsonArray detections;
};

struct ContinuityTrackingTuning {
    float trackMatchIouThreshold = 0.35f;
    float newTrackMinConfidence = 0.35f;
    bool primaryFaceOnly = false;
    int staleTrackFrameWindow = 48;
    int tentativeTrackHitCount = 2;
    int tentativeTrackMaxMisses = 1;
    float matchScoreThreshold = 0.28f;
    float maxCenterDistanceRatio = 1.75f;
    float maxAreaRatio = 2.5f;
    float maxAspectDelta = 0.65f;
    float positionSmoothing = 0.45f;
    float sizeSmoothing = 0.40f;
};

double continuityIou(const QRectF& a, const QRectF& b);

QJsonObject compactDetectionJson(const Detection& detection, const QSize& frameSize);

QJsonObject compactTrackDetectionJson(const Detection& detection,
                                      int frameNumber,
                                      const QSize& frameSize);

void updateContinuityTracks(QVector<ContinuityTrack>* tracks,
                            const QVector<Detection>& detections,
                            int frameNumber,
                            const QSize& frameSize,
                            const ContinuityTrackingTuning& tuning);

QJsonArray frameTrackDetections(const QVector<ContinuityTrack>& tracks, int frameNumber);

QJsonArray buildContinuityTrackRows(const QVector<ContinuityTrack>& tracks);

QVector<ContinuityTrack> buildContinuityTracksFromDetectionFrames(
    const QJsonArray& rawDetectionFrames,
    const ContinuityTrackingTuning& tuning);

} // namespace jcut::facedetections
