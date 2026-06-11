#include "speakers_tab.h"
#include "speaker_flow_debug.h"

#include "clip_serialization.h"
#include "facedetections_artifact_utils.h"
#include "facedetections_runtime.h"
#include "detector_settings.h"
#include "editor_shared.h"
#include "json_io_utils.h"
#include "render_internal.h"
#include "transcript_engine.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#endif

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#endif

using namespace jcut::facedetections;

namespace {
bool uiAutomationEnabled()
{
    return qEnvironmentVariableIntValue("JCUT_UI_AUTOMATION") > 0;
}

void showAutomationAwareWarning(const QString& title, const QString& message)
{
    if (uiAutomationEnabled()) {
        qWarning().noquote() << title << ":" << message;
        return;
    }
    QMessageBox::warning(nullptr, title, message);
}

void showAutomationAwareInfo(const QString& title, const QString& message)
{
    if (uiAutomationEnabled()) {
        qInfo().noquote() << title << ":" << message;
        return;
    }
    QMessageBox::information(nullptr, title, message);
}

QString faceStreamSourceMediaPath(const TimelineClip& clip)
{
    const QString sourcePath = clip.filePath.trimmed();
    const QFileInfo sourceInfo(sourcePath);
    if (!sourcePath.isEmpty() &&
        sourceInfo.exists() &&
        (sourceInfo.isFile() || isImageSequencePath(sourcePath))) {
        return sourcePath;
    }
    return {};
}

QString faceStreamProxyMediaPath(const TimelineClip& clip)
{
    const QString proxyPath = playbackProxyPathForClip(clip);
    const QFileInfo proxyInfo(proxyPath);
    if (!proxyPath.trimmed().isEmpty() &&
        proxyInfo.exists() &&
        (proxyInfo.isFile() || isImageSequencePath(proxyPath))) {
        return proxyPath;
    }
    return {};
}

QString resolveMediaPathForFaceDetections(const TimelineClip& clip, bool useProxySource)
{
    return useProxySource ? faceStreamProxyMediaPath(clip) : faceStreamSourceMediaPath(clip);
}

bool readJsonObject(const QString& path, QJsonObject* objectOut, QString* errorOut = nullptr)
{
    return jcut::jsonio::readJsonFile(path, objectOut, errorOut);
}

QJsonObject existingFaceDetectionsRequest(const QString& requestPath)
{
    QJsonObject request;
    readJsonObject(requestPath, &request, nullptr);
    return request;
}

QJsonObject existingFaceDetectionsLaunchControl(const QString& artifactDir)
{
    QJsonObject control;
    readJsonObject(QDir(artifactDir).absoluteFilePath(QStringLiteral("launch_control.json")),
                   &control,
                   nullptr);
    return control;
}

QString normalizedFaceDetectionsLaunchMode(const QJsonObject& launchControl)
{
    const QString mode =
        launchControl.value(QStringLiteral("mode")).toString(QStringLiteral("auto")).trimmed().toLower();
    if (mode == QStringLiteral("fixed") ||
        mode == QStringLiteral("benchmark") ||
        mode == QStringLiteral("auto")) {
        return mode;
    }
    return QStringLiteral("auto");
}

QString normalizedFaceDetectionsLaunchProfile(const QJsonObject& launchControl)
{
    const QString profile =
        launchControl.value(QStringLiteral("launch_profile"))
            .toString(QStringLiteral("interactive"))
            .trimmed()
            .toLower();
    if (profile == QStringLiteral("interactive") ||
        profile == QStringLiteral("throughput")) {
        return profile;
    }
    return QStringLiteral("interactive");
}

bool launchControlBool(const QJsonObject& launchControl,
                       const QString& key,
                       bool fallback)
{
    if (!launchControl.contains(key)) {
        return fallback;
    }
    const QJsonValue value = launchControl.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    const QString text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1") ||
        text == QStringLiteral("yes") || text == QStringLiteral("on")) {
        return true;
    }
    if (text == QStringLiteral("false") || text == QStringLiteral("0") ||
        text == QStringLiteral("no") || text == QStringLiteral("off")) {
        return false;
    }
    return fallback;
}

QString facedetectionsOffscreenExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName = QStringLiteral("jcut_vulkan_facedetections_offscreen");
    const QString candidate = QDir(appDir).absoluteFilePath(exeName);
    if (QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QString buildCandidate = QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("build/%1").arg(exeName));
    if (QFileInfo::exists(buildCandidate)) {
        return buildCandidate;
    }
    return candidate;
}

struct FaceDetectionsProcessResult {
    bool started = false;
    bool canceled = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QString standardOutput;
    QString standardError;
};

