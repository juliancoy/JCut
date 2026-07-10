#include "mask_sidecar.h"

#include "editor_timeline_types.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace editor::masks {

QString stableMaskSidecarId(const QString& directory)
{
    const QString canonical = QFileInfo(directory).canonicalFilePath();
    const QByteArray identity = (canonical.isEmpty() ? QDir::cleanPath(directory) : canonical).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(16));
}

MaskSidecar inspectMaskSidecar(const QString& directory, const QString& mediaStem)
{
    MaskSidecar sidecar;
    const QFileInfo dirInfo(directory);
    if (!dirInfo.exists() || !dirInfo.isDir()) return sidecar;
    const QDir dir(dirInfo.absoluteFilePath());
    const QStringList frames = dir.entryList(
        QStringList{QStringLiteral("frame_*.png")}, QDir::Files, QDir::Name);
    if (frames.isEmpty()) return sidecar;

    sidecar.directory = dir.absolutePath();
    sidecar.id = stableMaskSidecarId(sidecar.directory);
    sidecar.frameCount = frames.size();
    sidecar.displayName = dirInfo.fileName();
    const QString prefix = mediaStem + QStringLiteral("_sam3_");
    if (!mediaStem.isEmpty() && sidecar.displayName.startsWith(prefix)) {
        sidecar.displayName.remove(0, prefix.size());
    }
    sidecar.displayName.remove(QStringLiteral("_binary_masks"));

    const QRegularExpression framePattern(QStringLiteral("^frame_(\\d+)\\.png$"));
    for (const QString& frameName : frames) {
        const QRegularExpressionMatch match = framePattern.match(frameName);
        if (!match.hasMatch()) continue;
        const int64_t frame = match.captured(1).toLongLong();
        if (sidecar.firstFrame < 0) sidecar.firstFrame = frame;
        sidecar.lastFrame = qMax(sidecar.lastFrame, frame);
    }
    return sidecar;
}

QVector<MaskSidecar> discoverMaskSidecars(const TimelineClip& clip)
{
    const QFileInfo mediaInfo(clip.filePath);
    const QString stem = mediaInfo.completeBaseName();
    if (stem.isEmpty()) return {};
    QVector<MaskSidecar> result;
    QSet<QString> ids;
    const QFileInfoList candidates = mediaInfo.dir().entryInfoList(
        QStringList{QStringLiteral("%1_sam3_*_binary_masks").arg(stem)},
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    auto append = [&](const QString& path) {
        MaskSidecar sidecar = inspectMaskSidecar(path, stem);
        if (sidecar.isValid() && !ids.contains(sidecar.id)) {
            ids.insert(sidecar.id);
            result.push_back(std::move(sidecar));
        }
    };
    append(clip.maskFramesDir);
    for (const QFileInfo& candidate : candidates) append(candidate.absoluteFilePath());
    return result;
}

} // namespace editor::masks
