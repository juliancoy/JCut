#pragma once

#include <QJsonObject>
#include <QString>

qint64 facestreamArtifactRevisionMsForTranscript(const QString& transcriptPath);
QString continuityFacestreamsByClipKey();
QJsonObject continuityFacestreamsByClipObject(const QJsonObject& artifactRoot);
QJsonObject continuityRootForClip(const QJsonObject& artifactRoot, const QString& clipId);
void setContinuityFacestreamsByClipObject(QJsonObject* artifactRoot, const QJsonObject& byClip);