void writeDetachedGeneratorStatus(const QString& root,
                                  const QString& state,
                                  qint64 pid,
                                  int exitCode,
                                  const QString& program,
                                  const QStringList& args,
                                  const QString& note = QString())
{
    if (root.trimmed().isEmpty()) {
        return;
    }
    QDir().mkpath(root);
    const QDir dir(root);
    QJsonObject status;
    const QString statusPath = dir.absoluteFilePath(QStringLiteral("generator.status.json"));
    readJsonObject(statusPath, &status, nullptr);
    status[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_generator_status_v1");
    status[QStringLiteral("state")] = state;
    status[QStringLiteral("updated_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    status[QStringLiteral("artifact_dir")] = root;
    status[QStringLiteral("program")] = program;
    status[QStringLiteral("arguments")] = args.join(QLatin1Char(' '));
    status[QStringLiteral("stdout_log")] =
        dir.absoluteFilePath(QStringLiteral("generator.stdout.log"));
    status[QStringLiteral("stderr_log")] =
        dir.absoluteFilePath(QStringLiteral("generator.stderr.log"));
    status[QStringLiteral("exit_file")] =
        dir.absoluteFilePath(QStringLiteral("generator.exit"));
    if (!status.contains(QStringLiteral("started_at_utc")) ||
        state == QStringLiteral("running")) {
        status[QStringLiteral("started_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    }
    if (pid > 0) {
        status[QStringLiteral("pid")] = static_cast<qint64>(pid);
    } else {
        status[QStringLiteral("pid")] = QJsonValue(QJsonValue::Null);
    }
    if (exitCode >= 0) {
        status[QStringLiteral("exit_code")] = exitCode;
    } else {
        status.remove(QStringLiteral("exit_code"));
    }
    if (!note.trimmed().isEmpty()) {
        status[QStringLiteral("note")] = note.trimmed();
    }
    QString error;
    if (!jcut::jsonio::writeJsonFile(statusPath, status, true, &error)) {
        qWarning().noquote() << "Failed to write FaceDetections generator status:" << error;
    }
}

QString argumentValue(const QStringList& args, const QString& optionName)
{
    const int index = args.indexOf(optionName);
    if (index >= 0 && index + 1 < args.size()) {
        return args.at(index + 1);
    }
    return {};
}

QString durableProcessRoot(const QStringList& args)
{
    const QString outDir = argumentValue(args, QStringLiteral("--out-dir")).trimmed();
    if (!outDir.isEmpty()) {
        return outDir;
    }
    return QDir::temp().absoluteFilePath(QStringLiteral("jcut_facedetections_detached"));
}

bool requiredDetachedGeneratorArtifactsExist(const QString& root)
{
    if (root.trimmed().isEmpty()) {
        return false;
    }
    const QDir dir(root);
    const QStringList required{
        dir.absoluteFilePath(QStringLiteral("continuity_facedetections.bin")),
        dir.absoluteFilePath(QStringLiteral("detections.idx")),
        dir.absoluteFilePath(QStringLiteral("tracks.idx")),
        dir.absoluteFilePath(QStringLiteral("summary.json")),
    };
    for (const QString& path : required) {
        if (!QFileInfo::exists(path)) {
            return false;
        }
    }
    return true;
}

qint64 readGeneratorPidFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    bool ok = false;
    const qint64 pid = QString::fromUtf8(file.readAll()).trimmed().toLongLong(&ok);
    return ok ? pid : -1;
}

bool processExists(qint64 pid)
{
    if (pid <= 0) {
        return false;
    }
#if defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#elif defined(Q_OS_WIN)
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE,
                                static_cast<DWORD>(pid));
    if (!handle) {
        return false;
    }
    DWORD exitCode = 0;
    const bool running =
        GetExitCodeProcess(handle, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool faceDetectionsGeneratorAlreadyRunning(const QString& artifactDir, QString* messageOut)
{
    const QDir dir(artifactDir);
    const QString statusPath = dir.absoluteFilePath(QStringLiteral("generator.status.json"));
    const QString pidPath = dir.absoluteFilePath(QStringLiteral("generator.pid"));
    const QString exitPath = dir.absoluteFilePath(QStringLiteral("generator.exit"));
    QJsonObject status;
    readJsonObject(statusPath, &status, nullptr);
    qint64 pid = status.value(QStringLiteral("pid")).toVariant().toLongLong();
    if (pid <= 0) {
        pid = readGeneratorPidFile(pidPath);
    }
    if (pid > 0 && processExists(pid) && !QFileInfo::exists(exitPath)) {
        if (messageOut) {
            *messageOut =
                QStringLiteral("FaceDetections generation is already running for this clip.\n\nPID: %1\nArtifact directory: %2")
                    .arg(pid)
                    .arg(artifactDir);
        }
        return true;
    }
    const QFileInfo partInfo(dir.absoluteFilePath(QStringLiteral("facedetections.part")));
    const QFileInfo stdoutInfo(dir.absoluteFilePath(QStringLiteral("generator.stdout.log")));
    const qint64 partAgeMs = partInfo.exists()
        ? partInfo.lastModified().msecsTo(QDateTime::currentDateTime())
        : std::numeric_limits<qint64>::max();
    const qint64 stdoutAgeMs = stdoutInfo.exists()
        ? stdoutInfo.lastModified().msecsTo(QDateTime::currentDateTime())
        : std::numeric_limits<qint64>::max();
    if (!QFileInfo::exists(exitPath) && qMin(partAgeMs, stdoutAgeMs) < 5 * 60 * 1000) {
        if (messageOut) {
            *messageOut =
                QStringLiteral("FaceDetections generation appears active for this clip, but no reliable PID is available yet.\n\nArtifact directory: %1")
                    .arg(artifactDir);
        }
        return true;
    }
    return false;
}

FaceDetectionsProcessResult runDetachedDurableProcess(const QString& program,
                                                      const QStringList& args)
{
    FaceDetectionsProcessResult result;
    const QString root = durableProcessRoot(args);
    QDir().mkpath(root);
    const QDir dir(root);
    const QString pidPath = dir.absoluteFilePath(QStringLiteral("generator.pid"));
    const QString exitPath = dir.absoluteFilePath(QStringLiteral("generator.exit"));
    const QString stdoutPath = dir.absoluteFilePath(QStringLiteral("generator.stdout.log"));
    const QString stderrPath = dir.absoluteFilePath(QStringLiteral("generator.stderr.log"));

    QFile::remove(pidPath);
    QFile::remove(exitPath);
    QFile::remove(stdoutPath);
    QFile::remove(stderrPath);

    QProcess launcher;
    qint64 pid = -1;
    launcher.setProgram(program);
    launcher.setArguments(args);
    launcher.setWorkingDirectory(QFileInfo(program).absolutePath());
    launcher.setStandardOutputFile(stdoutPath, QIODevice::Truncate);
    launcher.setStandardErrorFile(stderrPath, QIODevice::Truncate);
    result.started = launcher.startDetached(&pid);
    if (!result.started) {
        result.started = false;
        result.standardError = launcher.errorString().trimmed().isEmpty()
            ? QStringLiteral("Failed to start detached FaceDetections generator.")
            : launcher.errorString();
        return result;
    }
    writeDetachedGeneratorStatus(root,
                                 QStringLiteral("running"),
                                 pid,
                                 -1,
                                 program,
                                 args,
                                 pid > 0
                                     ? QString()
                                     : QStringLiteral("Detached process started, but Qt did not return a process id."));
    if (pid > 0) {
        QFile pidFile(pidPath);
        if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pidFile.write(QByteArray::number(pid));
            pidFile.write("\n");
        }
    } else {
        QFile pidFile(pidPath);
        if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pidFile.write("unavailable\n");
        }
    }

    qint64 stdoutOffset = 0;
    qint64 stderrOffset = 0;
    qint64 lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    auto appendNewBytes = [](const QString& path, qint64* offset, QString* out) -> bool {
        QFile file(path);
        if (!offset || !out || !file.open(QIODevice::ReadOnly)) {
            return false;
        }
        if (*offset > file.size()) {
            *offset = 0;
        }
        file.seek(*offset);
        const QByteArray bytes = file.readAll();
        *offset = file.pos();
        if (!bytes.isEmpty()) {
            *out += QString::fromLocal8Bit(bytes);
            return true;
        }
        return false;
    };

    while (true) {
        const bool stdoutChanged = appendNewBytes(stdoutPath, &stdoutOffset, &result.standardOutput);
        const bool stderrChanged = appendNewBytes(stderrPath, &stderrOffset, &result.standardError);
        if (stdoutChanged || stderrChanged) {
            lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        }
        if (QFileInfo::exists(exitPath)) {
            QFile exitFile(exitPath);
            if (exitFile.open(QIODevice::ReadOnly)) {
                bool ok = false;
                const int code = QString::fromUtf8(exitFile.readAll()).trimmed().toInt(&ok);
                result.exitCode = ok ? code : -1;
            } else {
                result.exitCode = -1;
            }
            result.exitStatus = QProcess::NormalExit;
            break;
        }
        if (pid > 0 && !processExists(pid)) {
            result.exitStatus = QProcess::NormalExit;
            if (requiredDetachedGeneratorArtifactsExist(root)) {
                result.exitCode = 0;
                QFile exitFile(exitPath);
                if (exitFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    exitFile.write("0\n");
                }
                result.standardOutput += QStringLiteral(
                    "\nDetached FaceDetections generator finished final artifacts without writing generator.exit; recovered as successful completion.\n");
            } else {
                result.exitCode = -1;
                result.standardError += QStringLiteral(
                    "\nDetached FaceDetections generator exited without writing generator.exit.\n");
            }
            break;
        }
        if (pid <= 0 &&
            QDateTime::currentMSecsSinceEpoch() - lastActivityMs > 30 * 60 * 1000) {
            result.exitStatus = QProcess::NormalExit;
            if (requiredDetachedGeneratorArtifactsExist(root)) {
                result.exitCode = 0;
                QFile exitFile(exitPath);
                if (exitFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    exitFile.write("0\n");
                }
                result.standardOutput += QStringLiteral(
                    "\nDetached FaceDetections generator finished final artifacts without a PID or generator.exit; recovered as successful completion.\n");
            } else {
                result.exitCode = -1;
                result.standardError += QStringLiteral(
                    "\nDetached FaceDetections generator did not provide a PID, did not write generator.exit, and has not updated logs for 30 minutes.\n");
            }
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (QThread::currentThread()->isInterruptionRequested()) {
            result.canceled = true;
            result.exitCode = -1;
            result.exitStatus = QProcess::NormalExit;
            break;
        }
        QThread::msleep(100);
    }
    appendNewBytes(stdoutPath, &stdoutOffset, &result.standardOutput);
    appendNewBytes(stderrPath, &stderrOffset, &result.standardError);
    writeDetachedGeneratorStatus(root,
                                 result.exitCode == 0 ? QStringLiteral("completed")
                                                      : (result.canceled ? QStringLiteral("canceled")
                                                                         : QStringLiteral("failed")),
                                 pid,
                                 result.exitCode,
                                 program,
                                 args,
                                 result.standardError.trimmed());
    return result;
}

struct FaceDetectionsLaunchTopology {
    int detectorWorkers = 2;
    int detectorPipelineSlots = 2;
    QJsonObject benchmarkResult;
};

FaceDetectionsProcessResult runProcessWithEventLoop(const QString& program,
                                                    const QStringList& args,
                                                    QProcess::ProcessChannelMode channelMode)
{
    FaceDetectionsProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setProcessChannelMode(channelMode);

    QEventLoop loop;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &loop, [&]() {
        result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &loop, [&]() {
        result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
    });
    QObject::connect(&process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &loop,
                     [&](int exitCode, QProcess::ExitStatus exitStatus) {
        result.exitCode = exitCode;
        result.exitStatus = exitStatus;
        loop.quit();
    });
    QObject::connect(&process, &QProcess::errorOccurred, &loop, [&]() {
        if (!result.standardError.endsWith(QLatin1Char('\n')) &&
            !result.standardError.isEmpty()) {
            result.standardError += QLatin1Char('\n');
        }
        result.standardError += QStringLiteral("[process-error] %1\n").arg(process.errorString());
        if (process.state() == QProcess::NotRunning) {
            loop.quit();
        }
    });

    process.start();
    result.started = process.waitForStarted(5000);
    if (!result.started) {
        result.standardError += process.errorString();
        return result;
    }

    if (process.state() != QProcess::NotRunning) {
        loop.exec();
    } else {
        result.exitCode = process.exitCode();
        result.exitStatus = process.exitStatus();
    }
    result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
    result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}

QString benchmarkSlotCsv(const QJsonObject& launchControl)
{
    const QJsonArray configured = launchControl.value(QStringLiteral("benchmark_pipeline_slots")).toArray();
    QStringList parts;
    for (const QJsonValue& value : configured) {
        bool ok = false;
        const int slotValue = value.toVariant().toInt(&ok);
        if (ok && slotValue >= 1 && slotValue <= 10 && !parts.contains(QString::number(slotValue))) {
            parts.append(QString::number(slotValue));
        }
    }
    if (parts.isEmpty()) {
        parts = QStringList{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("8")};
    }
    return parts.join(QLatin1Char(','));
}

void setArgumentValue(QStringList* args, const QString& optionName, const QString& value)
{
    if (!args) {
        return;
    }
    const int optionIndex = args->indexOf(optionName);
    if (optionIndex >= 0 && optionIndex + 1 < args->size()) {
        (*args)[optionIndex + 1] = value;
        return;
    }
    args->append(optionName);
    args->append(value);
}

bool parsePipelineBenchmarkOutput(const QString& output,
                                  QJsonObject* benchmarkOut,
                                  QString* errorOut)
{
    const int prefixIndex = output.indexOf(QStringLiteral("pipeline_benchmark "));
    const int objectStart = prefixIndex >= 0
        ? output.indexOf(QLatin1Char('{'), prefixIndex)
        : output.indexOf(QLatin1Char('{'));
    const int objectEnd = output.lastIndexOf(QLatin1Char('}'));
    if (objectStart < 0 || objectEnd <= objectStart) {
        if (errorOut) {
            *errorOut = QStringLiteral("benchmark output did not contain a JSON result");
        }
        return false;
    }
    QString parseError;
    QJsonObject parsed;
    if (!jcut::jsonio::parseObjectBytes(output.mid(objectStart, objectEnd - objectStart + 1).toUtf8(),
                                        &parsed,
                                        &parseError)) {
        if (errorOut) {
            *errorOut = parseError;
        }
        return false;
    }
    if (benchmarkOut) {
        *benchmarkOut = parsed;
    }
    return true;
}

bool chooseBenchmarkedFaceDetectionsTopology(const QString& generatorProgram,
                                             const QStringList& baseArgs,
                                             const QJsonObject& launchControl,
                                             FaceDetectionsLaunchTopology* topology,
                                             QString* errorOut)
{
    if (!topology) {
        return false;
    }
    int benchmarkFrames =
        std::clamp(launchControl.value(QStringLiteral("benchmark_frames")).toInt(480), 60, 5000);
    const int requestedMaxFramesIndex = baseArgs.indexOf(QStringLiteral("--max-frames"));
    if (requestedMaxFramesIndex >= 0 && requestedMaxFramesIndex + 1 < baseArgs.size()) {
        bool ok = false;
        const int requestedMaxFrames = baseArgs.at(requestedMaxFramesIndex + 1).toInt(&ok);
        if (ok && requestedMaxFrames > 0) {
            benchmarkFrames = std::min(benchmarkFrames, requestedMaxFrames);
        }
    }
    QStringList benchmarkArgs = baseArgs;
    benchmarkArgs << QStringLiteral("--benchmark-pipeline-slots")
                  << benchmarkSlotCsv(launchControl);
    setArgumentValue(&benchmarkArgs, QStringLiteral("--max-frames"), QString::number(benchmarkFrames));

    const FaceDetectionsProcessResult benchmarkProcess =
        runProcessWithEventLoop(generatorProgram, benchmarkArgs, QProcess::MergedChannels);
    if (!benchmarkProcess.started) {
        if (errorOut) {
            *errorOut = benchmarkProcess.standardError;
        }
        return false;
    }
    const QString output = benchmarkProcess.standardOutput + benchmarkProcess.standardError;
    if (benchmarkProcess.exitStatus != QProcess::NormalExit || benchmarkProcess.exitCode != 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("benchmark process failed with exit code %1:\n%2")
                            .arg(benchmarkProcess.exitCode)
                            .arg(output.trimmed());
        }
        return false;
    }
    QJsonObject benchmark;
    if (!parsePipelineBenchmarkOutput(output, &benchmark, errorOut)) {
        return false;
    }
    const int bestWorkers = benchmark.value(QStringLiteral("best_detector_workers")).toInt(-1);
    const int bestSlots = benchmark.value(QStringLiteral("best_detector_pipeline_slots")).toInt(-1);
    if (bestWorkers < 1 || bestSlots < 1) {
        if (errorOut) {
            *errorOut = QStringLiteral("benchmark did not identify a valid worker/slot topology");
        }
        return false;
    }
    topology->detectorWorkers = std::clamp(bestWorkers, 1, 10);
    topology->detectorPipelineSlots = std::clamp(bestSlots, 1, 10);
    topology->benchmarkResult = benchmark;
    return true;
}

class ScopedAudioBackgroundDecodeSuppression {
public:
    explicit ScopedAudioBackgroundDecodeSuppression(const std::function<void(bool)>& setter)
        : m_setter(setter)
    {
        if (m_setter) {
            m_setter(true);
            m_active = true;
        }
    }

    ~ScopedAudioBackgroundDecodeSuppression()
    {
        if (m_active && m_setter) {
            m_setter(false);
        }
    }

    ScopedAudioBackgroundDecodeSuppression(const ScopedAudioBackgroundDecodeSuppression&) = delete;
    ScopedAudioBackgroundDecodeSuppression& operator=(const ScopedAudioBackgroundDecodeSuppression&) = delete;

private:
    std::function<void(bool)> m_setter;
    bool m_active = false;
};

class ScopedBoolReset {
public:
    explicit ScopedBoolReset(bool* value)
        : m_value(value)
    {
    }

    ~ScopedBoolReset()
    {
        if (m_value) {
            *m_value = false;
        }
    }

    ScopedBoolReset(const ScopedBoolReset&) = delete;
    ScopedBoolReset& operator=(const ScopedBoolReset&) = delete;

private:
    bool* m_value = nullptr;
};

FaceDetectionsProcessResult runFaceDetectionsGeneratorProcess(QWidget* parent,
                                                     const QString& program,
                                                     const QStringList& args,
                                                     bool livePreview)
{
    FaceDetectionsProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(QFileInfo(program).absolutePath());
    process.setProcessChannelMode(QProcess::SeparateChannels);

    if (uiAutomationEnabled()) {
        return runProcessWithEventLoop(program, args, QProcess::SeparateChannels);
    }

    if (livePreview) {
        return runDetachedDurableProcess(program, args);
    }

    QDialog progressDialog(parent);
    progressDialog.setWindowTitle(QStringLiteral("JCut DNN Detection + Continuity Generator"));
    progressDialog.setWindowFlag(Qt::Window, true);
    progressDialog.resize(760, 320);

    auto* progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(10, 10, 10, 10);
    progressLayout->setSpacing(8);

    auto* statusLabel = new QLabel(
        livePreview
            ? QStringLiteral("Running detection and continuity generation with live preview in a background process...")
            : QStringLiteral("Running detection and continuity generation headless in a background process..."),
        &progressDialog);
    statusLabel->setWordWrap(true);
    progressLayout->addWidget(statusLabel);

    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setRange(0, 0);
    progressLayout->addWidget(progressBar);

    auto* logView = new QPlainTextEdit(&progressDialog);
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(400);
    progressLayout->addWidget(logView, 1);

    auto appendLog = [logView](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        logView->moveCursor(QTextCursor::End);
        logView->insertPlainText(text);
        auto* bar = logView->verticalScrollBar();
        if (bar) {
            bar->setValue(bar->maximum());
        }
    };

    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch(1);
    auto* cancelRunButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    progressButtons->addWidget(cancelRunButton);
    progressLayout->addLayout(progressButtons);

    QObject::connect(cancelRunButton, &QPushButton::clicked, &progressDialog, [&]() {
        result.canceled = true;
        statusLabel->setText(QStringLiteral("Canceling detection and continuity generation..."));
        cancelRunButton->setEnabled(false);
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(1500)) {
                process.kill();
            }
        }
    });
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &progressDialog, [&]() {
        const QString chunk = QString::fromLocal8Bit(process.readAllStandardOutput());
        result.standardOutput += chunk;
        appendLog(chunk);
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &progressDialog, [&]() {
        const QString chunk = QString::fromLocal8Bit(process.readAllStandardError());
        result.standardError += chunk;
        appendLog(chunk);
    });
    QObject::connect(&process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &progressDialog,
                     [&](int exitCode, QProcess::ExitStatus exitStatus) {
        result.exitCode = exitCode;
        result.exitStatus = exitStatus;
        progressDialog.accept();
    });
    QObject::connect(&process, &QProcess::errorOccurred, &progressDialog, [&](QProcess::ProcessError error) {
        appendLog(QStringLiteral("\n[process-error] %1\n").arg(static_cast<int>(error)));
    });

    process.start();
    result.started = process.waitForStarted(5000);
    if (!result.started) {
        result.standardError += process.errorString();
        return result;
    }

    progressDialog.exec();
    if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(-1);
    }
    result.standardOutput += QString::fromLocal8Bit(process.readAllStandardOutput());
    result.standardError += QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}

} // namespace

