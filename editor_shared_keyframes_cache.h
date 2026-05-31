#pragma once

#include "facedetections_types.h"

#include <QString>
#include <QStringList>
#include <QVector>

QString assignedContinuityCacheKey(const QString& transcriptPath,
                                   const QString& clipId,
                                   const QString& speakerId);

bool cachedAssignedContinuityStreams(const QString& cacheKey,
                                     const QString& transcriptPath,
                                     const QString& processedPath,
                                     QVector<jcut::facedetections::FacestreamTrack>* streamsOut);

void storeAssignedContinuityStreams(const QString& cacheKey,
                                    const QString& transcriptPath,
                                    const QString& processedPath,
                                    const QStringList& referencedPaths,
                                    const QVector<jcut::facedetections::FacestreamTrack>& streams);
