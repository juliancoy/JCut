#include "editor.h"
#include "preview_debug.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QHBoxLayout>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QWidget>

using namespace editor;

void EditorWindow::setupWindowChrome()
{
    setWindowTitle(QStringLiteral("JCut"));
    resize(1500, 900);
}

void EditorWindow::setupMainLayout(QElapsedTimer &ctorTimer)
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    splitter->setObjectName(QStringLiteral("layout.main_splitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    splitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background: #1e2a36; }"
        "QSplitter::handle:hover { background: #3a5068; }"));
    rootLayout->addWidget(splitter);

    qDebug() << "[STARTUP] Building explorer pane...";
    m_explorerPane = new ExplorerPane(this);
    m_explorerPane->setObjectName(QStringLiteral("column.explorer"));
    m_explorerPane->setMinimumWidth(220);
    splitter->addWidget(m_explorerPane);

    connect(m_explorerPane, &ExplorerPane::fileActivated, this, [this](const QString& filePath) {
        addFileToTimeline(filePath);
    });
    connect(m_explorerPane, &ExplorerPane::transcriptionRequested, this, &EditorWindow::openTranscriptionWindow);
    connect(m_explorerPane, &ExplorerPane::folderRootChanged, this, [this](const QString& path) {
        // Update the projects root directory when media root changes
        setRootDirPath(path);
        // Reload projects from the new root
        loadProjectsFromFolders();
        refreshProjectsList();
    });
    connect(m_explorerPane, &ExplorerPane::stateChanged, this, [this]() {
        scheduleSaveState();
        pushHistorySnapshot();
    });
    qDebug() << "[STARTUP] Explorer pane built in" << ctorTimer.elapsed() << "ms";
    startupProfileMark(QStringLiteral("layout.explorer_pane.done"),
                       QJsonObject{{QStringLiteral("ctor_elapsed_ms"), ctorTimer.elapsed()}});

    qDebug() << "[STARTUP] Building editor pane...";
    QElapsedTimer editorPaneTimer;
    editorPaneTimer.start();
    QWidget* editorPane = buildEditorPane();
    if (editorPane) {
        editorPane->setObjectName(QStringLiteral("column.editor"));
        editorPane->setMinimumWidth(520);
    }
    splitter->addWidget(editorPane);
    m_explorerPane->setPreviewWindow(m_preview);
    qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";
    startupProfileMark(QStringLiteral("layout.editor_pane.done"),
                       QJsonObject{{QStringLiteral("elapsed_ms"), editorPaneTimer.elapsed()}});

    m_inspectorPane = new InspectorPane(this);
    m_inspectorPane->setObjectName(QStringLiteral("column.inspector"));
    m_inspectorPane->setMinimumWidth(240);
    splitter->addWidget(m_inspectorPane);
    m_inspectorTabs = m_inspectorPane->tabs();
    if (m_inspectorTabs) {
        m_profileAvatarButton = new QPushButton(m_inspectorTabs);
        m_profileAvatarButton->setObjectName(QStringLiteral("tabs.profile_avatar_button"));
        m_profileAvatarButton->setCursor(Qt::PointingHandCursor);
        m_profileAvatarButton->setMinimumHeight(26);
        m_profileAvatarButton->setToolTip(QStringLiteral("Guest"));
        connect(m_profileAvatarButton, &QPushButton::clicked, this, [this]() {
            onProfileAvatarButtonClicked();
        });
        m_inspectorTabs->setCornerWidget(m_profileAvatarButton, Qt::TopRightCorner);
        updateProfileAvatarButton();
    }
    if (m_inspectorTabs && m_preview) {
        const auto isTabNamed = [this](const QString& name) -> bool {
            if (!m_inspectorTabs) {
                return false;
            }
            const int index = m_inspectorTabs->currentIndex();
            return index >= 0 && m_inspectorTabs->tabText(index).compare(name, Qt::CaseInsensitive) == 0;
        };
        auto syncCorrectionOverlayVisibility = [this, isTabNamed]() {
            const bool show = isTabNamed(QStringLiteral("Corrections"));
            if (m_preview) {
                m_preview->setShowCorrectionOverlays(show);
            }
            if (!show && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        };
        auto syncTranscriptOverlayInteraction = [this, isTabNamed]() {
            if (!m_preview) {
                return;
            }
            const bool enabled = isTabNamed(QStringLiteral("Transcript"));
            m_preview->setTranscriptOverlayInteractionEnabled(enabled);
        };
        auto syncTitleOverlayInteraction = [this, isTabNamed]() {
            if (!m_preview) {
                return;
            }
            const bool titlesOnly = isTabNamed(QStringLiteral("Titles"));
            m_preview->setTitleOverlayInteractionOnly(titlesOnly);
        };
        connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [this, syncCorrectionOverlayVisibility](int) {
            syncCorrectionOverlayVisibility();
            scheduleSaveState();
        });
        connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [syncTranscriptOverlayInteraction](int) {
            syncTranscriptOverlayInteraction();
        });
        connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [syncTitleOverlayInteraction](int) {
            syncTitleOverlayInteraction();
        });
        syncCorrectionOverlayVisibility();
        syncTranscriptOverlayInteraction();
        syncTitleOverlayInteraction();
    }

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setCollapsible(2, false);
    splitter->setSizes({320, 900, 280});

    setCentralWidget(central);
    startupProfileMark(QStringLiteral("layout.central_widget.done"));
}

