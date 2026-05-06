#pragma once

#include "editor_shared.h"
#include "frame_handle.h"
#include "render.h"
#include "audio_engine.h"
#include "control_server.h"
#include "timeline_widget.h"
#include "opengl_preview.h"
#include "preview_surface.h"
#include "editor_pane.h"
#include "explorer_pane.h"
#include "inspector_pane.h"
#include "output_tab.h"
#include "profile_tab.h"
#include "projects.h"
#include "transcript_tab.h"
#include "grading_tab.h"
#include "opacity_tab.h"
#include "effects_tab.h"
#include "corrections_tab.h"
#include "titles_tab.h"
#include "video_keyframe_tab.h"
#include "clips_tab.h"
#include "history_tab.h"
#include "sync_tab.h"
#include "tracks_tab.h"
#include "properties_tab.h"
#include "speakers_tab.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QLockFile>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFontComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QStringList>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVector>

#include <atomic>
#include <memory>

namespace editor {

enum class ProfileAccessBadge {
    Unknown = 0,
    Basic = 1,
    Subscribed = 2
};

class EditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EditorWindow(quint16 controlPort);
    ~EditorWindow() override;

    void addFileToTimeline(const QString &filePath, int64_t startFrame = -1);
    bool prepareVulkanBoxStreamPreviewRun(const QString& filePath,
                                          bool createHarnessTranscript,
                                          QString* errorOut = nullptr);
    bool triggerGenerateBoxStreamForSelectedClip(QString* errorOut = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct PlaybackRuntimeConfig {
        qreal speed = 1.0;
        PlaybackClockSource clockSource = PlaybackClockSource::Auto;
        PlaybackAudioWarpMode audioWarpMode = PlaybackAudioWarpMode::Disabled;
        bool loopEnabled = false;
    };

    QWidget *buildEditorPane();

    void loadState();
    void openTranscriptionWindow(const QString &filePath, const QString &label);

    QString defaultProxyOutputPath(const TimelineClip &clip,
                                   const MediaProbeResult *knownProbe = nullptr,
                                   ProxyFormat format = ProxyFormat::ImageSequence) const;
    QString clipFileInfoSummary(const QString &filePath,
                                const MediaProbeResult *knownProbe = nullptr) const;
    void createProxyForClip(const QString &clipId, bool continueGeneration = false);
    void continueProxyForClip(const QString &clipId);
    void deleteProxyForClip(const QString &clipId);
    void requestAutoSyncForSelection(const QSet<QString>& selectedClipIds);

    void syncSliderRange();
    void focusGradingTab();
    void updateTransportLabels();
    QString frameToTimecode(int64_t frame) const;
    QJsonObject profilingSnapshot() const;
    QJsonObject startupProfileSnapshot() const;
    QJsonObject throttleConfigSnapshot() const;
    QJsonObject applyThrottleConfigPatch(const QJsonObject& patch);
    QJsonObject playbackConfigSnapshot() const;
    QJsonObject applyPlaybackConfigPatch(const QJsonObject& patch);
    void startupProfileMark(const QString& phase, const QJsonObject& extra = QJsonObject());

    void syncTranscriptTableToPlayhead();
    void syncKeyframeTableToPlayhead();
    void syncGradingTableToPlayhead();
    void syncOpacityTableToPlayhead();

    bool focusInTranscriptTable() const;
    bool focusInKeyframeTable() const;
    bool focusInGradingTable() const;
    bool focusInOpacityTable() const;
    bool focusInEditableInput() const;
    bool shouldBlockGlobalEditorShortcuts() const;

    void initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);
    void scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame);
    void cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame);

    // Root directory configuration (stored near executable in editor.config)
    QString configFilePath() const;
    QString rootDirPath() const;
    void setRootDirPath(const QString& path);

    QString projectsDirPath() const;
    QString currentProjectMarkerPath() const;
    QString currentProjectIdOrDefault() const;
    QString projectPath(const QString &projectId) const;
    QString stateFilePathForProject(const QString &projectId) const;
    QString historyFilePathForProject(const QString &projectId) const;
    QString stateFilePath() const;
    QString historyFilePath() const;
    QString sanitizedProjectId(const QString &name) const;
    void ensureProjectsDirectory() const;
    QStringList availableProjectIds() const;
    void ensureDefaultProjectExists() const;
    void loadProjectsFromFolders();
    void saveCurrentProjectMarker();
    QString currentProjectName() const;
    void refreshProjectsList();
    void switchToProject(const QString &projectId);
    void createProject();
    bool saveProjectPayload(const QString &projectId,
                            const QByteArray &statePayload,
                            const QByteArray &historyPayload);
    void saveProjectAs();
    void renameProject(const QString &projectId);
    QJsonObject buildStateJson() const;
    void scheduleSaveState();
    void saveStateNow();
    void saveHistoryNow();
    void pushHistorySnapshot();
    void undoHistory();
    void redoHistory();
    void restoreToHistoryIndex(int index);
    void setupAutosaveTimer();
    void saveAutosaveBackup();
    void applyStateJson(const QJsonObject &root);

    void advanceFrame();
    bool speechFilterPlaybackEnabled() const;
    int64_t filteredPlaybackSampleForAbsoluteSample(int64_t absoluteSample) const;
    QVector<ExportRangeSegment> effectivePlaybackRanges() const;
    int64_t nextPlaybackFrame(int64_t currentFrame) const;
    int64_t nextPlaybackSample(int64_t currentSample,
                               int64_t deltaSamples,
                               const QVector<ExportRangeSegment>& ranges,
                               bool* reachedEnd = nullptr) const;
    int64_t stepForwardFrame(int64_t currentFrame) const;
    int64_t stepBackwardFrame(int64_t currentFrame) const;
    QString clipLabelForId(const QString &clipId) const;
    QColor clipColorForId(const QString &clipId) const;
    bool parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const;
    void refreshSyncInspector();
    void onSyncTableSelectionChanged();
    void onSyncTableItemChanged(QTableWidgetItem* item);
    void onSyncTableItemDoubleClicked(QTableWidgetItem* item);
    void onSyncTableCustomContextMenu(const QPoint& pos);
    void refreshClipInspector();
    void refreshTracksTab();
    void onTrackTableItemChanged(QTableWidgetItem* item);
    void onRestartDecodersRequested();
    void refreshOutputInspector();
    void applyOutputRangeFromInspector();
    void renderFromOutputInspector();
    RenderRequest buildRenderRequestFromOutputControls() const;
    void renderTimelineFromOutputRequest(const RenderRequest &request);
    void exportVideoForSpeakersOnSelectedClip(const QStringList& speakerIds);
    void openAudioToolsDialog();
    void applyPreviewViewMode(const QString& modeText);
    void refreshAiIntegrationState();
    void configureAiGatewayLogin();
    void copySupabaseSignInUrl();
    void clearAiGatewayLogin();
    QString aiSecureStoreServiceName() const;
    bool readAiTokenFromSecureStore(QString* tokenOut,
                                    QString* refreshTokenOut = nullptr,
                                    QString* userIdOut = nullptr) const;
    bool writeAiTokenToSecureStore(const QString& token,
                                   const QString& refreshToken,
                                   QString* errorOut = nullptr) const;
    bool clearAiTokenFromSecureStore(QString* errorOut = nullptr) const;
    void runAiTranscribeForSelection();
    void runAiFindSpeakerNames();
    void runAiFindOrganizations();
    void runAiCleanAssignments();
    void runAiSubscribeCheckout();
    void refreshAccessTabData();
    void runAiChatPrompt();
    void appendAiChatLine(const QString& role, const QString& text);
    void ensureAiActivityWindow(const QString& title, bool clearLog = false);
    void appendAiActivityLine(const QString& phase,
                              const QString& details,
                              const QString& exactText = QString());
    QString buildAiUiHierarchySnapshot() const;
    void updateProfileAvatarButton();
    void requestProfileAvatarImage(const QString& imageUrl);
    QPixmap buildProfileAvatarChip() const;
    void onProfileAvatarButtonClicked();
    QJsonObject buildAiProjectContext() const;
    QJsonObject runAiAction(const QString& action, const QJsonObject& payload, bool* okOut = nullptr, QString* errorOut = nullptr);
    void refreshProfileInspector();
    void runDecodeBenchmarkFromProfile();
    bool profileBenchmarkClip(TimelineClip *out) const;
    QStringList availableHardwareDeviceTypes() const;
    void setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio = true, bool duringPlayback = false);
    void setCurrentFrame(int64_t frame, bool syncAudio = true);
    void setPlaybackSpeed(qreal speed);
    void setPlaybackClockSource(PlaybackClockSource source);
    void setPlaybackAudioWarpMode(PlaybackAudioWarpMode mode);
    PlaybackRuntimeConfig playbackRuntimeConfig() const;
    void applyPlaybackRuntimeConfig(const PlaybackRuntimeConfig& requestedConfig);
    bool shouldUseAudioMasterClock() const;
    qreal effectiveAudioWarpRate() const;
    void reconcileActivePlaybackAudioState();
    void updatePlaybackTimerInterval();
    void setPlaybackActive(bool playing);
    void togglePlayback();
    bool playbackActive() const;

    // Setup methods (defined in split files)
    void setupWindowChrome();
    void setupMainLayout(QElapsedTimer& ctorTimer);
    void bindInspectorWidgets();
    void setupPlaybackTimers();
    void setupShortcuts();
    void setupHeartbeat();
    void setupStateSaveTimer();
    void setupDeferredSeekTimers();
    void setupControlServer(quint16 controlPort, QElapsedTimer& ctorTimer);
    void setupAudioEngine();
    void setupSpeechFilterControls();
    void setupTrackInspectorControls();
    void setupPreviewControls();
    void setupTabs();
    void setupInspectorRefreshRouting();
    void setupStartupLoad();
    void bindEditorPaneWidgets(EditorPane* pane);
    void connectTransportControls(EditorPane* pane);
    void connectTimelineSignals();
    void connectPreviewSignals();
    bool handleTranscriptTableDelete(QObject* watched, QEvent* event);
    bool handleVideoKeyframeTableDelete(QObject* watched, QEvent* event);
    bool handleGradingKeyframeTableDelete(QObject* watched, QEvent* event);
    bool handleOpacityKeyframeTableDelete(QObject* watched, QEvent* event);
    void adjustGlobalFontSize(int deltaPoints);

    // Tab creation methods (editor_tabs.cpp)
    void createOutputTab();
    void createProfileTab();
    void createProjectsTab();
    void createTranscriptTab();
    void createGradingTab();
    void createOpacityTab();
    void createEffectsTab();
    void createCorrectionsTab();
    void createTitlesTab();
    void createVideoKeyframeTab();
    void createClipsTab();
    void createHistoryTab();
    void createSyncTab();
    void createTracksTab();
    void createPropertiesTab();
    void createSpeakersTab();

    QLabel *m_projectSectionLabel = nullptr;
    QListWidget *m_projectsList = nullptr;
    QPushButton *m_newProjectButton = nullptr;
    QPushButton *m_saveProjectAsButton = nullptr;
    QPushButton *m_renameProjectButton = nullptr;

    PreviewSurface *m_preview = nullptr;
    TimelineWidget *m_timeline = nullptr;

    QPushButton *m_playButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QComboBox *m_playbackSpeedCombo = nullptr;
    QComboBox *m_previewModeCombo = nullptr;
    QToolButton *m_audioToolsButton = nullptr;
    QLabel *m_aiStatusLabel = nullptr;
    QComboBox *m_aiModelCombo = nullptr;
    QPushButton *m_aiTranscribeButton = nullptr;
    QPushButton *m_aiFindSpeakerNamesButton = nullptr;
    QPushButton *m_aiFindOrganizationsButton = nullptr;
    QPushButton *m_aiCleanAssignmentsButton = nullptr;
    QPushButton *m_aiSubscribeButton = nullptr;
    QTextBrowser *m_aiChatHistoryEdit = nullptr;
    QTreeWidget *m_aiActivityLogTree = nullptr;
    QPlainTextEdit *m_aiChatInputLineEdit = nullptr;
    QPushButton *m_aiChatSendButton = nullptr;
    QPushButton *m_aiChatClearButton = nullptr;
    QPointer<QWidget> m_aiActivityDialog;
    QLabel *m_accessStatusLabel = nullptr;
    QTableWidget *m_accessTable = nullptr;
    QPushButton *m_accessRefreshButton = nullptr;
    QPushButton *m_profileAvatarButton = nullptr;
    QToolButton *m_profileCopyLoginButton = nullptr;
    QPointer<QNetworkAccessManager> m_aiProfileAvatarNetwork;
    QString m_aiProfileAvatarUrl;
    QPixmap m_aiProfileAvatarPixmap;
    bool m_aiProfileAvatarFetchInFlight = false;
    bool m_aiProfileAvatarLoadFailed = false;
    ProfileAccessBadge m_aiProfileBadge = ProfileAccessBadge::Unknown;
    QTimer m_profileAvatarAnimationTimer;
    int m_profileAvatarAnimationTick = 0;

    QToolButton *m_audioMuteButton = nullptr;
    QSlider *m_audioVolumeSlider = nullptr;
    QLabel *m_audioNowPlayingLabel = nullptr;

    QLabel *m_statusBadge = nullptr;
    QLabel *m_previewInfo = nullptr;

    QTabWidget *m_inspectorTabs = nullptr;

    QLabel *m_gradingClipLabel = nullptr;
    QLabel *m_gradingPathLabel = nullptr;
    QTableWidget *m_gradingKeyframeTable = nullptr;
    QCheckBox *m_gradingAutoScrollCheckBox = nullptr;
    QCheckBox *m_gradingFollowCurrentCheckBox = nullptr;
    QPushButton *m_gradingKeyAtPlayheadButton = nullptr;
    QPushButton *m_gradingFadeInButton = nullptr;
    QPushButton *m_gradingFadeOutButton = nullptr;
    QDoubleSpinBox *m_gradingFadeDurationSpin = nullptr;

    QLabel *m_videoInspectorClipLabel = nullptr;
    QLabel *m_videoInspectorDetailsLabel = nullptr;

    QLabel *m_keyframesInspectorClipLabel = nullptr;
    QLabel *m_keyframesInspectorDetailsLabel = nullptr;
    QCheckBox *m_keyframesAutoScrollCheckBox = nullptr;
    QCheckBox *m_keyframesFollowCurrentCheckBox = nullptr;

    QLabel *m_audioInspectorClipLabel = nullptr;
    QLabel *m_audioInspectorDetailsLabel = nullptr;
    QSpinBox *m_audioFadeSamplesSpin = nullptr;
    bool m_updatingAudioInspector = false;

    QLineEdit *m_transcriptInspectorClipLabel = nullptr;
    QLabel *m_transcriptInspectorDetailsLabel = nullptr;
    QLabel *m_clipInspectorClipLabel = nullptr;
    QLabel *m_clipProxyUsageLabel = nullptr;
    QLabel *m_clipPlaybackSourceLabel = nullptr;
    QLabel *m_clipOriginalInfoLabel = nullptr;
    QLabel *m_clipProxyInfoLabel = nullptr;
    QDoubleSpinBox *m_clipPlaybackRateSpin = nullptr;
    QLabel *m_trackInspectorLabel = nullptr;
    QLabel *m_trackInspectorDetailsLabel = nullptr;
    QLineEdit *m_trackNameEdit = nullptr;
    QSpinBox *m_trackHeightSpin = nullptr;
    QComboBox *m_trackVisualModeCombo = nullptr;
    QCheckBox *m_trackAudioEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_trackCrossfadeSecondsSpin = nullptr;
    QPushButton *m_trackCrossfadeButton = nullptr;
    QCheckBox *m_previewHideOutsideOutputCheckBox = nullptr;
    QCheckBox *m_previewShowSpeakerTrackPointsCheckBox = nullptr;
    QCheckBox *m_speakerShowBoxStreamBoxesCheckBox = nullptr;
    QComboBox *m_speakerBoxStreamOverlaySourceCombo = nullptr;
    QDoubleSpinBox *m_previewZoomSpin = nullptr;
    QPushButton *m_previewZoomResetButton = nullptr;
    QCheckBox *m_previewPlaybackCacheFallbackCheckBox = nullptr;
    QCheckBox *m_previewLeadPrefetchEnabledCheckBox = nullptr;
    QSpinBox *m_previewLeadPrefetchCountSpin = nullptr;
    QSpinBox *m_previewPlaybackWindowAheadSpin = nullptr;
    QSpinBox *m_previewVisibleQueueReserveSpin = nullptr;
    QSpinBox *m_timelineAudioEnvelopeGranularitySpin = nullptr;
    QCheckBox *m_preferencesFeatureAiPanelCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAiSpeakerCleanupCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAudioPreviewModeCheckBox = nullptr;
    QCheckBox *m_preferencesFeatureAudioDynamicsToolsCheckBox = nullptr;
    QCheckBox *m_audioAmplifyEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioAmplifyDbSpin = nullptr;
    QCheckBox *m_audioSpeakerHoverModalCheckBox = nullptr;
    QCheckBox *m_audioShowWaveformCheckBox = nullptr;
    QCheckBox *m_audioNormalizeEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioNormalizeTargetDbSpin = nullptr;
    QCheckBox *m_audioPeakReductionEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioPeakThresholdDbSpin = nullptr;
    QCheckBox *m_audioLimiterEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioLimiterThresholdDbSpin = nullptr;
    QCheckBox *m_audioCompressorEnabledCheckBox = nullptr;
    QDoubleSpinBox *m_audioCompressorThresholdDbSpin = nullptr;
    QDoubleSpinBox *m_audioCompressorRatioSpin = nullptr;
    QTableWidget *m_profileSummaryTable = nullptr;
    QPushButton *m_profileBenchmarkButton = nullptr;

    QLabel *m_syncInspectorClipLabel = nullptr;
    QLabel *m_syncInspectorDetailsLabel = nullptr;
    QPushButton *m_clearAllSyncPointsButton = nullptr;

    QDoubleSpinBox *m_brightnessSpin = nullptr;
    QDoubleSpinBox *m_contrastSpin = nullptr;
    QDoubleSpinBox *m_saturationSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    // Shadows/Midtones/Highlights (Lift/Gamma/Gain)
    QDoubleSpinBox *m_shadowsRSpin = nullptr;
    QDoubleSpinBox *m_shadowsGSpin = nullptr;
    QDoubleSpinBox *m_shadowsBSpin = nullptr;
    QDoubleSpinBox *m_midtonesRSpin = nullptr;
    QDoubleSpinBox *m_midtonesGSpin = nullptr;
    QDoubleSpinBox *m_midtonesBSpin = nullptr;
    QDoubleSpinBox *m_highlightsRSpin = nullptr;
    QDoubleSpinBox *m_highlightsGSpin = nullptr;
    QDoubleSpinBox *m_highlightsBSpin = nullptr;
    QCheckBox *m_bypassGradingCheckBox = nullptr;

    QSpinBox *m_outputWidthSpin = nullptr;
    QSpinBox *m_outputHeightSpin = nullptr;
    QSpinBox *m_exportStartSpin = nullptr;
    QSpinBox *m_exportEndSpin = nullptr;
    QComboBox *m_outputFormatCombo = nullptr;
    QComboBox *m_renderBackendCombo = nullptr;
    QLabel *m_outputRangeSummaryLabel = nullptr;
    QCheckBox *m_renderUseProxiesCheckBox = nullptr;
    QCheckBox *m_outputPlaybackCacheFallbackCheckBox = nullptr;
    QCheckBox *m_outputLeadPrefetchEnabledCheckBox = nullptr;
    QSpinBox *m_outputLeadPrefetchCountSpin = nullptr;
    QSpinBox *m_outputPlaybackWindowAheadSpin = nullptr;
    QSpinBox *m_outputVisibleQueueReserveSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxQueueDepthSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxInflightSpin = nullptr;
    QSpinBox *m_outputPrefetchMaxPerTickSpin = nullptr;
    QSpinBox *m_outputPrefetchSkipVisiblePendingThresholdSpin = nullptr;
    QSpinBox *m_outputDecoderLaneCountSpin = nullptr;
    QComboBox *m_outputDecodeModeCombo = nullptr;
    QCheckBox *m_outputDeterministicPipelineCheckBox = nullptr;
    QPushButton *m_outputResetPipelineDefaultsButton = nullptr;
    QSpinBox *m_autosaveIntervalMinutesSpin = nullptr;
    QSpinBox *m_autosaveMaxBackupsSpin = nullptr;
    QCheckBox *m_createImageSequenceCheckBox = nullptr;
    QComboBox *m_imageSequenceFormatCombo = nullptr;

    QDoubleSpinBox *m_videoTranslationXSpin = nullptr;
    QDoubleSpinBox *m_videoTranslationYSpin = nullptr;
    QDoubleSpinBox *m_videoRotationSpin = nullptr;
    QDoubleSpinBox *m_videoScaleXSpin = nullptr;
    QDoubleSpinBox *m_videoScaleYSpin = nullptr;
    QComboBox *m_videoInterpolationCombo = nullptr;

    QCheckBox *m_mirrorHorizontalCheckBox = nullptr;
    QCheckBox *m_mirrorVerticalCheckBox = nullptr;
    QCheckBox *m_lockVideoScaleCheckBox = nullptr;
    QCheckBox *m_keyframeSpaceCheckBox = nullptr;
    QCheckBox *m_keyframeSkipAwareTimingCheckBox = nullptr;

    QCheckBox *m_transcriptOverlayEnabledCheckBox = nullptr;
    QSpinBox *m_transcriptMaxLinesSpin = nullptr;
    QSpinBox *m_transcriptMaxCharsSpin = nullptr;
    QCheckBox *m_transcriptAutoScrollCheckBox = nullptr;
    QCheckBox *m_transcriptFollowCurrentWordCheckBox = nullptr;

    QSpinBox *m_transcriptPrependMsSpin = nullptr;
    QSpinBox *m_transcriptPostpendMsSpin = nullptr;
    QCheckBox *m_speechFilterEnabledCheckBox = nullptr;
    QSpinBox *m_speechFilterFadeSamplesSpin = nullptr;
    QCheckBox *m_speechFilterRangeCrossfadeCheckBox = nullptr;
    QComboBox *m_playbackClockSourceCombo = nullptr;
    QComboBox *m_playbackAudioWarpModeCombo = nullptr;
    int m_transcriptPrependMs = 150;
    int m_transcriptPostpendMs = 70;
    int m_speechFilterFadeSamples = 300;
    bool m_speechFilterRangeCrossfade = false;
    PlaybackClockSource m_playbackClockSource = PlaybackClockSource::Auto;
    PlaybackAudioWarpMode m_playbackAudioWarpMode = PlaybackAudioWarpMode::Disabled;
    bool m_playbackLoopEnabled = false;
    mutable TranscriptEngine m_transcriptEngine;

    QDoubleSpinBox *m_transcriptOverlayXSpin = nullptr;
    QDoubleSpinBox *m_transcriptOverlayYSpin = nullptr;
    QSpinBox *m_transcriptOverlayWidthSpin = nullptr;
    QSpinBox *m_transcriptOverlayHeightSpin = nullptr;

    QFontComboBox *m_transcriptFontFamilyCombo = nullptr;
    QSpinBox *m_transcriptFontSizeSpin = nullptr;
    QCheckBox *m_transcriptBoldCheckBox = nullptr;
    QCheckBox *m_transcriptItalicCheckBox = nullptr;

    QTableWidget *m_videoKeyframeTable = nullptr;
    QTableWidget *m_transcriptTable = nullptr;
    QTableWidget *m_syncTable = nullptr;
    QTableWidget *m_opacityKeyframeTable = nullptr;

    QPushButton *m_addVideoKeyframeButton = nullptr;
    QPushButton *m_removeVideoKeyframeButton = nullptr;
    QPushButton *m_flipHorizontalButton = nullptr;
    QPushButton *m_renderButton = nullptr;
    QString m_lastRenderOutputPath;
    QString m_lastAutoTranscriptSwitchClipId;

    std::unique_ptr<ControlServer> m_controlServer;
    std::unique_ptr<AudioEngine> m_audioEngine;
    std::unique_ptr<TranscriptTab> m_transcriptTab;
    std::unique_ptr<GradingTab> m_gradingTab;
    std::unique_ptr<OpacityTab> m_opacityTab;
    std::unique_ptr<EffectsTab> m_effectsTab;
    std::unique_ptr<CorrectionsTab> m_correctionsTab;
    std::unique_ptr<TitlesTab> m_titlesTab;
    std::unique_ptr<VideoKeyframeTab> m_videoKeyframeTab;
    std::unique_ptr<OutputTab> m_outputTab;
    std::unique_ptr<ProfileTab> m_profileTab;
    std::unique_ptr<ProjectsTab> m_projectsTab;
    std::unique_ptr<ClipsTab> m_clipsTab;
    std::unique_ptr<HistoryTab> m_historyTab;
    std::unique_ptr<SyncTab> m_syncTab;
    std::unique_ptr<TracksTab> m_tracksTab;
    std::unique_ptr<PropertiesTab> m_propertiesTab;
    std::unique_ptr<SpeakersTab> m_speakersTab;

    ExplorerPane *m_explorerPane = nullptr;
    InspectorPane *m_inspectorPane = nullptr;
    EditorPane *m_editorPane = nullptr;

    QTimer m_playbackTimer;
    QTimer m_mainThreadHeartbeatTimer;
    QTimer m_stateSaveTimer;
    QTimer m_historySaveTimer;
    QTimer m_autosaveTimer;

    bool m_ignoreSeekSignal = false;
    bool m_loadingState = false;
    bool m_pendingSaveAfterLoad = false;
    bool m_pendingSaveAfterPlayback = false;
    static constexpr int kDefaultAutosaveIntervalMinutes = 5;
    static constexpr int kDefaultAutosaveMaxBackups = 20;
    int m_autosaveIntervalMinutes = kDefaultAutosaveIntervalMinutes;
    int m_autosaveMaxBackups = kDefaultAutosaveMaxBackups;
    bool m_restoringHistory = false;
    bool m_suppressHistorySnapshots = false;
    bool m_updatingTracksTab = false;

    QColor m_backgroundColor = QColor(Qt::black);
    int64_t m_absolutePlaybackSample = 0;
    int64_t m_filteredPlaybackSample = 0;
    int64_t m_lastPlaybackUiSyncMs = 0;
    int64_t m_lastPlaybackStateSaveMs = 0;
    qreal m_playbackSpeed = 1.0;
    double m_timelineAdvanceCarrySamples = 0.0;
    int64_t m_lastTimelineAdvanceTickMs = 0;
    QTimer m_transcriptClickSeekTimer;
    int64_t m_pendingTranscriptClickTimelineFrame = -1;
    QTimer m_keyframeClickSeekTimer;
    int64_t m_pendingKeyframeClickTimelineFrame = -1;
    QTimer m_gradingClickSeekTimer;
    int64_t m_pendingGradingClickTimelineFrame = -1;
    QTimer m_syncClickSeekTimer;
    int64_t m_pendingSyncClickTimelineFrame = -1;

    QString m_currentProjectId;

    QByteArray m_lastSavedState;
    QJsonArray m_historyEntries;
    int m_historyIndex = -1;
    QJsonObject m_lastDecodeBenchmark;
    bool m_renderInProgress = false;
    QJsonObject m_liveRenderProfile;
    QJsonObject m_lastRenderProfile;
    bool m_correctionsEnabled = true;
    PreviewSurface::AudioDynamicsSettings m_previewAudioDynamics;
    bool m_audioSpeakerHoverModalEnabled = true;
    bool m_audioWaveformVisible = true;
    QString m_previewViewMode = QStringLiteral("video");
    QString m_renderBackendPreference = QStringLiteral("vulkan");
    bool m_aiIntegrationEnabled = false;
    QString m_aiIntegrationStatus;
    QString m_aiServiceUrl;
    QString m_aiProxyBaseUrl;
    QString m_aiAuthToken;
    QString m_aiRefreshToken;
    QString m_aiRejectedAuthToken;
    QString m_aiUserId;
    QString m_aiSelectedModel = QStringLiteral("deepseek-chat");
    QString m_aiContractVersion = QStringLiteral("unknown");
    bool m_aiOAuthInProgress = false;
    bool m_featureAiPanel = true;
    bool m_featureAiSpeakerCleanup = true;
    bool m_featureAudioPreviewMode = true;
    bool m_featureAudioDynamicsTools = true;
    int m_aiUsageBudgetCap = 200;
    int m_aiUsageRequests = 0;
    int m_aiUsageFailures = 0;
    int m_aiRateLimitPerMinute = 12;
    int m_aiRequestTimeoutMs = 15000;
    int m_aiRequestRetries = 1;
    QStringList m_aiFallbackModels;
    QVector<qint64> m_aiRecentRequestEpochMs;
    struct AiChatMessage {
        QString role;
        QString content;
    };
    QVector<AiChatMessage> m_aiChatMessages;
    int m_aiChatMaxMessages = 24;

    bool m_updatingTranscriptInspector = false;
    bool m_updatingSyncInspector = false;
    QHash<QString, int64_t> m_previewDragAnchorFrameByClip;

    std::atomic<qint64> m_fastCurrentFrame{0};
    std::atomic<bool> m_fastPlaybackActive{false};
    std::atomic<qint64> m_lastMainThreadHeartbeatMs{0};
    std::atomic<qint64> m_lastPlayheadAdvanceMs{0};
    std::atomic<qint64> m_lastSetCurrentPlaybackSampleDurationMs{0};
    std::atomic<qint64> m_maxSetCurrentPlaybackSampleDurationMs{0};
    std::atomic<qint64> m_setCurrentPlaybackSampleSlowCount{0};
    std::atomic<qint64> m_lastInspectorRefreshDurationMs{0};
    std::atomic<qint64> m_maxInspectorRefreshDurationMs{0};
    std::atomic<qint64> m_inspectorRefreshSlowCount{0};
    QElapsedTimer m_startupProfileTimer;
    qint64 m_startupProfileLastMarkMs = 0;
    qint64 m_startupProfileCompletedMs = -1;
    bool m_startupProfileCompleted = false;
    QJsonArray m_startupProfileEvents;
    qint64 m_playbackUiSyncMinIntervalMs = 100;
    qint64 m_playbackStateSaveMinIntervalMs = 1000;
    qint64 m_slowSeekWarnThresholdMs = 20;
    int m_playbackStartLookaheadFrames = 5;
    int m_playbackStartLookaheadTimeoutMs = 1200;
    int m_mainThreadHeartbeatIntervalMs = 100;
    int m_stateSaveDebounceIntervalMs = 250;
    int m_transcriptManualSelectionHoldMs = 1200;
    int m_audioClockStallTicks = 0;
    int m_audioClockStallThresholdTicks = 1;
};

} // namespace editor
