#include "inspector_pane.h"

#include "debug_controls.h"
#include "editor_shared.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QLabel *createTabHeading(const QString &text, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}

QVBoxLayout *createTabLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    return layout;
}

} // namespace

QWidget *InspectorPane::buildOutputTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Output"), page));

    m_outputRangeSummaryLabel = new QLabel(QStringLiteral("Timeline export range: 00:00:00:00 -> 00:00:10:00"), page);
    m_outputRangeSummaryLabel->setWordWrap(true);
    m_outputRangeSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *form = new QFormLayout;
    m_outputWidthSpin = new QSpinBox(page);
    m_outputHeightSpin = new QSpinBox(page);
    m_exportStartSpin = new QSpinBox(page);
    m_exportEndSpin = new QSpinBox(page);
    m_outputFormatCombo = new QComboBox(page);

    m_outputWidthSpin->setRange(16, 7680);
    m_outputWidthSpin->setValue(1080);
    m_outputHeightSpin->setRange(16, 4320);
    m_outputHeightSpin->setValue(1920);
    m_exportStartSpin->setRange(0, 999999);
    m_exportEndSpin->setRange(0, 999999);

    m_outputFormatCombo->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
    m_outputFormatCombo->addItem(QStringLiteral("MOV"), QStringLiteral("mov"));
    m_outputFormatCombo->addItem(QStringLiteral("WebM"), QStringLiteral("webm"));
    m_outputFormatCombo->addItem(QStringLiteral("PNG Sequence"), QStringLiteral("png"));
    m_outputFormatCombo->addItem(QStringLiteral("JPEG Sequence"), QStringLiteral("jpg"));
    m_renderUseProxiesCheckBox = new QCheckBox(QStringLiteral("Use Proxies For Render"), page);
    m_renderCreateVideoFromSequenceCheckBox = new QCheckBox(QStringLiteral("Also Create Video From Sequence"), page);
    m_renderCreateVideoFromSequenceCheckBox->setChecked(true);
    m_renderCreateVideoFromSequenceCheckBox->setToolTip(QStringLiteral("Create a video file from the image sequence with audio"));

    form->addRow(QStringLiteral("Output Width"), m_outputWidthSpin);
    form->addRow(QStringLiteral("Output Height"), m_outputHeightSpin);
    form->addRow(QStringLiteral("Export Start Frame"), m_exportStartSpin);
    form->addRow(QStringLiteral("Export End Frame"), m_exportEndSpin);
    form->addRow(QStringLiteral("Output Format"), m_outputFormatCombo);

    m_autosaveIntervalMinutesSpin = new QSpinBox(page);
    m_autosaveIntervalMinutesSpin->setRange(1, 120);
    m_autosaveIntervalMinutesSpin->setValue(5);
    m_autosaveIntervalMinutesSpin->setSuffix(QStringLiteral(" min"));
    m_autosaveIntervalMinutesSpin->setToolTip(
        QStringLiteral("How often editor state backups are written."));
    form->addRow(QStringLiteral("Autosave Interval"), m_autosaveIntervalMinutesSpin);

    m_autosaveMaxBackupsSpin = new QSpinBox(page);
    m_autosaveMaxBackupsSpin->setRange(1, 200);
    m_autosaveMaxBackupsSpin->setValue(20);
    m_autosaveMaxBackupsSpin->setToolTip(
        QStringLiteral("How many autosave backup files to keep per project."));
    form->addRow(QStringLiteral("Autosave Backups"), m_autosaveMaxBackupsSpin);

    m_backgroundColorButton = new QPushButton(page);
    m_backgroundColorButton->setText(QStringLiteral("Black"));
    m_backgroundColorButton->setToolTip(QStringLiteral("Background color for the rendered output"));
    m_backgroundColorButton->setStyleSheet(
        QStringLiteral("QPushButton { background: #000000; color: #ffffff; "
                        "border: 1px solid #2e3b4a; border-radius: 4px; padding: 4px 8px; }"));
    form->addRow(QStringLiteral("Background"), m_backgroundColorButton);

    m_outputPlaybackCacheFallbackCheckBox =
        new QCheckBox(QStringLiteral("Allow Cached-Frame Fallback During Playback"), page);
    m_outputPlaybackCacheFallbackCheckBox->setChecked(editor::debugPlaybackCacheFallbackEnabled());
    m_outputPlaybackCacheFallbackCheckBox->setToolTip(
        QStringLiteral("Reuse timeline-cache frames when decode misses during playback."));

    m_outputLeadPrefetchEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Lead Prefetch"), page);
    m_outputLeadPrefetchEnabledCheckBox->setChecked(editor::debugLeadPrefetchEnabled());
    m_outputLeadPrefetchEnabledCheckBox->setToolTip(
        QStringLiteral("Prefetch frames ahead of the playhead."));

    auto *decodeForm = new QFormLayout();
    m_outputLeadPrefetchCountSpin = new QSpinBox(page);
    m_outputLeadPrefetchCountSpin->setRange(0, 8);
    m_outputLeadPrefetchCountSpin->setValue(editor::debugLeadPrefetchCount());
    decodeForm->addRow(QStringLiteral("Lead Prefetch Count"), m_outputLeadPrefetchCountSpin);

    m_outputPlaybackWindowAheadSpin = new QSpinBox(page);
    m_outputPlaybackWindowAheadSpin->setRange(1, 24);
    m_outputPlaybackWindowAheadSpin->setValue(editor::debugPlaybackWindowAhead());
    decodeForm->addRow(QStringLiteral("Playback Window Ahead"), m_outputPlaybackWindowAheadSpin);

    m_outputVisibleQueueReserveSpin = new QSpinBox(page);
    m_outputVisibleQueueReserveSpin->setRange(0, 64);
    m_outputVisibleQueueReserveSpin->setValue(editor::debugVisibleQueueReserve());
    decodeForm->addRow(QStringLiteral("Visible Queue Reserve"), m_outputVisibleQueueReserveSpin);

    m_outputPrefetchMaxQueueDepthSpin = new QSpinBox(page);
    m_outputPrefetchMaxQueueDepthSpin->setRange(1, 32);
    m_outputPrefetchMaxQueueDepthSpin->setValue(editor::debugPrefetchMaxQueueDepth());
    decodeForm->addRow(QStringLiteral("Prefetch Max Queue"), m_outputPrefetchMaxQueueDepthSpin);

    m_outputPrefetchMaxInflightSpin = new QSpinBox(page);
    m_outputPrefetchMaxInflightSpin->setRange(1, 16);
    m_outputPrefetchMaxInflightSpin->setValue(editor::debugPrefetchMaxInflight());
    decodeForm->addRow(QStringLiteral("Prefetch Max Inflight"), m_outputPrefetchMaxInflightSpin);

    m_outputPrefetchMaxPerTickSpin = new QSpinBox(page);
    m_outputPrefetchMaxPerTickSpin->setRange(1, 16);
    m_outputPrefetchMaxPerTickSpin->setValue(editor::debugPrefetchMaxPerTick());
    decodeForm->addRow(QStringLiteral("Prefetch Max Per Tick"), m_outputPrefetchMaxPerTickSpin);

    m_outputPrefetchSkipVisiblePendingThresholdSpin = new QSpinBox(page);
    m_outputPrefetchSkipVisiblePendingThresholdSpin->setRange(0, 16);
    m_outputPrefetchSkipVisiblePendingThresholdSpin->setValue(editor::debugPrefetchSkipVisiblePendingThreshold());
    decodeForm->addRow(QStringLiteral("Skip Visible Pending Threshold"),
                       m_outputPrefetchSkipVisiblePendingThresholdSpin);

    m_outputDecoderLaneCountSpin = new QSpinBox(page);
    m_outputDecoderLaneCountSpin->setRange(0, 16);
    m_outputDecoderLaneCountSpin->setSpecialValueText(QStringLiteral("Auto"));
    m_outputDecoderLaneCountSpin->setValue(editor::debugDecoderLaneCount());
    m_outputDecoderLaneCountSpin->setToolTip(
        QStringLiteral("Decoder worker lane count. 0 uses automatic lane count."));
    decodeForm->addRow(QStringLiteral("Decoder Lane Count"), m_outputDecoderLaneCountSpin);

    m_outputDecodeModeCombo = new QComboBox(page);
    m_outputDecodeModeCombo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    m_outputDecodeModeCombo->addItem(QStringLiteral("GPU Upload"), QStringLiteral("hardware"));
    m_outputDecodeModeCombo->addItem(QStringLiteral("GPU Zero-Copy"), QStringLiteral("hardware_zero_copy"));
    m_outputDecodeModeCombo->addItem(QStringLiteral("CPU Software"), QStringLiteral("software"));
    const QString decodeMode = editor::decodePreferenceToString(editor::debugDecodePreference());
    const int decodeModeIndex = m_outputDecodeModeCombo->findData(decodeMode);
    if (decodeModeIndex >= 0) {
        m_outputDecodeModeCombo->setCurrentIndex(decodeModeIndex);
    }
    decodeForm->addRow(QStringLiteral("Render Pipeline"), m_outputDecodeModeCombo);

    m_outputDeterministicPipelineCheckBox =
        new QCheckBox(QStringLiteral("Deterministic Pipeline"), page);
    m_outputDeterministicPipelineCheckBox->setChecked(editor::debugDeterministicPipelineEnabled());
    m_outputDeterministicPipelineCheckBox->setToolTip(
        QStringLiteral("Prioritize reproducible decode/render behavior over throughput."));

    m_outputResetPipelineDefaultsButton =
        new QPushButton(QStringLiteral("Reset Pipeline Defaults"), page);
    m_outputResetPipelineDefaultsButton->setToolTip(
        QStringLiteral("Restore decoder/cache defaults chosen for available hardware and software."));

    m_renderButton = new QPushButton(QStringLiteral("Render"), page);

    layout->addWidget(m_outputRangeSummaryLabel);
    layout->addLayout(form);
    layout->addWidget(m_renderUseProxiesCheckBox);
    layout->addWidget(m_renderCreateVideoFromSequenceCheckBox);
    layout->addSpacing(8);
    layout->addWidget(createTabHeading(QStringLiteral("Decoder + Cache"), page));
    layout->addWidget(m_outputPlaybackCacheFallbackCheckBox);
    layout->addWidget(m_outputLeadPrefetchEnabledCheckBox);
    layout->addLayout(decodeForm);
    layout->addWidget(m_outputDeterministicPipelineCheckBox);
    layout->addWidget(m_outputResetPipelineDefaultsButton);
    layout->addWidget(m_renderButton);
    layout->addStretch(1);

    if (m_outputLeadPrefetchCountSpin && m_outputLeadPrefetchEnabledCheckBox) {
        m_outputLeadPrefetchCountSpin->setEnabled(m_outputLeadPrefetchEnabledCheckBox->isChecked());
    }

    return page;
}

