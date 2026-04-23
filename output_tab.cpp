#include "output_tab.h"
#include "debug_controls.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSignalBlocker>

OutputTab::OutputTab(const Widgets& widgets, const Dependencies& deps, QObject* parent)
    : QObject(parent)
    , m_widgets(widgets)
    , m_deps(deps)
{
    // Initialize output format combo box with video formats only
    if (m_widgets.outputFormatCombo) {
        m_widgets.outputFormatCombo->clear();
        // Video formats only - no image formats
        m_widgets.outputFormatCombo->addItem("MP4", "mp4");
        m_widgets.outputFormatCombo->addItem("MOV", "mov");
        m_widgets.outputFormatCombo->addItem("AVI", "avi");
        m_widgets.outputFormatCombo->addItem("MKV", "mkv");
        m_widgets.outputFormatCombo->addItem("WebM", "webm");
        // Set default to MP4
        m_widgets.outputFormatCombo->setCurrentIndex(0);
    }
    
    // Initialize image sequence format combo box
    if (m_widgets.imageSequenceFormatCombo) {
        m_widgets.imageSequenceFormatCombo->clear();
        m_widgets.imageSequenceFormatCombo->addItem("JPEG", "jpeg");
        m_widgets.imageSequenceFormatCombo->addItem("WEBP", "webp");
        m_widgets.imageSequenceFormatCombo->setCurrentIndex(0);
        // Initially disabled until checkbox is checked
        m_widgets.imageSequenceFormatCombo->setEnabled(false);
    }
    
    // Set checkbox label if not already set
    if (m_widgets.createImageSequenceCheckBox) {
        m_widgets.createImageSequenceCheckBox->setText("Create intermediate image sequence");
    }
}

void OutputTab::wire()
{
    if (m_widgets.outputWidthSpin) {
        connect(m_widgets.outputWidthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputWidthChanged);
    }
    if (m_widgets.outputHeightSpin) {
        connect(m_widgets.outputHeightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputHeightChanged);
    }
    if (m_widgets.exportStartSpin) {
        connect(m_widgets.exportStartSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onExportStartChanged);
    }
    if (m_widgets.exportEndSpin) {
        connect(m_widgets.exportEndSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onExportEndChanged);
    }
    if (m_widgets.outputFormatCombo) {
        connect(m_widgets.outputFormatCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &OutputTab::onOutputFormatChanged);
    }
    if (m_widgets.renderUseProxiesCheckBox) {
        connect(m_widgets.renderUseProxiesCheckBox, &QCheckBox::toggled,
                this, &OutputTab::onRenderUseProxiesToggled);
    }
    if (m_widgets.outputPlaybackCacheFallbackCheckBox) {
        connect(m_widgets.outputPlaybackCacheFallbackCheckBox, &QCheckBox::toggled,
                this, &OutputTab::onOutputPlaybackCacheFallbackToggled);
    }
    if (m_widgets.outputLeadPrefetchEnabledCheckBox) {
        connect(m_widgets.outputLeadPrefetchEnabledCheckBox, &QCheckBox::toggled,
                this, &OutputTab::onOutputLeadPrefetchEnabledToggled);
    }
    if (m_widgets.outputLeadPrefetchCountSpin) {
        connect(m_widgets.outputLeadPrefetchCountSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputLeadPrefetchCountChanged);
    }
    if (m_widgets.outputPlaybackWindowAheadSpin) {
        connect(m_widgets.outputPlaybackWindowAheadSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputPlaybackWindowAheadChanged);
    }
    if (m_widgets.outputVisibleQueueReserveSpin) {
        connect(m_widgets.outputVisibleQueueReserveSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputVisibleQueueReserveChanged);
    }
    if (m_widgets.outputPrefetchMaxQueueDepthSpin) {
        connect(m_widgets.outputPrefetchMaxQueueDepthSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputPrefetchMaxQueueDepthChanged);
    }
    if (m_widgets.outputPrefetchMaxInflightSpin) {
        connect(m_widgets.outputPrefetchMaxInflightSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputPrefetchMaxInflightChanged);
    }
    if (m_widgets.outputPrefetchMaxPerTickSpin) {
        connect(m_widgets.outputPrefetchMaxPerTickSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputPrefetchMaxPerTickChanged);
    }
    if (m_widgets.outputPrefetchSkipVisiblePendingThresholdSpin) {
        connect(m_widgets.outputPrefetchSkipVisiblePendingThresholdSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputPrefetchSkipVisiblePendingThresholdChanged);
    }
    if (m_widgets.outputDecoderLaneCountSpin) {
        connect(m_widgets.outputDecoderLaneCountSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onOutputDecoderLaneCountChanged);
    }
    if (m_widgets.outputDecodeModeCombo) {
        connect(m_widgets.outputDecodeModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &OutputTab::onOutputDecodeModeChanged);
    }
    if (m_widgets.outputDeterministicPipelineCheckBox) {
        connect(m_widgets.outputDeterministicPipelineCheckBox, &QCheckBox::toggled,
                this, &OutputTab::onOutputDeterministicPipelineToggled);
    }
    if (m_widgets.outputResetPipelineDefaultsButton) {
        connect(m_widgets.outputResetPipelineDefaultsButton, &QPushButton::clicked,
                this, &OutputTab::onOutputResetPipelineDefaultsClicked);
    }
    if (m_widgets.autosaveIntervalMinutesSpin) {
        connect(m_widgets.autosaveIntervalMinutesSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onAutosaveIntervalMinutesChanged);
    }
    if (m_widgets.autosaveMaxBackupsSpin) {
        connect(m_widgets.autosaveMaxBackupsSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, &OutputTab::onAutosaveMaxBackupsChanged);
    }
    if (m_widgets.createImageSequenceCheckBox) {
        connect(m_widgets.createImageSequenceCheckBox, &QCheckBox::toggled,
                this, [this](bool checked) {
                    if (m_widgets.imageSequenceFormatCombo) {
                        m_widgets.imageSequenceFormatCombo->setEnabled(checked);
                    }
                });
    }
    if (m_widgets.renderButton) {
        connect(m_widgets.renderButton, &QPushButton::clicked,
                this, &OutputTab::onRenderClicked);
    }
}

