#include "editor.h"
#include "birefnet_job_core.h"

#include "editor_shared_render_sync.h"
#include "inspector_pane.h"
#include "processing_job_docker.h"
#include "processing_job_manifest.h"
#include "timeline_widget.h"

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace editor;

namespace {

constexpr int kBiRefNetCudaOomExitCode = 42;
constexpr int kBiRefNetHostOomExitCode = 43;
constexpr qsizetype kBiRefNetRetainedLogBytes = 1024 * 1024;

void appendBoundedProcessLog(QByteArray* log, const QByteArray& chunk)
{
    if (!log || chunk.isEmpty()) return;
    log->append(chunk);
    if (log->size() > kBiRefNetRetainedLogBytes) {
        log->remove(0, log->size() - kBiRefNetRetainedLogBytes);
    }
}

QJsonObject parseBiRefNetFailureLine(const QByteArray& processLog)
{
    static const QByteArray prefix("JCUT_BIREFNET_ERROR_JSON=");
    const QList<QByteArray> lines = processLog.split('\n');
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
        if (!line->startsWith(prefix)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line->mid(prefix.size()));
        if (document.isObject()) return document.object();
    }
    return {};
}

QJsonObject biRefNetFailure(const QString& outputDir,
                            const QByteArray& processLog,
                            int exitCode)
{
    QFile artifact(QDir(outputDir).filePath(QStringLiteral("jcut_error.json")));
    if (artifact.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(artifact.readAll());
        if (document.isObject()) return document.object();
    }
    QJsonObject failure = parseBiRefNetFailureLine(processLog);
    if (!failure.isEmpty()) return failure;
    if (exitCode == kBiRefNetCudaOomExitCode) {
        failure.insert(QStringLiteral("kind"), QStringLiteral("cuda_oom"));
    } else if (exitCode == kBiRefNetHostOomExitCode) {
        failure.insert(QStringLiteral("kind"), QStringLiteral("host_oom"));
    } else if (exitCode == 137) {
        // SIGKILL is commonly an OOM kill, but can have other causes.
        failure.insert(QStringLiteral("kind"), QStringLiteral("possible_memory_kill"));
    }
    return failure;
}

QString biRefNetFailureTitle(const QJsonObject& failure)
{
    const QString kind = failure.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("cuda_oom")) {
        return QStringLiteral("BiRefNet GPU Memory Exhausted");
    }
    if (kind == QStringLiteral("host_oom")) {
        return QStringLiteral("BiRefNet System Memory Exhausted");
    }
    if (kind == QStringLiteral("possible_memory_kill")) {
        return QStringLiteral("BiRefNet Process Killed");
    }
    return QStringLiteral("BiRefNet Failed");
}

QString biRefNetFailureMessage(const QJsonObject& failure)
{
    const QString kind = failure.value(QStringLiteral("kind")).toString();
    const QString phase = failure.value(QStringLiteral("phase")).toString();
    const int64_t frame = failure.value(QStringLiteral("frame_index")).toVariant().toLongLong();
    QString location;
    if (!phase.isEmpty()) location = QStringLiteral(" during %1").arg(phase);
    if (frame > 0) location += QStringLiteral(" at frame %1").arg(frame);

    if (kind == QStringLiteral("cuda_oom")) {
        return QStringLiteral(
            "The GPU ran out of memory%1. Completed alpha frames remain resumable. "
            "Retry with FP16 enabled, CPU, or a lighter BiRefNet model.").arg(location);
    }
    if (kind == QStringLiteral("host_oom")) {
        return QStringLiteral(
            "The process ran out of system memory%1. Completed alpha frames remain "
            "resumable. Close memory-heavy applications or use a lighter model before retrying.")
            .arg(location);
    }
    if (kind == QStringLiteral("possible_memory_kill")) {
        return QStringLiteral(
            "The BiRefNet container was killed with exit code 137. This often indicates "
            "system or container memory pressure, but it is not definitive. Completed alpha "
            "frames remain resumable; inspect the log before retrying.");
    }
    const QString detail = failure.value(QStringLiteral("message")).toString().trimmed();
    return detail.isEmpty()
        ? QStringLiteral("BiRefNet exited before completing the matte. See the process log for details.")
        : QStringLiteral("BiRefNet failed%1: %2").arg(location, detail);
}

QString birefnetDefaultCache(const QString& leaf)
{
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(cache.isEmpty() ? QDir::currentPath() : cache)
        .absoluteFilePath(QStringLiteral("birefnet/%1").arg(leaf));
}

