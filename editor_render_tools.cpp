#include "editor.h"

#include "speaker_export_harness.h"

#include <QDialog>
#include <QFormLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QLabel>
#include <QProgressBar>
#include <QCoreApplication>

bool EditorWindow::parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

void EditorWindow::refreshSyncInspector()
{
    if (m_syncTab) {
        m_syncTab->refresh();
    }
}

void EditorWindow::onSyncTableSelectionChanged()
{
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableCustomContextMenu(const QPoint& pos)
{
    Q_UNUSED(pos);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::refreshClipInspector()
{
    if (m_propertiesTab) {
        m_propertiesTab->refresh();
    }
}

void EditorWindow::refreshTracksTab()
{
    if (m_tracksTab) {
        m_tracksTab->refresh();
    }
}

void EditorWindow::onTrackTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Tracks table interactions are handled by TracksTab.
}

void EditorWindow::refreshOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->refresh();
    }
}

void EditorWindow::applyOutputRangeFromInspector()
{
    if (m_outputTab) {
        m_outputTab->applyRangeFromInspector();
    }
}

void EditorWindow::renderFromOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->renderFromInspector();
    }
}

RenderRequest EditorWindow::buildRenderRequestFromOutputControls() const
{
    RenderRequest request;
    request.outputFormat = m_outputFormatCombo
        ? m_outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }

    request.outputSize = QSize(
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920);
    request.useProxyMedia = m_renderUseProxiesCheckBox &&
                            m_renderUseProxiesCheckBox->isChecked();
    request.createVideoFromImageSequence = m_createImageSequenceCheckBox &&
                                           m_createImageSequenceCheckBox->isChecked();
    if (request.createVideoFromImageSequence && m_imageSequenceFormatCombo) {
        request.imageSequenceFormat = m_imageSequenceFormatCombo->currentData().toString();
        if (request.imageSequenceFormat.isEmpty()) {
            request.imageSequenceFormat = QStringLiteral("jpeg");
        }
    }
    return request;
}

void EditorWindow::applyPreviewViewMode(const QString& modeText)
{
    if (!m_featureAudioPreviewMode) {
        m_previewViewMode = QStringLiteral("video");
        if (m_preview) {
            m_preview->setViewMode(PreviewSurface::ViewMode::Video);
        }
        return;
    }
    const QString normalized = modeText.trimmed().toLower();
    m_previewViewMode = (normalized.contains(QStringLiteral("audio")))
                            ? QStringLiteral("audio")
                            : QStringLiteral("video");
    if (m_preview) {
        m_preview->setViewMode(m_previewViewMode == QStringLiteral("audio")
                                   ? PreviewSurface::ViewMode::Audio
                                   : PreviewSurface::ViewMode::Video);
    }
    if (!m_loadingState) {
        scheduleSaveState();
    }
}

