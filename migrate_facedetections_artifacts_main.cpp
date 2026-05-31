#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "json_io_utils.h"
#include "transcript_engine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

#include <limits>

namespace {
constexpr quint32 kLegacyBinaryJsonMagic = 0x4A435554;
constexpr quint32 kRecordBinaryJsonMagic = 0x4A465352;
constexpr quint32 kRecordBinaryCborMagic = 0x4A465342;

struct ClipMigration {
    QString clipId;
    QString runId;
    QString artifactDir;
    QString sourceTracksPath;
    QString sourceDetectionsPath;
    QString tracksPath;
    QString detectionsPath;
    QString continuityPath;
};

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream& err()
{
    static QTextStream stream(stderr);
    return stream;
}

QString backupPathFor(const QString& path, const QString& stamp)
{
    return path + QStringLiteral(".bak-") + stamp;
}

bool backupExistingFile(const QString& path, const QString& stamp, QString* errorOut)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return true;
    }
    const QString backupPath = backupPathFor(path, stamp);
    if (QFileInfo::exists(backupPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Backup already exists: %1").arg(backupPath);
        }
        return false;
    }
    if (!QFile::rename(path, backupPath)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to rename %1 to %2").arg(path, backupPath);
        }
        return false;
    }
    out() << "Backed up " << path << " -> " << backupPath << "\n";
    return true;
}

quint32 binaryArtifactMagicAtPath(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    stream >> magic;
    return stream.status() == QDataStream::Ok ? magic : 0;
}

QString dataPathForIndexPath(const QString& indexPath)
{
    QFileInfo info(indexPath);
    return info.dir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".dat"));
}

bool writeIndexedRecords(const QString& indexPath,
                         const QString& kind,
                         const QVector<QJsonObject>& records,
                         QString* errorOut)
{
    QJsonArray tracks;
    QJsonArray frameSummaries;
    QJsonArray frames;
    QString video;
    QString backend;
    for (QJsonObject record : records) {
        const QString type = record.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("meta")) {
            video = record.value(QStringLiteral("video")).toString(video);
            backend = record.value(QStringLiteral("backend")).toString(backend);
            continue;
        }
        record.remove(QStringLiteral("type"));
        if (type == QStringLiteral("track")) {
            tracks.push_back(record);
        } else if (type == QStringLiteral("frame_summary")) {
            frameSummaries.push_back(record);
        } else if (type == QStringLiteral("frame")) {
            frames.push_back(record);
        }
    }
    const QString dataPath = dataPathForIndexPath(indexPath);
    return kind == QStringLiteral("tracks")
        ? jcut::facedetections::writeIndexedTrackArtifact(
              indexPath, dataPath, video, backend, tracks, frameSummaries, errorOut)
        : jcut::facedetections::writeIndexedFrameArtifact(
              indexPath, dataPath, video, backend, frames, errorOut);
}

bool writeIndexedRoot(const QString& indexPath,
                      const QString& kind,
                      const QJsonObject& root,
                      QString* errorOut)
{
    const QString dataPath = dataPathForIndexPath(indexPath);
    const QString video = root.value(QStringLiteral("video")).toString();
    const QString backend = root.value(QStringLiteral("backend")).toString();
    return kind == QStringLiteral("tracks")
        ? jcut::facedetections::writeIndexedTrackArtifact(
              indexPath,
              dataPath,
              video,
              backend,
              root.value(QStringLiteral("tracks")).toArray(),
              root.value(QStringLiteral("frame_summaries")).toArray(),
              errorOut)
        : jcut::facedetections::writeIndexedFrameArtifact(
              indexPath,
              dataPath,
              video,
              backend,
              root.value(QStringLiteral("frames")).toArray(),
              errorOut);
}

