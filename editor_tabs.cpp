#include "editor.h"
#include "history_tab.h"
#include "playback_debug.h"
#include "transform_skip_aware_timing.h"

#include <QColorDialog>
#include <QMessageBox>
#include <QSignalBlocker>

using namespace editor;

void EditorWindow::createOutputTab()
{
    m_outputTab = std::make_unique<OutputTab>(
        OutputTab::Widgets{
            m_outputWidthSpin, m_outputHeightSpin,
            m_exportStartSpin, m_exportEndSpin,
            m_outputFormatCombo, m_outputRangeSummaryLabel, m_renderUseProxiesCheckBox,
            m_outputPlaybackCacheFallbackCheckBox, m_outputLeadPrefetchEnabledCheckBox,
            m_outputLeadPrefetchCountSpin, m_outputPlaybackWindowAheadSpin, m_outputVisibleQueueReserveSpin,
            m_outputPrefetchMaxQueueDepthSpin, m_outputPrefetchMaxInflightSpin,
            m_outputPrefetchMaxPerTickSpin, m_outputPrefetchSkipVisiblePendingThresholdSpin,
            m_outputDecoderLaneCountSpin, m_outputDecodeModeCombo,
            m_outputDeterministicPipelineCheckBox, m_outputResetPipelineDefaultsButton,
            m_autosaveIntervalMinutesSpin, m_autosaveMaxBackupsSpin,
            m_createImageSequenceCheckBox, m_imageSequenceFormatCombo, m_renderButton},
        OutputTab::Dependencies{
            [this]() { return m_timeline != nullptr; },
            [this]() { return m_timeline && !m_timeline->clips().isEmpty(); },
            [this]() -> int64_t { return m_timeline ? m_timeline->totalFrames() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportStartFrame() : 0; },
            [this]() -> int64_t { return m_timeline ? m_timeline->exportEndFrame() : 0; },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t startFrame, int64_t endFrame) { if (m_timeline) m_timeline->setExportRange(startFrame, endFrame); },
            [this](const QSize& size) { if (m_preview) m_preview->setOutputSize(size); },
            [this]() { setPlaybackActive(false); },
            [this]() { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this]() { return m_timeline ? m_timeline->tracks() : QVector<TimelineTrack>{}; },
            [this]() { return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{}; },
            [this](const RenderRequest& request) { renderTimelineFromOutputRequest(request); },
            [this]() { return m_lastRenderOutputPath; },
            [this](const QString& path) {
                m_lastRenderOutputPath = path;
                scheduleSaveState();
            },
            [this]() { return m_autosaveIntervalMinutes; },
            [this](int minutes) {
                m_autosaveIntervalMinutes = qBound(1, minutes, 120);
                m_autosaveTimer.setInterval(m_autosaveIntervalMinutes * 60 * 1000);
            },
            [this]() { return m_autosaveMaxBackups; },
            [this](int maxBackups) {
                m_autosaveMaxBackups = qBound(1, maxBackups, 200);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_outputTab->wire();
}

void EditorWindow::createProfileTab()
{
    m_profileTab = std::make_unique<ProfileTab>(
        ProfileTab::Widgets{
            m_profileSummaryTable,
            m_profileBenchmarkButton,
            m_inspectorPane ? m_inspectorPane->profileH26xThreadingModeCombo() : nullptr},
        ProfileTab::Dependencies{
            [this]() { return profilingSnapshot(); },
            [this](TimelineClip* clipOut) { return profileBenchmarkClip(clipOut); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { scheduleSaveState(); }});
    m_profileTab->wire();
}

void EditorWindow::createProjectsTab()
{
    m_projectsTab = std::make_unique<ProjectsTab>(
        ProjectsTab::Widgets{
            m_projectSectionLabel,
            m_projectsList,
            m_newProjectButton,
            m_saveProjectAsButton,
            m_renameProjectButton},
        ProjectsTab::Dependencies{
            [this]() { return availableProjectIds(); },
            [this]() { return currentProjectName(); },
            [this](const QString& projectId) { return projectPath(projectId); },
            [this](const QString& projectId) { switchToProject(projectId); },
            [this]() { createProject(); },
            [this]() { saveProjectAs(); },
            [this](const QString& projectId) { renameProject(projectId); }});
    m_projectsTab->wire();
}

void EditorWindow::createTranscriptTab()
{
    m_transcriptTab = std::make_unique<TranscriptTab>(
        TranscriptTab::Widgets{
            m_transcriptInspectorClipLabel, m_transcriptInspectorDetailsLabel,
            m_transcriptTable, m_transcriptOverlayEnabledCheckBox,
            m_inspectorPane->transcriptBackgroundVisibleCheckBox(),
            m_inspectorPane->transcriptShowSpeakerTitleCheckBox(),
            m_transcriptMaxLinesSpin, m_transcriptMaxCharsSpin,
            m_transcriptAutoScrollCheckBox, m_transcriptFollowCurrentWordCheckBox,
            m_transcriptOverlayXSpin, m_transcriptOverlayYSpin,
            m_inspectorPane->transcriptCenterHorizontalButton(),
            m_inspectorPane->transcriptCenterVerticalButton(),
            m_transcriptOverlayWidthSpin, m_transcriptOverlayHeightSpin,
            m_transcriptFontFamilyCombo, m_transcriptFontSizeSpin,
            m_transcriptBoldCheckBox, m_transcriptItalicCheckBox,
            m_transcriptPrependMsSpin, m_transcriptPostpendMsSpin,
            m_speechFilterEnabledCheckBox, m_speechFilterFadeSamplesSpin,
            m_inspectorPane->transcriptUnifiedEditModeCheckBox(),
            m_inspectorPane->transcriptSearchFilterLineEdit(),
            m_inspectorPane->transcriptSpeakerFilterCombo(),
            m_inspectorPane->transcriptScriptVersionCombo(),
            m_inspectorPane->transcriptNewVersionButton(),
            m_inspectorPane->transcriptDeleteVersionButton(),
            m_inspectorPane->transcriptShowExcludedLinesCheckBox()},
        TranscriptTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() { return effectivePlaybackRanges(); },
            [this](int64_t frame) { setCurrentFrame(frame); }});
    m_transcriptTab->wire();
    m_transcriptTab->setManualSelectionHoldMs(m_transcriptManualSelectionHoldMs);

    connect(m_transcriptTab.get(), &TranscriptTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
        }
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        }
        m_inspectorPane->refresh();
    });

    connect(m_transcriptTab.get(), &TranscriptTab::speechFilterParametersChanged, this, [this]() {
        m_transcriptPrependMs = m_transcriptTab->transcriptPrependMs();
        m_transcriptPostpendMs = m_transcriptTab->transcriptPostpendMs();
        m_speechFilterFadeSamples = m_transcriptTab->speechFilterFadeSamples();
        m_transcriptEngine.invalidateCache();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        }
        m_inspectorPane->refresh();
    });
}