void OutputTab::refresh()
{
    if (m_updating ||
        !m_widgets.outputWidthSpin ||
        !m_widgets.outputHeightSpin ||
        !m_widgets.exportStartSpin ||
        !m_widgets.exportEndSpin) {
        return;
    }

    m_updating = true;

    const bool hasTimeline = m_deps.hasTimeline && m_deps.hasTimeline();
    const QVector<ExportRangeSegment> ranges =
        m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges() : QVector<ExportRangeSegment>{};
    const int64_t startFrame = ranges.isEmpty() ? 0 : ranges.constFirst().startFrame;
    const int64_t endFrame = ranges.isEmpty() ? 0 : ranges.constLast().endFrame;

    {
        QSignalBlocker startBlocker(m_widgets.exportStartSpin);
        QSignalBlocker endBlocker(m_widgets.exportEndSpin);
        m_widgets.exportStartSpin->setEnabled(hasTimeline);
        m_widgets.exportEndSpin->setEnabled(hasTimeline);
        m_widgets.exportStartSpin->setValue(static_cast<int>(startFrame));
        m_widgets.exportEndSpin->setValue(static_cast<int>(endFrame));
    }
    if (m_widgets.autosaveIntervalMinutesSpin && m_deps.autosaveIntervalMinutes) {
        QSignalBlocker blocker(m_widgets.autosaveIntervalMinutesSpin);
        m_widgets.autosaveIntervalMinutesSpin->setValue(m_deps.autosaveIntervalMinutes());
    }
    if (m_widgets.autosaveMaxBackupsSpin && m_deps.autosaveMaxBackups) {
        QSignalBlocker blocker(m_widgets.autosaveMaxBackupsSpin);
        m_widgets.autosaveMaxBackupsSpin->setValue(m_deps.autosaveMaxBackups());
    }
    if (m_widgets.outputPlaybackCacheFallbackCheckBox) {
        QSignalBlocker blocker(m_widgets.outputPlaybackCacheFallbackCheckBox);
        m_widgets.outputPlaybackCacheFallbackCheckBox->setChecked(editor::debugPlaybackCacheFallbackEnabled());
    }
    if (m_widgets.outputLeadPrefetchEnabledCheckBox) {
        QSignalBlocker blocker(m_widgets.outputLeadPrefetchEnabledCheckBox);
        m_widgets.outputLeadPrefetchEnabledCheckBox->setChecked(editor::debugLeadPrefetchEnabled());
    }
    if (m_widgets.outputLeadPrefetchCountSpin) {
        QSignalBlocker blocker(m_widgets.outputLeadPrefetchCountSpin);
        m_widgets.outputLeadPrefetchCountSpin->setValue(editor::debugLeadPrefetchCount());
        if (m_widgets.outputLeadPrefetchEnabledCheckBox) {
            m_widgets.outputLeadPrefetchCountSpin->setEnabled(
                m_widgets.outputLeadPrefetchEnabledCheckBox->isChecked());
        }
    }
    if (m_widgets.outputPlaybackWindowAheadSpin) {
        QSignalBlocker blocker(m_widgets.outputPlaybackWindowAheadSpin);
        m_widgets.outputPlaybackWindowAheadSpin->setValue(editor::debugPlaybackWindowAhead());
    }
    if (m_widgets.outputVisibleQueueReserveSpin) {
        QSignalBlocker blocker(m_widgets.outputVisibleQueueReserveSpin);
        m_widgets.outputVisibleQueueReserveSpin->setValue(editor::debugVisibleQueueReserve());
    }
    if (m_widgets.outputPrefetchMaxQueueDepthSpin) {
        QSignalBlocker blocker(m_widgets.outputPrefetchMaxQueueDepthSpin);
        m_widgets.outputPrefetchMaxQueueDepthSpin->setValue(editor::debugPrefetchMaxQueueDepth());
    }
    if (m_widgets.outputPrefetchMaxInflightSpin) {
        QSignalBlocker blocker(m_widgets.outputPrefetchMaxInflightSpin);
        m_widgets.outputPrefetchMaxInflightSpin->setValue(editor::debugPrefetchMaxInflight());
    }
    if (m_widgets.outputPrefetchMaxPerTickSpin) {
        QSignalBlocker blocker(m_widgets.outputPrefetchMaxPerTickSpin);
        m_widgets.outputPrefetchMaxPerTickSpin->setValue(editor::debugPrefetchMaxPerTick());
    }
    if (m_widgets.outputPrefetchSkipVisiblePendingThresholdSpin) {
        QSignalBlocker blocker(m_widgets.outputPrefetchSkipVisiblePendingThresholdSpin);
        m_widgets.outputPrefetchSkipVisiblePendingThresholdSpin->setValue(editor::debugPrefetchSkipVisiblePendingThreshold());
    }
    if (m_widgets.outputDecoderLaneCountSpin) {
        QSignalBlocker blocker(m_widgets.outputDecoderLaneCountSpin);
        m_widgets.outputDecoderLaneCountSpin->setValue(editor::debugDecoderLaneCount());
    }
    if (m_widgets.outputDecodeModeCombo) {
        QSignalBlocker blocker(m_widgets.outputDecodeModeCombo);
        const QString decodeMode = editor::decodePreferenceToString(editor::debugDecodePreference());
        const int index = m_widgets.outputDecodeModeCombo->findData(decodeMode);
        if (index >= 0) {
            m_widgets.outputDecodeModeCombo->setCurrentIndex(index);
        }
    }
    if (m_widgets.outputDeterministicPipelineCheckBox) {
        QSignalBlocker blocker(m_widgets.outputDeterministicPipelineCheckBox);
        m_widgets.outputDeterministicPipelineCheckBox->setChecked(
            editor::debugDeterministicPipelineEnabled());
    }

    updateRangeSummary();
    updateRenderButtonState();
    m_updating = false;
}

