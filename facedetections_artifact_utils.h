#pragma once

#include "facedetections_time_mapping.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>

#include <chrono>

struct FacedetectionsArtifactMetadataSnapshot
{
    bool transcriptExists = false;
    qint64 transcriptModifiedMs = 0;
    qint64 artifactRevisionMs = -1;
};

inline FacedetectionsArtifactMetadataSnapshot
facedetectionsArtifactMetadataForTranscript(const QString& transcriptPath)
{
    constexpr qint64 kFilesystemRefreshMs = 1000;
    struct CacheEntry
    {
        FacedetectionsArtifactMetadataSnapshot snapshot;
        qint64 nextValidationMs = 0;
    };
    static QMutex cacheMutex;
    static QHash<QString, CacheEntry> cache;

    const QString absolutePath = QFileInfo(transcriptPath.trimmed()).absoluteFilePath();
    if (absolutePath.isEmpty()) {
        return {};
    }
    const qint64 nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    {
        QMutexLocker locker(&cacheMutex);
        const auto cached = cache.constFind(absolutePath);
        if (cached != cache.cend() && nowMs < cached->nextValidationMs) {
            return cached->snapshot;
        }
    }

    FacedetectionsArtifactMetadataSnapshot snapshot;
    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile()) {
        QMutexLocker locker(&cacheMutex);
        cache.insert(absolutePath, CacheEntry{snapshot, nowMs + kFilesystemRefreshMs});
        return snapshot;
    }
    snapshot.transcriptExists = true;
    snapshot.transcriptModifiedMs = info.lastModified().toMSecsSinceEpoch();
    const QDir dir = info.dir();
    const QString base = info.completeBaseName();
    const QStringList candidates{
        dir.filePath(base + QStringLiteral("_facedetections.bin")),
        dir.filePath(base + QStringLiteral("_facedetections_processed.bin")),
        dir.filePath(QStringLiteral("facedetections_artifact/tracks.idx")),
        dir.filePath(QStringLiteral("facedetections_artifact/tracks.dat")),
        dir.filePath(QStringLiteral("facedetections_artifact/detections.idx")),
        dir.filePath(QStringLiteral("facedetections_artifact/detections.dat")),
    };
    for (const QString& path : candidates) {
        const QFileInfo candidate(path);
        if (candidate.exists() && candidate.isFile()) {
            snapshot.artifactRevisionMs =
                qMax<qint64>(snapshot.artifactRevisionMs,
                             candidate.lastModified().toMSecsSinceEpoch());
        }
    }
    {
        QMutexLocker locker(&cacheMutex);
        cache.insert(absolutePath, CacheEntry{snapshot, nowMs + kFilesystemRefreshMs});
    }
    return snapshot;
}

inline qint64 facedetectionsArtifactRevisionMsForTranscript(const QString& transcriptPath)
{
    return facedetectionsArtifactMetadataForTranscript(transcriptPath).artifactRevisionMs;
}

inline QString facedetectionsSidecarToken(const QString& raw)
{
    QString token = raw.trimmed();
    if (token.isEmpty()) {
        return QStringLiteral("unknown");
    }
    for (QChar& ch : token) {
        const bool ok =
            (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
            (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
            (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
            ch == QLatin1Char('.') ||
            ch == QLatin1Char('_') ||
            ch == QLatin1Char('-');
        if (!ok) {
            ch = QLatin1Char('_');
        }
    }
    while (token.contains(QStringLiteral("__"))) {
        token.replace(QStringLiteral("__"), QStringLiteral("_"));
    }
    token = token.left(96);
    return token.isEmpty() ? QStringLiteral("unknown") : token;
}

inline QString mediaSidecarRootPath(const QString& mediaPath)
{
    const QFileInfo info(mediaPath);
    const QString stem = facedetectionsSidecarToken(info.completeBaseName());
    return info.dir().filePath(stem + QStringLiteral(".jcut"));
}

inline QString facedetectionsClipSidecarDir(const QString& mediaPath,
                                            const QString& clipId)
{
    return QDir(mediaSidecarRootPath(mediaPath))
        .filePath(QStringLiteral("facedetections/%1")
                      .arg(facedetectionsSidecarToken(
                          clipId.trimmed().isEmpty()
                              ? QStringLiteral("unknown_clip")
                              : clipId)));
}

inline QString trackMemoryClipSidecarDir(const QString& mediaPath,
                                         const QString& clipId)
{
    return QDir(mediaSidecarRootPath(mediaPath))
        .filePath(QStringLiteral("track_memory/%1")
                      .arg(facedetectionsSidecarToken(
                          clipId.trimmed().isEmpty()
                              ? QStringLiteral("unknown_clip")
                              : clipId)));
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
