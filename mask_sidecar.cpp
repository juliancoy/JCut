#include "mask_sidecar.h"

#include "editor_timeline_types.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <sys/stat.h>

namespace editor::masks {

namespace {

struct FrameIndexMapInspection {
    bool populated = false;
    int64_t mappedFrameCount = 0;
    int64_t firstSourceFrame = -1;
    int64_t lastSourceFrame = -1;
    int64_t lastMaskFrame = -1;
    QString mapSha256;
};

constexpr qint64 kIdentityHashBytes = 1024 * 1024;
constexpr qint64 kSourceHashChunkBytes = 4 * 1024 * 1024;
constexpr int kSourceHashCacheEntries = 8;
const QString kSourceIdentitySchema =
    QStringLiteral("jcut_source_content_identity_v1");

QJsonObject readJsonObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject readAlphaMetadata(const QDir& directory)
{
    return readJsonObject(directory.filePath(QStringLiteral("jcut_alpha.json")));
}

QJsonObject readMaskMetadata(const QDir& directory)
{
    return readJsonObject(directory.filePath(QStringLiteral("jcut_mask.json")));
}

QJsonObject readGeneratorMetadata(const QDir& directory)
{
    const QJsonObject mask = readMaskMetadata(directory);
    return mask.isEmpty() ? readAlphaMetadata(directory) : mask;
}

bool isSha256(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern.match(value.trimmed()).hasMatch();
}

QString portableContentHash(const QJsonObject& identity)
{
    if (identity.value(QStringLiteral("identity_schema")).toString() !=
        kSourceIdentitySchema) {
        return {};
    }
    const QString contentHash =
        identity.value(QStringLiteral("content_sha256")).toString().trimmed();
    return isSha256(contentHash) ? contentHash.toLower() : QString{};
}

bool statFieldsMatch(const QJsonObject& left, const QJsonObject& right)
{
    const qint64 leftSize = left.value(QStringLiteral("size")).toInteger(-1);
    const qint64 rightSize = right.value(QStringLiteral("size")).toInteger(-1);
    const auto requiredVersionFieldMatches = [&left, &right](const QString& key) {
        const QString leftValue = left.value(key).toString().trimmed();
        return !leftValue.isEmpty() && leftValue == right.value(key).toString().trimmed();
    };
    return leftSize >= 0 && leftSize == rightSize &&
           requiredVersionFieldMatches(QStringLiteral("mtime_ns")) &&
           requiredVersionFieldMatches(QStringLiteral("ctime_ns")) &&
           requiredVersionFieldMatches(QStringLiteral("device")) &&
           requiredVersionFieldMatches(QStringLiteral("inode"));
}

bool versionTokensMatch(const QJsonObject& left, const QJsonObject& right)
{
    const QString leftHash =
        left.value(QStringLiteral("head_tail_sha256")).toString().trimmed();
    const QString rightHash =
        right.value(QStringLiteral("head_tail_sha256")).toString().trimmed();
    const QString leftMiddleHash =
        left.value(QStringLiteral("middle_sha256")).toString().trimmed();
    const QString rightMiddleHash =
        right.value(QStringLiteral("middle_sha256")).toString().trimmed();
    return statFieldsMatch(left, right) && isSha256(leftHash) &&
           leftHash.compare(rightHash, Qt::CaseInsensitive) == 0 &&
           isSha256(leftMiddleHash) &&
           leftMiddleHash.compare(rightMiddleHash, Qt::CaseInsensitive) == 0;
}

bool identityObjectsMatch(const QJsonObject& left, const QJsonObject& right)
{
    const qint64 leftSize = left.value(QStringLiteral("size")).toInteger(-1);
    const qint64 rightSize = right.value(QStringLiteral("size")).toInteger(-1);
    const QString leftContentHash =
        left.value(QStringLiteral("content_sha256")).toString().trimmed();
    const QString rightContentHash =
        right.value(QStringLiteral("content_sha256")).toString().trimmed();
    for (const QJsonObject& identity : {left, right}) {
        const QString schema =
            identity.value(QStringLiteral("identity_schema")).toString().trimmed();
        if (!schema.isEmpty() && schema != kSourceIdentitySchema) {
            return false;
        }
    }
    if ((!leftContentHash.isEmpty() && !isSha256(leftContentHash)) ||
        (!rightContentHash.isEmpty() && !isSha256(rightContentHash))) {
        return false;
    }
    const QString leftPortableHash = portableContentHash(left);
    const QString rightPortableHash = portableContentHash(right);
    if (!leftPortableHash.isEmpty() && !rightPortableHash.isEmpty()) {
        return leftSize >= 0 && leftSize == rightSize &&
               leftPortableHash == rightPortableHash;
    }
    if ((!leftContentHash.isEmpty() && leftPortableHash.isEmpty()) ||
        (!rightContentHash.isEmpty() && rightPortableHash.isEmpty())) {
        return false;
    }
    // Transitional compatibility for a current v2 map/completion pair where
    // one or both manifests predate whole-file identities. This path is
    // deliberately local-token-only and cannot authenticate a copied source.
    return versionTokensMatch(left, right);
}

bool identityObjectIsUsable(const QJsonObject& identity)
{
    if (identity.value(QStringLiteral("size")).toInteger(-1) < 0) {
        return false;
    }
    return !portableContentHash(identity).isEmpty() ||
           versionTokensMatch(identity, identity);
}

QJsonObject sourceStatFields(const QString& absolutePath)
{
    struct stat sourceStat {};
    const QByteArray nativePath = QFile::encodeName(absolutePath);
    if (::stat(nativePath.constData(), &sourceStat) != 0 ||
        !S_ISREG(sourceStat.st_mode) || sourceStat.st_size < 0) {
        return {};
    }
    return QJsonObject{
        {QStringLiteral("size"), static_cast<qint64>(sourceStat.st_size)},
        {QStringLiteral("mtime_ns"),
         QString::number(static_cast<qint64>(sourceStat.st_mtim.tv_sec) * 1000000000 +
                         sourceStat.st_mtim.tv_nsec)},
        {QStringLiteral("ctime_ns"),
         QString::number(static_cast<qint64>(sourceStat.st_ctim.tv_sec) * 1000000000 +
                         sourceStat.st_ctim.tv_nsec)},
        {QStringLiteral("device"),
         QString::number(static_cast<qulonglong>(sourceStat.st_dev))},
        {QStringLiteral("inode"),
         QString::number(static_cast<qulonglong>(sourceStat.st_ino))},
    };
}

QJsonObject sourceVersionTokenForFile(const QString& sourceMediaPath)
{
    const QString absolutePath = QFileInfo(sourceMediaPath).absoluteFilePath();
    const QJsonObject before = sourceStatFields(absolutePath);
    const qint64 sourceSize = before.value(QStringLiteral("size")).toInteger(-1);
    if (before.isEmpty() || sourceSize < 0) {
        return {};
    }
    QFile source(absolutePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    QCryptographicHash middleDigest(QCryptographicHash::Sha256);
    digest.addData(QByteArray::number(sourceSize));
    digest.addData(QByteArray(1, '\0'));
    middleDigest.addData(QByteArray::number(sourceSize));
    middleDigest.addData(QByteArray(1, '\0'));
    digest.addData(source.read(kIdentityHashBytes));
    if (sourceSize > kIdentityHashBytes) {
        if (!source.seek(qMax<qint64>(0, sourceSize - kIdentityHashBytes))) {
            return {};
        }
        digest.addData(source.read(kIdentityHashBytes));
    }
    const qint64 middleOffset =
        qMax<qint64>(0, (sourceSize - kIdentityHashBytes) / 2);
    if (!source.seek(middleOffset)) {
        return {};
    }
    middleDigest.addData(source.read(kIdentityHashBytes));
    const QJsonObject after = sourceStatFields(absolutePath);
    if (!statFieldsMatch(before, after)) {
        return {};
    }
    QJsonObject result = before;
    result.insert(QStringLiteral("cache_token_schema"),
                  QStringLiteral("jcut_source_version_token_v1"));
    result.insert(QStringLiteral("path"), absolutePath);
    result.insert(QStringLiteral("head_tail_sha256"),
                  QString::fromLatin1(digest.result().toHex()));
    result.insert(QStringLiteral("middle_sha256"),
                  QString::fromLatin1(middleDigest.result().toHex()));
    result.insert(QStringLiteral("hash_bytes_per_edge"), kIdentityHashBytes);
    result.insert(QStringLiteral("middle_hash_bytes"), kIdentityHashBytes);
    return result;
}

QString sourceHashCacheKey(const QString& sourceMediaPath, const QJsonObject& token)
{
    QStringList fields{QFileInfo(sourceMediaPath).absoluteFilePath()};
    for (const QString& key : {QStringLiteral("size"),
                               QStringLiteral("mtime_ns"),
                               QStringLiteral("ctime_ns"),
                               QStringLiteral("device"),
                               QStringLiteral("inode"),
                               QStringLiteral("head_tail_sha256"),
                               QStringLiteral("middle_sha256")}) {
        fields.push_back(token.value(key).toVariant().toString());
    }
    return fields.join(QChar(0));
}

QString cachedWholeFileSha256(const QString& sourceMediaPath,
                              const QJsonObject& token)
{
    static QMutex cacheMutex;
    static QHash<QString, QString> cache;
    static QStringList cacheOrder;
    const QString cacheKey = sourceHashCacheKey(sourceMediaPath, token);
    {
        const QMutexLocker lock(&cacheMutex);
        const auto found = cache.constFind(cacheKey);
        if (found != cache.cend()) {
            cacheOrder.removeAll(cacheKey);
            cacheOrder.push_back(cacheKey);
            return found.value();
        }
    }

    const QString absolutePath = QFileInfo(sourceMediaPath).absoluteFilePath();
    const QJsonObject before = sourceStatFields(absolutePath);
    if (!statFieldsMatch(token, before)) {
        return {};
    }
    QFile source(absolutePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kSourceHashChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            return {};
        }
        digest.addData(chunk);
    }
    if (!statFieldsMatch(before, sourceStatFields(absolutePath))) {
        return {};
    }
    const QString contentHash = QString::fromLatin1(digest.result().toHex());
    {
        const QMutexLocker lock(&cacheMutex);
        cache.insert(cacheKey, contentHash);
        cacheOrder.removeAll(cacheKey);
        cacheOrder.push_back(cacheKey);
        while (cacheOrder.size() > kSourceHashCacheEntries) {
            cache.remove(cacheOrder.takeFirst());
        }
    }
    return contentHash;
}

QJsonObject sourceIdentityForFile(const QString& sourceMediaPath,
                                  const QJsonObject& expectedIdentity)
{
    QJsonObject actual = sourceVersionTokenForFile(sourceMediaPath);
    if (actual.isEmpty()) {
        return {};
    }
    const QString expectedHash =
        portableContentHash(expectedIdentity);
    if (!expectedIdentity.value(QStringLiteral("content_sha256"))
             .toString().trimmed().isEmpty() && expectedHash.isEmpty()) {
        return {};
    }
    if (expectedHash.isEmpty()) {
        return actual; // Strict local-token compatibility for legacy v2 metadata.
    }
    if (!isSha256(expectedHash)) {
        return {};
    }
    const QString actualHash = versionTokensMatch(expectedIdentity, actual)
        ? expectedHash
        : cachedWholeFileSha256(sourceMediaPath, actual);
    if (!isSha256(actualHash)) {
        return {};
    }
    actual.insert(QStringLiteral("identity_schema"), kSourceIdentitySchema);
    actual.insert(QStringLiteral("content_sha256"), actualHash.toLower());
    return actual;
}

bool metadataIdentityMatchesSource(const QJsonObject& metadata,
                                   const QString& sourceMediaPath)
{
    const QJsonObject expected =
        metadata.value(QStringLiteral("source_identity")).toObject();
    if (sourceMediaPath.trimmed().isEmpty()) {
        return identityObjectIsUsable(expected);
    }
    const QJsonObject actual = sourceIdentityForFile(sourceMediaPath, expected);
    return !actual.isEmpty() && identityObjectsMatch(expected, actual);
}

bool frameDomainUsesOrdinals(const QString& frameDomain)
{
    const QString normalized = frameDomain.trimmed().toLower();
    return normalized == QStringLiteral("ordinal") ||
           normalized.endsWith(QStringLiteral("_ordinal"));
}

bool generatedDirectoryUsesOrdinals(const QString& directoryName)
{
    return directoryName.contains(QStringLiteral("sam3"), Qt::CaseInsensitive) ||
           directoryName.contains(QStringLiteral("sam2"), Qt::CaseInsensitive) ||
           directoryName.contains(QStringLiteral("birefnet"), Qt::CaseInsensitive);
}

bool sourceTypeUsesOrdinals(const QString& sourceType)
{
    return sourceType.contains(QStringLiteral("sam3"), Qt::CaseInsensitive) ||
           sourceType.contains(QStringLiteral("sam2"), Qt::CaseInsensitive) ||
           sourceType.contains(QStringLiteral("birefnet"), Qt::CaseInsensitive);
}

bool sourceTypeIsBiRefNet(const QString& directoryName, const QString& sourceType)
{
    return directoryName.contains(QStringLiteral("birefnet"), Qt::CaseInsensitive) ||
           sourceType.contains(QStringLiteral("birefnet"), Qt::CaseInsensitive);
}

FrameIndexMapInspection inspectFrameIndexMap(const QDir& directory)
{
    FrameIndexMapInspection result;
    QFile mapFile(directory.filePath(QStringLiteral("jcut_frame_map.tsv")));
    if (!mapFile.open(QIODevice::ReadOnly)) {
        return result;
    }
    result.mapSha256 = QString::fromLatin1(
        QCryptographicHash::hash(mapFile.readAll(), QCryptographicHash::Sha256).toHex());
    if (!mapFile.seek(0)) {
        return {};
    }
    QTextStream stream(&mapFile);
    int64_t previousSourceFrame = -1;
    int64_t previousMaskFrame = -1;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = line.split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        bool sourceOk = false;
        bool maskOk = false;
        const int64_t sourceFrame = fields.value(0).toLongLong(&sourceOk);
        const int64_t maskFrame = fields.value(1).toLongLong(&maskOk);
        if (!sourceOk || !maskOk || sourceFrame < 0 || maskFrame < 0 ||
            maskFrame != result.mappedFrameCount ||
            (result.mappedFrameCount > 0 &&
             (sourceFrame < previousSourceFrame || maskFrame < previousMaskFrame))) {
            return {};
        }
        if (result.firstSourceFrame < 0) {
            result.firstSourceFrame = sourceFrame;
        }
        result.lastSourceFrame = sourceFrame;
        result.lastMaskFrame = qMax(result.lastMaskFrame, maskFrame);
        previousSourceFrame = sourceFrame;
        previousMaskFrame = maskFrame;
        ++result.mappedFrameCount;
    }
    result.populated = result.mappedFrameCount > 0;
    return result;
}

bool frameIndexMetadataMatches(const QDir& directory,
                               const FrameIndexMapInspection& map,
                               const QString& sourceMediaPath,
                               QJsonObject* matchedMetadata = nullptr)
{
    if (!map.populated) {
        return false;
    }
    const QJsonObject metadata = readJsonObject(
        directory.filePath(QStringLiteral("jcut_frame_map.json")));
    const bool matches = metadata.value(QStringLiteral("schema")).toString() ==
               QStringLiteral("jcut_frame_index_map_v2") &&
           metadata.value(QStringLiteral("status")).toString() ==
               QStringLiteral("ready") &&
           metadata.value(QStringLiteral("frame_domain")).toString() ==
               QStringLiteral("source_timestamp_to_generated_ordinal") &&
           metadata.value(QStringLiteral("map_file")).toString() ==
               QStringLiteral("jcut_frame_map.tsv") &&
           metadata.contains(QStringLiteral("output_fps")) &&
           metadata.value(QStringLiteral("output_fps")).isNull() &&
           metadata.value(QStringLiteral("mapped_frame_count")).toInteger(-1) ==
               map.mappedFrameCount &&
           metadata.value(QStringLiteral("min_source_frame")).toInteger(-1) ==
               map.firstSourceFrame &&
           metadata.value(QStringLiteral("max_source_frame")).toInteger(-1) ==
               map.lastSourceFrame &&
           metadata.value(QStringLiteral("max_mask_frame")).toInteger(-1) ==
               map.lastMaskFrame &&
           metadata.value(QStringLiteral("expected_output_frame_count")).toInteger(-1) ==
               map.lastMaskFrame + 1 &&
           metadata.value(QStringLiteral("map_sha256")).toString().compare(
               map.mapSha256, Qt::CaseInsensitive) == 0 &&
           metadataIdentityMatchesSource(metadata, sourceMediaPath);
    if (matches && matchedMetadata) {
        *matchedMetadata = metadata;
    }
    return matches;
}

bool completionMetadataConfirms(const QDir& directory,
                                const QString& sourceType,
                                int64_t lastMappedMaskFrame,
                                const QJsonObject& frameIndexMetadata)
{
    const bool biRefNet = sourceTypeIsBiRefNet(directory.dirName(), sourceType);
    const QJsonObject metadata = biRefNet
        ? readAlphaMetadata(directory)
        : readMaskMetadata(directory);
    if (metadata.isEmpty() || lastMappedMaskFrame < 0) {
        return false;
    }
    const QString schema = metadata.value(QStringLiteral("schema")).toString();
    const QString expectedSchema = biRefNet
        ? QStringLiteral("jcut_alpha_sidecar_v1")
        : QStringLiteral("jcut_mask_sidecar_v1");
    if (schema != expectedSchema ||
        !metadata.value(QStringLiteral("complete")).toBool(false) ||
        metadata.value(QStringLiteral("frame_domain")).toString() !=
            QStringLiteral("decode_ordinal") ||
        metadata.value(QStringLiteral("frame_index_map")).toString() !=
            QStringLiteral("jcut_frame_map.tsv") ||
        metadata.value(QStringLiteral("frame_index_metadata")).toString() !=
            QStringLiteral("jcut_frame_map.json")) {
        return false;
    }
    const int64_t expected =
        metadata.value(QStringLiteral("expected_frame_count")).toInteger(-1);
    if (expected != lastMappedMaskFrame + 1) {
        return false;
    }
    if (metadata.value(QStringLiteral("frame_map_sha256")).toString().compare(
            frameIndexMetadata.value(QStringLiteral("map_sha256")).toString(),
            Qt::CaseInsensitive) != 0 ||
        metadata.value(QStringLiteral("frame_map_sha256")).toString().isEmpty()) {
        return false;
    }
    return identityObjectsMatch(
        metadata.value(QStringLiteral("source_identity")).toObject(),
        frameIndexMetadata.value(QStringLiteral("source_identity")).toObject());
}

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
               QStringLiteral("jcut_alpha.json"))).exists() ||
           QFileInfo(QDir(directory.absoluteFilePath()).filePath(
               QStringLiteral("jcut_mask.json"))).exists();
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