void EditorWindow::createSpeakersTab()
{
    m_speakersTab = std::make_unique<SpeakersTab>(
        SpeakersTab::Widgets{
            m_inspectorPane->speakersInspectorClipLabel(),
            m_inspectorPane->speakersInspectorDetailsLabel(),
            m_inspectorPane->speakersTable(),
            m_inspectorPane->selectedSpeakerIdLabel(),
            m_inspectorPane->selectedSpeakerRef1ImageLabel(),
            m_inspectorPane->selectedSpeakerRef2ImageLabel(),
            m_inspectorPane->selectedSpeakerPreviousSentenceButton(),
            m_inspectorPane->selectedSpeakerNextSentenceButton(),
            m_inspectorPane->selectedSpeakerRandomSentenceButton(),
            m_inspectorPane->speakerCurrentSentenceLabel(),
            m_inspectorPane->speakerSetReference1Button(),
            m_inspectorPane->speakerSetReference2Button(),
            m_inspectorPane->speakerPickReference1Button(),
            m_inspectorPane->speakerPickReference2Button(),
            m_inspectorPane->speakerClearReferencesButton(),
            m_inspectorPane->speakerRunAutoTrackButton(),
            m_inspectorPane->speakerBoxstreamSettingsButton(),
            m_inspectorPane->speakerEnableTrackingButton(),
            m_inspectorPane->speakerDisableTrackingButton(),
            m_inspectorPane->speakerDeletePointstreamButton(),
            m_inspectorPane->speakerGuideButton(),
            m_inspectorPane->speakerPrecropFacesButton(),
            m_inspectorPane->speakerAiFindNamesButton(),
            m_inspectorPane->speakerAiFindOrganizationsButton(),
            m_inspectorPane->speakerAiCleanAssignmentsButton(),
            m_inspectorPane->speakerTrackingStatusLabel(),
            m_inspectorPane->speakerFramingTargetXSpin(),
            m_inspectorPane->speakerFramingTargetYSpin(),
            m_inspectorPane->speakerFramingTargetBoxSpin(),
            m_inspectorPane->speakerFramingZoomEnabledCheckBox(),
            m_inspectorPane->speakerApplyFramingToClipCheckBox(),
            m_inspectorPane->speakerClipFramingStatusLabel(),
            m_inspectorPane->speakerRefsChipLabel(),
            m_inspectorPane->speakerPointstreamChipLabel(),
            m_inspectorPane->speakerTrackingChipButton(),
            m_inspectorPane->speakerStabilizeChipButton(),
            m_inspectorPane->speakerBoxStreamTable(),
            m_inspectorPane->speakerBoxStreamDetailsEdit()},
        SpeakersTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this]() -> QVector<RenderSyncMarker> {
                return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{};
            },
            [this](const QString& clipId, const std::function<void(TimelineClip&)>& updater) -> bool {
                return m_timeline && m_timeline->updateClipById(clipId, updater);
            },
            [this]() -> QSize {
                return m_preview ? m_preview->outputSize() : QSize(1080, 1920);
            },
            [this]() {
                if (!m_preview || !m_timeline) {
                    return;
                }
                m_preview->setTimelineClips(m_timeline->clips());
                m_preview->asWidget()->update();
            },
            [this](QString* errorOut) -> bool {
                refreshAiIntegrationState();
                if (!m_featureAiSpeakerCleanup) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI speaker actions disabled by feature flag.");
                    }
                    return false;
                }
                if (!m_aiIntegrationEnabled) {
                    if (errorOut) {
                        *errorOut = m_aiIntegrationStatus;
                    }
                    return false;
                }
                if (m_aiAuthToken.trimmed().isEmpty()) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("AI login required. Use top-right Log In.");
                    }
                    return false;
                }
                return true;
            },
            [this](const QStringList& speakerIds) {
                exportVideoForSpeakersOnSelectedClip(speakerIds);
            }});
    m_speakersTab->wire();

    connect(m_speakersTab.get(), &SpeakersTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        setTransformSkipAwareTimelineRanges(
            speechFilterPlaybackEnabled() ? ranges : QVector<ExportRangeSegment>{});
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
        }
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        }
        m_inspectorPane->refresh();
    });
}

