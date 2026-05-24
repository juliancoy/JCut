#include "editor.h"
#include "opengl_preview_debug.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolTip>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

using namespace editor;

namespace editor_startup {
QJsonObject loadStartupStatePayload(const QString& projectId,
                                    const QString& statePath,
                                    const QString& historyPath);
}

namespace {
bool restVulkanDiagnosticsModeEnabled()
{
    const QString value = qEnvironmentVariable("JCUT_REST_VULKAN_DIAGNOSTICS").trimmed().toLower();
    return value == QStringLiteral("1") ||
           value == QStringLiteral("true") ||
           value == QStringLiteral("yes") ||
           value == QStringLiteral("on");
}
}

void EditorWindow::setupWindowChrome()
{
    setWindowTitle(QStringLiteral("JCut"));
    QScreen* targetScreen = screen();
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    const QRect available = targetScreen ? targetScreen->availableGeometry()
                                         : QRect(0, 0, 1280, 800);
    const int maxWidth = qMax(640, available.width() - 80);
    const int maxHeight = qMax(480, available.height() - 120);
    const int width = qMin(1500, maxWidth);
    const int height = qMin(860, maxHeight);
    resize(width, height);
    move(available.center() - rect().center());
}

void EditorWindow::setupMainLayout(QElapsedTimer &ctorTimer)
{
    if (restVulkanDiagnosticsModeEnabled()) {
        qputenv("JCUT_PREVIEW_BACKEND", "vulkan");
        qputenv("JCUT_VULKAN_PREVIEW_PRESENTER", "direct");
        qputenv("JCUT_RENDER_BACKEND", "vulkan");
        qDebug() << "[STARTUP] REST Vulkan diagnostics mode enabled; forcing Vulkan preview path";
    }

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
    m_explorerPane->setMinimumWidth(140);
    splitter->addWidget(m_explorerPane);

    connect(m_explorerPane, &ExplorerPane::fileActivated, this, [this](const QString& filePath) {
        addFileToTimeline(filePath);
    });
    connect(m_explorerPane, &ExplorerPane::transcriptionRequested, this, &EditorWindow::openTranscriptionWindow);
    connect(m_explorerPane, &ExplorerPane::folderRootChosen, this, [this](const QString& path) {
        // Persist only an explicit user-picked media root.
        if (m_projectManager) {
            m_projectManager->setRootDirPath(path);
            m_projectManager->loadProjectsFromFolders();
        }
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
        editorPane->setMinimumWidth(360);
    }
    splitter->addWidget(editorPane);
    m_explorerPane->setPreviewWindow(m_preview);
    qDebug() << "[STARTUP] Editor pane built in" << editorPaneTimer.elapsed() << "ms";
    startupProfileMark(QStringLiteral("layout.editor_pane.done"),
                       QJsonObject{{QStringLiteral("elapsed_ms"), editorPaneTimer.elapsed()}});

    m_inspectorPane = new InspectorPane(this);
    m_inspectorPane->setObjectName(QStringLiteral("column.inspector"));
    m_inspectorPane->setMinimumWidth(180);
    splitter->addWidget(m_inspectorPane);
    m_inspectorTabs = m_inspectorPane->tabs();
    if (m_inspectorPane) {
        QWidget* authHeader = new QWidget(m_inspectorPane);
        auto* authRow = new QHBoxLayout(authHeader);
        authRow->setContentsMargins(0, 0, 0, 0);
        authRow->setSpacing(4);

        m_profileAvatarButton = new QPushButton(authHeader);
        m_profileAvatarButton->setObjectName(QStringLiteral("tabs.profile_avatar_button"));
        m_profileAvatarButton->setCursor(Qt::PointingHandCursor);
        m_profileAvatarButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        m_profileAvatarButton->setMinimumHeight(30);
        m_profileAvatarButton->setToolTip(QStringLiteral("Sign in to sync purchases and licenses"));
        connect(m_profileAvatarButton, &QPushButton::clicked, this, [this]() {
            onProfileAvatarButtonClicked();
        });

        m_profileCopyLoginButton = new QToolButton(authHeader);
        m_profileCopyLoginButton->setObjectName(QStringLiteral("tabs.profile_copy_login_button"));
        m_profileCopyLoginButton->setCursor(Qt::PointingHandCursor);
        m_profileCopyLoginButton->setText(QStringLiteral("📋"));
        m_profileCopyLoginButton->setStyleSheet(QStringLiteral(
            "QToolButton#tabs\\.profile_copy_login_button {"
            " border: 1px solid #2e3b4a; border-radius: 6px; padding: 0; background: #1b2430; font-size: 14px; }"
            "QToolButton#tabs\\.profile_copy_login_button:hover { background: #233142; border-color: #4a5c71; }"));
        m_profileCopyLoginButton->setFixedSize(30, 30);
        m_profileCopyLoginButton->setToolTip(QStringLiteral("Copy browser sign-in URL"));
        connect(m_profileCopyLoginButton, &QToolButton::clicked, this, [this]() {
            copySupabaseSignInUrl();
        });

        authRow->addWidget(m_profileAvatarButton);
        authRow->addWidget(m_profileCopyLoginButton);
        m_inspectorPane->setHeaderWidget(authHeader);
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
    qApp->installEventFilter(this);

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
            refreshTimelineStructureInspectorViews();
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
            refreshTimelineStructureInspectorViews();
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
        shortcut->setAutoRepeat(true);
        const auto adjust = [this, deltaPoints]() {
            adjustGlobalFontSize(deltaPoints);
        };
        connect(shortcut, &QShortcut::activated, this, adjust);
        connect(shortcut, &QShortcut::activatedAmbiguously, this, adjust);
    };

    bindGlobalFontShortcut(QKeySequence::ZoomIn, +1);
    bindGlobalFontShortcut(QKeySequence::ZoomOut, -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl++")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+=")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+KP_Add")), +1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+-")), -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+_")), -1);
    bindGlobalFontShortcut(QKeySequence(QStringLiteral("Ctrl+KP_Subtract")), -1);
    bindGlobalFontShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus), -1);
    bindGlobalFontShortcut(QKeySequence(Qt::CTRL | Qt::Key_Underscore), -1);
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
    pointSize = qBound(8, pointSize + deltaPoints, 96);
    appFont.setPointSize(pointSize);
    QApplication::setFont(appFont);
    QToolTip::setFont(appFont);
    for (QWidget* widget : QApplication::allWidgets()) {
        if (!widget) {
            continue;
        }
        widget->setFont(appFont);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->updateGeometry();
        widget->update();
    }
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

    m_historySaveTimer.setSingleShot(true);
    m_historySaveTimer.setInterval(m_stateSaveDebounceIntervalMs);
    connect(&m_historySaveTimer, &QTimer::timeout, this, [this]() { saveHistoryNow(); });
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
            const QString projectId = m_projectManager
                ? m_projectManager->currentProjectIdOrDefault()
                : QStringLiteral("default");
            return QJsonObject{
                {QStringLiteral("currentProjectId"), projectId},
                {QStringLiteral("currentProjectName"), m_projectManager ? m_projectManager->currentProjectName() : QString()},
                {QStringLiteral("projectPath"), m_projectManager ? m_projectManager->projectPath(projectId) : QString()},
                {QStringLiteral("stateFilePath"), m_projectManager ? m_projectManager->stateFilePath() : QString()},
                {QStringLiteral("historyFilePath"), m_projectManager ? m_projectManager->historyFilePath() : QString()},
                {QStringLiteral("projectsDirPath"), m_projectManager ? m_projectManager->projectsDirPath() : QString()},
                {QStringLiteral("rootDirPath"), m_projectManager ? m_projectManager->rootDirPath() : QString()}
            };
        },
        [this]() {
            return QJsonObject{
                {QStringLiteral("index"), m_historyIndex},
                {QStringLiteral("entries"), m_historyEntries}
            };
        },
        [this]() { return profilingSnapshot(); },
        [this]() { return pipelineSnapshot(); },
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
    m_audioEngine->setTranscriptNormalizeEnabled(m_previewAudioDynamics.transcriptNormalizeEnabled);
    m_audioEngine->setTranscriptNormalizeRanges(effectiveTranscriptNormalizeRanges());
    m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
    // Pre-warm audio backend and decode workers off the startup event loop so
    // first playback doesn't pay full initialization/decode startup latency.
    if (!restVulkanDiagnosticsModeEnabled()) {
        QTimer::singleShot(0, this, [this]() {
            AudioEngine* const audioEngine = m_audioEngine.get();
            if (audioEngine) {
                (void)QtConcurrent::run([audioEngine]() {
                    audioEngine->initialize();
                });
            }
        });
    } else {
        qDebug() << "[STARTUP] REST Vulkan diagnostics mode: skipping audio backend prewarm";
    }
}