bool cheapOrdinalSidecarReady(const QString& directory,
                              const QString& sourceMediaPath)
{
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir()) {
        return false;
    }
    const QDir dir(directoryInfo.absoluteFilePath());
    const QJsonObject generatorMetadata = readGeneratorMetadata(dir);
    const QString sourceType = generatorMetadata
                                   .value(QStringLiteral("source_type"))
                                   .toString();
    const FrameIndexMapInspection map = inspectFrameIndexMap(dir);
    QJsonObject mapMetadata;
    if (!frameIndexMetadataMatches(dir, map, sourceMediaPath, &mapMetadata)) {
        return false;
    }
    const QFileInfo firstFrame(dir.filePath(QStringLiteral("frame_000001.png")));
    const QFileInfo lastFrame(dir.filePath(
        QStringLiteral("frame_%1.png")
            .arg(map.lastMaskFrame + 1, 6, 10, QLatin1Char('0'))));
    return firstFrame.isFile() && firstFrame.size() > 0 &&
           lastFrame.isFile() && lastFrame.size() > 0 &&
           completionMetadataConfirms(dir, sourceType, map.lastMaskFrame, mapMetadata);
}

bool directoryHasAnyMaskFrame(const QString& directory)
{
    QDirIterator frames(directory,
                        QStringList{QStringLiteral("frame_*.png")},
                        QDir::Files | QDir::NoSymLinks);
    return frames.hasNext();
}

} // namespace