void EditorWindow::createGradingTab()
{
    m_gradingTab = std::make_unique<GradingTab>(
        GradingTab::Widgets{
            m_gradingPathLabel, m_brightnessSpin, m_contrastSpin,
            m_saturationSpin,
            // Shadows/Midtones/Highlights
            m_shadowsRSpin, m_shadowsGSpin, m_shadowsBSpin,
            m_midtonesRSpin, m_midtonesGSpin, m_midtonesBSpin,
            m_highlightsRSpin, m_highlightsGSpin, m_highlightsBSpin,
            m_gradingKeyframeTable,
            m_gradingAutoScrollCheckBox, m_gradingFollowCurrentCheckBox,
            m_gradingKeyAtPlayheadButton,
            m_inspectorPane->gradingAutoOpposeButton(),
            m_inspectorPane->gradingCurveChannelCombo(),
            m_inspectorPane->gradingCurveThreePointLockCheckBox(),
            m_inspectorPane->gradingCurveSmoothingCheckBox(),
            m_inspectorPane->gradingHistogramWidget()},
        GradingTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {},
            [this]() -> QImage {
                if (!m_preview || !m_timeline) {
                    return QImage();
                }
                const QString clipId = m_timeline->selectedClipId();
                if (clipId.isEmpty()) {
                    return QImage();
                }
                return m_preview->latestPresentedFrameImageForClip(clipId);
            },
            [this]() -> bool {
                return !playbackActive();
            }});
    m_gradingTab->wire();
}

