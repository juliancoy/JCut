#include "editor.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>

#include "transform_skip_aware_timing.h"

using namespace editor;

void EditorWindow::bindInspectorWidgets()
{
    m_transcriptTable = m_inspectorPane->transcriptTable();
    m_transcriptInspectorClipLabel = m_inspectorPane->transcriptInspectorClipLabel();
    m_transcriptInspectorDetailsLabel = m_inspectorPane->transcriptInspectorDetailsLabel();
    m_clipInspectorClipLabel = m_inspectorPane->clipInspectorClipLabel();
    m_clipProxyUsageLabel = m_inspectorPane->clipProxyUsageLabel();
    m_clipPlaybackSourceLabel = m_inspectorPane->clipPlaybackSourceLabel();
    m_clipOriginalInfoLabel = m_inspectorPane->clipOriginalInfoLabel();
    m_clipProxyInfoLabel = m_inspectorPane->clipProxyInfoLabel();
    m_clipPlaybackRateSpin = m_inspectorPane->clipPlaybackRateSpin();
    {
        QTableWidget *tracksTable = m_inspectorPane->tracksTable();
        if (tracksTable) {
            connect(tracksTable, &QTableWidget::itemChanged,
                    this, &EditorWindow::onTrackTableItemChanged);
        }
    }
    m_trackInspectorLabel = m_inspectorPane->trackInspectorLabel();
    m_trackInspectorDetailsLabel = m_inspectorPane->trackInspectorDetailsLabel();
    m_trackNameEdit = m_inspectorPane->trackNameEdit();
    m_trackHeightSpin = m_inspectorPane->trackHeightSpin();
    m_trackVisualModeCombo = m_inspectorPane->trackVisualModeCombo();
    m_trackAudioEnabledCheckBox = m_inspectorPane->trackAudioEnabledCheckBox();
    m_trackCrossfadeSecondsSpin = m_inspectorPane->trackCrossfadeSecondsSpin();
    m_trackCrossfadeButton = m_inspectorPane->trackCrossfadeButton();
    m_previewHideOutsideOutputCheckBox = m_inspectorPane->previewHideOutsideOutputCheckBox();
    m_previewZoomSpin = m_inspectorPane->previewZoomSpin();
    m_previewZoomResetButton = m_inspectorPane->previewZoomResetButton();
    m_transcriptOverlayEnabledCheckBox = m_inspectorPane->transcriptOverlayEnabledCheckBox();
    m_transcriptMaxLinesSpin = m_inspectorPane->transcriptMaxLinesSpin();
    m_transcriptMaxCharsSpin = m_inspectorPane->transcriptMaxCharsSpin();
    m_transcriptAutoScrollCheckBox = m_inspectorPane->transcriptAutoScrollCheckBox();
    m_transcriptFollowCurrentWordCheckBox = m_inspectorPane->transcriptFollowCurrentWordCheckBox();
    m_transcriptOverlayXSpin = m_inspectorPane->transcriptOverlayXSpin();
    m_transcriptOverlayYSpin = m_inspectorPane->transcriptOverlayYSpin();
    m_transcriptOverlayWidthSpin = m_inspectorPane->transcriptOverlayWidthSpin();
    m_transcriptOverlayHeightSpin = m_inspectorPane->transcriptOverlayHeightSpin();
    m_transcriptFontFamilyCombo = m_inspectorPane->transcriptFontFamilyCombo();
    m_transcriptFontSizeSpin = m_inspectorPane->transcriptFontSizeSpin();
    m_transcriptBoldCheckBox = m_inspectorPane->transcriptBoldCheckBox();
    m_transcriptItalicCheckBox = m_inspectorPane->transcriptItalicCheckBox();
    m_syncTable = m_inspectorPane->syncTable();
    m_syncInspectorClipLabel = m_inspectorPane->syncInspectorClipLabel();
    m_syncInspectorDetailsLabel = m_inspectorPane->syncInspectorDetailsLabel();
    m_clearAllSyncPointsButton = m_inspectorPane->clearAllSyncPointsButton();

    if (m_syncTable) {
        connect(m_syncTable, &QTableWidget::itemSelectionChanged,
                this, &EditorWindow::onSyncTableSelectionChanged);
        connect(m_syncTable, &QTableWidget::itemChanged,
                this, &EditorWindow::onSyncTableItemChanged);
        connect(m_syncTable, &QTableWidget::itemDoubleClicked,
                this, &EditorWindow::onSyncTableItemDoubleClicked);
        m_syncTable->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_syncTable, &QWidget::customContextMenuRequested,
                this, &EditorWindow::onSyncTableCustomContextMenu);
    }
    if (m_clearAllSyncPointsButton) {
        connect(m_clearAllSyncPointsButton, &QPushButton::clicked, this, [this]() {
            if (!m_timeline) {
                return;
            }
            const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
            if (markers.isEmpty()) {
                QMessageBox::information(this,
                                         QStringLiteral("Clear Sync Points"),
                                         QStringLiteral("There are no sync points to clear."));
                return;
            }
            const int response = QMessageBox::question(
                this,
                QStringLiteral("Clear All Sync Points"),
                QStringLiteral("Remove all %1 sync points from the timeline?")
                    .arg(markers.size()),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (response != QMessageBox::Yes) {
                return;
            }
            m_timeline->setRenderSyncMarkers({});
            refreshSyncInspector();
            scheduleSaveState();
            pushHistorySnapshot();
        });
    }

    m_gradingPathLabel = m_inspectorPane->gradingPathLabel();
    m_brightnessSpin = m_inspectorPane->brightnessSpin();
    m_contrastSpin = m_inspectorPane->contrastSpin();
    m_saturationSpin = m_inspectorPane->saturationSpin();
    m_opacitySpin = m_inspectorPane->opacitySpin();
    m_opacityKeyframeTable = m_inspectorPane->opacityKeyframeTable();
    // Shadows/Midtones/Highlights
    m_shadowsRSpin = m_inspectorPane->shadowsRSpin();
    m_shadowsGSpin = m_inspectorPane->shadowsGSpin();
    m_shadowsBSpin = m_inspectorPane->shadowsBSpin();
    m_midtonesRSpin = m_inspectorPane->midtonesRSpin();
    m_midtonesGSpin = m_inspectorPane->midtonesGSpin();
    m_midtonesBSpin = m_inspectorPane->midtonesBSpin();
    m_highlightsRSpin = m_inspectorPane->highlightsRSpin();
    m_highlightsGSpin = m_inspectorPane->highlightsGSpin();
    m_highlightsBSpin = m_inspectorPane->highlightsBSpin();
    m_gradingKeyframeTable = m_inspectorPane->gradingKeyframeTable();
    m_gradingAutoScrollCheckBox = m_inspectorPane->gradingAutoScrollCheckBox();
    m_gradingFollowCurrentCheckBox = m_inspectorPane->gradingFollowCurrentCheckBox();
    m_gradingKeyAtPlayheadButton = m_inspectorPane->gradingKeyAtPlayheadButton();
    m_gradingFadeInButton = m_inspectorPane->gradingFadeInButton();
    m_gradingFadeOutButton = m_inspectorPane->gradingFadeOutButton();
    m_gradingFadeDurationSpin = m_inspectorPane->gradingFadeDurationSpin();
    m_keyframesInspectorClipLabel = m_inspectorPane->keyframesInspectorClipLabel();
    m_keyframesInspectorDetailsLabel = m_inspectorPane->keyframesInspectorDetailsLabel();
    m_keyframesAutoScrollCheckBox = m_inspectorPane->keyframesAutoScrollCheckBox();
    m_keyframesFollowCurrentCheckBox = m_inspectorPane->keyframesFollowCurrentCheckBox();
    m_audioInspectorClipLabel = m_inspectorPane->audioInspectorClipLabel();
    m_audioInspectorDetailsLabel = m_inspectorPane->audioInspectorDetailsLabel();
    m_videoTranslationXSpin = m_inspectorPane->videoTranslationXSpin();
    m_videoTranslationYSpin = m_inspectorPane->videoTranslationYSpin();
    m_videoRotationSpin = m_inspectorPane->videoRotationSpin();
    m_videoScaleXSpin = m_inspectorPane->videoScaleXSpin();
    m_videoScaleYSpin = m_inspectorPane->videoScaleYSpin();
    m_videoKeyframeTable = m_inspectorPane->videoKeyframeTable();
    m_videoInterpolationCombo = m_inspectorPane->videoInterpolationCombo();
    m_mirrorHorizontalCheckBox = m_inspectorPane->mirrorHorizontalCheckBox();
    m_mirrorVerticalCheckBox = m_inspectorPane->mirrorVerticalCheckBox();
    m_lockVideoScaleCheckBox = m_inspectorPane->lockVideoScaleCheckBox();
    m_keyframeSpaceCheckBox = m_inspectorPane->keyframeSpaceCheckBox();
    m_keyframeSkipAwareTimingCheckBox = m_inspectorPane->keyframeSkipAwareTimingCheckBox();
    m_addVideoKeyframeButton = m_inspectorPane->addVideoKeyframeButton();
    m_removeVideoKeyframeButton = m_inspectorPane->removeVideoKeyframeButton();
    m_flipHorizontalButton = m_inspectorPane->flipHorizontalButton();
    m_outputWidthSpin = m_inspectorPane->outputWidthSpin();
    m_outputHeightSpin = m_inspectorPane->outputHeightSpin();
    m_exportStartSpin = m_inspectorPane->exportStartSpin();
    m_exportEndSpin = m_inspectorPane->exportEndSpin();
    m_outputFormatCombo = m_inspectorPane->outputFormatCombo();
    m_outputRangeSummaryLabel = m_inspectorPane->outputRangeSummaryLabel();
    m_renderUseProxiesCheckBox = m_inspectorPane->renderUseProxiesCheckBox();
    m_renderButton = m_inspectorPane->renderButton();
    m_profileSummaryTable = m_inspectorPane->profileSummaryTable();
    m_profileBenchmarkButton = m_inspectorPane->profileBenchmarkButton();
    m_projectSectionLabel = m_inspectorPane->projectSectionLabel();
    m_projectsList = m_inspectorPane->projectsList();
    m_newProjectButton = m_inspectorPane->newProjectButton();
    m_saveProjectAsButton = m_inspectorPane->saveProjectAsButton();
    m_renameProjectButton = m_inspectorPane->renameProjectButton();
    m_transcriptPrependMsSpin = m_inspectorPane->transcriptPrependMsSpin();
    m_transcriptPostpendMsSpin = m_inspectorPane->transcriptPostpendMsSpin();
    m_speechFilterEnabledCheckBox = m_inspectorPane->speechFilterEnabledCheckBox();
    m_speechFilterFadeSamplesSpin = m_inspectorPane->speechFilterFadeSamplesSpin();
}

