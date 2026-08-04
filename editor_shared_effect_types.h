#pragma once

#include "editor_timeline_types.h"

struct EffectiveVisualEffects {
    TimelineClip::GradingKeyframe grading;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    int maskFeatherFalloff = 0;
    qreal maskEdgeGrayAmount = 0.0;
    qreal maskEdgeGrayWidth = 0.25;
    qreal maskEdgeGrayGamma = 1.0;
    QVector<TimelineClip::CorrectionPolygon> correctionPolygons;
};
