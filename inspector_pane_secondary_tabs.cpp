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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
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

struct DisclosureSection {
    QWidget* container = nullptr;
    QVBoxLayout* body = nullptr;
};

DisclosureSection createDisclosureSection(QWidget* parent,
                                          const QString& title,
                                          bool expanded = true)
{
    DisclosureSection section;
    auto* container = new QWidget(parent);
    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto* toggle = new QToolButton(container);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toggle->setText(QStringLiteral("%1 %2").arg(expanded ? QStringLiteral("▼") : QStringLiteral("▶"), title));
    toggle->setStyleSheet(QStringLiteral(
        "QToolButton { color: #9fb3c8; font-weight: 700; border: none; text-align: left; padding: 2px 0; }"
        "QToolButton:hover { color: #d6dee8; }"));
    outer->addWidget(toggle);

    auto* content = new QWidget(container);
    content->setVisible(expanded);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(14, 2, 0, 2);
    body->setSpacing(6);
    outer->addWidget(content);

    QObject::connect(toggle, &QToolButton::toggled, content, [toggle, title, content](bool checked) {
        toggle->setText(QStringLiteral("%1 %2").arg(checked ? QStringLiteral("▼") : QStringLiteral("▶"), title));
        content->setVisible(checked);
    });

    section.container = container;
    section.body = body;
    return section;
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
    m_outputRangeSummaryLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        " border: 1px solid #314459;"
        " border-radius: 10px;"
        " background: #142234;"
        " color: #d8e6f5;"
        " padding: 6px 10px;"
        "}"));

    auto *form = new QFormLayout;
    m_outputWidthSpin = new QSpinBox(page);
    m_outputHeightSpin = new QSpinBox(page);
    m_exportStartSpin = new QSpinBox(page);
    m_exportEndSpin = new QSpinBox(page);
    m_outputFormatCombo = new QComboBox(page);
    m_renderBackendCombo = new QComboBox(page);

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
    m_renderBackendCombo->addItem(QStringLiteral("Vulkan"), QStringLiteral("vulkan"));
    m_renderBackendCombo->addItem(QStringLiteral("OpenGL"), QStringLiteral("opengl"));
    m_renderBackendCombo->setToolTip(
        QStringLiteral("Renderer used for final export. Vulkan is strict and fails if unavailable; preview rendering is configured separately."));
    m_renderUseProxiesCheckBox = new QCheckBox(QStringLiteral("Use Proxies For Render"), page);
    m_renderCreateVideoFromSequenceCheckBox = new QCheckBox(QStringLiteral("Also Create Video From Sequence"), page);
    m_renderCreateVideoFromSequenceCheckBox->setChecked(true);
    m_renderCreateVideoFromSequenceCheckBox->setToolTip(QStringLiteral("Create a video file from the image sequence with audio"));

    auto *rangeBubblesRow = new QHBoxLayout;
    auto *startBubble = new QLabel(QStringLiteral("Start"), page);
    auto *endBubble = new QLabel(QStringLiteral("End"), page);
    for (QLabel* bubble : {startBubble, endBubble}) {
        bubble->setStyleSheet(QStringLiteral(
            "QLabel {"
            " border: 1px solid #314459;"
            " border-radius: 9px;"
            " background: #142234;"
            " color: #d8e6f5;"
            " padding: 3px 8px;"
            " font-size: 11px;"
            " font-weight: 700;"
            "}"));
    }
    rangeBubblesRow->addWidget(startBubble);
    rangeBubblesRow->addWidget(m_exportStartSpin, 1);
    rangeBubblesRow->addSpacing(8);
    rangeBubblesRow->addWidget(endBubble);
    rangeBubblesRow->addWidget(m_exportEndSpin, 1);

    form->addRow(QStringLiteral("Output Width"), m_outputWidthSpin);
    form->addRow(QStringLiteral("Output Height"), m_outputHeightSpin);
    form->addRow(QStringLiteral("Export Range"), rangeBubblesRow);
    form->addRow(QStringLiteral("Output Format"), m_outputFormatCombo);
    form->addRow(QStringLiteral("Export Backend"), m_renderBackendCombo);

    m_backgroundColorButton = new QPushButton(page);
    m_backgroundColorButton->setText(QStringLiteral("Black"));
    m_backgroundColorButton->setToolTip(QStringLiteral("Background color for the rendered output"));
    m_backgroundColorButton->setStyleSheet(
        QStringLiteral("QPushButton { background: #000000; color: #ffffff; "
                        "border: 1px solid #2e3b4a; border-radius: 4px; padding: 4px 8px; }"));
    form->addRow(QStringLiteral("Background"), m_backgroundColorButton);

    m_renderButton = new QPushButton(QStringLiteral("Render"), page);

    auto rangeSection = createDisclosureSection(page, QStringLiteral("Export Range"), true);
    rangeSection.body->addWidget(m_outputRangeSummaryLabel);

    auto settingsSection = createDisclosureSection(page, QStringLiteral("Render Settings"), true);
    settingsSection.body->addLayout(form);
    settingsSection.body->addWidget(m_renderUseProxiesCheckBox);
    settingsSection.body->addWidget(m_renderCreateVideoFromSequenceCheckBox);

    auto actionSection = createDisclosureSection(page, QStringLiteral("Render Action"), true);
    actionSection.body->addWidget(m_renderButton);

    layout->addWidget(rangeSection.container);
    layout->addWidget(settingsSection.container);
    layout->addWidget(actionSection.container);
    layout->addStretch(1);

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
        new QCheckBox(QStringLiteral("Show FaceStream Points"), page);
    m_previewShowSpeakerTrackPointsCheckBox->setChecked(false);
    m_previewShowSpeakerTrackPointsCheckBox->setToolTip(
        QStringLiteral("Draw all speaker framing keyframe points on top of active clips."));

    m_previewVulkanPresenterCombo = new QComboBox(page);
    m_previewVulkanPresenterCombo->addItem(QStringLiteral("Embedded In Preview Pane"), QStringLiteral("embedded"));
    m_previewVulkanPresenterCombo->addItem(QStringLiteral("Direct Native Swapchain Window"), QStringLiteral("direct"));
    m_previewVulkanPresenterCombo->setToolTip(
        QStringLiteral("How Vulkan preview is presented. Embedded keeps video inside the editor pane; "
                       "Direct uses a separate native Vulkan window. Requires restart."));
    auto *vulkanPresenterForm = new QFormLayout();
    vulkanPresenterForm->addRow(QStringLiteral("Vulkan Presenter"), m_previewVulkanPresenterCombo);

    // Zoom control section
    auto *zoomSectionLabel = new QLabel(QStringLiteral("Zoom"), page);
    zoomSectionLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #8fa3b8; margin-top: 8px;"));
    
    auto *zoomLayout = new QHBoxLayout();
    m_previewZoomSpin = new QDoubleSpinBox(page);
    m_previewZoomSpin->setDecimals(2);
    m_previewZoomSpin->setRange(0.1, 100000.0);
    m_previewZoomSpin->setSingleStep(0.1);
    m_previewZoomSpin->setValue(1.0);
    m_previewZoomSpin->setSuffix(QStringLiteral("x"));
    m_previewZoomSpin->setToolTip(
        QStringLiteral("Preview zoom level. Audio mode supports deep zoom; use mouse wheel over preview for smooth zoom."));
    
    m_previewZoomResetButton = new QPushButton(QStringLiteral("Reset"), page);
    m_previewZoomResetButton->setToolTip(QStringLiteral("Reset zoom to 1.0x and center the view"));
    
    zoomLayout->addWidget(m_previewZoomSpin);
    zoomLayout->addWidget(m_previewZoomResetButton);
    
    auto *zoomHelpLabel = new QLabel(
        QStringLiteral("Mouse wheel: zoom at cursor position. Drag to pan when zoomed."), page);
    zoomHelpLabel->setWordWrap(true);
    zoomHelpLabel->setStyleSheet(QStringLiteral("color: #6b7a8f; font-size: 11px;"));

    layout->addWidget(summary);
    layout->addWidget(m_previewHideOutsideOutputCheckBox);
    layout->addWidget(m_previewShowSpeakerTrackPointsCheckBox);
    layout->addLayout(vulkanPresenterForm);
    layout->addSpacing(12);
    layout->addWidget(zoomSectionLabel);
    layout->addLayout(zoomLayout);
    layout->addWidget(zoomHelpLabel);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildPreferencesTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Preferences"), page));

    auto *summary = new QLabel(
        QStringLiteral("App-level defaults and feature flags. These settings affect editor behavior globally."), page);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto autosaveSection = createDisclosureSection(page, QStringLiteral("Autosave"), true);
    auto *autosaveForm = new QFormLayout();
    m_autosaveIntervalMinutesSpin = new QSpinBox(page);
    m_autosaveIntervalMinutesSpin->setRange(1, 120);
    m_autosaveIntervalMinutesSpin->setValue(5);
    m_autosaveIntervalMinutesSpin->setSuffix(QStringLiteral(" min"));
    m_autosaveIntervalMinutesSpin->setToolTip(
        QStringLiteral("How often editor state backups are written."));
    autosaveForm->addRow(QStringLiteral("Autosave Interval"), m_autosaveIntervalMinutesSpin);
    m_autosaveMaxBackupsSpin = new QSpinBox(page);
    m_autosaveMaxBackupsSpin->setRange(1, 200);
    m_autosaveMaxBackupsSpin->setValue(20);
    m_autosaveMaxBackupsSpin->setToolTip(
        QStringLiteral("How many autosave backup files to keep per project."));
    autosaveForm->addRow(QStringLiteral("Autosave Backups"), m_autosaveMaxBackupsSpin);
    autosaveSection.body->addLayout(autosaveForm);
    layout->addWidget(autosaveSection.container);

    auto timelineSection = createDisclosureSection(page, QStringLiteral("Timeline"), false);
    auto *timelineForm = new QFormLayout();
    m_timelineAudioEnvelopeGranularitySpin = new QSpinBox(page);
    m_timelineAudioEnvelopeGranularitySpin->setRange(64, 8192);
    m_timelineAudioEnvelopeGranularitySpin->setValue(editor::debugTimelineAudioEnvelopeGranularity());
    m_timelineAudioEnvelopeGranularitySpin->setSuffix(QStringLiteral(" samples"));
    m_timelineAudioEnvelopeGranularitySpin->setToolTip(
        QStringLiteral("Sample window size used to build audio envelope peaks for timeline waveform rendering."));
    timelineForm->addRow(QStringLiteral("Audio Envelope Granularity"), m_timelineAudioEnvelopeGranularitySpin);
    timelineSection.body->addLayout(timelineForm);
    layout->addWidget(timelineSection.container);

    auto decodeSection = createDisclosureSection(page, QStringLiteral("Playback + Decode"), false);
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
    decodeSection.body->addWidget(m_outputPlaybackCacheFallbackCheckBox);
    decodeSection.body->addWidget(m_outputLeadPrefetchEnabledCheckBox);
    decodeSection.body->addLayout(decodeForm);
    decodeSection.body->addWidget(m_outputDeterministicPipelineCheckBox);
    decodeSection.body->addWidget(m_outputResetPipelineDefaultsButton);
    layout->addWidget(decodeSection.container);
    if (m_outputLeadPrefetchCountSpin) {
        m_outputLeadPrefetchCountSpin->setEnabled(editor::debugLeadPrefetchEnabled());
    }

    auto featureSection = createDisclosureSection(page, QStringLiteral("Feature Flags"), false);
    m_preferencesFeatureAiPanelCheckBox = new QCheckBox(QStringLiteral("Enable AI Assist"), page);
    m_preferencesFeatureAiSpeakerCleanupCheckBox =
        new QCheckBox(QStringLiteral("Enable AI Speaker Cleanup"), page);
    m_preferencesFeatureAudioPreviewModeCheckBox =
        new QCheckBox(QStringLiteral("Enable Audio Preview Mode"), page);
    m_preferencesFeatureAudioDynamicsToolsCheckBox =
        new QCheckBox(QStringLiteral("Enable Audio Dynamics Tools"), page);
    featureSection.body->addWidget(m_preferencesFeatureAiPanelCheckBox);
    featureSection.body->addWidget(m_preferencesFeatureAiSpeakerCleanupCheckBox);
    featureSection.body->addWidget(m_preferencesFeatureAudioPreviewModeCheckBox);
    featureSection.body->addWidget(m_preferencesFeatureAudioDynamicsToolsCheckBox);
    layout->addWidget(featureSection.container);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildAudioTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Audio"), page));

    auto *summary = new QLabel(
        QStringLiteral("Audio dynamics are applied to preview playback and export rendering."), page);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *form = new QFormLayout;
    m_audioAmplifyEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioAmplifyDbSpin = new QDoubleSpinBox(page);
    m_audioAmplifyDbSpin->setRange(-36.0, 36.0);
    m_audioAmplifyDbSpin->setSingleStep(0.5);
    m_audioAmplifyDbSpin->setDecimals(1);
    m_audioAmplifyDbSpin->setSuffix(QStringLiteral(" dB"));
    m_audioSpeakerHoverModalCheckBox =
        new QCheckBox(QStringLiteral("Show Speaker Hover Modal"), page);
    m_audioSpeakerHoverModalCheckBox->setChecked(true);
    m_audioShowWaveformCheckBox =
        new QCheckBox(QStringLiteral("Show Waveform"), page);
    m_audioShowWaveformCheckBox->setChecked(true);
    m_audioWaveformPreviewProcessedCheckBox =
        new QCheckBox(QStringLiteral("Preview"), page);
    m_audioWaveformPreviewProcessedCheckBox->setChecked(true);
    m_audioWaveformPreviewProcessedCheckBox->setToolTip(
        QStringLiteral("When enabled, waveform reflects preview post-processing. "
                       "When disabled, waveform reflects decoded on-disk audio."));
    form->addRow(QStringLiteral("Amplify"), m_audioAmplifyEnabledCheckBox);
    form->addRow(QStringLiteral("Amplify Gain"), m_audioAmplifyDbSpin);
    form->addRow(QStringLiteral("Hover Info"), m_audioSpeakerHoverModalCheckBox);
    form->addRow(QStringLiteral("Waveform"), m_audioShowWaveformCheckBox);
    form->addRow(QStringLiteral("Waveform Source"), m_audioWaveformPreviewProcessedCheckBox);

    m_audioNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioNormalizeTargetDbSpin = new QDoubleSpinBox(page);
    m_audioNormalizeTargetDbSpin->setRange(-24.0, 0.0);
    m_audioNormalizeTargetDbSpin->setSingleStep(0.5);
    m_audioNormalizeTargetDbSpin->setDecimals(1);
    m_audioNormalizeTargetDbSpin->setSuffix(QStringLiteral(" dB"));
    form->addRow(QStringLiteral("Normalize"), m_audioNormalizeEnabledCheckBox);
    form->addRow(QStringLiteral("Normalize Target"), m_audioNormalizeTargetDbSpin);

    m_audioSelectiveNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioSelectiveNormalizeMinSecondsSpin = new QDoubleSpinBox(page);
    m_audioSelectiveNormalizeMinSecondsSpin->setRange(0.1, 30.0);
    m_audioSelectiveNormalizeMinSecondsSpin->setSingleStep(0.1);
    m_audioSelectiveNormalizeMinSecondsSpin->setDecimals(1);
    m_audioSelectiveNormalizeMinSecondsSpin->setSuffix(QStringLiteral(" s"));
    m_audioSelectiveNormalizePeakDbSpin = new QDoubleSpinBox(page);
    m_audioSelectiveNormalizePeakDbSpin->setRange(-36.0, 0.0);
    m_audioSelectiveNormalizePeakDbSpin->setSingleStep(0.5);
    m_audioSelectiveNormalizePeakDbSpin->setDecimals(1);
    m_audioSelectiveNormalizePeakDbSpin->setSuffix(QStringLiteral(" dBFS"));
    m_audioSelectiveNormalizePeakDbSpin->setValue(-12.0);
    m_audioSelectiveNormalizePassesSpin = new QSpinBox(page);
    m_audioSelectiveNormalizePassesSpin->setRange(1, 8);
    m_audioSelectiveNormalizePassesSpin->setValue(1);
    m_audioSelectiveNormalizeOverlayVisibleCheckBox = new QCheckBox(QStringLiteral("Show"), page);
    m_audioSelectiveNormalizeOverlayVisibleCheckBox->setChecked(true);
    m_audioTranscriptNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    form->addRow(QStringLiteral("Selective Normalize"), m_audioSelectiveNormalizeEnabledCheckBox);
    form->addRow(QStringLiteral("Selective Min Segment"), m_audioSelectiveNormalizeMinSecondsSpin);
    form->addRow(QStringLiteral("Selective Peak Threshold"), m_audioSelectiveNormalizePeakDbSpin);
    form->addRow(QStringLiteral("Selective Passes"), m_audioSelectiveNormalizePassesSpin);
    form->addRow(QStringLiteral("Selective Overlay"), m_audioSelectiveNormalizeOverlayVisibleCheckBox);
    form->addRow(QStringLiteral("Transcript Normalize"), m_audioTranscriptNormalizeEnabledCheckBox);

    m_audioPeakReductionEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioPeakThresholdDbSpin = new QDoubleSpinBox(page);
    m_audioPeakThresholdDbSpin->setRange(-24.0, 0.0);
    m_audioPeakThresholdDbSpin->setSingleStep(0.5);
    m_audioPeakThresholdDbSpin->setDecimals(1);
    m_audioPeakThresholdDbSpin->setSuffix(QStringLiteral(" dB"));
    form->addRow(QStringLiteral("Peak Reduction"), m_audioPeakReductionEnabledCheckBox);
    form->addRow(QStringLiteral("Peak Threshold"), m_audioPeakThresholdDbSpin);

    m_audioLimiterEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioLimiterThresholdDbSpin = new QDoubleSpinBox(page);
    m_audioLimiterThresholdDbSpin->setRange(-12.0, 0.0);
    m_audioLimiterThresholdDbSpin->setSingleStep(0.1);
    m_audioLimiterThresholdDbSpin->setDecimals(1);
    m_audioLimiterThresholdDbSpin->setSuffix(QStringLiteral(" dB"));
    form->addRow(QStringLiteral("Limiter"), m_audioLimiterEnabledCheckBox);
    form->addRow(QStringLiteral("Limiter Threshold"), m_audioLimiterThresholdDbSpin);

    m_audioCompressorEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), page);
    m_audioCompressorThresholdDbSpin = new QDoubleSpinBox(page);
    m_audioCompressorThresholdDbSpin->setRange(-30.0, -1.0);
    m_audioCompressorThresholdDbSpin->setSingleStep(0.5);
    m_audioCompressorThresholdDbSpin->setDecimals(1);
    m_audioCompressorThresholdDbSpin->setSuffix(QStringLiteral(" dB"));
    m_audioCompressorRatioSpin = new QDoubleSpinBox(page);
    m_audioCompressorRatioSpin->setRange(1.0, 20.0);
    m_audioCompressorRatioSpin->setSingleStep(0.1);
    m_audioCompressorRatioSpin->setDecimals(1);
    m_audioCompressorRatioSpin->setSuffix(QStringLiteral(":1"));
    form->addRow(QStringLiteral("Compressor"), m_audioCompressorEnabledCheckBox);
    form->addRow(QStringLiteral("Compressor Threshold"), m_audioCompressorThresholdDbSpin);
    form->addRow(QStringLiteral("Compressor Ratio"), m_audioCompressorRatioSpin);

    layout->addLayout(form);
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