void EditorWindow::setupSpeechFilterControls()
{
    const auto refreshSpeechFilterRouting = [this](bool pushHistory = false) {
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        if (pushHistory) pushHistorySnapshot();
    };

    connect(m_speechFilterEnabledCheckBox, &QCheckBox::toggled, this,
            [refreshSpeechFilterRouting](bool) { refreshSpeechFilterRouting(true); });
    connect(m_transcriptPrependMsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_transcriptPrependMs = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });
    connect(m_transcriptPostpendMsSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_transcriptPostpendMs = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });
    connect(m_speechFilterFadeSamplesSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, refreshSpeechFilterRouting](int value) {
                m_speechFilterFadeSamples = qMax(0, value);
                refreshSpeechFilterRouting(true);
            });

    connect(m_clipPlaybackRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (!m_timeline || !m_clipPlaybackRateSpin) {
            return;
        }
        const TimelineClip *clip = m_timeline->selectedClip();
        if (!clip || !clipHasVisuals(*clip)) {
            return;
        }
        const qreal playbackRate = qBound<qreal>(0.001, value, 4.0);
        if (qFuzzyCompare(clip->playbackRate, playbackRate)) {
            return;
        }
        if (!m_timeline->updateClipById(clip->id, [playbackRate](TimelineClip &editableClip) {
                editableClip.playbackRate = playbackRate;
                normalizeClipTiming(editableClip);
            })) {
            return;
        }
        if (m_preview) {
            m_preview->setTimelineClips(m_timeline->clips());
        }
        m_inspectorPane->refresh();
        scheduleSaveState();
        pushHistorySnapshot();
    });
}