void EditorWindow::setupPlaybackTimers()
{
    connect(&m_playbackTimer, &QTimer::timeout, this, &EditorWindow::advanceFrame);
    m_playbackTimer.setTimerType(Qt::PreciseTimer);
    updatePlaybackTimerInterval();
}

void EditorWindow::setupShortcuts()
{
    auto *undoShortcut = new QShortcut(QKeySequence::Undo, this);
    connect(undoShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        undoHistory();
    });

    auto *redoShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")), this);
    connect(redoShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        redoHistory();
    });

    auto *splitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
    connect(splitShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline && m_timeline->splitSelectedClipAtFrame(m_timeline->currentFrame())) {
            m_inspectorPane->refresh();
        }
    });

    auto *razorShortcut = new QShortcut(QKeySequence(QStringLiteral("B")), this);
    connect(razorShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) return;
        if (!m_timeline) return;
        m_timeline->setToolMode(
            m_timeline->toolMode() == TimelineWidget::ToolMode::Razor
                ? TimelineWidget::ToolMode::Select
                : TimelineWidget::ToolMode::Razor);
    });

    auto *escRazorShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escRazorShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) return;
        if (m_timeline && m_timeline->toolMode() != TimelineWidget::ToolMode::Select)
            m_timeline->setToolMode(TimelineWidget::ToolMode::Select);
    });

    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, this);
    connect(deleteShortcut, &QShortcut::activated, this, [this]() {
        if (focusInTranscriptTable() || focusInKeyframeTable() || focusInGradingTable() || focusInOpacityTable() ||
            shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline && m_timeline->deleteSelectedClip()) {
            m_inspectorPane->refresh();
        }
    });

    auto *nudgeLeftShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Left")), this);
    connect(nudgeLeftShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) m_timeline->nudgeSelectedClip(-1);
    });

    auto *nudgeRightShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+Right")), this);
    connect(nudgeRightShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) m_timeline->nudgeSelectedClip(1);
    });

    auto *playbackShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(playbackShortcut, &QShortcut::activated, this, [this]() {
        if (shouldBlockGlobalEditorShortcuts()) {
            return;
        }
        if (m_timeline) togglePlayback();
    });

    auto bindGlobalFontShortcut = [this](const QKeySequence &sequence, int deltaPoints) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, deltaPoints]() {
            adjustGlobalFontSize(deltaPoints);
        });
    };

    bindGlobalFontShortcut(QKeySequence::ZoomIn, +1);
    bindGlobalFontShortcut(QKeySequence::ZoomOut, -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl++")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+=")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+KP_Add")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+-")), -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+_")), -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+KP_Subtract")), -1);
}

void EditorWindow::adjustGlobalFontSize(int deltaPoints)
{
    if (deltaPoints == 0) {
        return;
    }
    QFont appFont = QApplication::font();
    int pointSize = appFont.pointSize();
    if (pointSize <= 0) {
        pointSize = 10;
    }
    pointSize = qBound(8, pointSize + deltaPoints, 36);
    appFont.setPointSize(pointSize);
    QApplication::setFont(appFont);
}

void EditorWindow::setupHeartbeat()
{
    m_mainThreadHeartbeatTimer.setInterval(m_mainThreadHeartbeatIntervalMs);
    connect(&m_mainThreadHeartbeatTimer, &QTimer::timeout, this, [this]() {
        m_lastMainThreadHeartbeatMs.store(nowMs());
    });
    m_lastMainThreadHeartbeatMs.store(nowMs());
    m_mainThreadHeartbeatTimer.start();

    m_fastCurrentFrame.store(0);
    m_fastPlaybackActive.store(false);
}

void EditorWindow::setupStateSaveTimer()
{
    m_stateSaveTimer.setSingleShot(true);
    m_stateSaveTimer.setInterval(m_stateSaveDebounceIntervalMs);
    connect(&m_stateSaveTimer, &QTimer::timeout, this, [this]() { saveStateNow(); });
}