void EditorWindow::openAudioToolsDialog()
{
    if (!m_featureAudioDynamicsTools) {
        QMessageBox::information(this,
                                 QStringLiteral("Audio Dynamics"),
                                 QStringLiteral("Audio dynamics tools are disabled by feature flag."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Audio Dynamics"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto makeSpin = [&dialog](qreal min, qreal max, qreal step, int decimals, qreal value) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setValue(value);
        return spin;
    };

    auto* normalizeCheck = new QCheckBox(QStringLiteral("Normalize"), &dialog);
    normalizeCheck->setChecked(m_previewAudioDynamics.normalizeEnabled);
    auto* normalizeTarget = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.normalizeTargetDb);
    auto* peakReductionCheck = new QCheckBox(QStringLiteral("Peak Reduction"), &dialog);
    peakReductionCheck->setChecked(m_previewAudioDynamics.peakReductionEnabled);
    auto* peakThreshold = makeSpin(-24.0, 0.0, 0.5, 1, m_previewAudioDynamics.peakThresholdDb);
    auto* limiterCheck = new QCheckBox(QStringLiteral("Limiter"), &dialog);
    limiterCheck->setChecked(m_previewAudioDynamics.limiterEnabled);
    auto* limiterThreshold = makeSpin(-12.0, 0.0, 0.1, 1, m_previewAudioDynamics.limiterThresholdDb);
    auto* compressorCheck = new QCheckBox(QStringLiteral("Compressor"), &dialog);
    compressorCheck->setChecked(m_previewAudioDynamics.compressorEnabled);
    auto* compressorThreshold =
        makeSpin(-30.0, -1.0, 0.5, 1, m_previewAudioDynamics.compressorThresholdDb);
    auto* compressorRatio = makeSpin(1.0, 20.0, 0.1, 1, m_previewAudioDynamics.compressorRatio);

    auto* form = new QFormLayout;
    form->addRow(normalizeCheck, normalizeTarget);
    form->addRow(peakReductionCheck, peakThreshold);
    form->addRow(limiterCheck, limiterThreshold);
    form->addRow(compressorCheck, compressorThreshold);
    form->addRow(QStringLiteral("Compressor Ratio"), compressorRatio);
    layout->addLayout(form);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"), &dialog);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(applyBtn);
    layout->addLayout(btnRow);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(applyBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_previewAudioDynamics.normalizeEnabled = normalizeCheck->isChecked();
    m_previewAudioDynamics.normalizeTargetDb = normalizeTarget->value();
    m_previewAudioDynamics.peakReductionEnabled = peakReductionCheck->isChecked();
    m_previewAudioDynamics.peakThresholdDb = peakThreshold->value();
    m_previewAudioDynamics.limiterEnabled = limiterCheck->isChecked();
    m_previewAudioDynamics.limiterThresholdDb = limiterThreshold->value();
    m_previewAudioDynamics.compressorEnabled = compressorCheck->isChecked();
    m_previewAudioDynamics.compressorThresholdDb = compressorThreshold->value();
    m_previewAudioDynamics.compressorRatio = compressorRatio->value();

    if (m_preview) {
        m_preview->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    scheduleSaveState();
}


void EditorWindow::renderTimelineFromOutputRequest(const RenderRequest &request)
{
    RenderRequest effectiveRequest = request;
    effectiveRequest.correctionsEnabled = m_correctionsEnabled;
    if (effectiveRequest.useProxyMedia)
    {
        for (TimelineClip &clip : effectiveRequest.clips)
        {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty())
            {
                clip.filePath = proxyPath;
            }
        }
    }

    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment &range : std::as_const(effectiveRequest.exportRanges))
    {
        totalFramesToRender += qMax<int64_t>(0, range.endFrame - range.startFrame + 1);
    }
    if (totalFramesToRender <= 0)
    {
        totalFramesToRender = qMax<int64_t>(1, effectiveRequest.exportEndFrame - effectiveRequest.exportStartFrame + 1);
    }

    const bool verticalRenderOutput =
        effectiveRequest.outputSize.height() > effectiveRequest.outputSize.width();

    QDialog progressDialog(this);
    progressDialog.setWindowTitle(QStringLiteral("Render Export"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumWidth(verticalRenderOutput ? 920 : 560);
    progressDialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f3ee; }"
        "QLabel { color: #1f2430; font-size: 13px; }"
        "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
        "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
        "QPushButton { min-width: 96px; padding: 6px 14px; }"));
    auto *progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(16, 16, 16, 16);
    progressLayout->setSpacing(10);

    auto *renderPreviewLabel = new QLabel(&progressDialog);
    renderPreviewLabel->setAlignment(Qt::AlignCenter);
    renderPreviewLabel->setMinimumSize(360, 202);
    renderPreviewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #11151c; color: #d9e1ea; border: 1px solid #c9c2b8; border-radius: 6px; }"));
    renderPreviewLabel->setText(QStringLiteral("Waiting for first rendered frame..."));

    auto *renderStatusLabel = new QLabel(QStringLiteral("Preparing render..."), &progressDialog);
    renderStatusLabel->setWordWrap(true);
    renderStatusLabel->setAlignment(Qt::AlignCenter);

    auto *showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), &progressDialog);
    showRenderPreviewCheckBox->setChecked(true);

    auto *renderSourcesLabel = new QLabel(QStringLiteral("Sources In Use (Current Frame)"), &progressDialog);
    renderSourcesLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto *renderSourcesList = new QPlainTextEdit(&progressDialog);
    renderSourcesList->setReadOnly(true);
    renderSourcesList->setMinimumHeight(140);
    renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));

    if (verticalRenderOutput) {
        auto *contentRow = new QHBoxLayout;
        contentRow->setSpacing(12);

        auto *leftColumn = new QVBoxLayout;
        leftColumn->setSpacing(10);
        leftColumn->addWidget(renderStatusLabel);
        leftColumn->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        leftColumn->addWidget(renderSourcesLabel);
        leftColumn->addWidget(renderSourcesList, 1);

        auto *rightColumn = new QVBoxLayout;
        rightColumn->setSpacing(10);
        rightColumn->addWidget(renderPreviewLabel, 1);

        contentRow->addLayout(leftColumn, 3);
        contentRow->addLayout(rightColumn, 2);
        progressLayout->addLayout(contentRow);
    } else {
        progressLayout->addWidget(renderStatusLabel);
        progressLayout->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        progressLayout->addWidget(renderPreviewLabel);
        progressLayout->addWidget(renderSourcesLabel);
        progressLayout->addWidget(renderSourcesList);
    }

    auto *renderProgressBar = new QProgressBar(&progressDialog);
    renderProgressBar->setRange(0, static_cast<int>(qMin<int64_t>(totalFramesToRender, std::numeric_limits<int>::max())));
    renderProgressBar->setValue(0);
    progressLayout->addWidget(renderProgressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *cancelRenderButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    buttonRow->addWidget(cancelRenderButton);
    progressLayout->addLayout(buttonRow);

    bool renderCancelled = false;
    QObject::connect(cancelRenderButton, &QPushButton::clicked, &progressDialog, [&renderCancelled, cancelRenderButton]() {
        renderCancelled = true;
        cancelRenderButton->setEnabled(false);
    });
    QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, &progressDialog, [renderPreviewLabel](bool checked) {
        renderPreviewLabel->setVisible(checked);
    });
    progressDialog.show();

    const QString outputPath = effectiveRequest.outputPath;
    const auto formatEta = [](qint64 remainingMs) -> QString
    {
        if (remainingMs <= 0)
        {
            return QStringLiteral("calculating...");
        }
        const qint64 totalSeconds = remainingMs / 1000;
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
        {
            return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
        }
        if (minutes > 0)
        {
            return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
        }
        return QStringLiteral("%1s").arg(seconds);
    };
    const auto stageSummary = [](qint64 stageMs, int64_t completedFrames) -> QString
    {
        if (stageMs <= 0 || completedFrames <= 0)
        {
            return QStringLiteral("0 ms");
        }
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(stageMs)
            .arg(QString::number(static_cast<double>(stageMs) / static_cast<double>(completedFrames), 'f', 2));
    };
    const auto renderProfileFromProgress = [&formatEta](const RenderProgress &progress) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, progress.framesCompleted);
        const double fps = progress.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(progress.framesCompleted)) / static_cast<double>(progress.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("output_path"), QString()},
            {QStringLiteral("frames_completed"), static_cast<qint64>(progress.framesCompleted)},
            {QStringLiteral("total_frames"), static_cast<qint64>(progress.totalFrames)},
            {QStringLiteral("segment_index"), progress.segmentIndex},
            {QStringLiteral("segment_count"), progress.segmentCount},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(progress.timelineFrame)},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(progress.segmentStartFrame)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(progress.segmentEndFrame)},
            {QStringLiteral("using_gpu"), progress.usingGpu},
            {QStringLiteral("using_hardware_encode"), progress.usingHardwareEncode},
            {QStringLiteral("encoder_label"), progress.encoderLabel},
            {QStringLiteral("elapsed_ms"), progress.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), progress.estimatedRemainingMs},
            {QStringLiteral("eta_text"), formatEta(progress.estimatedRemainingMs)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), progress.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), progress.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), progress.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), progress.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), progress.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), progress.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), progress.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), progress.convertStageMs},
            {QStringLiteral("encode_stage_ms"), progress.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), progress.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), progress.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), progress.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), progress.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), progress.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), progress.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), progress.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), progress.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), progress.renderStageTable},
            {QStringLiteral("worst_frame_table"), progress.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(progress.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(progress.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(progress.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(progress.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(progress.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(progress.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(progress.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(progress.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(progress.encodeStageMs) / static_cast<double>(completedFrames)}};
    };
    const auto renderProfileFromResult = [&formatEta, &outputPath](const RenderResult &result) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, result.framesRendered);
        const double fps = result.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(result.framesRendered)) / static_cast<double>(result.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), result.success ? QStringLiteral("completed")
                                                      : (result.cancelled ? QStringLiteral("cancelled")
                                                                          : QStringLiteral("failed"))},
            {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
            {QStringLiteral("frames_completed"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("total_frames"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("segment_index"), 0},
            {QStringLiteral("segment_count"), 0},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(0)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(0)},
            {QStringLiteral("using_gpu"), result.usedGpu},
            {QStringLiteral("using_hardware_encode"), result.usedHardwareEncode},
            {QStringLiteral("encoder_label"), result.encoderLabel},
            {QStringLiteral("elapsed_ms"), result.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), static_cast<qint64>(0)},
            {QStringLiteral("eta_text"), formatEta(0)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), result.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), result.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), result.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), result.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), result.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), result.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), result.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), result.convertStageMs},
            {QStringLiteral("encode_stage_ms"), result.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), result.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), result.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), result.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), result.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), result.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), result.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), result.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), result.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), result.renderStageTable},
            {QStringLiteral("worst_frame_table"), result.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(result.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(result.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(result.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(result.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(result.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(result.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(result.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(result.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(result.encodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("message"), result.message}};
    };
    m_renderInProgress = true;
    m_liveRenderProfile = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("starting")},
        {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
        {QStringLiteral("frames_completed"), static_cast<qint64>(0)},
        {QStringLiteral("total_frames"), static_cast<qint64>(totalFramesToRender)}};
    refreshProfileInspector();

    const auto activeRenderSourcesText = [&effectiveRequest](int64_t timelineFrame) -> QString {
        QStringList lines;
        lines.reserve(effectiveRequest.clips.size() * 2);
        for (const TimelineClip& clip : effectiveRequest.clips) {
            if (timelineFrame < clip.startFrame ||
                timelineFrame >= clip.startFrame + qMax<int64_t>(1, clip.durationFrames)) {
                continue;
            }
            const QString clipLabel = clip.label.isEmpty() ? QStringLiteral("(unnamed clip)") : clip.label;
            if (clipVisualPlaybackEnabled(clip, effectiveRequest.tracks) && !clip.filePath.isEmpty()) {
                lines.push_back(QStringLiteral("V | %1 | %2")
                                    .arg(clipLabel, QDir::toNativeSeparators(clip.filePath)));
            }
            if (clipAudioPlaybackEnabled(clip)) {
                const QString audioPath = playbackAudioPathForClip(clip);
                if (!audioPath.isEmpty()) {
                    lines.push_back(QStringLiteral("A | %1 | %2")
                                        .arg(clipLabel, QDir::toNativeSeparators(audioPath)));
                }
            }
        }
        if (lines.isEmpty()) {
            return QStringLiteral("No active clip sources at this frame.");
        }
        lines.removeDuplicates();
        std::sort(lines.begin(), lines.end());
        return lines.join(QLatin1Char('\n'));
    };

    const RenderResult result = renderTimelineToFile(
        effectiveRequest,
        [this, &progressDialog, renderStatusLabel, renderProgressBar, renderPreviewLabel,
         renderSourcesList, showRenderPreviewCheckBox, &renderCancelled,
         formatEta, stageSummary, renderProfileFromProgress, outputPath,
         activeRenderSourcesText](const RenderProgress &progress)
        {
            renderProgressBar->setMaximum(qMax(1, static_cast<int>(qMin<int64_t>(progress.totalFrames, std::numeric_limits<int>::max()))));
            renderProgressBar->setValue(static_cast<int>(qMin<int64_t>(progress.framesCompleted, std::numeric_limits<int>::max())));
            const QString rendererMode = progress.usingGpu ? QStringLiteral("GPU render") : QStringLiteral("CPU render");
            const QString encoderMode = progress.usingHardwareEncode
                                            ? QStringLiteral("Hardware encode")
                                            : QStringLiteral("Software encode");
            const QString encoderLabel = progress.encoderLabel.isEmpty()
                                             ? QStringLiteral("unknown")
                                             : progress.encoderLabel;
            m_liveRenderProfile = renderProfileFromProgress(progress);
            m_liveRenderProfile[QStringLiteral("output_path")] = QDir::toNativeSeparators(outputPath);
            refreshProfileInspector();
            const QString metricsTable = QStringLiteral(
                "<table cellspacing='0' cellpadding='2' style='margin: 0 auto;'>"
                "<tr>"
                "<td align='right'><b>Render</b></td><td>%1</td>"
                "<td align='right'><b>Decode</b></td><td>%2</td>"
                "<td align='right'><b>Texture</b></td><td>%3</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Composite</b></td><td>%4</td>"
                "<td align='right'><b>GPU NV12</b></td><td>%5</td>"
                "<td align='right'><b>Readback</b></td><td>%6</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Convert</b></td><td>%7</td>"
                "<td align='right'><b>Encode</b></td><td>%8</td>"
                "<td></td><td></td>"
                "</tr>"
                "</table>")
                .arg(stageSummary(progress.renderStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderDecodeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderTextureStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderCompositeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderNv12StageMs, progress.framesCompleted))
                .arg(stageSummary(progress.gpuReadbackMs, progress.framesCompleted))
                .arg(stageSummary(progress.convertStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.encodeStageMs, progress.framesCompleted));
            renderStatusLabel->setText(
                QStringLiteral("<b>Rendering frame %1 of %2</b><br>"
                               "Segment %3/%4: %5-%6<br>"
                               "%7 | %8 (%9)<br>"
                               "ETA: %10<br>%11")
                    .arg(progress.framesCompleted + 1)
                    .arg(qMax<int64_t>(1, progress.totalFrames))
                    .arg(progress.segmentIndex)
                    .arg(progress.segmentCount)
                    .arg(progress.segmentStartFrame)
                    .arg(progress.segmentEndFrame)
                    .arg(rendererMode)
                    .arg(encoderMode)
                    .arg(encoderLabel)
                    .arg(formatEta(progress.estimatedRemainingMs))
                    .arg(metricsTable));
            if (showRenderPreviewCheckBox->isChecked() && !progress.previewFrame.isNull())
            {
                const QPixmap pixmap = QPixmap::fromImage(progress.previewFrame).scaled(
                    renderPreviewLabel->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                renderPreviewLabel->setPixmap(pixmap);
                renderPreviewLabel->setText(QString());
            }
            renderSourcesList->setPlainText(activeRenderSourcesText(progress.timelineFrame));
            QCoreApplication::processEvents();
            return !renderCancelled;
        });
    renderProgressBar->setValue(renderProgressBar->maximum());
    progressDialog.close();
    m_renderInProgress = false;
    m_lastRenderProfile = renderProfileFromResult(result);
    m_liveRenderProfile = QJsonObject{};
    refreshProfileInspector();

    if (result.success)
    {
        QMessageBox::information(this, QStringLiteral("Render Complete"), result.message);
        return;
    }

    const QString message = result.message.isEmpty()
                                ? QStringLiteral("Render failed.")
                                : result.message;
    QMessageBox::warning(this,
                         result.cancelled ? QStringLiteral("Render Cancelled") : QStringLiteral("Render Failed"),
                         message);
}

void EditorWindow::exportVideoForSpeakersOnSelectedClip(const QStringList& speakerIds)
{
    if (!m_timeline || speakerIds.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("Select a clip first."));
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript not found for the selected clip."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc =
        QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript JSON is invalid for the selected clip."));
        return;
    }

    auto appendMergedRange = [](QVector<ExportRangeSegment>& ranges, int64_t startFrame, int64_t endFrame) {
        if (endFrame < startFrame) {
            return;
        }
        if (ranges.isEmpty() || startFrame > ranges.constLast().endFrame + 1) {
            ranges.push_back(ExportRangeSegment{startFrame, endFrame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, endFrame);
    };

    QSet<QString> selectedSpeakerSet;
    selectedSpeakerSet.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        const QString trimmed = speakerId.trimmed();
        if (!trimmed.isEmpty()) {
            selectedSpeakerSet.insert(trimmed);
        }
    }
    if (selectedSpeakerSet.isEmpty()) {
        return;
    }

    QVector<ExportRangeSegment> sourceWordRanges;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (wordText.isEmpty()) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (!selectedSpeakerSet.contains(wordSpeaker)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            appendMergedRange(sourceWordRanges, startFrame, endFrame);
        }
    }
    if (sourceWordRanges.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("No spoken words found for the selected speakers in this clip."));
        return;
    }

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    QVector<ExportRangeSegment> timelineRanges;
    timelineRanges.reserve(sourceWordRanges.size());
    int sourceRangeIndex = 0;
    const int64_t clipStartFrame = clip->startFrame;
    const int64_t clipEndFrame = clip->startFrame + clip->durationFrames - 1;
    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            *clip, frameToSamples(timelineFrame), markers);
        while (sourceRangeIndex < sourceWordRanges.size() &&
               sourceWordRanges.at(sourceRangeIndex).endFrame < transcriptFrame) {
            ++sourceRangeIndex;
        }
        if (sourceRangeIndex >= sourceWordRanges.size()) {
            break;
        }
        const ExportRangeSegment& sourceRange = sourceWordRanges.at(sourceRangeIndex);
        if (transcriptFrame < sourceRange.startFrame || transcriptFrame > sourceRange.endFrame) {
            continue;
        }
        appendMergedRange(timelineRanges, timelineFrame, timelineFrame);
    }
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Video"),
            QStringLiteral("Could not map selected speaker words to timeline frames."));
        return;
    }

    setPlaybackActive(false);

    RenderRequest request = buildRenderRequestFromOutputControls();
    const QString suggestedBase = QStringLiteral("speaker_export_%1")
        .arg(selectedSpeakerSet.size() == 1 ? *selectedSpeakerSet.constBegin()
                                            : QStringLiteral("multi"));
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Video"),
        QDir::current().filePath(QStringLiteral("%1.%2").arg(suggestedBase, request.outputFormat)),
        QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
    if (selectedPath.isEmpty()) {
        return;
    }

    request.outputPath = selectedPath;
    m_lastRenderOutputPath = selectedPath;
    scheduleSaveState();

    request.clips = m_timeline->clips();
    request.tracks = m_timeline->tracks();
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;
    renderTimelineFromOutputRequest(request);
}