void EditorWindow::setupTrackInspectorControls()
{
    connect(m_trackNameEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_timeline || !m_trackNameEdit) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack *track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track) {
            return;
        }
        const QString nextName = m_trackNameEdit->text().trimmed().isEmpty()
            ? QStringLiteral("Track %1").arg(trackIndex + 1)
            : m_trackNameEdit->text().trimmed();
        if (track->name == nextName) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [nextName](TimelineTrack &editableTrack) {
                editableTrack.name = nextName;
            })) {
            return;
        }
        refreshClipInspector();
    });

    connect(m_trackHeightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (!m_timeline || !m_trackHeightSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        const TimelineTrack *track = m_timeline->selectedTrack();
        if (trackIndex < 0 || !track || track->height == value) {
            return;
        }
        if (!m_timeline->updateTrackByIndex(trackIndex, [value](TimelineTrack &editableTrack) {
                editableTrack.height = value;
            })) {
            return;
        }
        refreshClipInspector();
    });

    connect(m_trackVisualModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_timeline || !m_trackVisualModeCombo) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        const TrackVisualMode mode = static_cast<TrackVisualMode>(
            m_trackVisualModeCombo->currentData().toInt());
        if (m_timeline->updateTrackVisualMode(trackIndex, mode)) {
            refreshClipInspector();
        }
    });

    connect(m_trackAudioEnabledCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_timeline || !m_trackAudioEnabledCheckBox) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->updateTrackAudioEnabled(trackIndex, checked)) {
            refreshClipInspector();
        }
    });

    connect(m_trackCrossfadeButton, &QPushButton::clicked, this, [this]() {
        if (!m_timeline || !m_trackCrossfadeButton || !m_trackCrossfadeSecondsSpin) {
            return;
        }
        const int trackIndex = m_timeline->selectedTrackIndex();
        if (trackIndex < 0) {
            return;
        }
        if (m_timeline->crossfadeTrack(trackIndex, m_trackCrossfadeSecondsSpin->value())) {
            refreshClipInspector();
        }
    });
}

