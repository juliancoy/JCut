#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

QString assignedContinuityCacheKey(const QString& transcriptPath,
                                   const QString& clipId,
                                   const QString& speakerId);

bool cachedAssignedContinuityStreams(const QString& cacheKey,
                                     const QString& transcriptPath,
                                     const QString& identityPath,
                                     const QString& processedPath,
                                     QVector<QJsonObject>* streamsOut);

void storeAssignedContinuityStreams(const QString& cacheKey,
                                    const QString& transcriptPath,
                                    const QString& identityPath,
                                    const QString& processedPath,
                                    const QStringList& referencedPaths,
                                    const QVector<QJsonObject>& streams);
