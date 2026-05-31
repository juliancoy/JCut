#pragma once

#include "editor_shared.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace jcut::speakertrack {

struct ResolvedAssignments {
    QHash<int, QString> identityByTrackId;
    QHash<QString, QVector<int>> trackIdsByIdentity;
};

QJsonObject makeTrackAnchor(int trackId,
                            const QString& streamId,
                            int64_t sourceFrame,
                            qreal xNorm,
                            qreal yNorm,
                            qreal boxSizeNorm);

QJsonArray assignmentMapForClip(const QJsonObject& transcriptRoot, const QString& clipId);

QJsonArray upsertAssignmentRows(const QJsonArray& currentMap,
                                const QString& identityId,
                                const QJsonArray& trackAnchors,
                                const QString& resolutionSource,
                                const QString& timestampUtc,
                                bool evictExistingForIdentity);

void setAssignmentMapForClip(QJsonObject* transcriptRoot,
                             const QString& clipId,
                             const QJsonArray& assignmentMap,
                             const QString& timestampUtc);

ResolvedAssignments resolveAssignments(const QJsonObject& transcriptRoot,
                                       const TimelineClip& clip,
                                       const QJsonArray& streams,
                                       const QVector<RenderSyncMarker>& renderSyncMarkers);

QSet<int> trackIdsForIdentity(const QJsonObject& transcriptRoot,
                              const TimelineClip& clip,
                              const QJsonArray& streams,
                              const QVector<RenderSyncMarker>& renderSyncMarkers,
                              const QString& identityId);

} // namespace jcut::speakertrack
