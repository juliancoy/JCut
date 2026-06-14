#include "profile_tab.h"
#include "async_decoder.h"
#include "decoder_benchmark_utils.h"
#include "decoder_context.h"
#include "frame_handle.h"
#include "editor_shared.h"
#include "debug_controls.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStringList>
#include <QTableWidgetItem>
#include <QDir>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QGuiApplication>
#include <algorithm>

extern "C"
{
#include <libavutil/hwcontext.h>
}

ProfileTab::ProfileTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
}

void ProfileTab::wire()
{
    if (m_widgets.profileBenchmarkButton) {
        connect(m_widgets.profileBenchmarkButton, &QPushButton::clicked,
                this, &ProfileTab::onBenchmarkClicked);
    }
    if (m_widgets.profileH26xThreadingModeCombo) {
        connect(m_widgets.profileH26xThreadingModeCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                [this](int) {
                    if (!m_widgets.profileH26xThreadingModeCombo) {
                        return;
                    }
                    editor::H26xSoftwareThreadingMode mode = editor::H26xSoftwareThreadingMode::Auto;
                    if (!editor::parseH26xSoftwareThreadingMode(
                            m_widgets.profileH26xThreadingModeCombo->currentData().toString(),
                            &mode)) {
                        return;
                    }
                    editor::setDebugH26xSoftwareThreadingMode(mode);
                    if (m_deps.scheduleSaveState) {
                        m_deps.scheduleSaveState();
                    }
                    if (m_deps.refreshInspector) {
                        m_deps.refreshInspector();
                    }
                });
    }
}

void ProfileTab::refresh()
{
    if (m_widgets.profileH26xThreadingModeCombo) {
        QSignalBlocker blocker(m_widgets.profileH26xThreadingModeCombo);
        const QString currentMode =
            editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode());
        const int modeIndex = m_widgets.profileH26xThreadingModeCombo->findData(currentMode);
        if (modeIndex >= 0) {
            m_widgets.profileH26xThreadingModeCombo->setCurrentIndex(modeIndex);
        }
    }
    if (!m_widgets.profileSummaryTable) return;

    const QJsonObject runtimeProfile = m_deps.profilingSnapshot();
    const QJsonObject previewProfile = runtimeProfile.value(QStringLiteral("preview")).toObject();
    const QJsonObject decoderProfile = previewProfile.value(QStringLiteral("decoder")).toObject();
    const QJsonObject cacheProfile = previewProfile.value(QStringLiteral("cache")).toObject();
    const QJsonObject playbackPipelineProfile = previewProfile.value(QStringLiteral("playback_pipeline")).toObject();
    const QJsonObject memoryBudgetProfile = previewProfile.value(QStringLiteral("memory_budget")).toObject();

    updateProfileTable(runtimeProfile, decoderProfile, cacheProfile,
                       playbackPipelineProfile, memoryBudgetProfile);
}

