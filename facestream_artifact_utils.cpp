#include "facestream_artifact_utils.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace {
constexpr auto kContinuityFacestreamsByClipKey = "continuity_facestreams_by_clip";
constexpr auto kLegacyContinuityFacestreamsByClipKey = "continuity_boxstreams_by_clip";
}

qint64 facestreamArtifactRevisionMsForTranscript(const QString& transcriptPath)
{
    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return -1;
    }
    const QDir dir = info.dir();
    const QString base = info.completeBaseName();
    const QStringList candidates{
        dir.filePath(base + QStringLiteral("_facestream.bin")),
        dir.filePath(base + QStringLiteral("_facestream_processed.bin")),
        dir.filePath(base + QStringLiteral("_facestream.bin")),
        dir.filePath(base + QStringLiteral("_facestream.json")),
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

QString continuityFacestreamsByClipKey()
{
    return QString(QLatin1StringView(kContinuityFacestreamsByClipKey));
}

QJsonObject continuityFacestreamsByClipObject(const QJsonObject& artifactRoot)
{
    QJsonObject byClip = artifactRoot.value(QLatin1StringView(kContinuityFacestreamsByClipKey)).toObject();
    if (!byClip.isEmpty()) {
        return byClip;
    }
    return artifactRoot.value(QLatin1StringView(kLegacyContinuityFacestreamsByClipKey)).toObject();
}

QJsonObject continuityRootForClip(const QJsonObject& artifactRoot, const QString& clipId)
{
    return continuityFacestreamsByClipObject(artifactRoot).value(clipId.trimmed()).toObject();
}

void setContinuityFacestreamsByClipObject(QJsonObject* artifactRoot, const QJsonObject& byClip)
{
    if (!artifactRoot) {
        return;
    }
    artifactRoot->remove(QLatin1StringView(kLegacyContinuityFacestreamsByClipKey));
    (*artifactRoot)[QString(QLatin1StringView(kContinuityFacestreamsByClipKey))] = byClip;
}
