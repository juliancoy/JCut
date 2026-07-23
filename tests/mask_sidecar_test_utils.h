#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>
#include <sys/stat.h>

namespace mask_sidecar_test {

constexpr qint64 kIdentityHashBytes = 1024 * 1024;

inline QJsonObject sourceIdentity(const QString& path)
{
    const QFileInfo info(path);
    QFile source(info.absoluteFilePath());
    if (!info.isFile() || !source.open(QIODevice::ReadOnly)) {
        return {};
    }
#if defined(Q_OS_WIN)
    struct _stat64 sourceStat {};
    const std::wstring nativePath = info.absoluteFilePath().toStdWString();
    if (::_wstat64(nativePath.c_str(), &sourceStat) != 0) {
        return {};
    }
    const qint64 modifiedNanoseconds =
        static_cast<qint64>(sourceStat.st_mtime) * 1000000000;
    const qint64 changedNanoseconds =
        static_cast<qint64>(sourceStat.st_ctime) * 1000000000;
#else
    struct stat sourceStat {};
    const QByteArray nativePath = QFile::encodeName(info.absoluteFilePath());
    if (::stat(nativePath.constData(), &sourceStat) != 0) {
        return {};
    }
#if defined(Q_OS_DARWIN)
    const qint64 modifiedNanoseconds =
        static_cast<qint64>(sourceStat.st_mtimespec.tv_sec) * 1000000000 +
        sourceStat.st_mtimespec.tv_nsec;
    const qint64 changedNanoseconds =
        static_cast<qint64>(sourceStat.st_ctimespec.tv_sec) * 1000000000 +
        sourceStat.st_ctimespec.tv_nsec;
#else
    const qint64 modifiedNanoseconds =
        static_cast<qint64>(sourceStat.st_mtim.tv_sec) * 1000000000 +
        sourceStat.st_mtim.tv_nsec;
    const qint64 changedNanoseconds =
        static_cast<qint64>(sourceStat.st_ctim.tv_sec) * 1000000000 +
        sourceStat.st_ctim.tv_nsec;
#endif
#endif
    QCryptographicHash headTail(QCryptographicHash::Sha256);
    QCryptographicHash middle(QCryptographicHash::Sha256);
    for (QCryptographicHash* digest : {&headTail, &middle}) {
        digest->addData(QByteArray::number(info.size()));
        digest->addData(QByteArray(1, '\0'));
    }
    headTail.addData(source.read(kIdentityHashBytes));
    if (info.size() > kIdentityHashBytes) {
        if (!source.seek(qMax<qint64>(0, info.size() - kIdentityHashBytes))) {
            return {};
        }
        headTail.addData(source.read(kIdentityHashBytes));
    }
    if (!source.seek(qMax<qint64>(0, (info.size() - kIdentityHashBytes) / 2))) {
        return {};
    }
    middle.addData(source.read(kIdentityHashBytes));
    if (!source.seek(0)) {
        return {};
    }
    QCryptographicHash content(QCryptographicHash::Sha256);
    content.addData(source.readAll());
    return QJsonObject{
        {QStringLiteral("identity_schema"),
         QStringLiteral("jcut_source_content_identity_v1")},
        {QStringLiteral("cache_token_schema"),
         QStringLiteral("jcut_source_version_token_v1")},
        {QStringLiteral("path"), info.absoluteFilePath()},
        {QStringLiteral("size"), info.size()},
        {QStringLiteral("mtime_ns"),
         QString::number(modifiedNanoseconds)},
        {QStringLiteral("ctime_ns"),
         QString::number(changedNanoseconds)},
        {QStringLiteral("device"),
         QString::number(static_cast<qulonglong>(sourceStat.st_dev))},
        {QStringLiteral("inode"),
         QString::number(static_cast<qulonglong>(sourceStat.st_ino))},
        {QStringLiteral("head_tail_sha256"),
         QString::fromLatin1(headTail.result().toHex())},
        {QStringLiteral("middle_sha256"),
         QString::fromLatin1(middle.result().toHex())},
        {QStringLiteral("content_sha256"),
         QString::fromLatin1(content.result().toHex())},
        {QStringLiteral("hash_bytes_per_edge"), kIdentityHashBytes},
        {QStringLiteral("middle_hash_bytes"), kIdentityHashBytes},
    };
}

inline bool writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) > 0;
}

inline QString fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

inline bool writeSingleFrameMapMetadata(const QString& directory,
                                        const QString& sourcePath)
{
    const QString mapPath =
        QDir(directory).filePath(QStringLiteral("jcut_frame_map.tsv"));
    return writeJson(
        QDir(directory).filePath(QStringLiteral("jcut_frame_map.json")),
        QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("jcut_frame_index_map_v2")},
            {QStringLiteral("status"), QStringLiteral("ready")},
            {QStringLiteral("frame_domain"),
             QStringLiteral("source_timestamp_to_generated_ordinal")},
            {QStringLiteral("source_identity"), sourceIdentity(sourcePath)},
            {QStringLiteral("output_fps"), QJsonValue::Null},
            {QStringLiteral("map_file"), QStringLiteral("jcut_frame_map.tsv")},
            {QStringLiteral("map_sha256"), fileSha256(mapPath)},
            {QStringLiteral("mapped_frame_count"), 1},
            {QStringLiteral("min_source_frame"), 0},
            {QStringLiteral("max_source_frame"), 0},
            {QStringLiteral("max_mask_frame"), 0},
            {QStringLiteral("expected_output_frame_count"), 1},
        });
}

inline bool writeSingleFrameCompletion(const QString& directory,
                                       const QString& sourcePath,
                                       bool biRefNet,
                                       bool complete = true)
{
    const QString fileName = biRefNet
        ? QStringLiteral("jcut_alpha.json")
        : QStringLiteral("jcut_mask.json");
    return writeJson(
        QDir(directory).filePath(fileName),
        QJsonObject{
            {QStringLiteral("schema"), biRefNet
                 ? QStringLiteral("jcut_alpha_sidecar_v1")
                 : QStringLiteral("jcut_mask_sidecar_v1")},
            {QStringLiteral("complete"), complete},
            {QStringLiteral("source_type"), biRefNet
                 ? QStringLiteral("birefnet_continuous_alpha")
                 : QStringLiteral("sam3_binary_frames")},
            {QStringLiteral("frame_domain"), QStringLiteral("decode_ordinal")},
            {QStringLiteral("frame_index_map"), QStringLiteral("jcut_frame_map.tsv")},
            {QStringLiteral("frame_index_metadata"), QStringLiteral("jcut_frame_map.json")},
            {QStringLiteral("frame_map_sha256"), fileSha256(
                 QDir(directory).filePath(QStringLiteral("jcut_frame_map.tsv")))},
            {QStringLiteral("expected_frame_count"), 1},
            {QStringLiteral("source_identity"), sourceIdentity(sourcePath)},
        });
}

} // namespace mask_sidecar_test