bool readLegacyJsonRecords(const QString& path, QVector<QJsonObject>* recordsOut, QString* errorOut)
{
    if (recordsOut) {
        recordsOut->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1").arg(path);
        }
        return false;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    QVector<QJsonObject> records;
    while (!stream.atEnd()) {
        quint32 magic = 0;
        quint32 version = 0;
        quint32 compressedSize = 0;
        stream >> magic;
        stream >> version;
        stream >> compressedSize;
        if (stream.status() != QDataStream::Ok ||
            magic != kRecordBinaryJsonMagic ||
            version != 1 ||
            compressedSize > static_cast<quint32>(std::numeric_limits<int>::max())) {
            if (errorOut) {
                *errorOut = QStringLiteral("Invalid legacy record artifact header in %1").arg(path);
            }
            return false;
        }

        QByteArray compressed(static_cast<int>(compressedSize), Qt::Uninitialized);
        if (compressedSize > 0 &&
            stream.readRawData(compressed.data(), static_cast<int>(compressedSize)) !=
                static_cast<int>(compressedSize)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Truncated legacy record artifact in %1").arg(path);
            }
            return false;
        }

        QJsonObject object;
        if (!jcut::jsonio::parseObjectBytes(qUncompress(compressed), &object, errorOut)) {
            return false;
        }
        records.push_back(object);
    }

    if (recordsOut) {
        *recordsOut = records;
    }
    return true;
}

bool convertArtifactToIndexedIfNeeded(const QString& sourcePath,
                                   const QString& targetIndexPath,
                                   const QString& kind,
                                   const QString& stamp,
                                   bool dryRun,
                                   bool backup,
                                   QString* errorOut)
{
    if (sourcePath.trimmed().isEmpty() || !QFileInfo::exists(sourcePath)) {
        return true;
    }
    if (QFileInfo::exists(targetIndexPath) && binaryArtifactMagicAtPath(targetIndexPath) != 0) {
        out() << "Already indexed " << kind << " artifact: " << targetIndexPath << "\n";
        return true;
    }
    const quint32 magic = binaryArtifactMagicAtPath(sourcePath);
    if (magic == kRecordBinaryCborMagic) {
        out() << "Will convert CBOR record " << kind << " artifact to indexed shape: "
              << sourcePath << " -> " << targetIndexPath << "\n";
        if (dryRun) {
            return true;
        }
        QVector<QJsonObject> records;
        if (!jcut::jsonio::readBinaryCborRecords(sourcePath, &records, kRecordBinaryCborMagic, 1, errorOut)) {
            return false;
        }
        if (backup && !backupExistingFile(sourcePath, stamp, errorOut)) {
            return false;
        }
        return writeIndexedRecords(targetIndexPath, kind, records, errorOut);
    }
    if (magic == kRecordBinaryJsonMagic) {
        out() << "Will convert JSON record " << kind << " artifact to indexed shape: "
              << sourcePath << " -> " << targetIndexPath << "\n";
        if (dryRun) {
            return true;
        }
        QVector<QJsonObject> records;
        if (!readLegacyJsonRecords(sourcePath, &records, errorOut)) {
            return false;
        }
        if (backup && !backupExistingFile(sourcePath, stamp, errorOut)) {
            return false;
        }
        return writeIndexedRecords(targetIndexPath, kind, records, errorOut);
    }
    if (magic != kLegacyBinaryJsonMagic) {
        if (errorOut) {
            *errorOut = QStringLiteral("Unsupported %1 artifact format: %2").arg(kind, sourcePath);
        }
        return false;
    }
    out() << "Will convert monolithic " << kind << " artifact to indexed shape: "
          << sourcePath << " -> " << targetIndexPath << "\n";
    if (dryRun) {
        return true;
    }

    QJsonObject root;
    if (!jcut::jsonio::readBinaryJsonObject(sourcePath, &root, kLegacyBinaryJsonMagic, 1, errorOut)) {
        return false;
    }
    if (backup && !backupExistingFile(sourcePath, stamp, errorOut)) {
        return false;
    }
    const bool ok = writeIndexedRoot(targetIndexPath, kind, root, errorOut);
    if (ok) {
        out() << "Converted " << targetIndexPath << "\n";
    }
    return ok;
}

QFileInfo latestRunTracksArtifact(const QDir& clipDir)
{
    const QFileInfoList runs =
        clipDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo& run : runs) {
        const QFileInfo tracks(QDir(run.absoluteFilePath())
                                   .filePath(QStringLiteral("facedetections_artifact/tracks.bin")));
        if (tracks.exists() && tracks.isFile()) {
            return tracks;
        }
    }
    return {};
}

