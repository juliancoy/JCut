#include "inspector_pane.h"

#include "preview_surface.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QLabel* createTabHeading(const QString& text, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13px; font-weight: 700; color: #8fa0b5; "
        "padding: 2px 0 6px 0; }"));
    return label;
}

QVBoxLayout* createTabLayout(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);
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

void addFormRow(QFormLayout* form, const QString& label, QWidget* widget)
{
    form->addRow(label, widget);
}

void configureForm(QFormLayout* form)
{
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setSpacing(6);
}

} // namespace

QWidget* InspectorPane::buildAudioTab()
{
    auto* page = new QWidget;
    auto* layout = createTabLayout(page);
    layout->addWidget(createTabHeading(QStringLiteral("Audio"), page));

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* contentLayout = createTabLayout(content);

    m_audioTrackTitleLabel = new QLabel(QStringLiteral("No track selected"), content);
    m_audioTrackDetailsLabel = new QLabel(QStringLiteral("Select a track with audio clips."), content);
    m_audioCurrentSpeakerTitleLabel = new QLabel(QStringLiteral("No current speaker"), content);
    m_audioCurrentSpeakerDetailsLabel =
        new QLabel(QStringLiteral("Select an audio-backed clip and move the playhead into spoken content."), content);
    for (QLabel* label : {m_audioTrackTitleLabel, m_audioTrackDetailsLabel,
                          m_audioCurrentSpeakerTitleLabel, m_audioCurrentSpeakerDetailsLabel}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    m_audioTrackTitleLabel->setStyleSheet(QStringLiteral("font-weight: 700; color: #edf2f7;"));
    m_audioCurrentSpeakerTitleLabel->setStyleSheet(QStringLiteral("font-weight: 700; color: #edf2f7;"));

    m_audioAmplifyEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioAmplifyDbSpin = new QDoubleSpinBox(content);
    m_audioAmplifyDbSpin->setRange(-36.0, 36.0);
    m_audioAmplifyDbSpin->setSingleStep(0.5);
    m_audioAmplifyDbSpin->setDecimals(1);
    m_audioAmplifyDbSpin->setSuffix(QStringLiteral(" dB"));

    m_audioSpeakerHoverModalCheckBox = new QCheckBox(QStringLiteral("Speaker Hover"), content);
    m_audioSpeakerHoverModalCheckBox->setChecked(true);
    m_audioShowWaveformCheckBox = new QCheckBox(QStringLiteral("Timeline Waveform"), content);
    m_audioShowWaveformCheckBox->setChecked(true);

    m_audioVisualizationModeCombo = new QComboBox(content);
    m_audioVisualizationModeCombo->addItem(QStringLiteral("Waveform"),
                                           static_cast<int>(PreviewSurface::AudioVisualizationMode::Waveform));
    m_audioVisualizationModeCombo->addItem(QStringLiteral("Spectrum"),
                                           static_cast<int>(PreviewSurface::AudioVisualizationMode::Spectrum));
    m_loiaconoSpectrumSettingsButton = new QPushButton(QStringLiteral("Settings..."), content);

    m_audioWaveformPreviewProcessedCheckBox =
        new QCheckBox(QStringLiteral("Processed Preview"), content);
    m_audioWaveformPreviewProcessedCheckBox->setChecked(true);
    m_audioWaveformPreviewProcessedCheckBox->setToolTip(
        QStringLiteral("When enabled, waveform reflects preview post-processing. "
                       "When disabled, waveform reflects decoded on-disk audio."));

    m_audioNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioNormalizeTargetDbSpin = new QDoubleSpinBox(content);
    m_audioNormalizeTargetDbSpin->setRange(-24.0, 0.0);
    m_audioNormalizeTargetDbSpin->setSingleStep(0.5);
    m_audioNormalizeTargetDbSpin->setDecimals(1);
    m_audioNormalizeTargetDbSpin->setSuffix(QStringLiteral(" dB"));

    m_audioStereoToMonoCheckBox = new QCheckBox(QStringLiteral("Stereo to Mono"), content);
    m_audioStereoToMonoCheckBox->setToolTip(
        QStringLiteral("Downmix stereo output to mono and send the same signal to left and right channels."));

    m_audioSelectiveNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioSelectiveNormalizeMinSecondsSpin = new QDoubleSpinBox(content);
    m_audioSelectiveNormalizeMinSecondsSpin->setRange(0.1, 30.0);
    m_audioSelectiveNormalizeMinSecondsSpin->setSingleStep(0.1);
    m_audioSelectiveNormalizeMinSecondsSpin->setDecimals(1);
    m_audioSelectiveNormalizeMinSecondsSpin->setSuffix(QStringLiteral(" s"));
    m_audioSelectiveNormalizePeakDbSpin = new QDoubleSpinBox(content);
    m_audioSelectiveNormalizePeakDbSpin->setRange(-36.0, 0.0);
    m_audioSelectiveNormalizePeakDbSpin->setSingleStep(0.5);
    m_audioSelectiveNormalizePeakDbSpin->setDecimals(1);
    m_audioSelectiveNormalizePeakDbSpin->setSuffix(QStringLiteral(" dBFS"));
    m_audioSelectiveNormalizePeakDbSpin->setValue(-12.0);
    m_audioSelectiveNormalizePassesSpin = new QSpinBox(content);
    m_audioSelectiveNormalizePassesSpin->setRange(1, 8);
    m_audioSelectiveNormalizePassesSpin->setValue(1);
    m_audioSelectiveNormalizeOverlayVisibleCheckBox = new QCheckBox(QStringLiteral("Overlay"), content);
    m_audioSelectiveNormalizeOverlayVisibleCheckBox->setChecked(true);
    m_audioTranscriptNormalizeEnabledCheckBox = new QCheckBox(QStringLiteral("Transcript Normalize"), content);

    m_audioPeakReductionEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioPeakThresholdDbSpin = new QDoubleSpinBox(content);
    m_audioPeakThresholdDbSpin->setRange(-24.0, 0.0);
    m_audioPeakThresholdDbSpin->setSingleStep(0.5);
    m_audioPeakThresholdDbSpin->setDecimals(1);
    m_audioPeakThresholdDbSpin->setSuffix(QStringLiteral(" dB"));

    m_audioLimiterEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioLimiterThresholdDbSpin = new QDoubleSpinBox(content);
    m_audioLimiterThresholdDbSpin->setRange(-12.0, 0.0);
    m_audioLimiterThresholdDbSpin->setSingleStep(0.1);
    m_audioLimiterThresholdDbSpin->setDecimals(1);
    m_audioLimiterThresholdDbSpin->setSuffix(QStringLiteral(" dB"));

    m_audioCompressorEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioCompressorThresholdDbSpin = new QDoubleSpinBox(content);
    m_audioCompressorThresholdDbSpin->setRange(-30.0, -1.0);
    m_audioCompressorThresholdDbSpin->setSingleStep(0.5);
    m_audioCompressorThresholdDbSpin->setDecimals(1);
    m_audioCompressorThresholdDbSpin->setSuffix(QStringLiteral(" dB"));
    m_audioCompressorRatioSpin = new QDoubleSpinBox(content);
    m_audioCompressorRatioSpin->setRange(1.0, 20.0);
    m_audioCompressorRatioSpin->setSingleStep(0.1);
    m_audioCompressorRatioSpin->setDecimals(1);
    m_audioCompressorRatioSpin->setSuffix(QStringLiteral(":1"));
    m_audioSoftClipEnabledCheckBox = new QCheckBox(QStringLiteral("Enable"), content);
    m_audioSoftClipEnabledCheckBox->setToolTip(
        QStringLiteral("Apply smooth saturation before the limiter to tame transients without hard clipping."));

    auto trackSection = createDisclosureSection(content, QStringLiteral("Selected Track"), true);
    auto* trackForm = new QFormLayout;
    configureForm(trackForm);
    addFormRow(trackForm, QStringLiteral("Waveform"), m_audioShowWaveformCheckBox);
    trackSection.body->addWidget(m_audioTrackTitleLabel);
    trackSection.body->addWidget(m_audioTrackDetailsLabel);
    trackSection.body->addLayout(trackForm);

    auto speakerSection = createDisclosureSection(content, QStringLiteral("Current Speaker"), true);
    speakerSection.body->addWidget(m_audioCurrentSpeakerTitleLabel);
    speakerSection.body->addWidget(m_audioCurrentSpeakerDetailsLabel);

    auto viewSection = createDisclosureSection(content, QStringLiteral("Visualization"), true);
    auto* viewForm = new QFormLayout;
    configureForm(viewForm);
    addFormRow(viewForm, QStringLiteral("Preview"), m_audioVisualizationModeCombo);
    addFormRow(viewForm, QStringLiteral("Spectrum"), m_loiaconoSpectrumSettingsButton);
    addFormRow(viewForm, QStringLiteral("Timeline Source"), m_audioWaveformPreviewProcessedCheckBox);
    addFormRow(viewForm, QStringLiteral("Speaker Hover"), m_audioSpeakerHoverModalCheckBox);
    viewSection.body->addLayout(viewForm);

    auto loudnessSection = createDisclosureSection(content, QStringLiteral("Loudness"), true);
    auto* loudnessForm = new QFormLayout;
    configureForm(loudnessForm);
    addFormRow(loudnessForm, QStringLiteral("Amplify"), m_audioAmplifyEnabledCheckBox);
    addFormRow(loudnessForm, QStringLiteral("Gain"), m_audioAmplifyDbSpin);
    addFormRow(loudnessForm, QStringLiteral("Normalize"), m_audioNormalizeEnabledCheckBox);
    addFormRow(loudnessForm, QStringLiteral("Target"), m_audioNormalizeTargetDbSpin);
    addFormRow(loudnessForm, QStringLiteral("Channels"), m_audioStereoToMonoCheckBox);
    addFormRow(loudnessForm, QStringLiteral("Transcript"), m_audioTranscriptNormalizeEnabledCheckBox);
    loudnessSection.body->addLayout(loudnessForm);

    auto selectiveSection = createDisclosureSection(content, QStringLiteral("Selective Normalize"), false);
    auto* selectiveForm = new QFormLayout;
    configureForm(selectiveForm);
    addFormRow(selectiveForm, QStringLiteral("Process"), m_audioSelectiveNormalizeEnabledCheckBox);
    addFormRow(selectiveForm, QStringLiteral("Min Segment"), m_audioSelectiveNormalizeMinSecondsSpin);
    addFormRow(selectiveForm, QStringLiteral("Peak"), m_audioSelectiveNormalizePeakDbSpin);
    addFormRow(selectiveForm, QStringLiteral("Passes"), m_audioSelectiveNormalizePassesSpin);
    addFormRow(selectiveForm, QStringLiteral("Overlay"), m_audioSelectiveNormalizeOverlayVisibleCheckBox);
    selectiveSection.body->addLayout(selectiveForm);

    auto dynamicsSection = createDisclosureSection(content, QStringLiteral("Dynamics"), false);
    auto* dynamicsForm = new QFormLayout;
    configureForm(dynamicsForm);
    addFormRow(dynamicsForm, QStringLiteral("Peak Reduction"), m_audioPeakReductionEnabledCheckBox);
    addFormRow(dynamicsForm, QStringLiteral("Peak Threshold"), m_audioPeakThresholdDbSpin);
    addFormRow(dynamicsForm, QStringLiteral("Limiter"), m_audioLimiterEnabledCheckBox);
    addFormRow(dynamicsForm, QStringLiteral("Limit"), m_audioLimiterThresholdDbSpin);
    addFormRow(dynamicsForm, QStringLiteral("Compressor"), m_audioCompressorEnabledCheckBox);
    addFormRow(dynamicsForm, QStringLiteral("Threshold"), m_audioCompressorThresholdDbSpin);
    addFormRow(dynamicsForm, QStringLiteral("Ratio"), m_audioCompressorRatioSpin);
    addFormRow(dynamicsForm, QStringLiteral("Soft Clip"), m_audioSoftClipEnabledCheckBox);
    dynamicsSection.body->addLayout(dynamicsForm);

    contentLayout->addWidget(trackSection.container);
    contentLayout->addWidget(speakerSection.container);
    contentLayout->addWidget(viewSection.container);
    contentLayout->addWidget(loudnessSection.container);
    contentLayout->addWidget(selectiveSection.container);
    contentLayout->addWidget(dynamicsSection.container);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    return page;
}
