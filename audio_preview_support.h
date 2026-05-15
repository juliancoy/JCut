#pragma once

#include "editor_shared.h"
#include "preview_surface.h"

#include <QVector>

struct AudioPreviewViewport {
    qreal zoom = 1.0;
    qreal visibleFraction = 1.0;
    qreal startNorm = 0.0;
    qreal endNorm = 1.0;
    qreal playheadClipNorm = 0.0;
    qreal playheadVisibleNorm = 0.0;
    bool playheadVisible = false;
};

QString audioPreviewDynamicsCacheKey(const PreviewSurface::AudioDynamicsSettings& settings);

int64_t resolvedAudioPreviewClipSamples(const TimelineClip& clip);

AudioPreviewViewport resolveAudioPreviewViewport(const TimelineClip& clip,
                                                int rowCount,
                                                qreal previewZoom,
                                                qreal previewPanNorm,
                                                int64_t currentSample);

bool queryAudioWaveformEnvelopeForClip(const TimelineClip& clip,
                                       const PreviewSurface::AudioDynamicsSettings& settings,
                                       int binCount,
                                       qreal rangeStartNorm,
                                       qreal rangeEndNorm,
                                       const QVector<RenderSyncMarker>& markers,
                                       QVector<qreal>* minOut,
                                       QVector<qreal>* maxOut);
