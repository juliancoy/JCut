#include "processing_job_docker.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace jcut::jobs {
namespace {

QString normalizedDockerName(QString name)
{
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('/'))) {
        name.remove(0, 1);
    }
    return name;
}

QString manifestJobRoot(const QJsonObject& manifest)
{
    const QString topLevel = manifest.value(QStringLiteral("job_root")).toString().trimmed();
    if (!topLevel.isEmpty()) {
        return QFileInfo(topLevel).absoluteFilePath();
    }
    const QString artifactRoot =
        manifest.value(QStringLiteral("artifacts")).toObject().value(QStringLiteral("job_root")).toString().trimmed();
    return artifactRoot.isEmpty() ? QString() : QFileInfo(artifactRoot).absoluteFilePath();
}

QString jobRootLeaf(const QString& jobRoot)
{
    return QFileInfo(jobRoot).fileName();
}

QString dockerLabel(const DockerContainerInfo& container, const QString& name)
{
    return container.labels.value(name).toString().trimmed();
}

QString jsonString(const QJsonObject& object, const QString& key)
{
    return object.value(key).toString().trimmed();
}

DockerContainerInfo containerFromJsonLine(const QByteArray& line)
{
    DockerContainerInfo info;
    const QJsonObject object = QJsonDocument::fromJson(line).object();
    info.id = jsonString(object, QStringLiteral("ID"));
    info.name = normalizedDockerName(jsonString(object, QStringLiteral("Names")));
    info.image = jsonString(object, QStringLiteral("Image"));
    info.status = jsonString(object, QStringLiteral("Status"));
    info.state = jsonString(object, QStringLiteral("State"));
    info.command = jsonString(object, QStringLiteral("Command"));
    return info;
}

void attachLabels(QVector<DockerContainerInfo>* containers)
{
    if (!containers || containers->isEmpty()) {
        return;
    }
    const QString docker = QStandardPaths::findExecutable(QStringLiteral("docker"));
    if (docker.isEmpty()) {
        return;
    }
    for (DockerContainerInfo& container : *containers) {
        const QString identifier = dockerContainerIdentifier(container);
        if (identifier.isEmpty()) {
            continue;
        }
        QProcess inspect;
        inspect.setProgram(docker);
        inspect.setArguments(QStringList{
            QStringLiteral("inspect"),
            QStringLiteral("--format"),
            QStringLiteral("{{json .}}"),
            identifier,
        });
        inspect.start();
        if (!inspect.waitForFinished(1500) ||
            inspect.exitStatus() != QProcess::NormalExit ||
            inspect.exitCode() != 0) {
            continue;
        }
        const QJsonObject inspected =
            QJsonDocument::fromJson(inspect.readAllStandardOutput().trimmed()).object();
        const QJsonObject config = inspected.value(QStringLiteral("Config")).toObject();
        const QJsonObject state = inspected.value(QStringLiteral("State")).toObject();
        container.labels = config.value(QStringLiteral("Labels")).toObject();
        const QJsonArray cmd = config.value(QStringLiteral("Cmd")).toArray();
        if (!cmd.isEmpty()) {
            QStringList parts;
            parts.reserve(cmd.size());
            for (const QJsonValue& value : cmd) {
                parts.push_back(value.toString());
            }
            container.command = parts.join(QLatin1Char(' '));
        }
        const QString inspectedImage = config.value(QStringLiteral("Image")).toString().trimmed();
        if (!inspectedImage.isEmpty()) {
            container.image = inspectedImage;
        }
        const QString inspectedState = state.value(QStringLiteral("Status")).toString().trimmed();
        if (!inspectedState.isEmpty()) {
            container.state = inspectedState;
        }
    }
}

} // namespace

QVector<DockerContainerInfo> listDockerContainers(QString* errorOut)
{
    if (errorOut) {
        errorOut->clear();
    }
    const QString docker = QStandardPaths::findExecutable(QStringLiteral("docker"));
    if (docker.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("docker was not found in PATH");
        }
        return {};
    }

    QProcess process;
    process.setProgram(docker);
    process.setArguments(QStringList{
        QStringLiteral("ps"),
        QStringLiteral("--format"),
        QStringLiteral("{{json .}}"),
    });
    process.start();
    if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished(500);
        if (errorOut) {
            *errorOut = QStringLiteral("timed out while listing Docker containers");
        }
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            *errorOut = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        }
        return {};
    }

    QVector<DockerContainerInfo> containers;
    const QList<QByteArray> lines = process.readAllStandardOutput().split('\n');
    containers.reserve(lines.size());
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const DockerContainerInfo info = containerFromJsonLine(line);
        if (!info.id.isEmpty() || !info.name.isEmpty()) {
            containers.push_back(info);
        }
    }
    attachLabels(&containers);
    return containers;
}

QString dockerContainerNameFromManifest(const QJsonObject& manifest)
{
    const QJsonObject process = manifest.value(QStringLiteral("process")).toObject();
    const QJsonObject docker = process.value(QStringLiteral("docker")).toObject();
    const QString nested = docker.value(QStringLiteral("container_name")).toString().trimmed();
    if (!nested.isEmpty()) {
        return normalizedDockerName(nested);
    }
    return normalizedDockerName(manifest.value(QStringLiteral("docker_container_name")).toString());
}

QString dockerContainerIdFromManifest(const QJsonObject& manifest)
{
    const QJsonObject process = manifest.value(QStringLiteral("process")).toObject();
    const QJsonObject docker = process.value(QStringLiteral("docker")).toObject();
    return docker.value(QStringLiteral("container_id")).toString().trimmed();
}

QString dockerContainerIdentifier(const DockerContainerInfo& container)
{
    return !container.name.isEmpty() ? container.name : container.id;
}

bool dockerContainerIsRunning(const DockerContainerInfo& container)
{
    return container.state.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0 ||
           container.status.startsWith(QStringLiteral("Up "), Qt::CaseInsensitive);
}

const DockerContainerInfo* findDockerContainerForManifest(
    const QJsonObject& manifest,
    const QVector<DockerContainerInfo>& containers)
{
    const QString manifestName = dockerContainerNameFromManifest(manifest);
    const QString manifestId = dockerContainerIdFromManifest(manifest);
    const QString jobRoot = manifestJobRoot(manifest);
    const QString leaf = jobRootLeaf(jobRoot);
    const QString operation = manifest.value(QStringLiteral("operation")).toString();

    for (const DockerContainerInfo& container : containers) {
        if (!manifestId.isEmpty() && container.id.startsWith(manifestId)) {
            return &container;
        }
        if (!manifestName.isEmpty() &&
            container.name.compare(manifestName, Qt::CaseInsensitive) == 0) {
            return &container;
        }
    }

    for (const DockerContainerInfo& container : containers) {
        const QString labelJobRoot = dockerLabel(container, QStringLiteral("jcut.job_root"));
        if (!jobRoot.isEmpty() &&
            !labelJobRoot.isEmpty() &&
            QFileInfo(labelJobRoot).absoluteFilePath() == jobRoot) {
            return &container;
        }
    }

    if (!leaf.isEmpty() && operation.compare(QStringLiteral("sam3"), Qt::CaseInsensitive) == 0) {
        for (const DockerContainerInfo& container : containers) {
            if (container.image.startsWith(QStringLiteral("sam3"), Qt::CaseInsensitive) &&
                container.command.contains(leaf)) {
                return &container;
            }
        }
    }

    return nullptr;
}

} // namespace jcut::jobs
