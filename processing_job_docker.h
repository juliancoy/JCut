#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace jcut::jobs {

struct DockerContainerInfo {
    QString id;
    QString name;
    QString image;
    QString status;
    QString state;
    QString command;
    QJsonObject labels;
};

QVector<DockerContainerInfo> listDockerContainers(QString* errorOut = nullptr);
QString dockerContainerNameFromManifest(const QJsonObject& manifest);
QString dockerContainerIdFromManifest(const QJsonObject& manifest);
QString dockerContainerIdentifier(const DockerContainerInfo& container);
const DockerContainerInfo* findDockerContainerForManifest(
    const QJsonObject& manifest,
    const QVector<DockerContainerInfo>& containers);
bool dockerContainerIsRunning(const DockerContainerInfo& container);

} // namespace jcut::jobs