const TimelineClip* clipById(const TimelineWidget* timeline, const QString& clipId)
{
    if (!timeline) return nullptr;
    for (const TimelineClip& clip : timeline->clips()) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

QImage alphaCompositePreview(const QImage& sourceImage, const QImage& alphaImage)
{
    const QImage source = sourceImage.convertToFormat(QImage::Format_ARGB32);
    const QImage alpha = alphaImage.scaled(
        source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    QImage foreground = source;
    for (int y = 0; y < foreground.height(); ++y) {
        QRgb* pixels = reinterpret_cast<QRgb*>(foreground.scanLine(y));
        const uchar* alphaPixels = alpha.constScanLine(y);
        for (int x = 0; x < foreground.width(); ++x) {
            pixels[x] = qRgba(qRed(pixels[x]), qGreen(pixels[x]), qBlue(pixels[x]), alphaPixels[x]);
        }
    }
    QImage composite(source.size(), QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&composite);
    constexpr int tile = 16;
    for (int y = 0; y < composite.height(); y += tile) {
        for (int x = 0; x < composite.width(); x += tile) {
            painter.fillRect(x, y, tile, tile,
                             ((x / tile) + (y / tile)) % 2 == 0
                                 ? QColor(52, 56, 62) : QColor(82, 87, 95));
        }
    }
    painter.drawImage(0, 0, foreground);
    return composite;
}

bool showBiRefNetPreview(QWidget* parent,
                         const QString& scriptPath,
                         const QString& inputPath,
                         const QString& model,
                         const QString& revision,
                         const QString& modelCache,
                         const QString& runtimeCache,
                         const QString& device,
                         bool fp16,
                         bool rootMode,
                         double alphaTolerance,
                         int64_t frameIndex)
{
    QTemporaryDir previewDirectory(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .absoluteFilePath(QStringLiteral("jcut-birefnet-preview-XXXXXX")));
    if (!previewDirectory.isValid()) {
        QMessageBox::warning(parent, QStringLiteral("BiRefNet Preview"),
                             QStringLiteral("Could not create a temporary preview directory."));
        return false;
    }
    QDir().mkpath(modelCache);
    QDir().mkpath(runtimeCache);
    QStringList command{scriptPath, inputPath,
                        QStringLiteral("--output-dir"), previewDirectory.path(),
                        QStringLiteral("--model"), model,
                        QStringLiteral("--revision"), revision,
                        QStringLiteral("--alpha-tolerance"), QString::number(alphaTolerance, 'f', 4),
                        QStringLiteral("--source-frame"), QString::number(frameIndex)};
    if (device == QStringLiteral("cpu")) command << QStringLiteral("--cpu");
    if (!fp16) command << QStringLiteral("--fp32");

    QProcess process;
    process.setWorkingDirectory(QDir::currentPath());
    process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("BIREFNET_MODEL_CACHE"), modelCache);
    environment.insert(QStringLiteral("BIREFNET_RUNTIME_CACHE"), runtimeCache);
    if (rootMode) environment.insert(QStringLiteral("BIREFNET_DOCKER_RUN_AS_ROOT"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);

    QProgressDialog progress(
        QStringLiteral("Generating BiRefNet preview for source frame %1…").arg(frameIndex),
        QStringLiteral("Cancel"), 0, 0, parent);
    progress.setWindowTitle(QStringLiteral("BiRefNet Preview"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    process.start(QStringLiteral("/bin/bash"), command);
    if (!process.waitForStarted(5000)) {
        QMessageBox::warning(parent, QStringLiteral("BiRefNet Preview"),
                             QStringLiteral("Could not start the BiRefNet preview runner."));
        return false;
    }
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        if (progress.wasCanceled()) {
            process.terminate();
            if (!process.waitForFinished(2000)) process.kill();
            return false;
        }
    }
    progress.close();
    const QByteArray runnerOutput = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QJsonObject failure = biRefNetFailure(
            previewDirectory.path(), runnerOutput, process.exitCode());
        QMessageBox details(QMessageBox::Warning,
                            biRefNetFailureTitle(failure),
                            biRefNetFailureMessage(failure),
                            QMessageBox::Ok, parent);
        details.setDetailedText(QString::fromLocal8Bit(runnerOutput));
        details.exec();
        return false;
    }
    const QString sourcePath = QDir(previewDirectory.path()).filePath(QStringLiteral("preview_source.png"));
    const QStringList alphaFrames = QDir(previewDirectory.path()).entryList(
        QStringList{QStringLiteral("frame_*.png")},
        QDir::Files | QDir::NoSymLinks,
        QDir::Name);
    const QString alphaPath = alphaFrames.size() == 1
        ? QDir(previewDirectory.path()).filePath(alphaFrames.constFirst())
        : QString();
    const QImage source(sourcePath);
    const QImage alpha(alphaPath);
    if (source.isNull() || alpha.isNull()) {
        QMessageBox::warning(parent, QStringLiteral("BiRefNet Preview"),
                             QStringLiteral("The preview runner completed without usable image artifacts."));
        return false;
    }

    QDialog preview(parent);
    preview.setWindowTitle(QStringLiteral("BiRefNet Preview — Frame %1").arg(frameIndex));
    auto* layout = new QVBoxLayout(&preview);
    auto* images = new QHBoxLayout;
    auto addImage = [&](const QString& title, const QImage& image) {
        auto* column = new QVBoxLayout;
        auto* heading = new QLabel(title, &preview);
        heading->setAlignment(Qt::AlignCenter);
        auto* imageLabel = new QLabel(&preview);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setMinimumSize(260, 180);
        imageLabel->setPixmap(QPixmap::fromImage(image).scaled(
            QSize(360, 300), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        column->addWidget(heading);
        column->addWidget(imageLabel, 1);
        images->addLayout(column, 1);
    };
    addImage(QStringLiteral("Source"), source);
    addImage(QStringLiteral("Alpha Matte"), alpha);
    addImage(QStringLiteral("Composite Check"), alphaCompositePreview(source, alpha));
    layout->addLayout(images, 1);
    auto* note = new QLabel(
        QStringLiteral("Single-frame preview checks subject selection and edge quality. "
                       "BiRefNet is frame-based, so this preview does not evaluate temporal flicker."),
        &preview);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* close = new QPushButton(QStringLiteral("Back to Settings"), &preview);
    QObject::connect(close, &QPushButton::clicked, &preview, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    preview.resize(1120, 500);
    preview.exec();
    return true;
}

} // namespace

void EditorWindow::openBiRefNetDetectorWindow(const QString& clipId)
{
    const TimelineClip* clip = clipById(m_timeline, clipId);
    if (!clip || clip->mediaType != ClipMediaType::Video) {
        QMessageBox::warning(this, QStringLiteral("BiRefNet"),
                             QStringLiteral("Select an existing video clip first."));
        return;
    }
    const QFileInfo inputInfo(clip->filePath);
    const TimelineClip clipSnapshot = *clip;
    const QString clipLabel = clipSnapshot.label;
    int64_t previewOffset = m_timeline->currentFrame() - clipSnapshot.startFrame;
    if (previewOffset < 0 || previewOffset >= clipSnapshot.durationFrames) {
        previewOffset = qMax<int64_t>(0, clipSnapshot.durationFrames / 2);
    }
    previewOffset = qBound<int64_t>(
        0, previewOffset, qMax<int64_t>(0, clipSnapshot.durationFrames - 1));
    const int64_t previewTimelineFrame = clipSnapshot.startFrame + previewOffset;
    const int64_t previewSourceFrame = requestedSourceFrameForGeneratedMaskPreview(
        clipSnapshot,
        m_timeline->clips(),
        static_cast<qreal>(previewTimelineFrame),
        m_timeline->renderSyncMarkers());
    if (!inputInfo.isFile()) {
        QMessageBox::warning(this, QStringLiteral("BiRefNet"),
                             QStringLiteral("The source video does not exist:\n%1")
                                 .arg(QDir::toNativeSeparators(clip->filePath)));
        return;
    }
    const QString scriptPath = QDir::current().absoluteFilePath(QStringLiteral("birefnet.sh"));
    if (!QFileInfo::exists(scriptPath)) {
        QMessageBox::warning(this, QStringLiteral("BiRefNet"),
                             QStringLiteral("birefnet.sh was not found:\n%1").arg(scriptPath));
        return;
    }

    QSettings settings(QStringLiteral("PanelTalkEditor"), QStringLiteral("JCut"));
    QDialog preflight(this);
    preflight.setWindowTitle(QStringLiteral("BiRefNet Alpha Preflight"));
    auto* layout = new QVBoxLayout(&preflight);
    auto* explanation = new QLabel(
        QStringLiteral("BiRefNet automatically extracts the most salient foreground and writes "
                       "continuous-alpha PNG frames. It does not accept a text prompt."),
        &preflight);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* form = new QFormLayout;
    auto* modelEdit = new QLineEdit(
        settings.value(QStringLiteral("birefnet/model"),
                       QStringLiteral("ZhengPeng7/BiRefNet-matting")).toString(), &preflight);
    form->addRow(QStringLiteral("Model"), modelEdit);
    auto* revisionEdit = new QLineEdit(
        settings.value(QStringLiteral("birefnet/revision"),
                       QStringLiteral("57f9f68b43ba337c75762b14cf3075d659007268")).toString(),
        &preflight);
    revisionEdit->setToolTip(QStringLiteral("Pinned Hugging Face commit for reproducible model code and weights."));
    form->addRow(QStringLiteral("Revision"), revisionEdit);
    auto* cacheEdit = new QLineEdit(
        settings.value(QStringLiteral("birefnet/modelCache"), birefnetDefaultCache(QStringLiteral("hf"))).toString(),
        &preflight);
    auto* cacheRow = new QHBoxLayout;
    auto* browseCache = new QPushButton(QStringLiteral("Browse"), &preflight);
    cacheRow->addWidget(cacheEdit, 1);
    cacheRow->addWidget(browseCache);
    form->addRow(QStringLiteral("Model cache"), cacheRow);
    auto* runtimeEdit = new QLineEdit(
        settings.value(QStringLiteral("birefnet/runtimeCache"), birefnetDefaultCache(QStringLiteral("runtime"))).toString(),
        &preflight);
    auto* runtimeRow = new QHBoxLayout;
    auto* browseRuntime = new QPushButton(QStringLiteral("Browse"), &preflight);
    runtimeRow->addWidget(runtimeEdit, 1);
    runtimeRow->addWidget(browseRuntime);
    form->addRow(QStringLiteral("Runtime cache"), runtimeRow);
    auto* deviceCombo = new QComboBox(&preflight);
    deviceCombo->addItem(QStringLiteral("CUDA (recommended)"), QStringLiteral("cuda"));
    deviceCombo->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
    deviceCombo->setCurrentIndex(settings.value(QStringLiteral("birefnet/device"), 0).toInt());
    form->addRow(QStringLiteral("Device"), deviceCombo);
    auto* fp16 = new QCheckBox(QStringLiteral("Use FP16 on CUDA"), &preflight);
    fp16->setChecked(settings.value(QStringLiteral("birefnet/fp16"), true).toBool());
    form->addRow(QString(), fp16);
    auto* alphaToleranceSpin = new QDoubleSpinBox(&preflight);
    alphaToleranceSpin->setObjectName(QStringLiteral("birefnet.alpha_tolerance"));
    alphaToleranceSpin->setRange(0.0, 99.0);
    alphaToleranceSpin->setDecimals(1);
    alphaToleranceSpin->setSingleStep(1.0);
    alphaToleranceSpin->setSuffix(QStringLiteral(" %"));
    alphaToleranceSpin->setValue(
        settings.value(QStringLiteral("birefnet/alphaTolerancePercent"), 0.0).toDouble());
    alphaToleranceSpin->setToolTip(QStringLiteral(
        "Minimum foreground confidence. Higher values remove faint background leakage; "
        "0% preserves BiRefNet's original continuous alpha."));
    form->addRow(QStringLiteral("Alpha tolerance"), alphaToleranceSpin);
    auto* maskMatteNote = new QLabel(QStringLiteral(
        "The generated alpha is added as a source-linked Mask Matte child."),
        &preflight);
    maskMatteNote->setWordWrap(true);
    maskMatteNote->setToolTip(QStringLiteral(
        "The Mask Matte child owns the sidecar and visual treatment while the source "
        "parent remains authoritative for media, timing, and transforms."));
    form->addRow(QStringLiteral("Timeline"), maskMatteNote);
    auto* rootMode = new QCheckBox(QStringLiteral("Run Docker container as root"), &preflight);
    rootMode->setChecked(false);
    form->addRow(QString(), rootMode);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout;
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), &preflight);
    auto* previewButton = new QPushButton(
        QStringLiteral("Preview Frame %1").arg(previewSourceFrame), &preflight);
    previewButton->setToolTip(
        QStringLiteral("Run BiRefNet on the source frame under the playhead, or the clip midpoint when the playhead is outside the clip."));
    auto* run = new QPushButton(QStringLiteral("Generate Alpha"), &preflight);
    run->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(previewButton);
    buttons->addWidget(run);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, &preflight, &QDialog::reject);
    connect(previewButton, &QPushButton::clicked, &preflight, [&]() {
        const QString previewModel = modelEdit->text().trimmed();
        const QString previewRevision = revisionEdit->text().trimmed();
        const QString previewModelCache = QFileInfo(cacheEdit->text()).absoluteFilePath();
        const QString previewRuntimeCache = QFileInfo(runtimeEdit->text()).absoluteFilePath();
        if (previewModel.isEmpty() || previewRevision.isEmpty() ||
            previewModelCache.isEmpty() || previewRuntimeCache.isEmpty()) {
            QMessageBox::information(&preflight, QStringLiteral("BiRefNet Preview"),
                                     QStringLiteral("Complete the model, revision, and cache settings first."));
            return;
        }
        showBiRefNetPreview(&preflight,
                            scriptPath,
                            inputInfo.absoluteFilePath(),
                            previewModel,
                            previewRevision,
                            previewModelCache,
                            previewRuntimeCache,
                            deviceCombo->currentData().toString(),
                            fp16->isChecked(),
                            rootMode->isChecked(),
                            alphaToleranceSpin->value() / 100.0,
                            previewSourceFrame);
    });
    connect(run, &QPushButton::clicked, &preflight, [&]() {
        if (!modelEdit->text().trimmed().isEmpty() &&
            !revisionEdit->text().trimmed().isEmpty() &&
            !cacheEdit->text().trimmed().isEmpty() &&
            !runtimeEdit->text().trimmed().isEmpty()) preflight.accept();
    });
    connect(browseCache, &QPushButton::clicked, &preflight, [&]() {
        const QString path = QFileDialog::getExistingDirectory(&preflight, QStringLiteral("BiRefNet Model Cache"), cacheEdit->text());
        if (!path.isEmpty()) cacheEdit->setText(path);
    });
    connect(browseRuntime, &QPushButton::clicked, &preflight, [&]() {
        const QString path = QFileDialog::getExistingDirectory(&preflight, QStringLiteral("BiRefNet Runtime Cache"), runtimeEdit->text());
        if (!path.isEmpty()) runtimeEdit->setText(path);
    });
    if (preflight.exec() != QDialog::Accepted) return;

    const QString model = modelEdit->text().trimmed();
    const QString revision = revisionEdit->text().trimmed();
    const QString modelCache = QFileInfo(cacheEdit->text()).absoluteFilePath();
    const QString runtimeCache = QFileInfo(runtimeEdit->text()).absoluteFilePath();
    if (!QDir().mkpath(modelCache) || !QDir().mkpath(runtimeCache)) {
        QMessageBox::warning(this, QStringLiteral("BiRefNet"),
                             QStringLiteral("Could not create the selected cache directories."));
        return;
    }
    settings.setValue(QStringLiteral("birefnet/model"), model);
    settings.setValue(QStringLiteral("birefnet/revision"), revision);
    settings.setValue(QStringLiteral("birefnet/modelCache"), modelCache);
    settings.setValue(QStringLiteral("birefnet/runtimeCache"), runtimeCache);
    settings.setValue(QStringLiteral("birefnet/device"), deviceCombo->currentIndex());
    settings.setValue(QStringLiteral("birefnet/fp16"), fp16->isChecked());
    const double alphaTolerance = alphaToleranceSpin->value() / 100.0;
    settings.setValue(
        QStringLiteral("birefnet/alphaTolerancePercent"), alphaToleranceSpin->value());
    jcut::jobs::BiRefNetJobRequestCore sharedRequest;
    sharedRequest.scriptPath = scriptPath.toStdString();
    sharedRequest.mediaPath = inputInfo.absoluteFilePath().toStdString();
    sharedRequest.model = model.toStdString();
    sharedRequest.revision = revision.toStdString();
    sharedRequest.modelCachePath = modelCache.toStdString();
    sharedRequest.runtimeCachePath = runtimeCache.toStdString();
    sharedRequest.device =
        deviceCombo->currentData().toString().toStdString();
    sharedRequest.fp16 = fp16->isChecked();
    sharedRequest.runDockerAsRoot = rootMode->isChecked();
    sharedRequest.alphaTolerance = alphaTolerance;
    jcut::jobs::BiRefNetJobPlanCore sharedPlan =
        jcut::jobs::buildBiRefNetJobPlanCore(sharedRequest);
    const QString outputDir =
        QString::fromStdString(sharedPlan.outputDirectory);
    const QString jobRoot =
        QString::fromStdString(sharedPlan.jobRoot);
    const QString manifestPath =
        QString::fromStdString(sharedPlan.manifestPath);
    const QString jobLogPath =
        QString::fromStdString(sharedPlan.logPath);
    const QString progressPath =
        QString::fromStdString(sharedPlan.progressPath);
    const QString containerName =
        QString::fromStdString(sharedPlan.containerName);
    bool restart = false;
    if (QFileInfo::exists(manifestPath)) {
        QJsonObject oldManifest;
        jcut::jobs::readManifest(manifestPath, &oldManifest, nullptr);
        const QJsonObject oldParameters =
            oldManifest.value(QStringLiteral("parameters")).toObject();
        const bool generationParametersChanged =
            oldParameters.value(QStringLiteral("model")).toString() != model ||
            oldParameters.value(QStringLiteral("revision")).toString() != revision ||
            oldParameters.value(QStringLiteral("device")).toString() !=
                deviceCombo->currentData().toString() ||
            oldParameters.value(QStringLiteral("fp16")).toBool(true) != fp16->isChecked() ||
            !qFuzzyCompare(
                1.0 + oldParameters.value(QStringLiteral("alpha_tolerance")).toDouble(0.0),
                1.0 + alphaTolerance);
        if (generationParametersChanged) {
            // Existing frames cannot be resumed under different inference or
            // alpha-remapping settings without creating a mixed matte.
            restart = true;
        } else if (oldManifest.value(QStringLiteral("status")).toString() !=
                   QStringLiteral("completed")) {
            QMessageBox prompt(this);
            prompt.setWindowTitle(QStringLiteral("Resume BiRefNet"));
            prompt.setText(QStringLiteral("An unfinished BiRefNet job exists."));
            auto* resumeButton = prompt.addButton(QStringLiteral("Resume"), QMessageBox::AcceptRole);
            auto* restartButton = prompt.addButton(QStringLiteral("Restart"), QMessageBox::DestructiveRole);
            prompt.addButton(QMessageBox::Cancel);
            prompt.exec();
            if (prompt.clickedButton() == restartButton) restart = true;
            else if (prompt.clickedButton() != resumeButton) return;
        }
    }
    if (restart) {
        QDir(outputDir).removeRecursively();
        QFile::remove(manifestPath);
        QFile::remove(jobLogPath);
        QFile::remove(progressPath);
    }
    QDir().mkpath(outputDir);

    sharedRequest.restart = restart;
    sharedPlan =
        jcut::jobs::buildBiRefNetJobPlanCore(sharedRequest);
    QStringList command;
    for (const std::string& argument : sharedPlan.command) {
        command.push_back(QString::fromStdString(argument));
    }
    QJsonObject manifest = jcut::jobs::makeManifest(
        QStringLiteral("birefnet"), jobRoot, inputInfo.absoluteFilePath(),
        QJsonObject{{QStringLiteral("model"), model},
                    {QStringLiteral("revision"), revision},
                    {QStringLiteral("device"), deviceCombo->currentData().toString()},
                    {QStringLiteral("fp16"), fp16->isChecked()},
                    {QStringLiteral("alpha_tolerance"), alphaTolerance},
                    {QStringLiteral("create_mask_marker"), true},
                    {QStringLiteral("docker_root_mode"), rootMode->isChecked()},
                    {QStringLiteral("live_preview"), true}},
        QJsonObject{{QStringLiteral("alpha_masks_dir"), outputDir},
                    {QStringLiteral("model_cache"), modelCache},
                    {QStringLiteral("runtime_cache"), runtimeCache},
                    {QStringLiteral("job_log"), jobLogPath},
                    {QStringLiteral("progress"), progressPath}}, command);
    const QString imageName = qEnvironmentVariable(
        "BIREFNET_IMAGE_NAME", QStringLiteral("jcut-birefnet:cu126"));
    manifest.insert(
        QStringLiteral("process"),
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("docker")},
            {QStringLiteral("docker"),
             QJsonObject{{QStringLiteral("container_name"), containerName},
                         {QStringLiteral("image"), imageName}}},
        });
    manifest.insert(QStringLiteral("docker_container_name"), containerName);
    manifest.insert(QStringLiteral("status"), QStringLiteral("starting"));
    QString manifestError;
    if (!jcut::jobs::writeManifest(manifestPath, manifest, &manifestError)) {
        QMessageBox::warning(this, QStringLiteral("BiRefNet"), manifestError);
        return;
    }

    auto* progress = new QDialog(this);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowFlag(Qt::WindowCloseButtonHint, false);
    progress->setWindowTitle(QStringLiteral("BiRefNet — %1").arg(clipLabel));
    progress->resize(980, 720);
    auto* progressLayout = new QVBoxLayout(progress);
    auto* previewHeading = new QLabel(
        QStringLiteral("Live result   •   Source  |  Alpha Matte  |  Composite"), progress);
    previewHeading->setAlignment(Qt::AlignCenter);
    progressLayout->addWidget(previewHeading);
    auto* statusLabel = new QLabel(QStringLiteral("Running BiRefNet…"), progress);
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: #b7c0ca; padding: 4px 8px; }"));
    progressLayout->addWidget(statusLabel);
    auto* frameProgress = new QProgressBar(progress);
    frameProgress->setRange(0, 1000);
    frameProgress->setValue(0);
    frameProgress->setFormat(QStringLiteral("Preparing worker…"));
    progressLayout->addWidget(frameProgress);
    auto* livePreview = new QLabel(QStringLiteral("Waiting for the first completed frame…"), progress);
    livePreview->setAlignment(Qt::AlignCenter);
    livePreview->setMinimumHeight(320);
    livePreview->setStyleSheet(QStringLiteral(
        "QLabel { background: #171a1f; color: #b7c0ca; border: 1px solid #343b44; "
        "border-radius: 8px; padding: 8px; }"));
    progressLayout->addWidget(livePreview, 1);
    auto* output = new QPlainTextEdit(progress);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setMaximumHeight(190);
    progressLayout->addWidget(output);
    auto* stop = new QPushButton(QStringLiteral("Stop"), progress);
    progressLayout->addWidget(stop, 0, Qt::AlignRight);
    auto* process = new QProcess(progress);
    const auto processLog = std::make_shared<QByteArray>();
    process->setWorkingDirectory(QDir::currentPath());
    process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const auto& [name, value] : sharedPlan.environment) {
        environment.insert(
            QString::fromStdString(name),
            QString::fromStdString(value));
    }
    // The Qt adapter reads QProcess output itself, so retain the script's
    // tee-backed durable log. The neutral process controller captures merged
    // output directly to this same path and therefore omits this override.
    environment.insert(QStringLiteral("BIREFNET_LOG_PATH"), jobLogPath);
    process->setProcessEnvironment(environment);
    const QString livePreviewPath =
        QString::fromStdString(sharedPlan.livePreviewPath);
    QFile::remove(livePreviewPath);
    auto* previewTimer = new QTimer(progress);
    previewTimer->setInterval(250);
    const auto lastPreviewKey = std::make_shared<qint64>(-1);
    const auto refreshLivePreview = [livePreview, livePreviewPath, lastPreviewKey]() {
        const QFileInfo info(livePreviewPath);
        if (!info.isFile() || info.size() <= 0) return;
        const qint64 key = info.lastModified().toMSecsSinceEpoch() ^ info.size();
        if (key == *lastPreviewKey) return;
        const QImage image(livePreviewPath);
        if (image.isNull()) return;
        *lastPreviewKey = key;
        livePreview->setText(QString());
        livePreview->setPixmap(QPixmap::fromImage(image).scaled(
            livePreview->size() - QSize(16, 16),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    };
    const auto refreshJobProgress = [frameProgress, statusLabel, progressPath]() {
        QFile file(progressPath);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QJsonObject jobProgress = QJsonDocument::fromJson(file.readAll()).object();
        const int current = jobProgress.value(QStringLiteral("current_frame")).toInt();
        const int total = jobProgress.value(QStringLiteral("total_frames")).toInt();
        if (total <= 0) return;
        const double percent = qBound(
            0.0, jobProgress.value(QStringLiteral("percent")).toDouble(), 100.0);
        frameProgress->setValue(qRound(percent * 10.0));
        frameProgress->setFormat(
            QStringLiteral("%1 / %2 frames  —  %3%")
                .arg(current)
                .arg(total)
                .arg(percent, 0, 'f', 1));
        statusLabel->setText(QStringLiteral("Running BiRefNet — frame %1 of %2")
                                 .arg(current)
                                 .arg(total));
    };
    connect(previewTimer, &QTimer::timeout, progress, refreshLivePreview);
    connect(previewTimer, &QTimer::timeout, progress, refreshJobProgress);
    connect(process, &QProcess::readyReadStandardOutput, progress,
            [process, output, processLog]() {
        const QByteArray chunk = process->readAllStandardOutput();
        appendBoundedProcessLog(processLog.get(), chunk);
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(QString::fromLocal8Bit(chunk));
        output->moveCursor(QTextCursor::End);
    });
    connect(process, &QProcess::errorOccurred, progress,
            [manifestPath, output, stop, previewTimer, statusLabel](QProcess::ProcessError error) {
        previewTimer->stop();
        jcut::jobs::updateManifestStatus(
            manifestPath, QStringLiteral("failed"),
            QJsonObject{{QStringLiteral("process_error"), static_cast<int>(error)},
                        {QStringLiteral("error_kind"), QStringLiteral("process_error")}}, nullptr);
        statusLabel->setText(QStringLiteral(
            "BiRefNet could not start or communicate with its worker process."));
        statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: #ff8a80; font-weight: 600; padding: 4px 8px; }"));
        output->appendPlainText(QStringLiteral("\nFailed to start or communicate with the BiRefNet process."));
        stop->setText(QStringLiteral("Close"));
    });
    const auto stopRequested = std::make_shared<bool>(false);
    connect(process, &QProcess::started, progress, [manifestPath, process]() {
        jcut::jobs::updateManifestStatus(
            manifestPath,
            QStringLiteral("starting"),
            QJsonObject{{QStringLiteral("pid"), static_cast<qint64>(process->processId())}},
            nullptr);
    });
    connect(stop, &QPushButton::clicked, progress,
            [progress, process, stop, output, statusLabel, containerName,
             stopRequested]() {
        if (process->state() == QProcess::NotRunning) {
            progress->close();
            return;
        }
        *stopRequested = true;
        stop->setEnabled(false);
        stop->setText(QStringLiteral("Stopping…"));
        statusLabel->setText(QStringLiteral("Stopping BiRefNet safely…"));
        const QString docker = QStandardPaths::findExecutable(QStringLiteral("docker"));
        if (docker.isEmpty()) {
            process->terminate();
            return;
        }
        auto* stopper = new QProcess(progress);
        stopper->setProcessChannelMode(QProcess::MergedChannels);
        connect(stopper, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                progress, [stopper, process, output, stop](int exitCode,
                                                           QProcess::ExitStatus exitStatus) {
            const QString message = QString::fromLocal8Bit(stopper->readAll()).trimmed();
            if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                if (!message.isEmpty()) {
                    output->appendPlainText(QStringLiteral("\n[stop] %1").arg(message));
                }
                // The launcher may still be building the image and have no container yet.
                process->terminate();
            }
            stop->setEnabled(true);
        });
        stopper->start(docker,
                       QStringList{QStringLiteral("stop"), QStringLiteral("--time"),
                                   QStringLiteral("15"), containerName});
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), progress,
            [this, manifestPath, output, stop, clipId, outputDir,
             previewTimer, refreshLivePreview, process, processLog,
             statusLabel, frameProgress, stopRequested](int exitCode, QProcess::ExitStatus status) {
        const QByteArray finalChunk = process->readAllStandardOutput();
        if (!finalChunk.isEmpty()) {
            appendBoundedProcessLog(processLog.get(), finalChunk);
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString::fromLocal8Bit(finalChunk));
            output->moveCursor(QTextCursor::End);
        }
        refreshLivePreview();
        previewTimer->stop();
        const bool succeeded = status == QProcess::NormalExit && exitCode == 0;
        const bool stopped = !succeeded &&
            (*stopRequested || (status == QProcess::NormalExit && exitCode == 143));
        const QJsonObject failure = succeeded || stopped
            ? QJsonObject()
            : biRefNetFailure(outputDir, *processLog, exitCode);
        QJsonObject manifestPatch{
            {QStringLiteral("exit_code"), exitCode},
            {QStringLiteral("exit_status"),
             status == QProcess::NormalExit ? QStringLiteral("normal")
                                            : QStringLiteral("crashed")},
        };
        if (!failure.isEmpty()) {
            manifestPatch.insert(QStringLiteral("error_kind"),
                                 failure.value(QStringLiteral("kind")));
            manifestPatch.insert(QStringLiteral("error_message"),
                                 failure.value(QStringLiteral("message")));
            manifestPatch.insert(QStringLiteral("error_phase"),
                                 failure.value(QStringLiteral("phase")));
            manifestPatch.insert(QStringLiteral("error_frame_index"),
                                 failure.value(QStringLiteral("frame_index")));
            const QString errorArtifact =
                QDir(outputDir).filePath(QStringLiteral("jcut_error.json"));
            if (QFileInfo::exists(errorArtifact)) {
                manifestPatch.insert(QStringLiteral("error_artifact"), errorArtifact);
            }
        }
        jcut::jobs::updateManifestStatus(
            manifestPath,
            succeeded ? QStringLiteral("completed")
                      : stopped ? QStringLiteral("stopped") : QStringLiteral("failed"),
            manifestPatch, nullptr);
        bool sourceAvailable = false;
        bool matteCreated = false;
        if (succeeded && m_timeline) {
            sourceAvailable = std::any_of(
                m_timeline->clips().cbegin(),
                m_timeline->clips().cend(),
                [&](const TimelineClip& clip) {
                    return clip.id == clipId &&
                           clip.clipRole == ClipRole::Media &&
                           clip.mediaType == ClipMediaType::Video;
                });
            if (sourceAvailable) {
                matteCreated = m_timeline->createOrReplaceMaskMatteForSidecar(
                    clipId, outputDir, false);
            }
        }
        if (sourceAvailable) {
            if (m_preview) {
                m_preview->setTimelineTracks(m_timeline->tracks());
                m_preview->setTimelineClips(m_timeline->clips());
            }
            if (m_inspectorPane) m_inspectorPane->refreshTab(QStringLiteral("Masks"));
        }
        if (succeeded && sourceAvailable) {
            frameProgress->setValue(1000);
            frameProgress->setFormat(QStringLiteral("Completed"));
            statusLabel->setText(QStringLiteral("BiRefNet completed successfully."));
            statusLabel->setStyleSheet(QStringLiteral(
                "QLabel { color: #78d99b; font-weight: 600; padding: 4px 8px; }"));
            output->appendPlainText(
                matteCreated
                    ? QStringLiteral("\nBiRefNet completed. The alpha sidecar is active and a Mask Matte layer was created.")
                    : QStringLiteral("\nBiRefNet completed. The alpha sidecar is available in the Masks tab."));
        } else {
            if (succeeded) {
                statusLabel->setText(QStringLiteral(
                    "BiRefNet completed, but the source clip is no longer available."));
                output->appendPlainText(QStringLiteral(
                    "\nBiRefNet completed, but the source clip no longer exists; the alpha sidecar remains on disk."));
            } else if (stopped) {
                statusLabel->setText(QStringLiteral(
                    "BiRefNet stopped safely. Run it again to resume from completed frames."));
                statusLabel->setStyleSheet(QStringLiteral(
                    "QLabel { color: #ffd180; font-weight: 600; padding: 4px 8px; }"));
                frameProgress->setFormat(QStringLiteral("Stopped — progress preserved"));
                output->appendPlainText(QStringLiteral(
                    "\nBiRefNet stopped. Existing alpha frames are preserved and resumable."));
            } else {
                const QString message = biRefNetFailureMessage(failure);
                statusLabel->setText(message);
                statusLabel->setStyleSheet(QStringLiteral(
                    "QLabel { color: #ff8a80; font-weight: 600; padding: 4px 8px; }"));
                output->appendPlainText(QStringLiteral("\n%1").arg(message));
            }
        }
        stop->setText(QStringLiteral("Close"));
        stop->setEnabled(true);
    });
    process->start(QStringLiteral("/bin/bash"), command);
    previewTimer->start();
    progress->show();
}
