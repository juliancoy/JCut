#pragma once

#include "facedetections_time_mapping.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <QStringList>

inline qint64 facedetectionsArtifactRevisionMsForTranscript(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return -1;
    }
    const QDir dir = info.dir();
    const QString base = info.completeBaseName();
    const QStringList candidates{
        dir.filePath(base + QStringLiteral("_facedetections.bin")),
        dir.filePath(base + QStringLiteral("_facedetections_processed.bin")),
    };
    qint64 revisionMs = -1;
    for (const QString& path : candidates) {
        const QFileInfo candidate(path);
        if (candidate.exists() && candidate.isFile()) {
            revisionMs = qMax<qint64>(revisionMs, candidate.lastModified().toMSecsSinceEpoch());
        }
    }
    return revisionMs;
}

inline QString continuityFacestreamsByClipKey()
{
    return QStringLiteral("continuity_facedetections_by_clip");
}

inline QJsonObject continuityFacestreamsByClipObject(const QJsonObject& artifactRoot)
{
    const QJsonObject current =
        artifactRoot.value(QStringLiteral("continuity_facedetections_by_clip")).toObject();
    if (!current.isEmpty()) {
        return current;
    }
    return artifactRoot.value(QStringLiteral("continuity_facestreams_by_clip")).toObject();
}

inline QJsonObject continuityRootForClip(const QJsonObject& artifactRoot, const QString& clipId)
{
    return continuityFacestreamsByClipObject(artifactRoot).value(clipId.trimmed()).toObject();
}

inline void setContinuityFacestreamsByClipObject(QJsonObject* artifactRoot, const QJsonObject& byClip)
{
    if (!artifactRoot) {
        return;
    }
    (*artifactRoot)[QStringLiteral("continuity_facedetections_by_clip")] = byClip;
}

inline QString facedetectionsFrameDomainString(FacestreamFrameDomain domain)
{
    switch (domain) {
    case FacestreamFrameDomain::ClipTimeline30Fps:
        return QStringLiteral("clip_timeline_30fps");
    case FacestreamFrameDomain::SourceAbsolute:
        return QStringLiteral("source_absolute");
    case FacestreamFrameDomain::SourceRelative:
    default:
        return QStringLiteral("source_relative");
    }
}

inline bool parseFacestreamFrameDomainString(const QString& value, FacestreamFrameDomain* domainOut)
{
    if (!domainOut) {
        return false;
    }
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("clip_timeline_30fps")) {
        *domainOut = FacestreamFrameDomain::ClipTimeline30Fps;
        return true;
    }
    if (normalized == QStringLiteral("source_absolute")) {
        *domainOut = FacestreamFrameDomain::SourceAbsolute;
        return true;
    }
    if (normalized == QStringLiteral("source_relative")) {
        *domainOut = FacestreamFrameDomain::SourceRelative;
        return true;
    }
    return false;
}

inline bool continuityPayloadFrameDomain(const QJsonObject& continuityRoot,
                                         const QString& key,
                                         FacestreamFrameDomain* domainOut)
{
    return parseFacestreamFrameDomainString(
        continuityRoot.value(key).toString(),
        domainOut);
}
