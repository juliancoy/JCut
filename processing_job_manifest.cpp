#include "processing_job_manifest.h"

#include "json_io_utils.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace jcut::jobs {

namespace {

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QJsonArray stringListToJson(const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values) {
        array.append(value);
    }
    return array;
}

} // namespace

QString sanitizedJobComponent(const QString& value) {
    QString out = value.trimmed();
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                QStringLiteral("_"));
    out.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    out = out.trimmed();
    if (out.isEmpty() || out == QStringLiteral(".") || out == QStringLiteral("..")) {
        return QStringLiteral("job");
    }
    return out.left(96);
}

QString defaultJobRootForInput(const QString& inputPath,
                               const QString& operation,
                               const QString& discriminator) {
    const QFileInfo inputInfo(inputPath);
    const QString stem = sanitizedJobComponent(inputInfo.completeBaseName());
    QString name = sanitizedJobComponent(operation);
    if (!discriminator.trimmed().isEmpty()) {
        name += QLatin1Char('_') + sanitizedJobComponent(discriminator);
    }
    return inputInfo.dir().absoluteFilePath(
        QStringLiteral(".jcut_jobs/%1_%2").arg(name, stem));
}

QString manifestPathForJobRoot(const QString& jobRoot) {
    return QDir(jobRoot).absoluteFilePath(QStringLiteral("manifest.json"));
}

QJsonObject fileIdentityObject(const QString& path) {
    const QFileInfo info(path);
    QJsonObject object{
        {QStringLiteral("path"), info.absoluteFilePath()},
        {QStringLiteral("exists"), info.exists()},
    };
    if (info.exists()) {
        object.insert(QStringLiteral("size"), static_cast<qint64>(info.size()));
        object.insert(QStringLiteral("modified_at_utc"),
                      info.lastModified().toUTC().toString(Qt::ISODate));
    }
    return object;
}

QJsonObject makeManifest(const QString& operation,
                         const QString& jobRoot,
                         const QString& inputPath,
                         const QJsonObject& parameters,
                         const QJsonObject& artifacts,
                         const QStringList& command) {
    const QString now = utcNow();
    return QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jcut_processing_job_v1")},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("status"), QStringLiteral("prepared")},
        {QStringLiteral("created_at_utc"), now},
        {QStringLiteral("updated_at_utc"), now},
        {QStringLiteral("job_root"), QFileInfo(jobRoot).absoluteFilePath()},
        {QStringLiteral("input"), fileIdentityObject(inputPath)},
        {QStringLiteral("parameters"), parameters},
        {QStringLiteral("artifacts"), artifacts},
        {QStringLiteral("command"), stringListToJson(command)},
    };
}

bool writeManifest(const QString& manifestPath,
                   QJsonObject manifest,
                   QString* errorOut) {
    QFileInfo info(manifestPath);
    if (!QDir().mkpath(info.dir().absolutePath())) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to create job directory: %1")
                            .arg(info.dir().absolutePath());
        }
        return false;
    }
    if (!manifest.contains(QStringLiteral("schema"))) {
        manifest.insert(QStringLiteral("schema"),
                        QStringLiteral("jcut_processing_job_v1"));
    }
    manifest.insert(QStringLiteral("updated_at_utc"), utcNow());
    return jcut::jsonio::writeJsonFile(manifestPath, manifest, true, errorOut);
}

bool updateManifestStatus(const QString& manifestPath,
                          const QString& status,
                          const QJsonObject& patch,
                          QString* errorOut) {
    QJsonObject manifest;
    readManifest(manifestPath, &manifest, nullptr);
    if (manifest.isEmpty()) {
        manifest.insert(QStringLiteral("schema"),
                        QStringLiteral("jcut_processing_job_v1"));
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        manifest.insert(it.key(), it.value());
    }
    manifest.insert(QStringLiteral("status"), status);
    return writeManifest(manifestPath, manifest, errorOut);
}

bool readManifest(const QString& manifestPath,
                  QJsonObject* manifestOut,
                  QString* errorOut) {
    if (manifestOut) {
        *manifestOut = QJsonObject();
    }
    return jcut::jsonio::readJsonFile(manifestPath, manifestOut, errorOut);
}

} // namespace jcut::jobs
