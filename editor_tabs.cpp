#include "editor.h"

#include <QColorDialog>

using namespace editor;

void EditorWindow::createOutputTab()
{
    m_outputTab = std::make_unique<OutputTab>(
        OutputTab::Widgets{
            m_outputWidthSpin, m_outputHeightSpin,
            m_exportStartSpin, m_exportEndSpin,
            m_outputFormatCombo, m_outputRangeSummaryLabel, m_renderUseProxiesCheckBox, m_renderButton},
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
            [this]() { scheduleSaveState(); },
            [this]() { pushHistorySnapshot(); }});
    m_outputTab->wire();
}

void EditorWindow::createProfileTab()
{
    m_profileTab = std::make_unique<ProfileTab>(
        ProfileTab::Widgets{m_profileSummaryTable, m_profileBenchmarkButton},
        ProfileTab::Dependencies{
            [this]() { return profilingSnapshot(); },
            [this](TimelineClip* clipOut) { return profileBenchmarkClip(clipOut); },
            [this](const TimelineClip& clip) { return playbackMediaPathForClip(clip); },
            [this]() { m_inspectorPane->refresh(); }});
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
            m_transcriptMaxLinesSpin, m_transcriptMaxCharsSpin,
            m_transcriptAutoScrollCheckBox, m_transcriptFollowCurrentWordCheckBox,
            m_transcriptOverlayXSpin, m_transcriptOverlayYSpin,
            m_transcriptOverlayWidthSpin, m_transcriptOverlayHeightSpin,
            m_transcriptFontFamilyCombo, m_transcriptFontSizeSpin,
            m_transcriptBoldCheckBox, m_transcriptItalicCheckBox,
            m_transcriptPrependMsSpin, m_transcriptPostpendMsSpin,
            m_speechFilterEnabledCheckBox, m_speechFilterFadeSamplesSpin},
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

    connect(m_transcriptTab.get(), &TranscriptTab::transcriptDocumentChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        if (m_preview) {
            if (const TimelineClip* clip = m_timeline ? m_timeline->selectedClip() : nullptr) {
                m_preview->invalidateTranscriptOverlayCache(clip->filePath);
            } else {
                m_preview->invalidateTranscriptOverlayCache();
            }
        }
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        }
        m_inspectorPane->refresh();
    });

    connect(m_transcriptTab.get(), &TranscriptTab::speechFilterParametersChanged, this, [this]() {
        m_transcriptEngine.invalidateCache();
        const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
        if (m_preview) m_preview->setExportRanges(ranges);
        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
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
            m_gradingKeyAtPlayheadButton},
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
            {}});
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

void EditorWindow::setupTabs()
{
    createOutputTab();
    createProfileTab();
    createProjectsTab();
    createTranscriptTab();
    createGradingTab();
    createOpacityTab();
    createEffectsTab();
    createTitlesTab();
    createVideoKeyframeTab();
    createClipsTab();
}

void EditorWindow::setupInspectorRefreshRouting()
{
    connect(m_inspectorPane, &InspectorPane::refreshRequested, this, [this]() {
        m_gradingTab->refresh();
        if (m_opacityTab) m_opacityTab->refresh();
        m_effectsTab->refresh();
        m_titlesTab->refresh();
        refreshSyncInspector();
        m_transcriptTab->refresh();
        refreshClipInspector();
        m_videoKeyframeTab->refresh();
        m_outputTab->refresh();
        m_profileTab->refresh();
        m_projectsTab->refresh();
        if (m_clipsTab) m_clipsTab->refresh();
        if (m_inspectorPane && m_inspectorPane->tracksTable()) {
            refreshTracksTab();
        }
    });
}