QWidget *InspectorPane::buildPreviewTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Preview"), page));

    auto *summary = new QLabel(QStringLiteral("Preview controls affect only the editor preview."), page);
    summary->setWordWrap(true);

    m_previewHideOutsideOutputCheckBox =
        new QCheckBox(QStringLiteral("Hide Content Outside Output Window"), page);
    m_previewHideOutsideOutputCheckBox->setChecked(false);
    m_previewHideOutsideOutputCheckBox->setToolTip(
        QStringLiteral("Clip the preview to the current output frame so off-frame content is hidden."));

    m_previewShowSpeakerTrackPointsCheckBox =
        new QCheckBox(QStringLiteral("Show BoxStream Points"), page);
    m_previewShowSpeakerTrackPointsCheckBox->setChecked(false);
    m_previewShowSpeakerTrackPointsCheckBox->setToolTip(
        QStringLiteral("Draw all speaker framing keyframe points on top of active clips."));

    // Zoom control section
    auto *zoomSectionLabel = new QLabel(QStringLiteral("Zoom"), page);
    zoomSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; margin-top: 8px;"));
    
    auto *zoomLayout = new QHBoxLayout();
    m_previewZoomSpin = new QDoubleSpinBox(page);
    m_previewZoomSpin->setDecimals(2);
    m_previewZoomSpin->setRange(0.1, 20.0);
    m_previewZoomSpin->setSingleStep(0.1);
    m_previewZoomSpin->setValue(1.0);
    m_previewZoomSpin->setSuffix(QStringLiteral("x"));
    m_previewZoomSpin->setToolTip(
        QStringLiteral("Preview zoom level (0.1x to 20.0x). Use mouse wheel over preview for smooth zoom."));
    
    m_previewZoomResetButton = new QPushButton(QStringLiteral("Reset"), page);
    m_previewZoomResetButton->setToolTip(QStringLiteral("Reset zoom to 1.0x and center the view"));
    
    zoomLayout->addWidget(m_previewZoomSpin);
    zoomLayout->addWidget(m_previewZoomResetButton);
    
    auto *zoomHelpLabel = new QLabel(
        QStringLiteral("Mouse wheel: zoom at cursor position. Drag to pan when zoomed."), page);
    zoomHelpLabel->setWordWrap(true);
    zoomHelpLabel->setStyleSheet(QStringLiteral("color: #6b7a8f; font-size: 11px;"));

    auto *bufferingSectionLabel = new QLabel(QStringLiteral("Playback Buffering"), page);
    bufferingSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; margin-top: 8px;"));

    m_previewPlaybackCacheFallbackCheckBox =
        new QCheckBox(QStringLiteral("Allow Cached-Frame Fallback During Playback"), page);
    m_previewPlaybackCacheFallbackCheckBox->setChecked(editor::debugPlaybackCacheFallbackEnabled());
    m_previewPlaybackCacheFallbackCheckBox->setToolTip(
        QStringLiteral("When playback buffers miss, reuse timeline cache frames so still images remain visible."));

    m_previewLeadPrefetchEnabledCheckBox = new QCheckBox(QStringLiteral("Enable Lead Prefetch"), page);
    m_previewLeadPrefetchEnabledCheckBox->setChecked(editor::debugLeadPrefetchEnabled());
    m_previewLeadPrefetchEnabledCheckBox->setToolTip(
        QStringLiteral("Prefetch frames ahead of playhead to reduce visible misses while playing."));

    auto *bufferingForm = new QFormLayout();
    m_previewLeadPrefetchCountSpin = new QSpinBox(page);
    m_previewLeadPrefetchCountSpin->setRange(0, 8);
    m_previewLeadPrefetchCountSpin->setValue(editor::debugLeadPrefetchCount());
    m_previewLeadPrefetchCountSpin->setToolTip(
        QStringLiteral("How many lead-prefetch requests to schedule (0-8)."));
    bufferingForm->addRow(QStringLiteral("Lead Prefetch Count"), m_previewLeadPrefetchCountSpin);

    m_previewPlaybackWindowAheadSpin = new QSpinBox(page);
    m_previewPlaybackWindowAheadSpin->setRange(1, 24);
    m_previewPlaybackWindowAheadSpin->setValue(editor::debugPlaybackWindowAhead());
    m_previewPlaybackWindowAheadSpin->setToolTip(
        QStringLiteral("Decode window ahead of playhead for playback pipeline clips (1-24)."));
    bufferingForm->addRow(QStringLiteral("Playback Window Ahead"), m_previewPlaybackWindowAheadSpin);

    m_previewVisibleQueueReserveSpin = new QSpinBox(page);
    m_previewVisibleQueueReserveSpin->setRange(0, 64);
    m_previewVisibleQueueReserveSpin->setValue(editor::debugVisibleQueueReserve());
    m_previewVisibleQueueReserveSpin->setToolTip(
        QStringLiteral("Reserved visible-request budget before aggressive prefetch throttling (0-64)."));
    bufferingForm->addRow(QStringLiteral("Visible Queue Reserve"), m_previewVisibleQueueReserveSpin);

    layout->addWidget(summary);
    layout->addWidget(m_previewHideOutsideOutputCheckBox);
    layout->addWidget(m_previewShowSpeakerTrackPointsCheckBox);
    layout->addSpacing(12);
    layout->addWidget(zoomSectionLabel);
    layout->addLayout(zoomLayout);
    layout->addWidget(zoomHelpLabel);
    layout->addSpacing(12);
    layout->addWidget(bufferingSectionLabel);
    layout->addWidget(m_previewPlaybackCacheFallbackCheckBox);
    layout->addWidget(m_previewLeadPrefetchEnabledCheckBox);
    layout->addLayout(bufferingForm);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildClipTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Properties"), page));
    auto *form = new QFormLayout;

    m_clipInspectorClipLabel = new QLabel(QStringLiteral("No clip selected"), page);
    m_clipProxyUsageLabel = new QLabel(QStringLiteral("Playback Source: None"), page);
    m_clipPlaybackSourceLabel = new QLabel(QStringLiteral("Proxy In Use: No"), page);
    m_clipOriginalInfoLabel = new QLabel(QStringLiteral("Original\nNo clip selected."), page);
    m_clipProxyInfoLabel = new QLabel(QStringLiteral("Proxy\nNo proxy configured."), page);
    m_clipPlaybackRateSpin = new QDoubleSpinBox(page);
    m_clipPlaybackRateSpin->setDecimals(4);
    m_clipPlaybackRateSpin->setRange(0.001, 4.0);
    m_clipPlaybackRateSpin->setSingleStep(0.0001);
    m_clipPlaybackRateSpin->setValue(1.0);
    m_clipPlaybackRateSpin->setToolTip(
        QStringLiteral("Adjust clip playback speed with fine precision. "
                       "Example: 0.9999 for a slight slow-down."));
    m_clipPlaybackRateSpin->setValue(1.0);
    auto *trackSectionLabel = new QLabel(QStringLiteral("Track"), page);
    trackSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_trackInspectorLabel = new QLabel(QStringLiteral("No track selected"), page);
    m_trackInspectorDetailsLabel = new QLabel(QStringLiteral("Select a track header to edit track-wide properties."), page);
    m_trackNameEdit = new QLineEdit(page);
    m_trackHeightSpin = new QSpinBox(page);
    m_trackHeightSpin->setRange(28, 240);
    m_trackVisualModeCombo = new QComboBox(page);
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::Enabled),
                                    static_cast<int>(TrackVisualMode::Enabled));
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::ForceOpaque),
                                    static_cast<int>(TrackVisualMode::ForceOpaque));
    m_trackVisualModeCombo->addItem(trackVisualModeLabel(TrackVisualMode::Hidden),
                                    static_cast<int>(TrackVisualMode::Hidden));
    m_trackAudioEnabledCheckBox = new QCheckBox(QStringLiteral("Track Audio Enabled"), page);
    m_trackCrossfadeSecondsSpin = new QDoubleSpinBox(page);
    m_trackCrossfadeSecondsSpin->setDecimals(2);
    m_trackCrossfadeSecondsSpin->setRange(0.01, 30.0);
    m_trackCrossfadeSecondsSpin->setSingleStep(0.05);
    m_trackCrossfadeSecondsSpin->setValue(0.50);
    m_trackCrossfadeButton = new QPushButton(QStringLiteral("Crossfade Consecutive Clips"), page);
    auto *audioSectionLabel = new QLabel(QStringLiteral("Audio"), page);
    audioSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8;"));
    m_audioInspectorClipLabel = new QLabel(QStringLiteral("No audio clip selected"), page);
    m_audioInspectorDetailsLabel = new QLabel(QStringLiteral("Select an audio clip to inspect playback details."), page);

    for (QLabel *label : {m_clipInspectorClipLabel, m_clipProxyUsageLabel, m_clipPlaybackSourceLabel,
                          m_clipOriginalInfoLabel, m_clipProxyInfoLabel,
                          m_trackInspectorLabel, m_trackInspectorDetailsLabel,
                          m_audioInspectorClipLabel, m_audioInspectorDetailsLabel})
    {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    layout->addWidget(m_clipInspectorClipLabel);
    layout->addWidget(m_clipProxyUsageLabel);
    layout->addWidget(m_clipPlaybackSourceLabel);
    form->addRow(QStringLiteral("Playback Speed"), m_clipPlaybackRateSpin);
    layout->addLayout(form);
    layout->addWidget(m_clipOriginalInfoLabel);
    layout->addWidget(m_clipProxyInfoLabel);
    auto *trackForm = new QFormLayout;
    trackForm->addRow(QStringLiteral("Track Name"), m_trackNameEdit);
    trackForm->addRow(QStringLiteral("Track Height"), m_trackHeightSpin);
    trackForm->addRow(QStringLiteral("Track Visuals"), m_trackVisualModeCombo);
    trackForm->addRow(QStringLiteral("Crossfade"), m_trackCrossfadeSecondsSpin);
    layout->addWidget(trackSectionLabel);
    layout->addWidget(m_trackInspectorLabel);
    layout->addWidget(m_trackInspectorDetailsLabel);
    layout->addLayout(trackForm);
    layout->addWidget(m_trackAudioEnabledCheckBox);
    layout->addWidget(m_trackCrossfadeButton);
    layout->addWidget(audioSectionLabel);
    layout->addWidget(m_audioInspectorClipLabel);
    layout->addWidget(m_audioInspectorDetailsLabel);
    layout->addStretch(1);

    return page;
}