void OutputTab::applyRangeFromInspector()
{
    if (m_updating ||
        !m_deps.hasTimeline || !m_deps.hasTimeline() ||
        !m_widgets.exportStartSpin ||
        !m_widgets.exportEndSpin ||
        !m_deps.setExportRange) {
        return;
    }

    const int64_t startFrame = qMin<int64_t>(m_widgets.exportStartSpin->value(),
                                             m_widgets.exportEndSpin->value());
    const int64_t endFrame = qMax<int64_t>(m_widgets.exportStartSpin->value(),
                                           m_widgets.exportEndSpin->value());
    m_deps.setExportRange(startFrame, endFrame);
    refresh();
}

void OutputTab::renderFromInspector()
{
    if (!m_deps.hasTimeline || !m_deps.hasTimeline() ||
        !m_deps.hasClips || !m_deps.hasClips() ||
        !m_deps.getTimelineClips ||
        !m_deps.renderTimeline) {
        return;
    }

    if (m_deps.stopPlayback) {
        m_deps.stopPlayback();
    }

    RenderRequest request;
    request.outputFormat = m_widgets.outputFormatCombo
        ? m_widgets.outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }

    const QString defaultFileName = QStringLiteral("render.%1").arg(request.outputFormat);
    QString defaultPath = defaultFileName;
    QString rememberedPath;
    if (m_deps.lastRenderOutputPath) {
        const QString previousPath = m_deps.lastRenderOutputPath();
        if (!previousPath.isEmpty()) {
            QFileInfo previousInfo(previousPath);
            const QString completeBaseName = previousInfo.completeBaseName().isEmpty()
                                                 ? QStringLiteral("render")
                                                 : previousInfo.completeBaseName();
            defaultPath = previousInfo.dir().filePath(QStringLiteral("%1.%2").arg(completeBaseName, request.outputFormat));
            rememberedPath = defaultPath;
        }
    }

    QString selectedPath = rememberedPath;
    if (selectedPath.isEmpty()) {
        selectedPath = QFileDialog::getSaveFileName(
            nullptr,
            QStringLiteral("Render Output"),
            defaultPath,
            QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
        if (selectedPath.isEmpty()) {
            return;
        }
    } else if (QFileInfo::exists(selectedPath)) {
        QMessageBox prompt;
        prompt.setIcon(QMessageBox::Question);
        prompt.setWindowTitle(QStringLiteral("Render Output"));
        prompt.setText(QStringLiteral("Use the previous render target?"));
        prompt.setInformativeText(QDir::toNativeSeparators(selectedPath));
        QPushButton* overwriteButton = prompt.addButton(QStringLiteral("Overwrite"), QMessageBox::AcceptRole);
        QPushButton* chooseNewButton = prompt.addButton(QStringLiteral("Choose New..."), QMessageBox::ActionRole);
        prompt.addButton(QMessageBox::Cancel);
        prompt.setDefaultButton(overwriteButton);
        prompt.exec();

        if (prompt.clickedButton() == chooseNewButton) {
            selectedPath = QFileDialog::getSaveFileName(
                nullptr,
                QStringLiteral("Render Output"),
                defaultPath,
                QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
            if (selectedPath.isEmpty()) {
                return;
            }
        } else if (prompt.clickedButton() != overwriteButton) {
            return;
        }
    }
    request.outputPath = selectedPath;
    if (m_deps.setLastRenderOutputPath) {
        m_deps.setLastRenderOutputPath(selectedPath);
    }

    request.outputSize = QSize(
        m_widgets.outputWidthSpin ? m_widgets.outputWidthSpin->value() : 1080,
        m_widgets.outputHeightSpin ? m_widgets.outputHeightSpin->value() : 1920);
    request.useProxyMedia = m_widgets.renderUseProxiesCheckBox &&
                            m_widgets.renderUseProxiesCheckBox->isChecked();
    
    // Image sequence settings
    request.createVideoFromImageSequence = m_widgets.createImageSequenceCheckBox &&
                                           m_widgets.createImageSequenceCheckBox->isChecked();
    if (request.createVideoFromImageSequence && m_widgets.imageSequenceFormatCombo) {
        request.imageSequenceFormat = m_widgets.imageSequenceFormatCombo->currentData().toString();
        if (request.imageSequenceFormat.isEmpty()) {
            request.imageSequenceFormat = "jpeg";  // Default to JPEG
        }
    }
    
    request.clips = m_deps.getTimelineClips();
    request.tracks = m_deps.getTimelineTracks ? m_deps.getTimelineTracks() : QVector<TimelineTrack>{};
    request.renderSyncMarkers = m_deps.getRenderSyncMarkers
        ? m_deps.getRenderSyncMarkers()
        : QVector<RenderSyncMarker>{};
    request.exportRanges = m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges()
                                                          : QVector<ExportRangeSegment>{};
    request.exportStartFrame = request.exportRanges.isEmpty()
        ? (m_deps.exportStartFrame ? m_deps.exportStartFrame() : 0)
        : request.exportRanges.constFirst().startFrame;
    request.exportEndFrame = request.exportRanges.isEmpty()
        ? (m_deps.exportEndFrame ? m_deps.exportEndFrame() : 0)
        : request.exportRanges.constLast().endFrame;

    m_deps.renderTimeline(request);
}

void OutputTab::onOutputWidthChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setOutputSize) {
        m_deps.setOutputSize(QSize(value, m_widgets.outputHeightSpin ? m_widgets.outputHeightSpin->value() : 1920));
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onOutputHeightChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setOutputSize) {
        m_deps.setOutputSize(QSize(m_widgets.outputWidthSpin ? m_widgets.outputWidthSpin->value() : 1080, value));
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onExportStartChanged(int value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyRangeFromInspector();
}

void OutputTab::onExportEndChanged(int value)
{
    Q_UNUSED(value);
    if (m_updating) return;
    applyRangeFromInspector();
}

void OutputTab::onOutputFormatChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating) return;
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onRenderClicked()
{
    renderFromInspector();
}