QVector<ClipMigration> discoverMigrations(const QString& transcriptPath,
                                          const QString& onlyClipId)
{
    QVector<ClipMigration> migrations;
    const QFileInfo transcriptInfo(transcriptPath);
    const QDir speakerFlowRoot(
        transcriptInfo.dir().filePath(QStringLiteral("debug/speaker_flow")));
    if (!speakerFlowRoot.exists()) {
        return migrations;
    }

    QFileInfoList clipDirs;
    if (!onlyClipId.trimmed().isEmpty()) {
        const QFileInfo clipDir(speakerFlowRoot.filePath(onlyClipId.trimmed()));
        if (clipDir.exists() && clipDir.isDir()) {
            clipDirs.push_back(clipDir);
        }
    } else {
        clipDirs = speakerFlowRoot.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    }

    for (const QFileInfo& clipDirInfo : clipDirs) {
        const QFileInfo tracks = latestRunTracksArtifact(QDir(clipDirInfo.absoluteFilePath()));
        if (!tracks.exists() || !tracks.isFile()) {
            continue;
        }
        const QDir artifactDir = tracks.dir();
        const QFileInfo runDir(artifactDir.absoluteFilePath(QStringLiteral("..")));
        ClipMigration migration;
        migration.clipId = clipDirInfo.fileName();
        migration.runId = runDir.canonicalFilePath().isEmpty()
            ? runDir.fileName()
            : QFileInfo(runDir.canonicalFilePath()).fileName();
        migration.artifactDir = artifactDir.absolutePath();
        migration.sourceTracksPath = tracks.absoluteFilePath();
        migration.tracksPath = artifactDir.filePath(QStringLiteral("tracks.idx"));
        const QFileInfo detections(
            artifactDir.filePath(QStringLiteral("detections.bin")));
        if (detections.exists() && detections.isFile()) {
            migration.sourceDetectionsPath = detections.absoluteFilePath();
            migration.detectionsPath = artifactDir.filePath(QStringLiteral("detections.idx"));
        }
        const QFileInfo continuity(
            artifactDir.filePath(QStringLiteral("continuity_facedetections.bin")));
        if (continuity.exists() && continuity.isFile()) {
            migration.continuityPath = continuity.absoluteFilePath();
        }
        migrations.push_back(migration);
    }
    return migrations;
}