QString stableMaskSidecarId(const QString& directory)
{
    const QString canonical = QFileInfo(directory).canonicalFilePath();
    const QByteArray identity =
        (canonical.isEmpty() ? QDir::cleanPath(directory) : canonical).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(16));
}

bool maskSidecarUsesDecodeOrdinalFrames(const QString& directory)
{
    const QFileInfo dirInfo(directory);
    const QDir dir(dirInfo.absoluteFilePath());
    const QJsonObject metadata = readGeneratorMetadata(dir);
    const QJsonObject frameIndexMetadata = readJsonObject(
        dir.filePath(QStringLiteral("jcut_frame_map.json")));
    if (QFileInfo::exists(dir.filePath(QStringLiteral("jcut_frame_map.tsv"))) ||
        frameIndexMetadata.value(QStringLiteral("schema")).toString() ==
            QStringLiteral("jcut_frame_index_map_v2")) {
        return true;
    }
    const QString frameDomain =
        metadata.value(QStringLiteral("frame_domain")).toString().trimmed();
    if (!frameDomain.isEmpty()) {
        return frameDomainUsesOrdinals(frameDomain);
    }
    return generatedDirectoryUsesOrdinals(dirInfo.fileName()) ||
           sourceTypeUsesOrdinals(
               metadata.value(QStringLiteral("source_type")).toString());
}