void EditorWindow::scheduleDeferredStartupUiWarmup(bool refreshProjects)
{
    QTimer::singleShot(0, this, [this, refreshProjects]() {
        if (refreshProjects) {
            refreshProjectsList();
        }
        refreshCurrentInspectorTab();
    });
}

void EditorWindow::setupStartupLoad()
{
    startupProfileMark(QStringLiteral("startup_load.queue_posted"));
    QTimer::singleShot(0, this, [this]() {
        startupProfileMark(QStringLiteral("startup_load.begin"));
        if (restVulkanDiagnosticsModeEnabled()) {
            qDebug() << "[STARTUP] REST Vulkan diagnostics mode: skipping project/state startup load";
            setupAutosaveTimer();
            startupProfileMark(QStringLiteral("startup_load.autosave_ready"));
            m_startupProfileCompletedMs = m_startupProfileTimer.isValid()
                ? m_startupProfileTimer.elapsed()
                : 0;
            startupProfileMark(QStringLiteral("startup_load.complete"),
                               QJsonObject{{QStringLiteral("total_ms"), m_startupProfileCompletedMs},
                                           {QStringLiteral("diagnostic_mode"), true}});
            m_startupProfileCompleted = true;
            scheduleDeferredStartupUiWarmup(false);
            return;
        }
        if (m_projectManager) {
            m_projectManager->loadProjectsFromFolders();
        }
        startupProfileMark(QStringLiteral("startup_load.projects_loaded"));
        const QString projectId = m_projectManager
            ? m_projectManager->currentProjectIdOrDefault()
            : QStringLiteral("default");
        const QString statePath = m_projectManager ? m_projectManager->stateFilePath() : QString();
        const QString historyPath = m_projectManager ? m_projectManager->historyFilePath() : QString();
        startupProfileMark(QStringLiteral("startup_load.state_parse.begin"),
                           QJsonObject{{QStringLiteral("project_id"), projectId}});
        m_startupStateLoadWatcher.setFuture(QtConcurrent::run([projectId, statePath, historyPath]() {
            return editor_startup::loadStartupStatePayload(projectId, statePath, historyPath);
        }));
    });
}
namespace editor_startup {
QJsonObject loadStartupStatePayload(const QString& projectId,
                                    const QString& statePath,
                                    const QString& historyPath);
}