void EditorWindow::setupDeferredSeekTimers()
{
    initializeDeferredTimelineSeek(&m_transcriptClickSeekTimer, &m_pendingTranscriptClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_keyframeClickSeekTimer, &m_pendingKeyframeClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_gradingClickSeekTimer, &m_pendingGradingClickTimelineFrame);
    initializeDeferredTimelineSeek(&m_syncClickSeekTimer, &m_pendingSyncClickTimelineFrame);
}

void EditorWindow::setupControlServer(quint16 controlPort, QElapsedTimer &ctorTimer)
{
    if (controlPort == 0) {
        qDebug() << "[STARTUP] ControlServer disabled (--no-rest)";
        return;
    }

    m_controlServer = std::make_unique<ControlServer>(
        this,
        [this]() {
            const qint64 now = nowMs();
            const qint64 heartbeatMs = m_lastMainThreadHeartbeatMs.load();
            const qint64 playheadMs = m_lastPlayheadAdvanceMs.load();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid())},
                {QStringLiteral("current_frame"), m_fastCurrentFrame.load()},
                {QStringLiteral("playback_active"), m_fastPlaybackActive.load()},
                {QStringLiteral("main_thread_heartbeat_ms"), heartbeatMs},
                {QStringLiteral("main_thread_heartbeat_age_ms"), heartbeatMs > 0 ? now - heartbeatMs : -1},
                {QStringLiteral("last_playhead_advance_ms"), playheadMs},
                {QStringLiteral("last_playhead_advance_age_ms"), playheadMs > 0 ? now - playheadMs : -1}};
        },
        [this]() {
            return buildStateJson();
        },
        [this]() {
            const QString projectId = currentProjectIdOrDefault();
            return QJsonObject{
                {QStringLiteral("currentProjectId"), projectId},
                {QStringLiteral("currentProjectName"), currentProjectName()},
                {QStringLiteral("projectPath"), projectPath(projectId)},
                {QStringLiteral("stateFilePath"), stateFilePath()},
                {QStringLiteral("historyFilePath"), historyFilePath()},
                {QStringLiteral("projectsDirPath"), projectsDirPath()},
                {QStringLiteral("rootDirPath"), rootDirPath()}
            };
        },
        [this]() {
            return QJsonObject{
                {QStringLiteral("index"), m_historyIndex},
                {QStringLiteral("entries"), m_historyEntries}
            };
        },
        [this]() { return profilingSnapshot(); },
        [this]() {
            if (m_preview) m_preview->resetProfilingStats();
            resetTranscriptSpeakerTrackingProfiling();
        },
        [this](int64_t frame) { setCurrentFrame(frame, false); },
        [this]() { return throttleConfigSnapshot(); },
        [this](const QJsonObject& patch) { return applyThrottleConfigPatch(patch); },
        [this]() { return playbackConfigSnapshot(); },
        [this](const QJsonObject& patch) { return applyPlaybackConfigPatch(patch); },
        nullptr, // renderResultCallback (empty/default)
        this);
    m_controlServer->start(controlPort);
    qDebug() << "[STARTUP] ControlServer started in" << ctorTimer.elapsed() << "ms";
}

void EditorWindow::setupAudioEngine()
{
    m_audioEngine = std::make_unique<AudioEngine>();
    m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
    m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
    m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
    m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
    // Pre-warm audio backend and decode workers off the startup event loop so
    // first playback doesn't pay full initialization/decode startup latency.
    QTimer::singleShot(0, this, [this]() {
        if (m_audioEngine) {
            m_audioEngine->initialize();
        }
    });
}

void EditorWindow::setupStartupLoad()
{
    startupProfileMark(QStringLiteral("startup_load.queue_posted"));
    QTimer::singleShot(0, this, [this]() {
        startupProfileMark(QStringLiteral("startup_load.begin"));
        loadProjectsFromFolders();
        startupProfileMark(QStringLiteral("startup_load.projects_loaded"));
        refreshProjectsList();
        startupProfileMark(QStringLiteral("startup_load.projects_refreshed"));
        loadState();
        startupProfileMark(QStringLiteral("startup_load.state_loaded"));
        setupAutosaveTimer();
        startupProfileMark(QStringLiteral("startup_load.autosave_ready"));
        m_inspectorPane->refresh();
        startupProfileMark(QStringLiteral("startup_load.inspector_refreshed"));
        m_startupProfileCompletedMs = m_startupProfileTimer.isValid()
            ? m_startupProfileTimer.elapsed()
            : 0;
        startupProfileMark(QStringLiteral("startup_load.complete"),
                           QJsonObject{{QStringLiteral("total_ms"), m_startupProfileCompletedMs}});
        m_startupProfileCompleted = true;
    });
}