bool maskSidecarCompletionConfirmedForRender(const QString& directory,
                                             int64_t lastMappedMaskFrame,
                                             const QString& sourceMediaPath)
{
    const QFileInfo dirInfo(directory);
    const QDir dir(dirInfo.absoluteFilePath());
    const QJsonObject metadata = readGeneratorMetadata(dir);
    const QJsonObject frameIndexMetadata = readJsonObject(
        dir.filePath(QStringLiteral("jcut_frame_map.json")));
    if (frameIndexMetadata.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("jcut_frame_index_map_v2") ||
        frameIndexMetadata.value(QStringLiteral("status")).toString() !=
            QStringLiteral("ready") ||
        frameIndexMetadata.value(QStringLiteral("frame_domain")).toString() !=
            QStringLiteral("source_timestamp_to_generated_ordinal") ||
        frameIndexMetadata.value(QStringLiteral("map_file")).toString() !=
            QStringLiteral("jcut_frame_map.tsv") ||
        !frameIndexMetadata.contains(QStringLiteral("output_fps")) ||
        !frameIndexMetadata.value(QStringLiteral("output_fps")).isNull() ||
        frameIndexMetadata.value(QStringLiteral("max_mask_frame")).toInteger(-1) !=
            lastMappedMaskFrame ||
        !metadataIdentityMatchesSource(frameIndexMetadata, sourceMediaPath)) {
        return false;
    }
    return completionMetadataConfirms(
        dir, metadata.value(QStringLiteral("source_type")).toString(),
        lastMappedMaskFrame, frameIndexMetadata);
}

