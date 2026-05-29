#include "facedetections_artifact_utils.h"
#include "transcript_engine.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>

namespace {

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

    QJsonObject rawByClip;
    QJsonObject processedByClip;
    for (const ClipMigration& migration : migrations) {
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

    if (parser.isSet(dryRunOption)) {
        out() << "Dry run complete. Would write:\n"
              << "  " << rawSidecarPath << "\n"
              << "  " << processedSidecarPath << "\n";
        return 0;
    }

    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss"));
    QString error;
    if (!parser.isSet(noBackupOption)) {
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
