#include "editor.h"
#include "preview_debug.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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
    splitter->addWidget(m_explorerPane);

    connect(m_explorerPane, &ExplorerPane::fileActivated, this, &EditorWindow::addFileToTimeline);
    connect(m_explorerPane, &ExplorerPane::transcriptionRequested, this, &EditorWindow::openTranscriptionWindow);
    connect(m_explorerPane, &ExplorerPane::folderRootChanged, this, [this](const QString& path) {
        // Update the projects root directory when explorer root changes
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

    qDebug() << "[STARTUP] Building editor pane...";
    QElapsedTimer editorPaneTimer;
    editorPaneTimer.start();
    splitter->addWidget(buildEditorPane());
    m_explorerPane->setPreviewWindow(m_preview);
    qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";

    m_inspectorPane = new InspectorPane(this);
    splitter->addWidget(m_inspectorPane);
    m_inspectorTabs = m_inspectorPane->tabs();
    if (m_inspectorTabs && m_preview) {
        static constexpr int kCorrectionsTabIndex = 3;
        auto syncCorrectionOverlayVisibility = [this]() {
            bool show = false;
            if (m_inspectorTabs) {
                const int index = m_inspectorTabs->currentIndex();
                show = index == kCorrectionsTabIndex;
            }
            if (m_preview) {
                m_preview->setShowCorrectionOverlays(show);
            }
            if (!show && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        };
        connect(m_inspectorTabs, &QTabWidget::currentChanged, this, [this, syncCorrectionOverlayVisibility](int) {
            syncCorrectionOverlayVisibility();
            scheduleSaveState();
        });
        syncCorrectionOverlayVisibility();
    }

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({320, 900, 280});

    setCentralWidget(central);
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
}

void EditorWindow::setupHeartbeat()
{
    m_mainThreadHeartbeatTimer.setInterval(100);
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
    m_stateSaveTimer.setInterval(250);
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
        [this]() { if (m_preview) m_preview->resetProfilingStats(); },
        [this](int64_t frame) { setCurrentFrame(frame, false); },
        nullptr, // renderResultCallback (empty/default)
        this);
    m_controlServer->start(controlPort);
    qDebug() << "[STARTUP] ControlServer started in" << ctorTimer.elapsed() << "ms";
}

void EditorWindow::setupAudioEngine()
{
    m_audioEngine = std::make_unique<AudioEngine>();
    m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
}

void EditorWindow::setupStartupLoad()
{
    QTimer::singleShot(0, this, [this]() {
        loadProjectsFromFolders();
        refreshProjectsList();
        loadState();
        setupAutosaveTimer();
        m_inspectorPane->refresh();
    });
}