MaskSidecar inspectMaskSidecar(const QString& directory,
                               const QString& mediaStem,
                               const QString& sourceMediaPath)
{
    MaskSidecar sidecar;
    const QFileInfo dirInfo(directory);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        return sidecar;
    }
    const QDir dir(dirInfo.absoluteFilePath());
    const QRegularExpression framePattern(QStringLiteral("^frame_(\\d+)\\.png$"));
    QDirIterator frames(dir.absolutePath(),
                        QStringList{QStringLiteral("frame_*.png")},
                        QDir::Files | QDir::NoSymLinks);
    while (frames.hasNext()) {
        const QString frameName = QFileInfo(frames.next()).fileName();
        const QRegularExpressionMatch match = framePattern.match(frameName);
        if (!match.hasMatch()) {
            continue;
        }
        const int64_t frame = match.captured(1).toLongLong();
        sidecar.firstFrame = sidecar.firstFrame < 0
            ? frame
            : qMin(sidecar.firstFrame, frame);
        sidecar.lastFrame = qMax(sidecar.lastFrame, frame);
        ++sidecar.frameCount;
    }
    if (sidecar.frameCount <= 0) {
        return {};
    }

    sidecar.directory = dir.absolutePath();
    sidecar.id = stableMaskSidecarId(sidecar.directory);
    sidecar.displayName = displayNameForDirectory(dirInfo.fileName(), mediaStem);
    const QJsonObject metadata = readGeneratorMetadata(dir);
    const QString metadataSourceType =
        metadata.value(QStringLiteral("source_type")).toString().trimmed();
    const bool isBiRefNet =
        sourceTypeIsBiRefNet(dirInfo.fileName(), metadataSourceType);
    if (!metadataSourceType.isEmpty()) {
        sidecar.sourceType = metadataSourceType;
    } else if (isBiRefNet) {
        sidecar.sourceType = QStringLiteral("birefnet_continuous_alpha");
    }
    sidecar.frameDomain =
        metadata.value(QStringLiteral("frame_domain")).toString().trimmed();
    const FrameIndexMapInspection map = inspectFrameIndexMap(dir);
    const QJsonObject rawFrameIndexMetadata = readJsonObject(
        dir.filePath(QStringLiteral("jcut_frame_map.json")));
    const bool mappedOrdinalSidecar = map.populated ||
        QFileInfo::exists(dir.filePath(QStringLiteral("jcut_frame_map.tsv"))) ||
        rawFrameIndexMetadata.value(QStringLiteral("schema")).toString() ==
            QStringLiteral("jcut_frame_index_map_v2");
    sidecar.decodeOrdinalFrames = mappedOrdinalSidecar ||
        (!sidecar.frameDomain.isEmpty()
             ? frameDomainUsesOrdinals(sidecar.frameDomain)
             : generatedDirectoryUsesOrdinals(dirInfo.fileName()) ||
                   sourceTypeUsesOrdinals(metadataSourceType));

    sidecar.frameIndexMapAvailable = map.populated;
    QJsonObject frameIndexMetadata;
    sidecar.frameIndexMetadataAvailable =
        frameIndexMetadataMatches(dir, map, sourceMediaPath, &frameIndexMetadata);
    sidecar.mappedFrameCount = map.mappedFrameCount;
    sidecar.firstMappedSourceFrame = map.firstSourceFrame;
    sidecar.lastMappedSourceFrame = map.lastSourceFrame;
    sidecar.lastMappedMaskFrame = map.lastMaskFrame;
    sidecar.completionConfirmed = !sidecar.decodeOrdinalFrames ||
        completionMetadataConfirms(
            dir, sidecar.sourceType, map.lastMaskFrame, frameIndexMetadata);

    if (sidecar.decodeOrdinalFrames && !sidecar.frameIndexMapAvailable) {
        sidecar.readinessIssue = QStringLiteral("Frame map missing");
    } else if (sidecar.decodeOrdinalFrames && !sidecar.frameIndexMetadataAvailable) {
        sidecar.readinessIssue = QStringLiteral("Frame map unverified");
    } else if (sidecar.decodeOrdinalFrames && !sidecar.completionConfirmed) {
        sidecar.readinessIssue = QStringLiteral("Generation incomplete");
    }
    if (sidecar.displayName.isEmpty()) {
        sidecar.displayName = QStringLiteral("Generated mask");
    }
    return sidecar;
}

