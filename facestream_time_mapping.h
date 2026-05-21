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

QString normalizedFacestreamOverlaySource(QString source);

bool facestreamOverlaySourceMatches(const QString& sourceFilter,
                                    const QString& trackSource,
                                    const QString& streamId);

FacestreamFrameDomain inferFacestreamFrameDomain(const TimelineClip& clip,
                                               int64_t keyframeMin,
                                               int64_t keyframeMax);

int64_t facestreamLookupFrameForDomain(FacestreamFrameDomain domain,
                                      int64_t localTimelineFrame,
                                      int64_t localSourceFrame,
                                      int64_t absoluteSourceFrame);

int64_t mapFacestreamFrameToSourceFrame(const TimelineClip& clip,
                                       int64_t facestreamFrame,
                                       FacestreamFrameDomain domain,
                                       const QVector<RenderSyncMarker>& markers);

int64_t facestreamTypicalFrameStep(const QVector<int64_t>& sortedFrames);

bool facestreamShouldBridgeGap(int64_t previousFrame,
                               int64_t nextFrame,
                               int64_t typicalStep);

int64_t facestreamMaxEdgeHoldFrames(int64_t typicalStep);

bool resolveFacestreamTrackAtPlayhead(const TimelineClip& clip,
                                      const FacestreamResolvedTrack& track,
                                      const QVector<RenderSyncMarker>& markers,
                                      int64_t playheadTimelineFrame,
                                      int64_t playheadSourceFrame,
                                      FacestreamResolvedSelection* selectionOut);
