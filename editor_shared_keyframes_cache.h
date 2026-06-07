#pragma once

#include "facedetections_types.h"

#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

using AssignedContinuityStreamsPtr =
    QSharedPointer<const QVector<jcut::facedetections::FacestreamTrack>>;

QString assignedContinuityCacheKey(const QString& transcriptPath,
                                   const QString& clipId,
                                   const QString& speakerId);

bool cachedAssignedContinuityStreams(const QString& cacheKey,
                                     const QString& transcriptPath,
                                     const QString& processedPath,
                                     QVector<jcut::facedetections::FacestreamTrack>* streamsOut);

bool cachedAssignedContinuityStreamsPtr(const QString& cacheKey,
                                        const QString& transcriptPath,
                                        const QString& processedPath,
                                        AssignedContinuityStreamsPtr* streamsOut);

bool cachedAssignedContinuityStreamsMemoryOnly(const QString& cacheKey,
                                               AssignedContinuityStreamsPtr* streamsOut);

void storeAssignedContinuityStreams(const QString& cacheKey,
                                    const QString& transcriptPath,
                                    const QString& processedPath,
                                    const QStringList& referencedPaths,
                                    const QVector<jcut::facedetections::FacestreamTrack>& streams);

void clearAssignedContinuityStreamsCache();