void EditorWindow::createOpacityTab()
{
    m_opacityTab = std::make_unique<OpacityTab>(
        OpacityTab::Widgets{
            m_inspectorPane->opacityPathLabel(),
            m_opacitySpin,
            m_inspectorPane->opacityKeyframeTable(),
            m_inspectorPane->opacityAutoScrollCheckBox(),
            m_inspectorPane->opacityFollowCurrentCheckBox(),
            m_inspectorPane->opacityKeyAtPlayheadButton(),
            m_inspectorPane->opacityFadeInButton(),
            m_inspectorPane->opacityFadeOutButton(),
            m_inspectorPane->opacityFadeDurationSpin()},
        OpacityTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_opacityTab->wire();
}

void EditorWindow::createEffectsTab()
{
    m_effectsTab = std::make_unique<EffectsTab>(
        EffectsTab::Widgets{
            m_inspectorPane->effectsPathLabel(),
            m_inspectorPane->maskFeatherSpin(),
            m_inspectorPane->maskFeatherGammaSpin(),
            m_inspectorPane->maskFeatherEnabledCheck(),
            nullptr},
        EffectsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this](const TimelineClip& clip) { return clipHasAlpha(clip); }});
    m_effectsTab->wire();
}

void EditorWindow::createCorrectionsTab()
{
    m_correctionsTab = std::make_unique<CorrectionsTab>(
        CorrectionsTab::Widgets{
            m_inspectorPane->correctionsClipLabel(),
            m_inspectorPane->correctionsStatusLabel(),
            m_inspectorPane->correctionsEnabledCheck(),
            m_inspectorPane->correctionsPolygonTable(),
            m_inspectorPane->correctionsVertexTable(),
            m_inspectorPane->correctionsDrawModeCheck(),
            m_inspectorPane->correctionsDrawPolygonButton(),
            m_inspectorPane->correctionsClosePolygonButton(),
            m_inspectorPane->correctionsCancelDraftButton(),
            m_inspectorPane->correctionsDeleteLastButton(),
            m_inspectorPane->correctionsClearAllButton()},
        CorrectionsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline ? m_timeline->updateClipById(id, updater) : false;
            },
            [this]() {
                if (m_preview && m_timeline) {
                    m_preview->setTimelineTracks(m_timeline->tracks());
                    m_preview->setTimelineClips(m_timeline->clips());
                }
            },
            [this]() { if (m_inspectorPane) m_inspectorPane->refresh(); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { return m_correctionsEnabled; },
            [this](bool enabled) {
                m_correctionsEnabled = enabled;
                if (m_preview) {
                    m_preview->setCorrectionsEnabled(enabled);
                    if (m_timeline) {
                        m_preview->setTimelineClips(m_timeline->clips());
                    }
                }
                scheduleSaveState();
                pushHistorySnapshot();
            },
            [this](bool enabled) { if (m_preview) m_preview->setCorrectionDrawMode(enabled); },
            [this](int polygonIndex) {
                if (m_preview) {
                    m_preview->setSelectedCorrectionPolygon(polygonIndex);
                }
            },
            [this](const QVector<QPointF>& points) {
                if (m_preview) {
                    m_preview->setCorrectionDraftPoints(points);
                }
            },
            [this](TimelineWidget::ToolMode mode) { if (m_timeline) m_timeline->setToolMode(mode); },
            [this]() -> TimelineWidget::ToolMode { return m_timeline ? m_timeline->toolMode() : TimelineWidget::ToolMode::Select; }});
    m_correctionsTab->wire();
}