void SpeakersTab::onSpeakerRunAutoTrackClicked()
{
    if (m_faceDetectionsGenerationInProgress) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("FaceDetections generation is already running for this editor instance."));
        return;
    }
    m_faceDetectionsGenerationInProgress = true;
    ScopedBoolReset resetGenerationInProgress(&m_faceDetectionsGenerationInProgress);

    if (!activeCutMutable() || !m_transcriptSession.hasObjectDocument()) {
        return;
    }
    const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!selectedClip) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"),
                                   QStringLiteral("Select a clip first."));
        return;
    }

    const QString sourceMediaPath = faceStreamSourceMediaPath(*selectedClip);
    const QString proxyMediaPath = faceStreamProxyMediaPath(*selectedClip);
    if (sourceMediaPath.isEmpty() && proxyMediaPath.isEmpty()) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"),
                                   QStringLiteral("No playable media was found for this clip."));
        return;
    }

    DetectorRuntimeSettings detectorSettings;
    const QString detectorSettingsPath = detectorSettingsPathForVideo(
        !sourceMediaPath.isEmpty() ? sourceMediaPath : proxyMediaPath);
    loadDetectorRuntimeSettingsFile(detectorSettingsPath, &detectorSettings, nullptr);
    const FacestreamSourceScanRange scanRange = facedetectionsSourceAbsoluteScanRangeForClip(*selectedClip);
    if (!scanRange.valid) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Cannot run FaceDetections: %1").arg(scanRange.error));
        return;
    }
    const int64_t startFrame = scanRange.startFrame;
    const int64_t sourceEndFrameExclusive = scanRange.endFrameExclusive;
    const int64_t maxFrames = scanRange.frameCount;

    const QString clipId = selectedClip->id.trimmed().isEmpty()
        ? QStringLiteral("unknown_clip")
        : selectedClip->id.trimmed();
    const QString artifactDir = facedetectionsClipSidecarDir(selectedClip->filePath, clipId);
    QDir().mkpath(artifactDir);
    QString runningGeneratorMessage;
    if (faceDetectionsGeneratorAlreadyRunning(artifactDir, &runningGeneratorMessage)) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            runningGeneratorMessage);
        return;
    }
    QJsonObject launchControl = existingFaceDetectionsLaunchControl(artifactDir);
    const bool explicitLaunchControl = !launchControl.isEmpty();
    const QString initialLaunchMode = normalizedFaceDetectionsLaunchMode(launchControl);
    const QString initialLaunchProfile = normalizedFaceDetectionsLaunchProfile(launchControl);
    const bool initialThroughputProfile = initialLaunchProfile == QStringLiteral("throughput");
    const bool initialLivePreview =
        launchControlBool(launchControl, QStringLiteral("live_preview"), true);
    const int preflightDetectorWorkersDefault = std::clamp(
        launchControl.contains(QStringLiteral("detector_workers"))
            ? launchControl.value(QStringLiteral("detector_workers")).toInt(2)
            : 2,
        1,
        10);

    const FaceDetectionsPreflightDialogResult preflight =
        runFaceDetectionsPreflightDialog(
            &detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            detectorSettings.scrfdTargetSize,
            detectorSettingsPath,
            FaceDetectionsPreflightDialogOptions{
                QStringLiteral("JCut DNN Detection + Continuity Generator"),
                QStringLiteral("This flow runs raw face detection, then forms identity-agnostic continuity tracks, then imports those artefacts for the selected clip.\n\n"
                               "Detector: SCRFD ncnn Vulkan only. CPU detector fallback is not used."),
                QStringLiteral("Input defaults to source media. Enable proxy input explicitly if you want detection and continuity generation to scan the proxy instead. "
                               "Artifact frame numbers are source-media absolute frames and do not depend on the current timeline FPS or playback clock. "
                               "Launch topology is selected by the saved benchmark unless you change the manual worker count here, which intentionally switches this clip to fixed topology. "
                               "Artifact: facedetections.part + tracks.idx/tracks.dat + detections.idx/detections.dat + continuity_facedetections.bin. Interrupted runs resume only when the input path still matches the checkpointed run."),
                QStringLiteral("Proceed"),
                QStringLiteral("Cancel"),
                QSize(760, 420),
                true,
                initialLivePreview,
                false,
                true,
                false,
                QStringLiteral("Apply selected clip grading during detection"),
                true,
                false,
                QStringLiteral("Restart from scratch (delete facedetections.part before launch)"),
                true,
                detectorSettings.useProxySource,
                QStringLiteral("Use proxy media as FaceDetections input"),
                true,
                preflightDetectorWorkersDefault,
                1,
                10,
                QStringLiteral("Manual detector workers")
            });
    if (!preflight.accepted) {
        return;
    }
    if (!preflight.saveError.trimmed().isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            preflight.saveError.isEmpty()
                ? QStringLiteral("Failed to save detector settings before launch.")
                : preflight.saveError);
        return;
    }

    const QString mediaPath = resolveMediaPathForFaceDetections(*selectedClip, preflight.useProxySource);
    if (mediaPath.isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            preflight.useProxySource
                ? QStringLiteral("Proxy media was requested for detection/continuity input, but no playable proxy media was found for this clip.")
                : QStringLiteral("No playable source media was found for this clip."));
        return;
    }

    auto debugRun = speaker_flow_debug::openLatestOrCreateRun(
        m_transcriptSession.transcriptPath(),
        selectedClip->id.trimmed().isEmpty() ? QStringLiteral("unknown_clip") : selectedClip->id,
        QFileInfo(mediaPath).completeBaseName());
    if (debugRun.projectRoot.trimmed().isEmpty() || debugRun.runDir.trimmed().isEmpty()) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Cannot create a detection/continuity debug run because the active transcript path is unavailable."));
        return;
    }

    const QDir initialRunDir(debugRun.runDir);
    const QString initialRequestPath = initialRunDir.absoluteFilePath(
        QStringLiteral("%1_facedetections_request.json").arg(debugRun.videoStem));
    const QJsonObject initialRequest = existingFaceDetectionsRequest(initialRequestPath);
    const QString previousMediaPath = initialRequest.value(QStringLiteral("media_path")).toString().trimmed();
    const QString currentMediaPath = QFileInfo(mediaPath).absoluteFilePath();
    const bool initialRunMediaMismatch =
        !previousMediaPath.isEmpty() &&
        QFileInfo(previousMediaPath).absoluteFilePath() != currentMediaPath;
    const QDir initialArtifactDir(artifactDir);
    const bool initialRunHasCheckpoint =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("facedetections.part")));
    const bool initialRunHasCompletedOutputs =
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("detections.idx"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("tracks.idx"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("continuity_facedetections.bin"))) ||
        QFileInfo::exists(initialArtifactDir.filePath(QStringLiteral("summary.json")));
    const bool shouldForceFreshRun =
        debugRun.reusedExistingRun &&
        (initialRunMediaMismatch ||
         (initialRunHasCompletedOutputs && !initialRunHasCheckpoint));
    if (shouldForceFreshRun) {
        debugRun = speaker_flow_debug::createNewRunFrom(debugRun);
    }
    const QString requestPath = QDir(debugRun.runDir).absoluteFilePath(
        QStringLiteral("%1_facedetections_request.json").arg(debugRun.videoStem));
    const QString outputPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("continuity_facedetections.bin"));
    const QString detectionsPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("detections.idx"));
    const QString tracksPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("tracks.idx"));
    const QString summaryPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("summary.json"));
    const QString facedetectionsPartPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("facedetections.part"));
    const QString facedetectionsResumeIndexPath =
        QDir(artifactDir).absoluteFilePath(QStringLiteral("facedetections.part.resume_index.json"));
    const QString clipJsonPath = QDir(artifactDir).absoluteFilePath(QStringLiteral("clip_input.json"));
    const QString indexPath = QDir(debugRun.runDir).absoluteFilePath(QStringLiteral("index.json"));

    if (preflight.restartFromScratch) {
        const QStringList restartCheckpointPaths{facedetectionsPartPath, facedetectionsResumeIndexPath};
        for (const QString& checkpointPath : restartCheckpointPaths) {
            if (QFileInfo::exists(checkpointPath) && !QFile::remove(checkpointPath)) {
                showAutomationAwareWarning(
                    QStringLiteral("JCut DNN Detection + Continuity Generator"),
                    QStringLiteral("Failed to delete restart checkpoint before launch.\n\n%1")
                        .arg(checkpointPath));
                return;
            }
        }
    }

    QString launchMode = initialLaunchMode;
    if (!explicitLaunchControl) {
        launchControl[QStringLiteral("schema")] = QStringLiteral("jcut_facedetections_launch_control_v1");
        launchControl[QStringLiteral("mode")] = launchMode;
        launchControl[QStringLiteral("launch_profile")] = QStringLiteral("interactive");
        launchControl[QStringLiteral("live_preview")] = true;
        launchControl[QStringLiteral("control_window")] = true;
        launchControl[QStringLiteral("progress_output")] = true;
        launchControl[QStringLiteral("benchmark_frames")] = 480;
        launchControl[QStringLiteral("runtime_mutable")] = false;
        launchControl[QStringLiteral("apply_scope")] = QStringLiteral("next_generator_launch");
        launchControl[QStringLiteral("editor_restart_required")] = false;
        launchControl[QStringLiteral("generator_relaunch_required")] = true;
        launchControl[QStringLiteral("requires_generator_relaunch")] = true;
        launchControl[QStringLiteral("auto_default")] = true;
    }
    bool preflightLaunchControlChanged = false;
    if (preflight.detectorWorkers != preflightDetectorWorkersDefault) {
        preflightLaunchControlChanged = true;
        launchMode = QStringLiteral("fixed");
        launchControl[QStringLiteral("mode")] = launchMode;
        launchControl[QStringLiteral("detector_workers")] = preflight.detectorWorkers;
        launchControl[QStringLiteral("detector_pipeline_slots")] = preflight.detectorWorkers;
        launchControl[QStringLiteral("updated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        launchControl[QStringLiteral("apply_scope")] = QStringLiteral("next_generator_launch");
        launchControl[QStringLiteral("editor_restart_required")] = false;
        launchControl[QStringLiteral("generator_relaunch_required")] = true;
        launchControl[QStringLiteral("requires_generator_relaunch")] = true;
        launchControl[QStringLiteral("note")] =
            QStringLiteral("Manual detector worker count changed in FaceDetections preflight; this clip is now fixed-topology for future generator launches.");
        QString launchControlWriteError;
        if (!jcut::jsonio::writeJsonFile(QDir(artifactDir).absoluteFilePath(QStringLiteral("launch_control.json")),
                                         launchControl,
                                         true,
                                         &launchControlWriteError)) {
            qWarning().noquote()
                << "Failed to persist FaceDetections preflight launch control:"
                << launchControlWriteError;
        }
    }
    const QString launchProfile = normalizedFaceDetectionsLaunchProfile(launchControl);
    const bool throughputProfile = launchProfile == QStringLiteral("throughput");
    const bool durableGeneratorLaunch = true;
    const bool launchLivePreview =
        throughputProfile
            ? launchControlBool(launchControl, QStringLiteral("live_preview"), false)
            : preflight.livePreview;
    const bool launchControlWindow =
        launchControlBool(launchControl, QStringLiteral("control_window"), !throughputProfile);
    const bool launchProgressOutput =
        durableGeneratorLaunch
            ? false
            : launchControlBool(launchControl, QStringLiteral("progress_output"), !throughputProfile);
    FaceDetectionsLaunchTopology launchTopology;
    launchTopology.detectorWorkers = std::clamp(
        launchControl.contains(QStringLiteral("detector_workers"))
            ? launchControl.value(QStringLiteral("detector_workers")).toInt(preflight.detectorWorkers)
            : preflight.detectorWorkers,
        1,
        10);
    launchTopology.detectorPipelineSlots = std::clamp(
        launchControl.contains(QStringLiteral("detector_pipeline_slots"))
            ? launchControl.value(QStringLiteral("detector_pipeline_slots")).toInt(launchTopology.detectorWorkers)
            : launchTopology.detectorWorkers,
        1,
        10);

    QStringList args{
        mediaPath,
        QStringLiteral("--detector"), QStringLiteral("scrfd-ncnn-vulkan"),
        QStringLiteral("--params-file"), detectorSettingsPath,
        QStringLiteral("--stride"), QString::number(qMax(1, detectorSettings.stride)),
        QStringLiteral("--threshold"), QString::number(detectorSettings.threshold, 'f', 4),
        QStringLiteral("--nms-iou"), QString::number(detectorSettings.nmsIouThreshold, 'f', 4),
        QStringLiteral("--track-match-iou"), QString::number(detectorSettings.trackMatchIouThreshold, 'f', 4),
        QStringLiteral("--new-track-min-confidence"), QString::number(detectorSettings.newTrackMinConfidence, 'f', 4),
        QStringLiteral("--max-faces-per-frame"), QString::number(qMax(0, detectorSettings.maxFacesPerFrame)),
        QStringLiteral("--max-detections"), QString::number(qMax(1, detectorSettings.maxDetections)),
        QStringLiteral("--scrfd-model"), normalizeScrfdModelVariantId(detectorSettings.scrfdModelVariant),
        QStringLiteral("--scrfd-target-size"), QString::number(qMax(320, detectorSettings.scrfdTargetSize)),
        QStringLiteral("--start-frame"), QString::number(startFrame),
        QStringLiteral("--quiet"),
        launchProgressOutput ? QStringLiteral("--progress") : QStringLiteral("--no-progress"),
        QStringLiteral("--detector-workers"), QString::number(launchTopology.detectorWorkers),
        QStringLiteral("--detector-pipeline-slots"), QString::number(launchTopology.detectorPipelineSlots),
        QStringLiteral("--out-dir"), artifactDir
    };
    args << (detectorSettings.primaryFaceOnly
                ? QStringLiteral("--primary-face-only")
                : QStringLiteral("--multi-face"));
    args << (detectorSettings.smallFaceFallback
                ? QStringLiteral("--small-face-fallback")
                : QStringLiteral("--no-small-face-fallback"));
    args << (detectorSettings.scrfdTiled
                ? QStringLiteral("--scrfd-tiling")
                : QStringLiteral("--no-scrfd-tiling"));
    args << QStringLiteral("--require-zero-copy");
    args << (launchControlWindow
                ? QStringLiteral("--control-window")
                : QStringLiteral("--no-control-window"));
    args << (launchLivePreview
                ? QStringLiteral("--preview-window")
                : QStringLiteral("--no-preview-window"));
    args << QStringLiteral("--no-preview-files");
    if (maxFrames > 0) {
        args << QStringLiteral("--max-frames") << QString::number(maxFrames);
    }
    if (preflight.applyClipGrading) {
        QString writeError;
        if (!jcut::jsonio::writeJsonFile(clipJsonPath, editor::clipToJson(*selectedClip), true, &writeError)) {
            showAutomationAwareWarning(
                QStringLiteral("JCut DNN FaceDetections Generator"),
                writeError.trimmed().isEmpty()
                    ? QStringLiteral("Failed to write clip grading input: %1").arg(clipJsonPath)
                    : writeError);
            return;
        }
        args << QStringLiteral("--clip-json") << clipJsonPath;
        args << QStringLiteral("--apply-clip-grading");
    }

    QJsonObject request;
    request[QStringLiteral("run_id")] = debugRun.runId;
    request[QStringLiteral("engine")] =
        QStringLiteral("jcut_vulkan_facedetections_offscreen_inprocess_scrfd_zero_copy_v1");
    request[QStringLiteral("execution_mode")] =
        QStringLiteral("inprocess_function");
    request[QStringLiteral("media_path")] = mediaPath;
    request[QStringLiteral("media_source_mode")] =
        preflight.useProxySource ? QStringLiteral("proxy") : QStringLiteral("source");
    request[QStringLiteral("clip_id")] = selectedClip->id;
    request[QStringLiteral("source_start_frame")] = static_cast<qint64>(startFrame);
    request[QStringLiteral("source_end_frame_exclusive")] = static_cast<qint64>(sourceEndFrameExclusive);
    request[QStringLiteral("max_frames")] = static_cast<qint64>(maxFrames);
    request[QStringLiteral("frame_domain")] =
        facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute);
    request[QStringLiteral("detector_settings_file")] = detectorSettingsPath;
    request[QStringLiteral("detector_settings")] =
        detectorRuntimeSettingsToJson(
            detectorSettings,
            QStringLiteral("scrfd-ncnn-vulkan"),
            detectorSettings.scrfdTargetSize);
    request[QStringLiteral("detector_workers")] = launchTopology.detectorWorkers;
    request[QStringLiteral("detector_pipeline_slots")] = launchTopology.detectorPipelineSlots;
    request[QStringLiteral("detector_launch_profile")] = launchProfile;
    request[QStringLiteral("detector_launch_live_preview")] = launchLivePreview;
    request[QStringLiteral("detector_launch_control_window")] = launchControlWindow;
    request[QStringLiteral("detector_launch_progress_output")] = launchProgressOutput;
    request[QStringLiteral("detector_durable_detached_launch")] = durableGeneratorLaunch;
    request[QStringLiteral("detector_launch_control_path")] =
        QDir(artifactDir).absoluteFilePath(QStringLiteral("launch_control.json"));
    request[QStringLiteral("detector_launch_control_applied")] =
        explicitLaunchControl || preflightLaunchControlChanged;
    request[QStringLiteral("artifact_out_dir")] = artifactDir;
    request[QStringLiteral("facedetections_part")] = facedetectionsPartPath;
    request[QStringLiteral("detections_bin")] = detectionsPath;
    request[QStringLiteral("tracks_bin")] = tracksPath;
    request[QStringLiteral("continuity_facedetections_bin")] = outputPath;
    request[QStringLiteral("summary_json")] = summaryPath;
    request[QStringLiteral("clip_json")] = preflight.applyClipGrading ? clipJsonPath : QString();
    request[QStringLiteral("apply_clip_grading")] = preflight.applyClipGrading;

    const QString generatorProgram = facedetectionsOffscreenExecutablePath();
    if (!QFileInfo::exists(generatorProgram)) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Detection/continuity generator executable was not found.\n\nExpected: %1")
                .arg(generatorProgram));
        return;
    }
    if (launchMode == QStringLiteral("benchmark") || launchMode == QStringLiteral("auto")) {
        QString benchmarkError;
        if (!chooseBenchmarkedFaceDetectionsTopology(generatorProgram,
                                                     args,
                                                     launchControl,
                                                     &launchTopology,
                                                     &benchmarkError)) {
            showAutomationAwareWarning(
                QStringLiteral("JCut DNN Detection + Continuity Generator"),
                QStringLiteral("FaceDetections launch benchmark failed.\n\n%1")
                    .arg(benchmarkError.trimmed().isEmpty()
                             ? QStringLiteral("No benchmark result was produced.")
                             : benchmarkError.trimmed()));
            return;
        }
        setArgumentValue(&args, QStringLiteral("--detector-workers"),
                         QString::number(launchTopology.detectorWorkers));
        setArgumentValue(&args, QStringLiteral("--detector-pipeline-slots"),
                         QString::number(launchTopology.detectorPipelineSlots));
        launchControl[QStringLiteral("detector_workers")] = launchTopology.detectorWorkers;
        launchControl[QStringLiteral("detector_pipeline_slots")] = launchTopology.detectorPipelineSlots;
        launchControl[QStringLiteral("mode")] = launchMode;
        launchControl[QStringLiteral("last_benchmark")] = launchTopology.benchmarkResult;
        launchControl[QStringLiteral("last_benchmark_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        launchControl[QStringLiteral("updated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        launchControl[QStringLiteral("runtime_mutable")] = false;
        launchControl[QStringLiteral("apply_scope")] = QStringLiteral("next_generator_launch");
        launchControl[QStringLiteral("editor_restart_required")] = false;
        launchControl[QStringLiteral("generator_relaunch_required")] = true;
        launchControl[QStringLiteral("requires_generator_relaunch")] = true;
        QString benchmarkControlWriteError;
        if (!jcut::jsonio::writeJsonFile(QDir(artifactDir).absoluteFilePath(QStringLiteral("launch_control.json")),
                                         launchControl,
                                         true,
                                         &benchmarkControlWriteError)) {
            qWarning().noquote()
                << "Failed to persist FaceDetections benchmark launch control:"
                << benchmarkControlWriteError;
        }
    }

    request[QStringLiteral("detector_workers")] = launchTopology.detectorWorkers;
    request[QStringLiteral("detector_pipeline_slots")] = launchTopology.detectorPipelineSlots;
    request[QStringLiteral("detector_launch_mode")] = launchMode;
    if (!launchTopology.benchmarkResult.isEmpty()) {
        request[QStringLiteral("detector_launch_benchmark")] = launchTopology.benchmarkResult;
    }
    request[QStringLiteral("arguments")] = args.join(QLatin1Char(' '));
    QString requestWriteError;
    if (!jcut::jsonio::writeJsonFile(requestPath, request, true, &requestWriteError)) {
        qWarning().noquote() << "Failed to write detection/continuity request file:" << requestWriteError;
    }

    const FaceDetectionsProcessResult processResult =
        [&]() {
            ScopedAudioBackgroundDecodeSuppression suppressAudioDecode(
                m_speakerDeps.setAudioBackgroundDecodeSuppressed);
            return runFaceDetectionsGeneratorProcess(nullptr, generatorProgram, args, true);
        }();
    if (!processResult.started) {
        showAutomationAwareWarning(
            QStringLiteral("JCut DNN Detection + Continuity Generator"),
            QStringLiteral("Failed to start detection/continuity generator.\n\n%1")
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? QStringLiteral("Unknown process start failure.")
                         : processResult.standardError.trimmed()));
        return;
    }
    if (processResult.canceled) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("canceled"),
            QStringLiteral("Detection and continuity generation canceled by user."),
            {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        return;
    }

    const bool processOk =
        processResult.exitStatus == QProcess::NormalExit &&
        processResult.exitCode == 0;
    if (!processOk) {
        const QString message =
            QStringLiteral("JCut DNN Detection + Continuity Generator failed (exit code %1).\n\n%2")
                .arg(processResult.exitCode)
                .arg(processResult.standardError.trimmed().isEmpty()
                         ? processResult.standardOutput.trimmed()
                         : processResult.standardError.trimmed());
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            message, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), message);
        return;
    }

    const QStringList requiredGeneratorOutputs{outputPath, detectionsPath, tracksPath, summaryPath};
    QStringList missingGeneratorOutputs;
    for (const QString& path : requiredGeneratorOutputs) {
        if (!QFileInfo::exists(path)) {
            missingGeneratorOutputs.append(path);
        }
    }
    if (!missingGeneratorOutputs.isEmpty()) {
        const QString message =
            QStringLiteral("JCut DNN Detection + Continuity Generator exited without producing required final artifacts.\n\nMissing:\n%1\n\nGenerator stderr:\n%2")
                .arg(missingGeneratorOutputs.join(QLatin1Char('\n')),
                     processResult.standardError.trimmed().isEmpty()
                         ? QStringLiteral("(empty)")
                         : processResult.standardError.trimmed());
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            message, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), message);
        return;
    }

    QString parseError;
    QJsonObject generatedArtifact;
    if (!jcut::jsonio::readBinaryJsonObject(outputPath, &generatedArtifact, 0x4A435554, 1, &parseError) &&
        !readJsonObject(outputPath, &generatedArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            parseError, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), parseError);
        return;
    }

    QJsonObject continuityRoot = continuityRootForClip(generatedArtifact, QStringLiteral("facedetections-offscreen-source"));
    QJsonObject rawTracksArtifact;
    if (!jcut::facedetections::readBinaryJsonObject(tracksPath, &rawTracksArtifact, &parseError) &&
        !readJsonObject(tracksPath, &rawTracksArtifact, &parseError)) {
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            parseError, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), parseError);
        return;
    }

    const QJsonArray rawTracks = rawTracksArtifact.value(QStringLiteral("tracks")).toArray();
    const QJsonArray rawFrames = rawTracksArtifact.value(QStringLiteral("frames")).toArray();
    const int rawFramesCount = rawFrames.isEmpty() && QFileInfo::exists(detectionsPath)
        ? -1
        : rawFrames.size();
    continuityRoot[QStringLiteral("raw_tracks_artifact_path")] = tracksPath;
    continuityRoot[QStringLiteral("raw_frames_artifact_path")] = detectionsPath;
    continuityRoot[QStringLiteral("continuity_artifact_path")] = outputPath;
    continuityRoot[QStringLiteral("raw_tracks_count")] = rawTracks.size();
    continuityRoot[QStringLiteral("raw_frames_count")] = rawFramesCount;
    continuityRoot[QStringLiteral("raw_tracks_schema")] = rawTracksArtifact.value(QStringLiteral("schema")).toString();
    continuityRoot[QStringLiteral("raw_frames_schema")] =
        QStringLiteral("jcut_facedetections_offscreen_detections_v1");
    const QString rawTracksFrameDomain =
        rawTracksArtifact.value(QStringLiteral("frame_domain")).toString(
            facedetectionsFrameDomainString(FacestreamFrameDomain::SourceAbsolute));
    const QString rawFramesFrameDomain = rawTracksFrameDomain;
    continuityRoot[QStringLiteral("raw_tracks_frame_domain")] = rawTracksFrameDomain;
    continuityRoot[QStringLiteral("raw_frames_frame_domain")] = rawFramesFrameDomain;
    continuityRoot[QStringLiteral("streams_frame_domain")] =
        rawTracksFrameDomain;
    if (!continuityRoot.contains(QStringLiteral("detector_mode"))) {
        continuityRoot[QStringLiteral("detector_mode")] = rawTracksArtifact.value(QStringLiteral("backend")).toString();
    }

    if (rawTracks.isEmpty() && rawFrames.isEmpty()) {
        const QString noDetectionsMessage = QStringLiteral("Generated artifact contains no raw face detections for this clip.");
        speaker_flow_debug::persistIndex(
            indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
            m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"), QStringLiteral("error"),
            noDetectionsMessage, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), noDetectionsMessage);
        return;
    }
    continuityRoot[QStringLiteral("run_id")] = debugRun.runId;
    continuityRoot[QStringLiteral("imported_from_artifact_dir")] = artifactDir;
    continuityRoot[QStringLiteral("media_sidecar_dir")] = artifactDir;
    continuityRoot[QStringLiteral("facedetections_part")] = facedetectionsPartPath;
    continuityRoot[QStringLiteral("summary_json")] = summaryPath;
    continuityRoot[QStringLiteral("updated_at_utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    continuityRoot[QStringLiteral("media_compatibility")] =
        jcut::facedetections::artifactCompatibilityMetadataForClip(*selectedClip);

    continuityRoot[QStringLiteral("clip_id")] = clipId;
    continuityRoot[QStringLiteral("processed_artifact_path")] =
        editor::TranscriptEngine().facedetectionsProcessedArtifactPath(m_transcriptSession.transcriptPath());
    QJsonObject artifactRoot;
    const bool saved = jcut::facedetections::saveContinuityArtifact(
        m_transcriptSession.transcriptPath(),
        clipId,
        continuityRoot,
        &artifactRoot);
    bool savedProcessed = true;
    if (saved && !rawTracks.isEmpty()) {
        savedProcessed = jcut::facedetections::saveProcessedContinuityArtifact(
            m_transcriptSession.transcriptPath(),
            clipId,
            continuityRoot,
            m_transcriptSession.rootObject(),
            nullptr);
    }
    QString statusMessage = saved
        ? QStringLiteral("Imported generated detections and continuity tracks.")
        : QStringLiteral("Generated detections and continuity tracks, but failed to save the transcript artifact.");
    if (saved && rawTracks.isEmpty()) {
        statusMessage += QStringLiteral(" Track processing now runs in a later step.");
    }
    if (saved && !savedProcessed) {
        statusMessage += QStringLiteral(" Processed continuity sidecar rebuild failed.");
    }
    speaker_flow_debug::persistIndex(
        indexPath, debugRun.runId, debugRun.clipToken, QFileInfo(selectedClip->filePath).fileName(),
        m_transcriptSession.transcriptPath(), QStringLiteral("stage_6_facedetections"),
        (saved && savedProcessed) ? QStringLiteral("ok") : QStringLiteral("error"),
        statusMessage, {requestPath, facedetectionsPartPath, detectionsPath, tracksPath, outputPath, summaryPath});

    if (!saved) {
        showAutomationAwareWarning(QStringLiteral("JCut DNN Detection + Continuity Generator"), statusMessage);
        refresh();
        return;
    }

    emit transcriptDocumentChanged();
    clearFaceDetectionsDerivedCaches();
    if (m_deps.scheduleSaveState) {
        m_deps.scheduleSaveState();
    }
    if (m_deps.pushHistorySnapshot) {
        m_deps.pushHistorySnapshot();
    }
    showAutomationAwareInfo(
        QStringLiteral("JCut DNN Detection + Continuity Generator"),
        QStringLiteral("Imported raw detections and continuity tracks.\n\nFrames: %1\nTracks: %2\nArtifact: %3")
            .arg(rawFramesCount < 0 ? QStringLiteral("referenced") : QString::number(rawFramesCount))
            .arg(rawTracks.size())
            .arg(artifactDir));
    refresh();
}
