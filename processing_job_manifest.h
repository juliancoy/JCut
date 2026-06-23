#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace jcut::jobs {

QString sanitizedJobComponent(const QString& value);
QString defaultJobRootForInput(const QString& inputPath,
                               const QString& operation,
                               const QString& discriminator = QString());
QString manifestPathForJobRoot(const QString& jobRoot);
QJsonObject fileIdentityObject(const QString& path);

QJsonObject makeManifest(const QString& operation,
                         const QString& jobRoot,
                         const QString& inputPath,
                         const QJsonObject& parameters,
                         const QJsonObject& artifacts = QJsonObject(),
                         const QStringList& command = QStringList());

bool writeManifest(const QString& manifestPath,
                   QJsonObject manifest,
                   QString* errorOut = nullptr);
bool updateManifestStatus(const QString& manifestPath,
                          const QString& status,
                          const QJsonObject& patch = QJsonObject(),
                          QString* errorOut = nullptr);
bool readManifest(const QString& manifestPath,
                  QJsonObject* manifestOut,
                  QString* errorOut = nullptr);

} // namespace jcut::jobs