void EditorWindow::createTitlesTab()
{
    m_titlesTab = std::make_unique<TitlesTab>(
        TitlesTab::Widgets{
            m_inspectorPane->titlesInspectorClipLabel(),
            m_inspectorPane->titlesInspectorDetailsLabel(),
            m_inspectorPane->titleKeyframeTable(),
            m_inspectorPane->titleTextEdit(),
            m_inspectorPane->titleXSpin(),
            m_inspectorPane->titleYSpin(),
            m_inspectorPane->titleFontSizeSpin(),
            m_inspectorPane->titleOpacitySpin(),
            m_inspectorPane->titleFontCombo(),
            m_inspectorPane->titleBoldCheck(),
            m_inspectorPane->titleItalicCheck(),
            m_inspectorPane->titleColorButton(),
            m_inspectorPane->titleShadowEnabledCheck(),
            m_inspectorPane->titleShadowColorButton(),
            m_inspectorPane->titleShadowOpacitySpin(),
            m_inspectorPane->titleShadowOffsetXSpin(),
            m_inspectorPane->titleShadowOffsetYSpin(),
            m_inspectorPane->titleWindowEnabledCheck(),
            m_inspectorPane->titleWindowColorButton(),
            m_inspectorPane->titleWindowOpacitySpin(),
            m_inspectorPane->titleWindowPaddingSpin(),
            m_inspectorPane->titleWindowFrameEnabledCheck(),
            m_inspectorPane->titleWindowFrameColorButton(),
            m_inspectorPane->titleWindowFrameOpacitySpin(),
            m_inspectorPane->titleWindowFrameWidthSpin(),
            m_inspectorPane->titleWindowFrameGapSpin(),
            m_inspectorPane->titleAutoScrollCheck(),
            m_inspectorPane->addTitleKeyframeButton(),
            m_inspectorPane->removeTitleKeyframeButton(),
            m_inspectorPane->titleCenterHorizontalButton(),
            m_inspectorPane->titleCenterVerticalButton()},
        TitlesTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_titlesTab->wire();
}

void EditorWindow::createVideoKeyframeTab()
{
    m_videoKeyframeTab = std::make_unique<VideoKeyframeTab>(
        VideoKeyframeTab::Widgets{
            m_keyframesInspectorClipLabel, m_keyframesInspectorDetailsLabel,
            m_videoKeyframeTable, m_videoTranslationXSpin, m_videoTranslationYSpin,
            m_videoRotationSpin, m_videoScaleXSpin, m_videoScaleYSpin,
            m_videoInterpolationCombo, m_mirrorHorizontalCheckBox,
            m_mirrorVerticalCheckBox, m_lockVideoScaleCheckBox,
            m_keyframeSpaceCheckBox, m_keyframeSkipAwareTimingCheckBox, m_keyframesAutoScrollCheckBox,
            m_keyframesFollowCurrentCheckBox, m_addVideoKeyframeButton, m_removeVideoKeyframeButton,
            m_flipHorizontalButton},
        VideoKeyframeTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this](const QString& id, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline->updateClipById(id, updater);
            },
            [this](const TimelineClip& clip) { return clip.filePath; },
            [this](const TimelineClip& clip) { return clipHasVisuals(clip); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this]() { m_inspectorPane->refresh(); },
            [this]() { m_preview->setTimelineTracks(m_timeline->tracks()); m_preview->setTimelineClips(m_timeline->clips()); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this]() -> int64_t { return m_timeline && m_timeline->selectedClip() ? m_timeline->selectedClip()->startFrame : 0; },
            [this]() -> QString { return m_timeline ? m_timeline->selectedClipId() : QString(); },
            [this](int64_t frame) { setCurrentFrame(frame); },
            {},
            {}});
    m_videoKeyframeTab->wire();
}

void EditorWindow::createClipsTab()
{
    m_clipsTab = std::make_unique<ClipsTab>(
        ClipsTab::Widgets{m_inspectorPane->clipsTable()},
        ClipsTab::Dependencies{
            [this]() { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this]() { return m_timeline ? m_timeline->tracks() : QVector<TimelineTrack>{}; },
            [this](const QString& clipId) { return m_timeline ? m_timeline->deleteClipById(clipId) : false; },
            [this](const QString& clipId) { if (m_timeline) m_timeline->setSelectedClipId(clipId); },
            [this](const QString& clipId, const std::function<void(TimelineClip&)>& updater) {
                return m_timeline ? m_timeline->updateClipById(clipId, updater) : false;
            },
            [this](int trackIndex, const std::function<void(TimelineTrack&)>& updater) {
                return m_timeline ? m_timeline->updateTrackByIndex(trackIndex, updater) : false;
            },
            [this]() { pushHistorySnapshot(); },
            [this]() { scheduleSaveState(); }});
    m_clipsTab->wire();
}

void EditorWindow::createHistoryTab()
{
    m_historyTab = std::make_unique<HistoryTab>(
        HistoryTab::Widgets{m_inspectorPane->historyTable()},
        HistoryTab::Dependencies{
            [this]() -> QJsonArray { return m_historyEntries; },
            [this]() -> int { return m_historyIndex; },
            [this](int index) { restoreToHistoryIndex(index); },
            [this]() { pushHistorySnapshot(); }});
    m_historyTab->wire();
}