QWidget *InspectorPane::buildAiTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("AI Assist"), page));

    auto *summary = new QLabel(
        QStringLiteral("Run AI transcript and speaker workflows from a dedicated tab."), page);
    summary->setWordWrap(true);

    m_aiStatusLabel = new QLabel(QStringLiteral("AI unavailable: use top-right Log In"), page);
    m_aiStatusLabel->setWordWrap(true);
    m_aiStatusLabel->setStyleSheet(QStringLiteral("color: #9fb3c8;"));

    auto *modelForm = new QFormLayout;
    m_aiModelCombo = new QComboBox(page);
    m_aiModelCombo->setObjectName(QStringLiteral("ai.model_combo"));
    modelForm->addRow(QStringLiteral("Model"), m_aiModelCombo);

    auto *actionsRow1 = new QHBoxLayout;
    m_aiTranscribeButton = new QPushButton(QStringLiteral("Transcribe (AI)"), page);
    m_aiFindSpeakerNamesButton = new QPushButton(QStringLiteral("Mine Transcript (AI)"), page);
    actionsRow1->addWidget(m_aiTranscribeButton);
    actionsRow1->addWidget(m_aiFindSpeakerNamesButton);

    auto *actionsRow2 = new QHBoxLayout;
    m_aiFindOrganizationsButton = new QPushButton(QStringLiteral("Find Organizations"), page);
    m_aiCleanAssignmentsButton = new QPushButton(QStringLiteral("Clean Assignments"), page);
    actionsRow2->addWidget(m_aiFindOrganizationsButton);
    actionsRow2->addWidget(m_aiCleanAssignmentsButton);

    auto *authHint = new QLabel(QStringLiteral("Use top-right Log In for AI account access."), page);
    authHint->setWordWrap(true);
    authHint->setStyleSheet(QStringLiteral("color: #8ea4bf;"));

    auto *subscriptionRow = new QHBoxLayout;
    m_aiSubscribeButton = new QPushButton(QStringLiteral("Subscribe to AI"), page);
    m_aiSubscribeButton->setObjectName(QStringLiteral("ai.subscribe_button"));
    subscriptionRow->addWidget(m_aiSubscribeButton);
    subscriptionRow->addStretch(1);

    auto aiActionsSection = createDisclosureSection(page, QStringLiteral("AI Actions"), true);
    auto chatSection = createDisclosureSection(page, QStringLiteral("Chat"), true);
    m_aiChatHistoryEdit = new QTextBrowser(page);
    m_aiChatHistoryEdit->setObjectName(QStringLiteral("ai.chat_history"));
    m_aiChatHistoryEdit->setPlaceholderText(
        QStringLiteral("Chat responses from DeepSeek will appear here."));
    m_aiChatHistoryEdit->setMinimumHeight(160);
    m_aiChatHistoryEdit->setOpenExternalLinks(false);

    auto *chatInputLabel = new QLabel(QStringLiteral("Prompt (Ctrl+Enter to send)"), page);
    chatInputLabel->setStyleSheet(QStringLiteral("color: #8ea4bf;"));
    m_aiChatInputLineEdit = new QPlainTextEdit(page);
    m_aiChatInputLineEdit->setObjectName(QStringLiteral("ai.chat_input"));
    m_aiChatInputLineEdit->setPlaceholderText(QStringLiteral("Ask DeepSeek anything..."));
    m_aiChatInputLineEdit->setMinimumHeight(70);
    m_aiChatInputLineEdit->setMaximumHeight(140);

    auto *chatInputRow = new QHBoxLayout;
    m_aiChatSendButton = new QPushButton(QStringLiteral("Send"), page);
    m_aiChatClearButton = new QPushButton(QStringLiteral("Clear"), page);
    m_aiChatClearButton->setEnabled(false);
    chatInputRow->addStretch(1);
    chatInputRow->addWidget(m_aiChatSendButton);
    chatInputRow->addWidget(m_aiChatClearButton);

    layout->addWidget(summary);
    layout->addWidget(m_aiStatusLabel);
    aiActionsSection.body->addLayout(modelForm);
    aiActionsSection.body->addLayout(actionsRow1);
    aiActionsSection.body->addLayout(actionsRow2);
    aiActionsSection.body->addWidget(authHint);
    aiActionsSection.body->addLayout(subscriptionRow);
    layout->addWidget(aiActionsSection.container);

    chatSection.body->addWidget(m_aiChatHistoryEdit);
    chatSection.body->addWidget(chatInputLabel);
    chatSection.body->addWidget(m_aiChatInputLineEdit);
    chatSection.body->addLayout(chatInputRow);
    layout->addWidget(chatSection.container);
    layout->addStretch(1);
    return page;
}