QVector<MaskSidecar> discoverMaskSidecars(const TimelineClip& clip)
{
    const QFileInfo mediaInfo(clip.filePath);
    const QString stem = mediaInfo.completeBaseName();
    if (stem.isEmpty()) {
        return {};
    }
    QVector<MaskSidecar> result;
    QSet<QString> ids;
    const QFileInfoList siblingDirectories = mediaInfo.dir().entryInfoList(
        QStringList{QStringLiteral("%1_*").arg(stem)},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    auto append = [&](const QString& path) {
        MaskSidecar sidecar = inspectMaskSidecar(path, stem, clip.filePath);
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

bool hasReadyMaskSidecar(const TimelineClip& clip)
{
    const QFileInfo mediaInfo(clip.filePath);
    const QString stem = mediaInfo.completeBaseName();
    if (stem.isEmpty()) {
        return false;
    }
    auto ready = [&clip](const QString& path) {
        if (path.trimmed().isEmpty()) {
            return false;
        }
        return maskSidecarUsesDecodeOrdinalFrames(path)
            ? cheapOrdinalSidecarReady(path, clip.filePath)
            : directoryHasAnyMaskFrame(path);
    };
    if (ready(clip.maskFramesDir)) {
        return true;
    }
    const QFileInfoList candidates = mediaInfo.dir().entryInfoList(
        QStringList{QStringLiteral("%1_*").arg(stem)},
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& candidate : candidates) {
        if (looksLikeGeneratedMaskDirectory(candidate, stem) &&
            ready(candidate.absoluteFilePath())) {
            return true;
        }
    }
    return false;
}

} // namespace editor::masks
