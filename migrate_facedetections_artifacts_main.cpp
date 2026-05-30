#include "facedetections_artifact_utils.h"
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

#include <functional>

namespace {
constexpr quint32 kLegacyBinaryJsonMagic = 0x4A435554;
constexpr quint32 kRecordBinaryJsonMagic = 0x4A465352;

struct ClipMigration {
    QString clipId;
    QString runId;
    QString artifactDir;
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

bool writeRecordArtifactFile(const QString& path,
                             const std::function<bool(QFile*)>& writeRecords,
                             QString* errorOut)
{
    const QString tempPath = path + QStringLiteral(".records-tmp");
    QFile::remove(tempPath);
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for writing.").arg(tempPath);
        }
        return false;
    }
    if (!writeRecords(&file)) {
        file.close();
        QFile::remove(tempPath);
        if (errorOut && errorOut->isEmpty()) {
            *errorOut = QStringLiteral("Failed to write record artifact %1.").arg(tempPath);
        }
        return false;
    }
    if (!file.flush()) {
        file.close();
        QFile::remove(tempPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to flush record artifact %1.").arg(tempPath);
        }
        return false;
    }
    file.close();
    QFile::remove(path);
    if (!QFile::rename(tempPath, path)) {
        QFile::remove(tempPath);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to replace %1 with converted record artifact.").arg(path);
        }
        return false;
    }
    return true;
}

bool appendRecord(QFile* file, const QJsonObject& object, QString* errorOut)
{
    return jcut::jsonio::appendBinaryJsonRecord(file, object, kRecordBinaryJsonMagic, 1, errorOut);
}

QJsonObject metaRecordForLegacyRoot(const QJsonObject& root,
                                    const QString& fallbackSchema,
                                    const QString& fallbackFrameDomain)
{
    QJsonObject meta{
        {QStringLiteral("type"), QStringLiteral("meta")},
        {QStringLiteral("schema"), root.value(QStringLiteral("schema")).toString(fallbackSchema)},
        {QStringLiteral("frame_domain"),
         root.value(QStringLiteral("frame_domain")).toString(fallbackFrameDomain)}
    };
    const QString video = root.value(QStringLiteral("video")).toString().trimmed();
    const QString backend = root.value(QStringLiteral("backend")).toString().trimmed();
    if (!video.isEmpty()) {
        meta[QStringLiteral("video")] = video;
    }
    if (!backend.isEmpty()) {
        meta[QStringLiteral("backend")] = backend;
    }
    return meta;
}

bool convertLegacyDetectionsArtifact(const QString& path,
                                     const QJsonObject& root,
                                     QString* errorOut)
{
    const QJsonArray frames = root.value(QStringLiteral("frames")).toArray();
    return writeRecordArtifactFile(path, [&](QFile* file) {
        if (!appendRecord(file,
                          metaRecordForLegacyRoot(
                              root,
                              QStringLiteral("jcut_facedetections_offscreen_detections_v1"),
                              QStringLiteral("source_absolute")),
                          errorOut)) {
            return false;
        }
        for (const QJsonValue& value : frames) {
            QJsonObject frame = value.toObject();
            frame[QStringLiteral("type")] = QStringLiteral("frame");
            if (!appendRecord(file, frame, errorOut)) {
                return false;
            }
        }
        return true;
    }, errorOut);
}

bool convertLegacyTracksArtifact(const QString& path,
                                 const QJsonObject& root,
                                 QString* errorOut)
{
    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    QJsonArray frameSummaries = root.value(QStringLiteral("frame_summaries")).toArray();
    if (frameSummaries.isEmpty()) {
        frameSummaries = root.value(QStringLiteral("frames")).toArray();
    }
    return writeRecordArtifactFile(path, [&](QFile* file) {
        if (!appendRecord(file,
                          metaRecordForLegacyRoot(
                              root,
                              QStringLiteral("jcut_facedetections_offscreen_tracks_v1"),
                              QStringLiteral("source_absolute")),
                          errorOut)) {
            return false;
        }
        for (const QJsonValue& value : tracks) {
            QJsonObject track = value.toObject();
            track[QStringLiteral("type")] = QStringLiteral("track");
            if (!appendRecord(file, track, errorOut)) {
                return false;
            }
        }
        for (const QJsonValue& value : frameSummaries) {
            QJsonObject frameSummary = value.toObject();
            frameSummary[QStringLiteral("type")] = QStringLiteral("frame_summary");
            if (!appendRecord(file, frameSummary, errorOut)) {
                return false;
            }
        }
        return true;
    }, errorOut);
}

bool convertLegacyArtifactIfNeeded(const QString& path,
                                   const QString& kind,
                                   const QString& stamp,
                                   bool dryRun,
                                   bool backup,
                                   QString* errorOut)
{
    if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
        return true;
    }
    const quint32 magic = binaryArtifactMagicAtPath(path);
    if (magic == kRecordBinaryJsonMagic) {
        out() << "Already record artifact: " << path << "\n";
        return true;
    }
    if (magic != kLegacyBinaryJsonMagic) {
        if (errorOut) {
            *errorOut = QStringLiteral("Unsupported %1 artifact format: %2").arg(kind, path);
        }
        return false;
    }
    out() << "Will convert legacy " << kind << " artifact to record format: " << path << "\n";
    if (dryRun) {
        return true;
    }

    QJsonObject root;
    if (!jcut::jsonio::readBinaryJsonObject(path, &root, kLegacyBinaryJsonMagic, 1, errorOut)) {
        return false;
    }
    if (backup && !backupExistingFile(path, stamp, errorOut)) {
        return false;
    }
    const bool ok = kind == QStringLiteral("detections")
        ? convertLegacyDetectionsArtifact(path, root, errorOut)
        : convertLegacyTracksArtifact(path, root, errorOut);
    if (ok) {
        out() << "Converted " << path << "\n";
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
        migration.tracksPath = tracks.absoluteFilePath();
        const QFileInfo detections(
            artifactDir.filePath(QStringLiteral("detections.bin")));
        if (detections.exists() && detections.isFile()) {
            migration.detectionsPath = detections.absoluteFilePath();
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
        QStringLiteral("Migrate generated FaceDetections record artifacts into compact canonical sidecars."));
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
        err() << "No generated FaceDetections record artifacts found for "
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
        if (!convertLegacyArtifactIfNeeded(migration.tracksPath,
                                           QStringLiteral("tracks"),
                                           stamp,
                                           dryRun,
                                           backup,
                                           &error) ||
            !convertLegacyArtifactIfNeeded(migration.detectionsPath,
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
              << " from " << migration.tracksPath << "\n";
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
