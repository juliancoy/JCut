#include "facestream_artifact_utils.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

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