void EditorWindow::setupPreviewControls()
{
    connect(m_previewHideOutsideOutputCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_preview) {
            m_preview->setHideOutsideOutputWindow(checked);
        }
        scheduleSaveState();
        pushHistorySnapshot();
    });

    // Preview zoom controls
    if (m_previewZoomSpin) {
        connect(m_previewZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            if (m_preview) {
                m_preview->setPreviewZoom(value);
            }
        });
    }
    
    if (m_previewZoomResetButton) {
        connect(m_previewZoomResetButton, &QPushButton::clicked, this, [this]() {
            if (m_preview) {
                m_preview->setPreviewZoom(1.0);
                m_preview->resetPreviewPan();
                if (m_previewZoomSpin) {
                    QSignalBlocker block(m_previewZoomSpin);
                    m_previewZoomSpin->setValue(1.0);
                }
            }
            scheduleSaveState();
        });
    }

    connect(m_inspectorPane->backgroundColorButton(), &QPushButton::clicked, this, [this]() {
        const QColor chosen = QColorDialog::getColor(m_backgroundColor, this, QStringLiteral("Background Color"));
        if (!chosen.isValid()) {
            return;
        }
        m_backgroundColor = chosen;
        auto *btn = m_inspectorPane->backgroundColorButton();
        btn->setText(chosen.name());
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; "
                           "border: 1px solid #2e3b4a; border-radius: 4px; padding: 4px 8px; }")
                .arg(chosen.name(),
                     chosen.lightness() > 128 ? QStringLiteral("#000000")
                                              : QStringLiteral("#ffffff")));
        if (m_preview) {
            m_preview->setBackgroundColor(m_backgroundColor);
        }
        scheduleSaveState();
    });
}