QJsonObject continuityRootForMigration(const ClipMigration& migration)
{
    QJsonObject root;
    root[QStringLiteral("clip_id")] = migration.clipId;
    root[QStringLiteral("run_id")] = migration.runId;
    root[QStringLiteral("updated_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("only_dialogue")] = false;
    root[QStringLiteral("raw_tracks_artifact_path")] = migration.tracksPath;
    root[QStringLiteral("raw_tracks_count")] = -1;
    root[QStringLiteral("raw_tracks_schema")] =
        QStringLiteral("jcut_facedetections_offscreen_tracks_v1");
    root[QStringLiteral("raw_tracks_frame_domain")] =
        facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    root[QStringLiteral("streams_frame_domain")] =
        facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    root[QStringLiteral("imported_from_artifact_dir")] = migration.artifactDir;
    if (!migration.detectionsPath.isEmpty()) {
        root[QStringLiteral("raw_frames_artifact_path")] = migration.detectionsPath;
        root[QStringLiteral("raw_frames_count")] = -1;
        root[QStringLiteral("raw_frames_schema")] =
            QStringLiteral("jcut_facedetections_offscreen_detections_v1");
        root[QStringLiteral("raw_frames_frame_domain")] =
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    }
    if (!migration.continuityPath.isEmpty()) {
        root[QStringLiteral("continuity_artifact_path")] = migration.continuityPath;
    }
    return root;
}

QJsonObject processedRootForMigration(const QJsonObject& rawRoot,
                                      const QString& rawSidecarPath)
{
    QJsonObject root = rawRoot;
    root[QStringLiteral("source_raw_artifact_path")] = rawSidecarPath;
    const QFileInfo rawInfo(rawSidecarPath);
    if (rawInfo.exists()) {
        root[QStringLiteral("source_raw_artifact_mtime_ms")] =
            rawInfo.lastModified().toMSecsSinceEpoch();
    }
    return root;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("jcut_migrate_facedetections_artifacts"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Migrate generated FaceDetections artifacts to indexed .idx/.dat storage."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("transcript"),
                                 QStringLiteral("Transcript JSON path to migrate."));
    const QCommandLineOption clipOption(
        QStringList{QStringLiteral("clip")},
        QStringLiteral("Migrate only one clip id."),
        QStringLiteral("clip_id"));
    const QCommandLineOption dryRunOption(
        QStringLiteral("dry-run"),
        QStringLiteral("Print planned changes without writing sidecars."));
    const QCommandLineOption noBackupOption(
        QStringLiteral("no-backup"),
        QStringLiteral("Replace existing sidecars without creating .bak files."));
    parser.addOption(clipOption);
    parser.addOption(dryRunOption);
    parser.addOption(noBackupOption);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.size() != 1) {
        parser.showHelp(2);
    }

    const QString transcriptPath = QFileInfo(args.at(0)).absoluteFilePath();
    if (!QFileInfo::exists(transcriptPath)) {
        err() << "Transcript does not exist: " << transcriptPath << "\n";
        return 2;
    }

    const QVector<ClipMigration> migrations =
        discoverMigrations(transcriptPath, parser.value(clipOption));
    if (migrations.isEmpty()) {
        err() << "No generated FaceDetections artifacts found for "
              << transcriptPath << "\n";
        return 3;
    }

    editor::TranscriptEngine engine;
    const QString rawSidecarPath = engine.facedetectionsArtifactPath(transcriptPath);
    const QString processedSidecarPath =
        engine.facedetectionsProcessedArtifactPath(transcriptPath);
    const bool dryRun = parser.isSet(dryRunOption);
    const bool backup = !parser.isSet(noBackupOption);
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss"));
    QString error;

    QJsonObject rawByClip;
    QJsonObject processedByClip;
    for (const ClipMigration& migration : migrations) {
        if (!convertArtifactToIndexedIfNeeded(migration.sourceTracksPath,
                                              migration.tracksPath,
                                              QStringLiteral("tracks"),
                                              stamp,
                                              dryRun,
                                              backup,
                                              &error) ||
            !convertArtifactToIndexedIfNeeded(migration.sourceDetectionsPath,
                                              migration.detectionsPath,
                                              QStringLiteral("detections"),
                                              stamp,
                                              dryRun,
                                              backup,
                                              &error)) {
            err() << error << "\n";
            return 4;
        }
        const QJsonObject rawRoot = continuityRootForMigration(migration);
        rawByClip[migration.clipId] = rawRoot;
        processedByClip[migration.clipId] =
            processedRootForMigration(rawRoot, rawSidecarPath);
        out() << "Will migrate clip " << migration.clipId
              << " from " << migration.sourceTracksPath << "\n";
    }

    QJsonObject rawArtifact;
    rawArtifact[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_v1");
    rawArtifact[QStringLiteral("updated_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    setContinuityFacestreamsByClipObject(&rawArtifact, rawByClip);

    QJsonObject processedArtifact;
    processedArtifact[QStringLiteral("schema")] =
        QStringLiteral("jcut_facedetections_processed_v1");
    processedArtifact[QStringLiteral("updated_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    setContinuityFacestreamsByClipObject(&processedArtifact, processedByClip);

    if (dryRun) {
        out() << "Dry run complete. Would write:\n"
              << "  " << rawSidecarPath << "\n"
              << "  " << processedSidecarPath << "\n";
        return 0;
    }

    if (backup) {
        if (!backupExistingFile(rawSidecarPath, stamp, &error) ||
            !backupExistingFile(processedSidecarPath, stamp, &error)) {
            err() << error << "\n";
            return 4;
        }
    }

    if (!engine.saveFacestreamArtifact(transcriptPath, rawArtifact)) {
        err() << "Failed to write " << rawSidecarPath << "\n";
        return 5;
    }
    if (!engine.saveFacestreamProcessedArtifact(transcriptPath, processedArtifact)) {
        err() << "Failed to write " << processedSidecarPath << "\n";
        return 6;
    }

    out() << "Migrated " << migrations.size() << " clip(s).\n"
          << "Wrote " << rawSidecarPath << "\n"
          << "Wrote " << processedSidecarPath << "\n";
    return 0;
}