QWidget *InspectorPane::buildProfileTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("System"), page));

    auto *decodeForm = new QFormLayout;
    m_profileH26xThreadingModeCombo = new QComboBox(page);
    m_profileH26xThreadingModeCombo->addItem(QStringLiteral("Auto (Recommended)"), QStringLiteral("auto"));
    m_profileH26xThreadingModeCombo->addItem(QStringLiteral("Single Thread (Safest)"), QStringLiteral("single_thread"));
    m_profileH26xThreadingModeCombo->addItem(QStringLiteral("Slice Threads (Balanced)"), QStringLiteral("slice_threads"));
    m_profileH26xThreadingModeCombo->addItem(QStringLiteral("Frame + Slice Threads (Fastest)"), QStringLiteral("frame_and_slice_threads"));
    m_profileH26xThreadingModeCombo->setToolTip(
        QStringLiteral("Software decode threading policy for H.264/H.265 clips. "
                       "Auto selects a stable default for this FFmpeg build."));
    const QString h26xThreadingMode =
        editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode());
    const int h26xThreadingModeIndex = m_profileH26xThreadingModeCombo->findData(h26xThreadingMode);
    if (h26xThreadingModeIndex >= 0) {
        m_profileH26xThreadingModeCombo->setCurrentIndex(h26xThreadingModeIndex);
    }
    decodeForm->addRow(QStringLiteral("H.264/H.265 CPU Threading"), m_profileH26xThreadingModeCombo);

    m_profileSummaryTable = new QTableWidget(page);
    m_profileSummaryTable->setColumnCount(2);
    m_profileSummaryTable->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    m_profileSummaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_profileSummaryTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_profileSummaryTable->setFocusPolicy(Qt::NoFocus);
    m_profileSummaryTable->verticalHeader()->setVisible(false);
    m_profileSummaryTable->horizontalHeader()->setStretchLastSection(true);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_profileSummaryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    m_profileBenchmarkButton = new QPushButton(QStringLiteral("Run Decode Benchmark"), page);
    m_restartDecodersButton = new QPushButton(QStringLiteral("Restart All Decoders"), page);

    connect(m_restartDecodersButton, &QPushButton::clicked,
            this, &InspectorPane::restartDecodersRequested);

    layout->addLayout(decodeForm);
    layout->addWidget(m_profileSummaryTable, 1);
    layout->addWidget(m_profileBenchmarkButton);
    layout->addWidget(m_restartDecodersButton);
    return page;
}

QWidget *InspectorPane::buildProjectsTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Projects"), page));

    m_projectSectionLabel = new QLabel(QStringLiteral("PROJECTS"), page);
    m_projectSectionLabel->setWordWrap(true);
    m_projectSectionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_projectSectionLabel);

    auto *buttonRow = new QHBoxLayout;
    m_newProjectButton = new QPushButton(QStringLiteral("New"), page);
    m_saveProjectAsButton = new QPushButton(QStringLiteral("Save As"), page);
    m_renameProjectButton = new QPushButton(QStringLiteral("Rename"), page);
    buttonRow->addWidget(m_newProjectButton);
    buttonRow->addWidget(m_saveProjectAsButton);
    buttonRow->addWidget(m_renameProjectButton);
    layout->addLayout(buttonRow);

    m_projectsList = new QListWidget(page);
    m_projectsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_projectsList->setAlternatingRowColors(true);
    layout->addWidget(m_projectsList, 1);

    return page;
}