void EditorWindow::createSyncTab()
{
    m_syncTab = std::make_unique<SyncTab>(
        SyncTab::Widgets{
            m_syncInspectorClipLabel,
            m_syncInspectorDetailsLabel,
            m_syncTable,
            m_clearAllSyncPointsButton},
        SyncTab::Dependencies{
            [this]() -> const TimelineClip* { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() -> QVector<RenderSyncMarker> { return m_timeline ? m_timeline->renderSyncMarkers() : QVector<RenderSyncMarker>{}; },
            [this](const QVector<RenderSyncMarker>& markers) { if (m_timeline) m_timeline->setRenderSyncMarkers(markers); },
            [this]() -> int64_t { return m_timeline ? m_timeline->currentFrame() : 0; },
            [this](int64_t frame) { setCurrentFrame(frame); },
            [this](const QString& clipId) { return clipLabelForId(clipId); },
            [this](const QString& clipId) { return clipColorForId(clipId); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); },
            [this](int markerCount) -> bool {
                const int response = QMessageBox::question(
                    this,
                    QStringLiteral("Clear All Sync Points"),
                    QStringLiteral("Remove all %1 sync points from the timeline?").arg(markerCount),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                return response == QMessageBox::Yes;
            },
            [this]() {
                QMessageBox::information(this,
                                         QStringLiteral("Clear Sync Points"),
                                         QStringLiteral("There are no sync points to clear."));
            }});
    m_syncTab->wire();
}

void EditorWindow::createTracksTab()
{
    m_tracksTab = std::make_unique<TracksTab>(
        TracksTab::Widgets{m_inspectorPane ? m_inspectorPane->tracksTable() : nullptr},
        TracksTab::Dependencies{
            [this]() -> QVector<TimelineTrack> { return m_timeline ? m_timeline->tracks() : QVector<TimelineTrack>{}; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackHasVisualClips(trackIndex) : false; },
            [this](int trackIndex) -> TrackVisualMode { return m_timeline ? m_timeline->trackVisualMode(trackIndex) : TrackVisualMode::Enabled; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackHasAudioClips(trackIndex) : false; },
            [this](int trackIndex) -> bool { return m_timeline ? m_timeline->trackAudioEnabled(trackIndex) : false; },
            [this](int trackIndex, TrackVisualMode mode) -> bool { return m_timeline ? m_timeline->updateTrackVisualMode(trackIndex, mode) : false; },
            [this](int trackIndex, bool enabled) -> bool { return m_timeline ? m_timeline->updateTrackAudioEnabled(trackIndex, enabled) : false; },
            [this]() { if (m_inspectorPane) m_inspectorPane->refresh(); },
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_tracksTab->wire();
}

void EditorWindow::createPropertiesTab()
{
    m_propertiesTab = std::make_unique<PropertiesTab>(
        PropertiesTab::Widgets{
            m_clipInspectorClipLabel,
            m_clipProxyUsageLabel,
            m_clipPlaybackSourceLabel,
            m_clipOriginalInfoLabel,
            m_clipProxyInfoLabel,
            m_clipPlaybackRateSpin,
            m_trackInspectorLabel,
            m_trackInspectorDetailsLabel,
            m_trackNameEdit,
            m_trackHeightSpin,
            m_trackVisualModeCombo,
            m_trackAudioEnabledCheckBox,
            m_trackCrossfadeSecondsSpin,
            m_trackCrossfadeButton},
        PropertiesTab::Dependencies{
            [this]() -> const TimelineClip* { return m_timeline ? m_timeline->selectedClip() : nullptr; },
            [this]() -> const TimelineTrack* { return m_timeline ? m_timeline->selectedTrack() : nullptr; },
            [this]() -> int { return m_timeline ? m_timeline->selectedTrackIndex() : -1; },
            [this]() -> QVector<TimelineClip> { return m_timeline ? m_timeline->clips() : QVector<TimelineClip>{}; },
            [this](const TimelineClip& clip) { return playbackProxyPathForClip(clip); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this](const TimelineClip& clip, const MediaProbeResult* knownProbe) { return clipFileInfoSummary(clip.filePath, knownProbe); },
            [this](const QString& path) { return clipFileInfoSummary(path); },
            [this](const TimelineClip& clip) { return defaultProxyOutputPath(clip); }});
}

void EditorWindow::setupTabs()
{
    createOutputTab();
    createProfileTab();
    createProjectsTab();
    createTranscriptTab();
    createSpeakersTab();
    createGradingTab();
    createOpacityTab();
    createEffectsTab();
    createCorrectionsTab();
    createTitlesTab();
    createVideoKeyframeTab();
    createClipsTab();
    createHistoryTab();
    createSyncTab();
    createTracksTab();
    createPropertiesTab();

    // Ensure correction draw mode is disabled when Corrections tab is not selected
    if (m_inspectorPane && m_inspectorPane->tabs()) {
        connect(m_inspectorPane->tabs(), &QTabWidget::currentChanged, this,
                [this](int index) {
            if (m_preview) {
                const QString tabName = m_inspectorPane->tabs()->tabText(index);
                const bool isCorrectionsTab = tabName.compare(QStringLiteral("Corrections"), Qt::CaseInsensitive) == 0;
                const bool isTitlesTab = tabName.compare(QStringLiteral("Titles"), Qt::CaseInsensitive) == 0;
                const bool isAudioTab = tabName.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0;
                if (!isCorrectionsTab && m_preview->correctionDrawMode()) {
                    m_preview->setCorrectionDrawMode(false);
                    if (m_correctionsTab) {
                        m_correctionsTab->stopDrawing();
                    }
                }
                m_preview->setTitleOverlayInteractionOnly(isTitlesTab);

                if (isAudioTab) {
                    if (m_previewViewMode.compare(QStringLiteral("audio"), Qt::CaseInsensitive) != 0) {
                        applyPreviewViewMode(QStringLiteral("audio"));
                    }
                } else if (m_previewViewMode.compare(QStringLiteral("video"), Qt::CaseInsensitive) != 0) {
                    applyPreviewViewMode(QStringLiteral("video"));
                }
                if (m_previewModeCombo) {
                    const QSignalBlocker block(m_previewModeCombo);
                    const int modeIndex =
                        m_previewModeCombo->findData(m_previewViewMode, Qt::MatchFixedString);
                    if (modeIndex >= 0) {
                        m_previewModeCombo->setCurrentIndex(modeIndex);
                    }
                }
            }
        });
    }
}

void EditorWindow::setupInspectorRefreshRouting()
{
    connect(m_inspectorPane, &InspectorPane::refreshRequested, this, [this]() {
        QElapsedTimer refreshTimer;
        refreshTimer.start();
        m_gradingTab->refresh();
        if (m_opacityTab) m_opacityTab->refresh();
        m_effectsTab->refresh();
        if (m_correctionsTab) m_correctionsTab->refresh();
        m_titlesTab->refresh();
        if (m_syncTab) m_syncTab->refresh();
        m_transcriptTab->refresh();
        if (m_speakersTab) m_speakersTab->refresh();
        if (m_propertiesTab) m_propertiesTab->refresh();
        m_videoKeyframeTab->refresh();
        m_outputTab->refresh();
        m_profileTab->refresh();
        m_projectsTab->refresh();
        if (m_clipsTab) m_clipsTab->refresh();
        if (m_historyTab) m_historyTab->refresh();
        if (m_tracksTab) m_tracksTab->refresh();

        const qint64 elapsedMs = refreshTimer.elapsed();
        m_lastInspectorRefreshDurationMs.store(elapsedMs);
        qint64 maxDuration = m_maxInspectorRefreshDurationMs.load();
        while (elapsedMs > maxDuration &&
               !m_maxInspectorRefreshDurationMs.compare_exchange_weak(maxDuration, elapsedMs)) {
        }
        constexpr qint64 kSlowInspectorRefreshThresholdMs = 30;
        if (elapsedMs >= kSlowInspectorRefreshThresholdMs) {
            m_inspectorRefreshSlowCount.fetch_add(1);
            if (debugPlaybackWarnEnabled()) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] slow inspector refresh: %1 ms")
                           .arg(elapsedMs);
            }
        } else if (debugPlaybackVerboseEnabled()) {
            playbackTrace(QStringLiteral("EditorWindow::setupInspectorRefreshRouting.refresh"),
                          QStringLiteral("elapsed_ms=%1").arg(elapsedMs));
        }
    });
}
