#include "editor_shared_keyframes_cache.h"

#include "facedetections_artifact_utils.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {
constexpr int kAssignedContinuityCacheMaxEntries = 16;

struct AssignedContinuityStreamsCacheEntry {
    QString revision;
    QStringList referencedPaths;
    AssignedContinuityStreamsPtr streams;
};

QString fileRevisionToken(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("<empty>");
    }
    const QFileInfo info(trimmed);
    if (!info.exists() || !info.isFile()) {
        return trimmed + QStringLiteral(":missing");
    }
    return trimmed +
           QStringLiteral(":") +
           QString::number(info.size()) +
           QStringLiteral(":") +
           QString::number(info.lastModified().toMSecsSinceEpoch());
}

QString assignedContinuityRevision(const QString& transcriptPath,
                                   const QString& processedPath,
                                   const QStringList& referencedPaths)
{
    QStringList tokens{
        fileRevisionToken(transcriptPath),
        fileRevisionToken(processedPath),
        QString::number(facedetectionsArtifactRevisionMsForTranscript(transcriptPath)),
    };
    for (const QString& path : referencedPaths) {
        tokens.push_back(fileRevisionToken(path));
    }
    return tokens.join(QLatin1Char('|'));
}

QMutex& assignedContinuityCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, AssignedContinuityStreamsCacheEntry>& assignedContinuityCache()
{
    static QHash<QString, AssignedContinuityStreamsCacheEntry> cache;
    return cache;
}

QStringList& assignedContinuityCacheInsertionOrder()
{
    static QStringList insertionOrder;
    return insertionOrder;
}
} // namespace

QString assignedContinuityCacheKey(const QString& transcriptPath,
                                   const QString& clipId,
                                   const QString& speakerId)
{
    return transcriptPath.trimmed() +
           QLatin1Char('|') +
           clipId.trimmed() +
           QLatin1Char('|') +
           speakerId.trimmed();
}

bool cachedAssignedContinuityStreams(const QString& cacheKey,
                                     const QString& transcriptPath,
                                     const QString& processedPath,
                                     QVector<jcut::facedetections::FacestreamTrack>* streamsOut)
{
    AssignedContinuityStreamsPtr streams;
    if (!cachedAssignedContinuityStreamsPtr(cacheKey, transcriptPath, processedPath, &streams)) {
        return false;
    }
    if (streamsOut) {
        *streamsOut = streams ? *streams : QVector<jcut::facedetections::FacestreamTrack>{};
    }
    return true;
}

bool cachedAssignedContinuityStreamsPtr(const QString& cacheKey,
                                        const QString& transcriptPath,
                                        const QString& processedPath,
                                        AssignedContinuityStreamsPtr* streamsOut)
{
    QMutexLocker locker(&assignedContinuityCacheMutex());
    auto& cache = assignedContinuityCache();
    const auto it = cache.constFind(cacheKey);
    if (it == cache.constEnd()) {
        return false;
    }
    const QString revision = assignedContinuityRevision(
        transcriptPath, processedPath, it.value().referencedPaths);
    if (revision != it.value().revision) {
        cache.remove(cacheKey);
        assignedContinuityCacheInsertionOrder().removeAll(cacheKey);
        return false;
    }
    if (streamsOut) {
        *streamsOut = it.value().streams;
    }
    return true;
}

bool cachedAssignedContinuityStreamsMemoryOnly(const QString& cacheKey,
                                               AssignedContinuityStreamsPtr* streamsOut)
{
    QMutexLocker locker(&assignedContinuityCacheMutex());
    const auto& cache = assignedContinuityCache();
    const auto it = cache.constFind(cacheKey);
    if (it == cache.constEnd()) {
        return false;
    }
    if (streamsOut) {
        *streamsOut = it.value().streams;
    }
    return true;
}

void storeAssignedContinuityStreams(const QString& cacheKey,
                                    const QString& transcriptPath,
                                    const QString& processedPath,
                                    const QStringList& referencedPaths,
                                    const QVector<jcut::facedetections::FacestreamTrack>& streams)
{
    AssignedContinuityStreamsCacheEntry entry;
    entry.referencedPaths = referencedPaths;
    entry.revision = assignedContinuityRevision(
        transcriptPath, processedPath, referencedPaths);
    entry.streams = AssignedContinuityStreamsPtr::create(streams);

    QMutexLocker locker(&assignedContinuityCacheMutex());
    auto& cache = assignedContinuityCache();
    auto& insertionOrder = assignedContinuityCacheInsertionOrder();
    if (!cache.contains(cacheKey)) {
        insertionOrder.push_back(cacheKey);
    }
    cache.insert(cacheKey, entry);
    while (insertionOrder.size() > kAssignedContinuityCacheMaxEntries) {
        const QString oldest = insertionOrder.takeFirst();
        cache.remove(oldest);
    }
}

void clearAssignedContinuityStreamsCache()
{
    QMutexLocker locker(&assignedContinuityCacheMutex());
    assignedContinuityCache().clear();
    assignedContinuityCacheInsertionOrder().clear();
}
