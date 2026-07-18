#include "mask_sidecar.h"

#include "editor_timeline_types.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace editor::masks {

namespace {

bool looksLikeGeneratedMaskDirectory(const QFileInfo& directory, const QString& mediaStem)
{
    const QString name = directory.fileName();
    if (!name.startsWith(mediaStem + QLatin1Char('_'), Qt::CaseInsensitive)) {
        return false;
    }
    const QString suffix = name.mid(mediaStem.size() + 1).toLower();
    return suffix.contains(QStringLiteral("mask")) ||
           suffix.contains(QStringLiteral("matte")) ||
           suffix.contains(QStringLiteral("segment")) ||
           suffix.contains(QStringLiteral("alpha")) ||
           QFileInfo(QDir(directory.absoluteFilePath()).filePath(
               QStringLiteral("jcut_alpha.json"))).exists();
}

QString displayNameForDirectory(QString name, const QString& mediaStem)
{
    const QString mediaPrefix = mediaStem + QLatin1Char('_');
    if (!mediaStem.isEmpty() && name.startsWith(mediaPrefix, Qt::CaseInsensitive)) {
        name.remove(0, mediaPrefix.size());
    }
    for (const QString& generator : {QStringLiteral("sam3_"),
                                     QStringLiteral("sam2_"),
                                     QStringLiteral("birefnet_"),
                                     QStringLiteral("ai_")}) {
        if (name.startsWith(generator, Qt::CaseInsensitive)) {
            name.remove(0, generator.size());
            break;
        }
    }
    for (const QString& suffix : {QStringLiteral("_binary_masks"),
                                  QStringLiteral("_alpha_masks"),
                                  QStringLiteral("_masks"),
                                  QStringLiteral("_mask"),
                                  QStringLiteral("_mattes"),
                                  QStringLiteral("_matte")}) {
        if (name.endsWith(suffix, Qt::CaseInsensitive)) {
            name.chop(suffix.size());
            break;
        }
    }
    return name.replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
}

} // namespace

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
    sidecar.displayName = displayNameForDirectory(dirInfo.fileName(), mediaStem);
    QFile metadataFile(dir.filePath(QStringLiteral("jcut_alpha.json")));
    if (metadataFile.open(QIODevice::ReadOnly)) {
        const QJsonObject metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
        const QString sourceType = metadata.value(QStringLiteral("source_type")).toString();
        if (!sourceType.isEmpty()) sidecar.sourceType = sourceType;
    }
    if (sidecar.displayName.isEmpty()) sidecar.displayName = QStringLiteral("Generated mask");

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
    const QFileInfoList siblingDirectories = mediaInfo.dir().entryInfoList(
        QStringList{QStringLiteral("%1_*").arg(stem)},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    auto append = [&](const QString& path) {
        MaskSidecar sidecar = inspectMaskSidecar(path, stem);
        if (sidecar.isValid() && !ids.contains(sidecar.id)) {
            ids.insert(sidecar.id);
            result.push_back(std::move(sidecar));
        }
    };
    append(clip.maskFramesDir);
    for (const QFileInfo& candidate : siblingDirectories) {
        if (looksLikeGeneratedMaskDirectory(candidate, stem)) {
            append(candidate.absoluteFilePath());
        }
    }
    std::sort(result.begin(), result.end(), [](const MaskSidecar& left, const MaskSidecar& right) {
        const int nameOrder = QString::localeAwareCompare(left.displayName, right.displayName);
        return nameOrder == 0 ? left.id < right.id : nameOrder < 0;
    });
    return result;
}

} // namespace editor::masks
