#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

#include <cstdint>

namespace jcut::facedetections {

struct FacestreamArtifactRef {
    QString transcriptPath;
    QString clipId;
    QString rawTracksPath;
    QString rawFramesPath;
    QString processedPath;
    qint64 revisionMs = -1;
};

struct FacestreamTrackSummary {
    int trackId = -1;
    QString streamId;
    QString source;
    QString frameDomain;
    qint64 minFrame = -1;
    qint64 maxFrame = -1;
    int keyframeCount = 0;
    qint64 typicalFrameStep = 1;
};

struct FacestreamKeyframe {
    qint64 frame = -1;
    qint64 sourceFrame = -1;
    QRectF boxNorm;
    float x = 0.5f;
    float y = 0.5f;
    float box = 0.2f;
    float confidence = 0.0f;
};

struct FacestreamTrack {
    FacestreamTrackSummary summary;
    QVector<FacestreamKeyframe> keyframes;

    int trackId() const { return summary.trackId; }
    QString streamId() const { return summary.streamId; }
};

struct FacestreamDetection {
    qint64 frame = -1;
    QRectF box;
    float confidence = 0.0f;
    int trackId = -1;
};

struct FacestreamFrameDetections {
    qint64 frame = -1;
    qreal frameWidth = 0.0;
    qreal frameHeight = 0.0;
    QVector<FacestreamDetection> detections;
};

} // namespace jcut::facedetections
