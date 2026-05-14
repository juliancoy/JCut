#pragma once

#include "editor_shared.h"
#include "preview_surface.h"

#include <QVector>

QString audioPreviewDynamicsCacheKey(const PreviewSurface::AudioDynamicsSettings& settings);

bool queryAudioWaveformEnvelopeForClip(const TimelineClip& clip,
                                       const PreviewSurface::AudioDynamicsSettings& settings,
                                       int binCount,
                                       qreal rangeStartNorm,
                                       qreal rangeEndNorm,
                                       QVector<qreal>* minOut,
                                       QVector<qreal>* maxOut);
