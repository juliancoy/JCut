#pragma once

#include "editor_shared.h"

#include <QRectF>
#include <QString>
#include <QVector>

enum class FacestreamFrameDomain {
    ClipTimeline30Fps,
    SourceRelative,
    SourceAbsolute
};

struct FacestreamResolvedKeyframe {
    int64_t frame = -1;
    QRectF boxNorm;
    qreal xNorm = 0.5;
    qreal yNorm = 0.5;
    qreal boxSizeNorm = -1.0;
    bool hasCenterBox = false;
    qreal confidence = 0.0;
    QString source;
};

struct FacestreamResolvedTrack {
    QString streamId;
    QString source;
    int trackId = -1;
    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    int64_t typicalFrameStep = 1;
    QVector<FacestreamResolvedKeyframe> keyframes;
};

struct FacestreamResolvedSelection {
    FacestreamResolvedKeyframe keyframe;
    int64_t lookupFrame = -1;
    int64_t sourceFrame = -1;
    bool interpolated = false;
};

struct FacestreamSourceScanRange {
    int64_t startFrame = 0;
    int64_t endFrameExclusive = 0;
    int64_t frameCount = 0;
    bool valid = false;
    QString error;
};

QString normalizedFacestreamOverlaySource(QString source);

bool facedetectionsOverlaySourceMatches(const QString& sourceFilter,
                                    const QString& trackSource,
                                    const QString& streamId);

FacestreamFrameDomain inferFacestreamFrameDomain(const TimelineClip& clip,
                                               int64_t keyframeMin,
                                               int64_t keyframeMax);

FacestreamSourceScanRange facedetectionsSourceAbsoluteScanRangeForClip(const TimelineClip& clip);

int64_t facedetectionsLookupFrameForDomain(FacestreamFrameDomain domain,
                                      int64_t localTimelineFrame,
                                      int64_t localSourceFrame,
                                      int64_t absoluteSourceFrame);

int64_t mapFacestreamFrameToSourceFrame(const TimelineClip& clip,
                                       int64_t facedetectionsFrame,
                                       FacestreamFrameDomain domain,
                                       const QVector<RenderSyncMarker>& markers);

bool facestreamLegacyTimelineFallbackAllowed(const TimelineClip& clip,
                                             FacestreamFrameDomain declaredDomain);

bool sourceAbsoluteFacestreamRangeLooksLikeClipTimeline(const TimelineClip& clip,
                                                        const QVector<int64_t>& sortedFrames);

int64_t facedetectionsTypicalFrameStep(const QVector<int64_t>& sortedFrames);

bool facedetectionsShouldBridgeGap(int64_t previousFrame,
                               int64_t nextFrame,
                               int64_t typicalStep);

int64_t facedetectionsMaxEdgeHoldFrames(int64_t typicalStep);

bool resolveFacestreamTrackAtPlayhead(const TimelineClip& clip,
                                      const FacestreamResolvedTrack& track,
                                      const QVector<RenderSyncMarker>& markers,
                                      int64_t playheadTimelineFrame,
                                      int64_t playheadSourceFrame,
                                      FacestreamResolvedSelection* selectionOut);