void ProfileTab::runDecodeBenchmark()
{
    TimelineClip clip;
    if (!m_deps.profileBenchmarkClip(&clip)) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Decode Benchmark"),
                             QStringLiteral("Select a visual clip or add one to the timeline first."));
        return;
    }

    const QString mediaPath = m_deps.playbackMediaPath(clip);
    if (mediaPath.isEmpty()) {
        QMessageBox::warning(nullptr,
                             QStringLiteral("Decode Benchmark"),
                             QStringLiteral("This clip has no playable media path."));
        return;
    }

    QProgressDialog progress(QStringLiteral("Running decode benchmark..."),
                             QString(),
                             0,
                             0,
                             nullptr);
    progress.setWindowTitle(QStringLiteral("Decode Benchmark"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.show();
    QCoreApplication::processEvents();

    QJsonObject benchmark{
        {QStringLiteral("success"), false},
        {QStringLiteral("clip_label"), clip.label},
        {QStringLiteral("path"), QDir::toNativeSeparators(mediaPath)}
    };

    const editor::DecodeBenchmarkResult result = editor::benchmarkDecodeFrames(mediaPath);
    if (!result.success) {
        benchmark[QStringLiteral("error")] = QStringLiteral("Failed to initialize decoder context.");
        m_lastDecodeBenchmark = benchmark;
        m_deps.refreshInspector();
        return;
    }

    benchmark[QStringLiteral("success")] = true;
    benchmark[QStringLiteral("codec")] = result.info.codecName;
    benchmark[QStringLiteral("decode_path")] =
        result.hardwareAccelerated ? QStringLiteral("hardware") : QStringLiteral("software");
    benchmark[QStringLiteral("frames_decoded")] = result.decodedFrames;
    benchmark[QStringLiteral("null_frames")] = result.nullFrames;
    benchmark[QStringLiteral("elapsed_ms")] = static_cast<qint64>(result.elapsedMs);
    benchmark[QStringLiteral("fps")] = result.fps;
    m_lastDecodeBenchmark = benchmark;
    m_deps.refreshInspector();
}

void ProfileTab::onBenchmarkClicked()
{
    runDecodeBenchmark();
}

void ProfileTab::updateProfileTable(const QJsonObject& runtimeProfile,
                                    const QJsonObject& decoderProfile,
                                    const QJsonObject& cacheProfile,
                                    const QJsonObject& playbackPipelineProfile,
                                    const QJsonObject& memoryBudgetProfile)
{
    QVector<QPair<QString, QString>> rows;
    auto addRow = [&rows](const QString& label, const QString& value) {
        rows.push_back({label, value});
    };

    const auto formatDuration = [](qint64 ms) -> QString {
        if (ms <= 0) return QStringLiteral("0 ms");
        const qint64 totalSeconds = ms / 1000;
        const qint64 minutes = totalSeconds / 60;
        const qint64 seconds = totalSeconds % 60;
        const qint64 remainderMs = ms % 1000;
        if (minutes > 0) {
            return QStringLiteral("%1m %2.%3s").arg(minutes).arg(seconds).arg(remainderMs / 100, 1, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1.%2s").arg(seconds).arg(remainderMs / 100, 1, 10, QLatin1Char('0'));
    };

    const auto formatStage = [](const QJsonObject& stats, const QString& totalKey, const QString& perFrameKey) -> QString {
        const qint64 totalMs = stats.value(totalKey).toVariant().toLongLong();
        const double perFrame = stats.value(perFrameKey).toDouble();
        if (totalMs <= 0) return QStringLiteral("0 ms");
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(totalMs)
            .arg(QString::number(perFrame, 'f', 2));
    };
    const auto formatPlaybackCounter = [](const QJsonObject& metric) -> QString {
        return QStringLiteral("attempts=%1 ok=%2 miss=%3 state=%4 reason=%5")
            .arg(metric.value(QStringLiteral("attempts")).toInteger(0))
            .arg(metric.value(QStringLiteral("successes")).toInteger(0))
            .arg(metric.value(QStringLiteral("source_unavailable")).toInteger(0))
            .arg(metric.value(QStringLiteral("last_state")).toString(QStringLiteral("n/a")),
                 metric.value(QStringLiteral("last_reason")).toString(QStringLiteral("n/a")));
    };

    const QJsonObject previewProfile = runtimeProfile.value(QStringLiteral("preview")).toObject();
    const QJsonObject renderBackendProfile = previewProfile.value(QStringLiteral("render_backend")).toObject();
    const QJsonObject debugProfile = runtimeProfile.value(QStringLiteral("debug")).toObject();
    const QString platform = QGuiApplication::platformName().trimmed();
    QString displayPath = platform.isEmpty() ? QStringLiteral("unknown") : platform;
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        displayPath += QStringLiteral(" (wayland)");
    } else if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        displayPath += QStringLiteral(" (x11)");
    }

    const QString requestedBackend =
        renderBackendProfile.value(QStringLiteral("requested")).toString().trimmed();
    const QString effectiveBackend =
        renderBackendProfile.value(QStringLiteral("effective")).toString().trimmed();
    const QString backendSummary =
        requestedBackend.isEmpty() && effectiveBackend.isEmpty()
            ? previewProfile.value(QStringLiteral("backend")).toString(QStringLiteral("unknown"))
            : QStringLiteral("%1 -> %2")
                  .arg(requestedBackend.isEmpty() ? QStringLiteral("unknown") : requestedBackend,
                       effectiveBackend.isEmpty() ? QStringLiteral("unknown") : effectiveBackend);

    addRow(QStringLiteral("Render Backend"), backendSummary);
    addRow(QStringLiteral("Presenter Path"),
           previewProfile.value(QStringLiteral("presenter")).toString(QStringLiteral("unknown")));
    addRow(QStringLiteral("Display Path"), displayPath);
    addRow(QStringLiteral("Decode Mode"),
           debugProfile.value(QStringLiteral("decode_mode")).toString(QStringLiteral("auto")));
    addRow(QStringLiteral("Playback Active"),
           runtimeProfile.value(QStringLiteral("playback_active")).toBool() ? QStringLiteral("Yes") : QStringLiteral("No"));
    addRow(QStringLiteral("Current Timeline Frame"),
           QString::number(runtimeProfile.value(QStringLiteral("current_frame")).toVariant().toLongLong()));
    addRow(QStringLiteral("Timeline Clips"),
           QString::number(runtimeProfile.value(QStringLiteral("timeline_clip_count")).toInt()));
    addRow(QStringLiteral("Decoder Workers"),
           QString::number(decoderProfile.value(QStringLiteral("worker_count")).toInt()));
    addRow(QStringLiteral("Pending Decode Requests"),
           QString::number(decoderProfile.value(QStringLiteral("pending_requests")).toInt()));
    addRow(QStringLiteral("Cached Frames"),
           QString::number(cacheProfile.value(QStringLiteral("total_cached_frames")).toInt()));
    addRow(QStringLiteral("Cache Hit Rate"),
           QStringLiteral("%1%").arg(cacheProfile.value(QStringLiteral("hit_rate")).toDouble() * 100.0, 0, 'f', 1));
    addRow(QStringLiteral("Playback Buffered Frames"),
           QString::number(playbackPipelineProfile.value(QStringLiteral("buffered_frames")).toInt()));
    addRow(QStringLiteral("Dropped Presentation Frames"),
           QString::number(playbackPipelineProfile.value(QStringLiteral("dropped_presentation_frames")).toInt()));
    addRow(QStringLiteral("CPU Memory Usage"),
           QStringLiteral("%1 / %2").arg(memoryBudgetProfile.value(QStringLiteral("cpu_usage")).toVariant().toLongLong())
                                    .arg(memoryBudgetProfile.value(QStringLiteral("cpu_max")).toVariant().toLongLong()));
    addRow(QStringLiteral("GPU Memory Usage"),
           QStringLiteral("%1 / %2").arg(memoryBudgetProfile.value(QStringLiteral("gpu_usage")).toVariant().toLongLong())
                                    .arg(memoryBudgetProfile.value(QStringLiteral("gpu_max")).toVariant().toLongLong()));
    addRow(QStringLiteral("Cache CPU Bytes"),
           QString::number(cacheProfile.value(QStringLiteral("cpu_bytes")).toVariant().toLongLong()));
    addRow(QStringLiteral("Cache GPU Bytes"),
           QString::number(cacheProfile.value(QStringLiteral("gpu_bytes")).toVariant().toLongLong()));
    addRow(QStringLiteral("Cached Hardware Frames"),
           QString::number(cacheProfile.value(QStringLiteral("hardware_frames")).toInt()));
    addRow(QStringLiteral("Cached CPU-backed Frames"),
           QString::number(cacheProfile.value(QStringLiteral("cpu_backed_frames")).toInt()));
    addRow(QStringLiteral("Cached GPU-texture Frames"),
           QString::number(cacheProfile.value(QStringLiteral("gpu_texture_frames")).toInt()));
    addRow(QStringLiteral("FFmpeg HW Device Types"),
           availableHardwareDeviceTypes().isEmpty()
               ? QStringLiteral("none")
               : availableHardwareDeviceTypes().join(QStringLiteral(", ")));
    addRow(QStringLiteral("Decode Policy"),
           QStringLiteral("Opaque video: hardware when supported"));
    addRow(QStringLiteral("H.264/H.265 CPU Threading"),
           editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode()));
    addRow(QStringLiteral("Decode Policy (Alpha/Images)"),
           QStringLiteral("Software decode"));
    addRow(QStringLiteral("Export Encode Policy"),
           QStringLiteral("Prefer hardware H.264 for MP4, fallback to software"));

    const QJsonArray decodeDetails = previewProfile.value(QStringLiteral("decode_status_details")).toArray();
    int decodeGpuTexture = 0;
    int decodeHardwareFrame = 0;
    int decodeCpuImage = 0;
    int decodeMissing = 0;
    for (const QJsonValue& value : decodeDetails) {
        const QJsonObject detail = value.toObject();
        if (!detail.value(QStringLiteral("active")).toBool()) {
            continue;
        }
        const bool hasFrame = detail.value(QStringLiteral("has_frame")).toBool();
        decodeGpuTexture += detail.value(QStringLiteral("gpu_texture")).toBool() ? 1 : 0;
        decodeHardwareFrame += detail.value(QStringLiteral("hardware_frame")).toBool() ? 1 : 0;
        decodeCpuImage += detail.value(QStringLiteral("cpu_image")).toBool() ? 1 : 0;
        decodeMissing += hasFrame ? 0 : 1;
    }
    addRow(QStringLiteral("Decode Active Clips"),
           QString::number(previewProfile.value(QStringLiteral("active_decode_status_clips")).toInt(decodeDetails.size())));
    addRow(QStringLiteral("Decode Path Summary"),
           QStringLiteral("gpu_texture:%1 hw_frame:%2 cpu_upload:%3 missing:%4")
               .arg(decodeGpuTexture)
               .arg(decodeHardwareFrame)
               .arg(decodeCpuImage)
               .arg(decodeMissing));
    const QJsonObject playbackStageMetrics =
        runtimeProfile.value(QStringLiteral("playback_pipeline_stages")).toObject();
    if (!playbackStageMetrics.isEmpty()) {
        QStringList stageNames = playbackStageMetrics.keys();
        std::sort(stageNames.begin(), stageNames.end());
        for (const QString& stageName : stageNames) {
            addRow(QStringLiteral("Playback Stage: %1").arg(stageName),
                   formatPlaybackCounter(playbackStageMetrics.value(stageName).toObject()));
        }
    }

    const QJsonObject activeExport = runtimeProfile.value(QStringLiteral("export")).toObject();
    const QJsonObject exportStats = activeExport.value(QStringLiteral("live")).toObject();
    if (exportStats.isEmpty()) {
        addRow(QStringLiteral("Export Status"), QStringLiteral("Idle"));
    } else {
        addRow(QStringLiteral("Export Status"),
               exportStats.value(QStringLiteral("status")).toString(QStringLiteral("Idle")));
        addRow(QStringLiteral("Export Output"),
               exportStats.value(QStringLiteral("output_path")).toString(QStringLiteral("unknown")));
        addRow(QStringLiteral("Export Progress"),
               QStringLiteral("%1 / %2 frames")
                   .arg(exportStats.value(QStringLiteral("frames_completed")).toVariant().toLongLong())
                   .arg(exportStats.value(QStringLiteral("total_frames")).toVariant().toLongLong()));
        addRow(QStringLiteral("Export Throughput"),
               QStringLiteral("%1 fps").arg(exportStats.value(QStringLiteral("fps")).toDouble(), 0, 'f', 1));
        addRow(QStringLiteral("Export Pipeline"),
               exportStats.value(QStringLiteral("export_pipeline")).toString(QStringLiteral("unknown")));
        addRow(QStringLiteral("Export Path Reason"),
               exportStats.value(QStringLiteral("export_path_fallback_reason")).toString(QStringLiteral("unknown")));
        addRow(QStringLiteral("Export Encoder Format"),
               QStringLiteral("%1 / sw:%2 / hw_frames:%3")
                   .arg(exportStats.value(QStringLiteral("encoder_pixel_format")).toString(QStringLiteral("unknown")),
                        exportStats.value(QStringLiteral("encoder_software_pixel_format")).toString(QStringLiteral("unknown")),
                        exportStats.value(QStringLiteral("encoder_hardware_frames")).toBool(false)
                            ? QStringLiteral("yes")
                            : QStringLiteral("no")));
        addRow(QStringLiteral("Export CUDA Interop"),
               QStringLiteral("%1 / %2")
                   .arg(exportStats.value(QStringLiteral("cuda_external_memory_supported")).toBool(false)
                            ? QStringLiteral("ready")
                            : QStringLiteral("unavailable"),
                        exportStats.value(QStringLiteral("cuda_external_memory_status")).toString(QStringLiteral("unknown"))));
        addRow(QStringLiteral("Export Render Stage"),
               formatStage(exportStats, QStringLiteral("render_stage_ms"), QStringLiteral("render_stage_per_frame_ms")));
        const QString gpuTransferLabel =
            exportStats.value(QStringLiteral("gpu_transfer_label")).toString(QStringLiteral("GPU Transfer"));
        addRow(QStringLiteral("Export %1").arg(gpuTransferLabel),
               formatStage(exportStats, QStringLiteral("gpu_readback_ms"), QStringLiteral("gpu_readback_per_frame_ms")));
        addRow(QStringLiteral("Export Encode Stage"),
               formatStage(exportStats, QStringLiteral("encode_stage_ms"), QStringLiteral("encode_stage_per_frame_ms")));
    }

    if (m_lastDecodeBenchmark.isEmpty()) {
        addRow(QStringLiteral("Last Benchmark"), QStringLiteral("Not run yet"));
    } else if (!m_lastDecodeBenchmark.value(QStringLiteral("success")).toBool()) {
        addRow(QStringLiteral("Last Benchmark"), QStringLiteral("Failed"));
        addRow(QStringLiteral("Benchmark Error"),
               m_lastDecodeBenchmark.value(QStringLiteral("error")).toString(QStringLiteral("unknown")));
    } else {
        addRow(QStringLiteral("Last Benchmark Clip"),
               m_lastDecodeBenchmark.value(QStringLiteral("clip_label")).toString());
        addRow(QStringLiteral("Last Benchmark Path"),
               m_lastDecodeBenchmark.value(QStringLiteral("path")).toString());
        addRow(QStringLiteral("Last Benchmark Codec"),
               m_lastDecodeBenchmark.value(QStringLiteral("codec")).toString());
        addRow(QStringLiteral("Last Benchmark Decode Path"),
               m_lastDecodeBenchmark.value(QStringLiteral("decode_path")).toString());
        addRow(QStringLiteral("Last Benchmark Frames Decoded"),
               QString::number(m_lastDecodeBenchmark.value(QStringLiteral("frames_decoded")).toInt()));
        addRow(QStringLiteral("Last Benchmark Throughput"),
               QStringLiteral("%1 fps").arg(m_lastDecodeBenchmark.value(QStringLiteral("fps")).toDouble(), 0, 'f', 1));
    }

    m_widgets.profileSummaryTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        auto* nameItem = new QTableWidgetItem(rows[row].first);
        auto* valueItem = new QTableWidgetItem(rows[row].second);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        m_widgets.profileSummaryTable->setItem(row, 0, nameItem);
        m_widgets.profileSummaryTable->setItem(row, 1, valueItem);
    }
    m_widgets.profileSummaryTable->resizeRowsToContents();
}

QStringList ProfileTab::availableHardwareDeviceTypes() const
{
    QStringList types;
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types(type)) {
        if (const char* name = av_hwdevice_get_type_name(type)) {
            types.push_back(QString::fromLatin1(name));
        }
    }
    return types;
}