QWidget *InspectorPane::buildAccessTab()
{
    auto *page = new QWidget;
    auto *layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Subscriptions & Purchases"), page));

    auto *summary = new QLabel(
        QStringLiteral("View account subscriptions, purchases, and active entitlements for the logged-in user."),
        page);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    m_accessStatusLabel = new QLabel(QStringLiteral("Log in to load account access data."), page);
    m_accessStatusLabel->setWordWrap(true);
    m_accessStatusLabel->setStyleSheet(QStringLiteral("color: #9fb3c8;"));
    layout->addWidget(m_accessStatusLabel);

    m_accessTable = new QTableWidget(0, 5, page);
    m_accessTable->setObjectName(QStringLiteral("access.table"));
    m_accessTable->setHorizontalHeaderLabels(
        QStringList{QStringLiteral("Type"),
                    QStringLiteral("Item"),
                    QStringLiteral("Status"),
                    QStringLiteral("Period"),
                    QStringLiteral("Source")});
    m_accessTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_accessTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_accessTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_accessTable->setAlternatingRowColors(true);
    m_accessTable->horizontalHeader()->setStretchLastSection(true);
    m_accessTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_accessTable, 1);

    auto *actions = new QHBoxLayout;
    m_accessRefreshButton = new QPushButton(QStringLiteral("Refresh"), page);
    actions->addStretch(1);
    actions->addWidget(m_accessRefreshButton);
    layout->addLayout(actions);

    return page;
}