void OutputTab::onRenderUseProxiesToggled(bool checked)
{
    Q_UNUSED(checked);
    if (m_updating) return;
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onOutputPlaybackCacheFallbackToggled(bool checked)
{
    if (m_updating) return;
    editor::setDebugPlaybackCacheFallbackEnabled(checked);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputLeadPrefetchEnabledToggled(bool checked)
{
    if (m_updating) return;
    editor::setDebugLeadPrefetchEnabled(checked);
    if (m_widgets.outputLeadPrefetchCountSpin) {
        m_widgets.outputLeadPrefetchCountSpin->setEnabled(checked);
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputLeadPrefetchCountChanged(int value)
{
    if (m_updating) return;
    editor::setDebugLeadPrefetchCount(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputPlaybackWindowAheadChanged(int value)
{
    if (m_updating) return;
    editor::setDebugPlaybackWindowAhead(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputVisibleQueueReserveChanged(int value)
{
    if (m_updating) return;
    editor::setDebugVisibleQueueReserve(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputPrefetchMaxQueueDepthChanged(int value)
{
    if (m_updating) return;
    editor::setDebugPrefetchMaxQueueDepth(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputPrefetchMaxInflightChanged(int value)
{
    if (m_updating) return;
    editor::setDebugPrefetchMaxInflight(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputPrefetchMaxPerTickChanged(int value)
{
    if (m_updating) return;
    editor::setDebugPrefetchMaxPerTick(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputPrefetchSkipVisiblePendingThresholdChanged(int value)
{
    if (m_updating) return;
    editor::setDebugPrefetchSkipVisiblePendingThreshold(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputDecoderLaneCountChanged(int value)
{
    if (m_updating) return;
    editor::setDebugDecoderLaneCount(value);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputDecodeModeChanged(int index)
{
    Q_UNUSED(index);
    if (m_updating || !m_widgets.outputDecodeModeCombo) return;
    editor::DecodePreference preference = editor::DecodePreference::Auto;
    if (!editor::parseDecodePreference(m_widgets.outputDecodeModeCombo->currentData().toString(), &preference)) {
        return;
    }
    editor::setDebugDecodePreference(preference);
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputDeterministicPipelineToggled(bool checked)
{
    if (m_updating) return;
    editor::setDebugDeterministicPipelineEnabled(checked);
    if (checked) {
        // Deterministic profile: remove queueing/thread nondeterminism.
        editor::setDebugPlaybackCacheFallbackEnabled(false);
        editor::setDebugLeadPrefetchEnabled(false);
        editor::setDebugLeadPrefetchCount(0);
        editor::setDebugPlaybackWindowAhead(1);
        editor::setDebugVisibleQueueReserve(0);
        editor::setDebugPrefetchMaxQueueDepth(1);
        editor::setDebugPrefetchMaxInflight(1);
        editor::setDebugPrefetchMaxPerTick(1);
        editor::setDebugPrefetchSkipVisiblePendingThreshold(0);
        editor::setDebugDecoderLaneCount(1);
    }
    refresh();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onOutputResetPipelineDefaultsClicked()
{
    if (m_updating) return;

    const editor::RenderPipelineDefaults defaults =
        editor::defaultRenderPipelineDefaultsForCurrentSystem();
    editor::setDebugDecodePreference(defaults.decodePreference);
    editor::setDebugDeterministicPipelineEnabled(defaults.deterministicPipeline);
    editor::setDebugPlaybackCacheFallbackEnabled(defaults.playbackCacheFallback);
    editor::setDebugLeadPrefetchEnabled(defaults.leadPrefetchEnabled);
    editor::setDebugLeadPrefetchCount(defaults.leadPrefetchCount);
    editor::setDebugPlaybackWindowAhead(defaults.playbackWindowAhead);
    editor::setDebugVisibleQueueReserve(defaults.visibleQueueReserve);
    editor::setDebugPrefetchMaxQueueDepth(defaults.prefetchMaxQueueDepth);
    editor::setDebugPrefetchMaxInflight(defaults.prefetchMaxInflight);
    editor::setDebugPrefetchMaxPerTick(defaults.prefetchMaxPerTick);
    editor::setDebugPrefetchSkipVisiblePendingThreshold(defaults.prefetchSkipVisiblePendingThreshold);
    editor::setDebugDecoderLaneCount(defaults.decoderLaneCount);

    refresh();
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
}

void OutputTab::onAutosaveIntervalMinutesChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setAutosaveIntervalMinutes) {
        m_deps.setAutosaveIntervalMinutes(value);
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::onAutosaveMaxBackupsChanged(int value)
{
    if (m_updating) return;
    if (m_deps.setAutosaveMaxBackups) {
        m_deps.setAutosaveMaxBackups(value);
    }
    if (m_deps.scheduleSaveState) m_deps.scheduleSaveState();
    if (m_deps.pushHistorySnapshot) m_deps.pushHistorySnapshot();
}

void OutputTab::updateRangeSummary()
{
    if (!m_widgets.outputRangeSummaryLabel) return;

    const QVector<ExportRangeSegment> ranges =
        m_deps.effectivePlaybackRanges ? m_deps.effectivePlaybackRanges() : QVector<ExportRangeSegment>{};
    if (ranges.isEmpty()) {
        m_widgets.outputRangeSummaryLabel->setText(QStringLiteral("Timeline export range: none"));
        return;
    }

    QStringList segments;
    segments.reserve(ranges.size());
    for (const ExportRangeSegment& range : ranges) {
        segments.push_back(QStringLiteral("%1-%2").arg(range.startFrame).arg(range.endFrame));
    }

    if (ranges.size() == 1) {
        m_widgets.outputRangeSummaryLabel->setText(
            QStringLiteral("Timeline export range: %1").arg(segments.constFirst()));
    } else {
        m_widgets.outputRangeSummaryLabel->setText(
            QStringLiteral("Timeline export ranges (%1): %2\nStart/End fields show the overall span.")
                .arg(ranges.size())
                .arg(segments.join(QStringLiteral(" | "))));
    }
    m_widgets.outputRangeSummaryLabel->setToolTip(m_widgets.outputRangeSummaryLabel->text());
}

void OutputTab::updateRenderButtonState()
{
    if (!m_widgets.renderButton) return;
    const bool enabled = (!m_deps.hasTimeline || m_deps.hasTimeline()) &&
                         (!m_deps.hasClips || m_deps.hasClips());
    m_widgets.renderButton->setEnabled(enabled);
}
