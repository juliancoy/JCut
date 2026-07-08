#pragma once

#include "editor_shared.h"

#include <QSet>
#include <functional>

struct AutoSyncAnchorPlan {
    TimelineClip clip;
    QString audioPath;
};

struct AutoSyncTargetPlan {
    TimelineClip clip;
    QString mediaPath;
    QString mode;
    bool replaceRenderSyncMarkers = false;
    QVector<AutoSyncAnchorPlan> anchors;
};

struct AutoSyncSelectionPlan {
    bool ok = false;
    QString message;
    AutoSyncAnchorPlan primaryAudioAnchor;
    QVector<AutoSyncAnchorPlan> lockedAudioAnchors;
    QVector<AutoSyncTargetPlan> targets;
    QSet<QString> syncMarkerClipIds;
};

AutoSyncSelectionPlan buildAutoSyncSelectionPlan(
    const QVector<TimelineClip>& clips,
    const QSet<QString>& selectedClipIds,
    const std::function<bool(const TimelineClip&)>& clipHasVisuals,
    const std::function<QString(const TimelineClip&)>& syncAudioPathForClip,
    const std::function<QString(const TimelineClip&)>& playbackMediaPathForClip);
