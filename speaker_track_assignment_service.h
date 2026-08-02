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

struct AssignmentRemoval {
    bool removed = false;
    QString streamId;
    int64_t sourceFrame = -1;
    qreal xNorm = 0.5;
    qreal yNorm = 0.5;
    qreal boxSizeNorm = 0.2;
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

// Owns the complete document mutation for removing a persisted identity/track
// assignment, including its audit record and speaker-profile face references.
// UI callers are responsible only for committing the returned document.
AssignmentRemoval removeAssignment(QJsonObject* transcriptRoot,
                                   const TimelineClip& clip,
                                   const QJsonArray& streams,
                                   const QVector<RenderSyncMarker>& renderSyncMarkers,
                                   const QString& identityId,
                                   int trackId,
                                   const QString& timestampUtc);

} // namespace jcut::speakertrack
