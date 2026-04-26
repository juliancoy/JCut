#include "editor.h"
#include "keyframe_table_shared.h"
#include "clip_serialization.h"
#include "transform_skip_aware_timing.h"
#include "debug_controls.h"
#include "speaker_export_harness.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QGridLayout>
#include <QPixmap>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTextStream>
#include <QTemporaryFile>
#include <QVBoxLayout>

#include <cmath>

using namespace editor;

#include "playback_debug.h"

// ============================================================================
// EditorWindow - Main application window
// ============================================================================
EditorWindow::EditorWindow(quint16 controlPort)
{
    QElapsedTimer ctorTimer;
    ctorTimer.start();

    setupWindowChrome();
    setupMainLayout(ctorTimer);
    bindInspectorWidgets();

    setupPlaybackTimers();
    setupShortcuts();
    setupHeartbeat();
    setupStateSaveTimer();
    setupDeferredSeekTimers();
    setupControlServer(controlPort, ctorTimer);
    setupAudioEngine();
    setupSpeechFilterControls();
    setupTrackInspectorControls();
    setupPreviewControls();
    setupTabs();
    setupInspectorRefreshRouting();
    setupStartupLoad();
}

EditorWindow::~EditorWindow()
{
    saveStateNow();
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    saveStateNow();
    QMainWindow::closeEvent(event);
}

void EditorWindow::syncTranscriptTableToPlayhead()
{
    if (!m_timeline || !m_transcriptTable || m_updatingTranscriptInspector) return;
    if (!m_transcriptFollowCurrentWordCheckBox || !m_transcriptFollowCurrentWordCheckBox->isChecked()) return;

    const TimelineClip *clip = m_timeline->selectedClip();
    if (!clip || !(clip->mediaType == ClipMediaType::Audio || clip->hasAudio)) {
        m_transcriptTable->clearSelection();
        return;
    }
    const qreal timelineFramePosition = samplesToFramePosition(m_absolutePlaybackSample);
    const qreal clipStart = static_cast<qreal>(clip->startFrame);
    const qreal clipEnd = static_cast<qreal>(clip->startFrame + qMax<int64_t>(0, clip->durationFrames - 1));
    if (timelineFramePosition < clipStart || timelineFramePosition > clipEnd) {
        m_transcriptTable->clearSelection();
        return;
    }

    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(
        *clip,
        m_absolutePlaybackSample,
        m_timeline->renderSyncMarkers());
    const double sourceSeconds = static_cast<double>(sourceSample) / static_cast<double>(kAudioSampleRate);
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
        *clip,
        m_absolutePlaybackSample,
        m_timeline->renderSyncMarkers());
    if (m_transcriptTab) {
        m_transcriptTab->syncTableToPlayhead(m_absolutePlaybackSample, sourceSeconds, sourceFrame);
    }
}

void EditorWindow::syncKeyframeTableToPlayhead()
{
    if (m_videoKeyframeTab) {
        m_videoKeyframeTab->syncTableToPlayhead();
    }
}

void EditorWindow::syncGradingTableToPlayhead()
{
    if (m_gradingTab) {
        m_gradingTab->syncTableToPlayhead();
    }
    if (m_opacityTab) {
        m_opacityTab->syncTableToPlayhead();
    }
}

void EditorWindow::syncOpacityTableToPlayhead()
{
    if (m_opacityTab) {
        m_opacityTab->syncTableToPlayhead();
    }
}

bool EditorWindow::focusInTranscriptTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_transcriptTable && focus && (focus == m_transcriptTable || m_transcriptTable->isAncestorOf(focus));
}

bool EditorWindow::focusInKeyframeTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_videoKeyframeTable && focus && (focus == m_videoKeyframeTable || m_videoKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInGradingTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_gradingKeyframeTable && focus && (focus == m_gradingKeyframeTable || m_gradingKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInOpacityTable() const
{
    QWidget *focus = QApplication::focusWidget();
    return m_opacityKeyframeTable && focus && (focus == m_opacityKeyframeTable || m_opacityKeyframeTable->isAncestorOf(focus));
}

bool EditorWindow::focusInEditableInput() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus) return false;
    
    if (qobject_cast<QLineEdit *>(focus) ||
        qobject_cast<QTextEdit *>(focus) ||
        qobject_cast<QPlainTextEdit *>(focus) ||
        qobject_cast<QAbstractSpinBox *>(focus))
    {
        return true;
    }
    if (auto *combo = qobject_cast<QComboBox *>(focus))
    {
        if (combo->isEditable()) return true;
    }
    for (QWidget *parent = focus->parentWidget(); parent; parent = parent->parentWidget())
    {
        if (qobject_cast<QLineEdit *>(parent) ||
            qobject_cast<QTextEdit *>(parent) ||
            qobject_cast<QPlainTextEdit *>(parent) ||
            qobject_cast<QAbstractSpinBox *>(parent) ||
            qobject_cast<QAbstractItemView *>(parent))
        {
            return true;
        }
    }
    return false;
}

bool EditorWindow::shouldBlockGlobalEditorShortcuts() const
{
    return focusInEditableInput();
}

void EditorWindow::initializeDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (!timer || !pendingFrame) return;
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, pendingFrame]()
    {
        if (!m_timeline || !pendingFrame || *pendingFrame < 0) return;
        setCurrentPlaybackSample(frameToSamples(*pendingFrame), false, true);
        *pendingFrame = -1;
    });
}

void EditorWindow::scheduleDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame, int64_t timelineFrame)
{
    if (!timer || !pendingFrame) return;
    *pendingFrame = timelineFrame;
    timer->start(QApplication::doubleClickInterval());
}

void EditorWindow::cancelDeferredTimelineSeek(QTimer *timer, int64_t *pendingFrame)
{
    if (timer) timer->stop();
    if (pendingFrame) *pendingFrame = -1;
}

void EditorWindow::undoHistory()
{
    if (m_historyIndex <= 0 || m_historyEntries.isEmpty())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex -= 1;
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::redoHistory()
{
    if (m_historyIndex >= m_historyEntries.size() - 1 || m_historyEntries.isEmpty())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex += 1;
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::restoreToHistoryIndex(int index)
{
    if (index < 0 || index >= m_historyEntries.size())
    {
        return;
    }

    m_restoringHistory = true;
    m_historyIndex = index;
    applyStateJson(m_historyEntries.at(m_historyIndex).toObject());
    m_restoringHistory = false;
    saveHistoryNow();
    scheduleSaveState();
}

void EditorWindow::applyStateJson(const QJsonObject &root)
{
    m_loadingState = true;

    // Default to the projects root from editor.config, then fall back to saved state, then current dir
    QString rootPath = root.value(QStringLiteral("explorerRoot")).toString(rootDirPath());
    if (rootPath.isEmpty() || !QDir(rootPath).exists()) {
        rootPath = QDir::currentPath();
    }
    QString galleryFolderPath = root.value(QStringLiteral("explorerGalleryPath")).toString();
    const int outputWidth = qMax(16, root.value(QStringLiteral("outputWidth")).toInt(1080));
    const int outputHeight = qMax(16, root.value(QStringLiteral("outputHeight")).toInt(1920));
    const QString outputFormat = root.value(QStringLiteral("outputFormat")).toString(QStringLiteral("mp4"));
    const QString lastRenderOutputPath = root.value(QStringLiteral("lastRenderOutputPath")).toString();
    const bool renderUseProxies = root.value(QStringLiteral("renderUseProxies")).toBool(false);
    const bool previewHideOutsideOutput = root.value(QStringLiteral("previewHideOutsideOutput")).toBool(false);
    const bool previewShowSpeakerTrackPoints =
        root.value(QStringLiteral("previewShowSpeakerTrackPoints")).toBool(false);
    const bool previewShowSpeakerTrackBoxes =
        root.value(QStringLiteral("previewShowSpeakerTrackBoxes")).toBool(false);
    const int autosaveIntervalMinutes = qBound(
        1,
        root.value(QStringLiteral("autosaveIntervalMinutes"))
            .toInt(kDefaultAutosaveIntervalMinutes),
        120);
    const int autosaveMaxBackups = qBound(
        1,
        root.value(QStringLiteral("autosaveMaxBackups"))
            .toInt(kDefaultAutosaveMaxBackups),
        200);
    const bool previewPlaybackCacheFallback =
        root.value(QStringLiteral("previewPlaybackCacheFallback")).toBool(editor::debugPlaybackCacheFallbackEnabled());
    const bool previewLeadPrefetchEnabled =
        root.value(QStringLiteral("previewLeadPrefetchEnabled")).toBool(editor::debugLeadPrefetchEnabled());
    const int previewLeadPrefetchCount =
        qBound(0, root.value(QStringLiteral("previewLeadPrefetchCount")).toInt(editor::debugLeadPrefetchCount()), 8);
    const int previewPlaybackWindowAhead =
        qBound(1, root.value(QStringLiteral("previewPlaybackWindowAhead")).toInt(editor::debugPlaybackWindowAhead()), 24);
    const int previewVisibleQueueReserve =
        qBound(0, root.value(QStringLiteral("previewVisibleQueueReserve")).toInt(editor::debugVisibleQueueReserve()), 64);
    const int debugPrefetchMaxQueueDepth =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxQueueDepth")).toInt(editor::debugPrefetchMaxQueueDepth()), 32);
    const int debugPrefetchMaxInflight =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxInflight")).toInt(editor::debugPrefetchMaxInflight()), 16);
    const int debugPrefetchMaxPerTick =
        qBound(1, root.value(QStringLiteral("debugPrefetchMaxPerTick")).toInt(editor::debugPrefetchMaxPerTick()), 16);
    const int debugPrefetchSkipVisiblePendingThreshold =
        qBound(0, root.value(QStringLiteral("debugPrefetchSkipVisiblePendingThreshold"))
                     .toInt(editor::debugPrefetchSkipVisiblePendingThreshold()),
               16);
    const int debugDecoderLaneCount =
        qBound(0, root.value(QStringLiteral("debugDecoderLaneCount")).toInt(editor::debugDecoderLaneCount()), 16);
    editor::DecodePreference debugDecodePreference = editor::debugDecodePreference();
    const QString debugDecodeModeText =
        root.value(QStringLiteral("debugDecodeMode"))
            .toString(editor::decodePreferenceToString(editor::debugDecodePreference()));
    editor::parseDecodePreference(debugDecodeModeText, &debugDecodePreference);
    editor::H26xSoftwareThreadingMode debugH26xSoftwareThreadingMode =
        editor::debugH26xSoftwareThreadingMode();
    const QString debugH26xSoftwareThreadingModeText =
        root.value(QStringLiteral("debugH26xSoftwareThreadingMode"))
            .toString(editor::h26xSoftwareThreadingModeToString(editor::debugH26xSoftwareThreadingMode()));
    editor::parseH26xSoftwareThreadingMode(debugH26xSoftwareThreadingModeText,
                                           &debugH26xSoftwareThreadingMode);
    const bool debugDeterministicPipeline =
        root.value(QStringLiteral("debugDeterministicPipeline"))
            .toBool(editor::debugDeterministicPipelineEnabled());
    const bool speechFilterEnabled = root.value(QStringLiteral("speechFilterEnabled")).toBool(false);
    const int transcriptPrependMs = root.value(QStringLiteral("transcriptPrependMs")).toInt(150);
    const int transcriptPostpendMs = root.value(QStringLiteral("transcriptPostpendMs")).toInt(70);
    const int speechFilterFadeSamples = root.value(QStringLiteral("speechFilterFadeSamples")).toInt(300);
    const bool speechFilterRangeCrossfade =
        root.value(QStringLiteral("speechFilterRangeCrossfade")).toBool(false);
    const bool transcriptUnifiedEditColors =
        root.value(QStringLiteral("transcriptUnifiedEditColors")).toBool(true);
    const bool transcriptShowExcludedLines =
        root.value(QStringLiteral("transcriptShowExcludedLines")).toBool(false);
    const QString transcriptSpeakerFilterValue =
        root.value(QStringLiteral("transcriptSpeakerFilterValue")).toString();
    const QString transcriptActiveCutPath =
        root.value(QStringLiteral("transcriptActiveCutPath")).toString();
    const QJsonArray transcriptColumnHidden =
        root.value(QStringLiteral("transcriptColumnHidden")).toArray();
    const bool transcriptFollowCurrentWord = root.value(QStringLiteral("transcriptFollowCurrentWord")).toBool(true);
    const bool correctionsEnabled = root.value(QStringLiteral("correctionsEnabled")).toBool(true);
    const bool gradingFollowCurrent = root.value(QStringLiteral("gradingFollowCurrent")).toBool(true);
    const bool gradingAutoScroll = root.value(QStringLiteral("gradingAutoScroll")).toBool(true);
    const bool gradingPreview = root.value(QStringLiteral("gradingPreview")).toBool(true);
    const bool keyframesFollowCurrent = root.value(QStringLiteral("keyframesFollowCurrent")).toBool(true);
    const bool keyframesAutoScroll = root.value(QStringLiteral("keyframesAutoScroll")).toBool(true);
    const int selectedInspectorTab = root.value(QStringLiteral("selectedInspectorTab")).toInt(0);
    const qreal playbackSpeed = qBound<qreal>(0.1, root.value(QStringLiteral("playbackSpeed")).toDouble(1.0), 3.0);
    const PlaybackClockSource playbackClockSource = playbackClockSourceFromString(
        root.value(QStringLiteral("playbackClockSource"))
            .toString(playbackClockSourceToString(PlaybackClockSource::Auto)));
    const PlaybackAudioWarpMode playbackAudioWarpMode = playbackAudioWarpModeFromString(
        root.value(QStringLiteral("playbackAudioWarpMode"))
            .toString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::Disabled)));
    const qreal timelineZoom = root.value(QStringLiteral("timelineZoom")).toDouble(4.0);
    const int timelineVerticalScroll = root.value(QStringLiteral("timelineVerticalScroll")).toInt(0);
    const int64_t exportStartFrame = root.value(QStringLiteral("exportStartFrame")).toVariant().toLongLong();
    const int64_t exportEndFrame = root.value(QStringLiteral("exportEndFrame")).toVariant().toLongLong();
    
    QVector<ExportRangeSegment> loadedExportRanges;
    const QJsonArray exportRanges = root.value(QStringLiteral("exportRanges")).toArray();
    loadedExportRanges.reserve(exportRanges.size());
    for (const QJsonValue &value : exportRanges)
    {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        ExportRangeSegment range;
        range.startFrame = qMax<int64_t>(0, obj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
        range.endFrame = qMax<int64_t>(0, obj.value(QStringLiteral("endFrame")).toVariant().toLongLong());
        loadedExportRanges.push_back(range);
    }
    
    QStringList expandedExplorerPaths;
    for (const QJsonValue &value : root.value(QStringLiteral("explorerExpandedFolders")).toArray())
    {
        const QString path = value.toString();
        if (!path.isEmpty()) expandedExplorerPaths.push_back(path);
    }
    
    QVector<TimelineClip> loadedClips;
    QVector<RenderSyncMarker> loadedRenderSyncMarkers;
    const int64_t currentFrame = root.value(QStringLiteral("currentFrame")).toVariant().toLongLong();
    const QString selectedClipId = root.value(QStringLiteral("selectedClipId")).toString();
    QVector<TimelineTrack> loadedTracks;

    const QJsonArray clips = root.value(QStringLiteral("timeline")).toArray();
    loadedClips.reserve(clips.size());
    for (const QJsonValue &value : clips)
    {
        if (!value.isObject()) continue;
        TimelineClip clip = clipFromJson(value.toObject());
        if (clip.trackIndex < 0) clip.trackIndex = loadedClips.size();
        if (!clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title)
            loadedClips.push_back(clip);
    }

    if (!m_restoringHistory && !loadedClips.isEmpty()) {
        const auto clipSourceExists = [](const TimelineClip& clip) {
            if (clip.filePath.isEmpty() || clip.mediaType == ClipMediaType::Title) {
                return true;
            }
            const QFileInfo info(clip.filePath);
            return info.exists() &&
                   (info.isFile() || (info.isDir() && isImageSequencePath(info.absoluteFilePath())));
        };

        QHash<QString, QString> relocatedByOriginalPath;
        QHash<QString, QString> relocatedDirByOriginalDir;
        bool skipRelocatePrompts = false;
        int relocatedCount = 0;
        int unresolvedCount = 0;

        for (TimelineClip& clip : loadedClips) {
            if (clipSourceExists(clip)) {
                continue;
            }

            const QString originalPath = QFileInfo(clip.filePath).absoluteFilePath();
            const QFileInfo originalInfo(originalPath);
            QString relocatedPath = relocatedByOriginalPath.value(originalPath);

            if (relocatedPath.isEmpty()) {
                const QString mappedDir = relocatedDirByOriginalDir.value(originalInfo.absolutePath());
                if (!mappedDir.isEmpty()) {
                    const QString candidatePath = QDir(mappedDir).filePath(originalInfo.fileName());
                    const QFileInfo candidateInfo(candidatePath);
                    if (candidateInfo.exists() &&
                        (candidateInfo.isFile() ||
                         (candidateInfo.isDir() && isImageSequencePath(candidateInfo.absoluteFilePath())))) {
                        relocatedPath = candidateInfo.absoluteFilePath();
                    }
                }
            }

            if (relocatedPath.isEmpty() && !skipRelocatePrompts) {
                QMessageBox prompt(this);
                prompt.setIcon(QMessageBox::Warning);
                prompt.setWindowTitle(QStringLiteral("Missing Media"));
                prompt.setText(QStringLiteral("Could not find media file:\n%1")
                                   .arg(QDir::toNativeSeparators(originalPath)));
                prompt.setInformativeText(QStringLiteral("Relocate this clip to a new file or folder?"));
                QPushButton* relocateButton =
                    prompt.addButton(QStringLiteral("Relocate"), QMessageBox::AcceptRole);
                QPushButton* skipButton =
                    prompt.addButton(QStringLiteral("Skip"), QMessageBox::RejectRole);
                QPushButton* skipAllButton =
                    prompt.addButton(QStringLiteral("Skip All"), QMessageBox::DestructiveRole);
                Q_UNUSED(skipButton);
                prompt.setDefaultButton(relocateButton);
                prompt.exec();

                if (prompt.clickedButton() == skipAllButton) {
                    skipRelocatePrompts = true;
                    ++unresolvedCount;
                    continue;
                }
                if (prompt.clickedButton() != relocateButton) {
                    ++unresolvedCount;
                    continue;
                }

                const QString startDir = originalInfo.dir().exists()
                    ? originalInfo.absolutePath()
                    : rootPath;
                QString selectedPath;
                if (clip.sourceKind == MediaSourceKind::ImageSequence) {
                    selectedPath = QFileDialog::getExistingDirectory(
                        this,
                        QStringLiteral("Locate Image Sequence Folder"),
                        startDir,
                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
                } else {
                    selectedPath = QFileDialog::getOpenFileName(
                        this,
                        QStringLiteral("Locate Media File"),
                        startDir);
                }

                if (selectedPath.isEmpty()) {
                    ++unresolvedCount;
                    continue;
                }
                relocatedPath = QFileInfo(selectedPath).absoluteFilePath();
            }

            if (relocatedPath.isEmpty()) {
                ++unresolvedCount;
                continue;
            }

            const QFileInfo relocatedInfo(relocatedPath);
            const bool relocatedValid =
                relocatedInfo.exists() &&
                (relocatedInfo.isFile() ||
                 (relocatedInfo.isDir() && isImageSequencePath(relocatedInfo.absoluteFilePath())));
            if (!relocatedValid) {
                ++unresolvedCount;
                continue;
            }

            const QString oldFileName = originalInfo.fileName();
            if (clip.label.trimmed().isEmpty() || clip.label == oldFileName) {
                clip.label = relocatedInfo.fileName();
            }
            clip.filePath = relocatedInfo.absoluteFilePath();
            clip.proxyPath.clear();

            relocatedByOriginalPath.insert(originalPath, clip.filePath);
            relocatedDirByOriginalDir.insert(originalInfo.absolutePath(), relocatedInfo.absolutePath());
            ++relocatedCount;
        }

        if (relocatedCount > 0) {
            m_pendingSaveAfterLoad = true;
        }

        if (relocatedCount > 0 || unresolvedCount > 0) {
            QString summary = QStringLiteral("Relocated %1 clip source(s).").arg(relocatedCount);
            if (unresolvedCount > 0) {
                summary += QStringLiteral("\n%1 clip source(s) are still missing.")
                               .arg(unresolvedCount);
            }
            QMessageBox::information(this, QStringLiteral("Media Relocation"), summary);
        }
    }

    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    loadedTracks.reserve(tracks.size());
    for (int i = 0; i < tracks.size(); ++i)
    {
        const QJsonObject obj = tracks.at(i).toObject();
        TimelineTrack track;
        track.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Track %1").arg(i + 1));
        track.height = qMax(28, obj.value(QStringLiteral("height")).toInt(44));
        if (obj.contains(QStringLiteral("visualMode"))) {
            track.visualMode = trackVisualModeFromString(obj.value(QStringLiteral("visualMode")).toString());
        } else if (obj.contains(QStringLiteral("visualEnabled")) &&
                   !obj.value(QStringLiteral("visualEnabled")).toBool(true)) {
            track.visualMode = TrackVisualMode::Hidden;
        }
        track.audioEnabled = obj.value(QStringLiteral("audioEnabled")).toBool(true);
        loadedTracks.push_back(track);
    }
       const QJsonArray renderSyncMarkers = root.value(QStringLiteral("renderSyncMarkers")).toArray();
    loadedRenderSyncMarkers.reserve(renderSyncMarkers.size());
    for (const QJsonValue &value : renderSyncMarkers)
    {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        RenderSyncMarker marker;
        marker.clipId = obj.value(QStringLiteral("clipId")).toString();
        marker.frame = qMax<int64_t>(0, obj.value(QStringLiteral("frame")).toVariant().toLongLong());
        marker.action = renderSyncActionFromString(obj.value(QStringLiteral("action")).toString());
        marker.count = qMax(1, obj.value(QStringLiteral("count")).toInt(1));
        loadedRenderSyncMarkers.push_back(marker);
    }

    const QString resolvedRootPath = QDir(rootPath).absolutePath();
    if (m_explorerPane) {
        m_explorerPane->setInitialRootPath(resolvedRootPath);
        m_explorerPane->restoreExpandedExplorerPaths(expandedExplorerPaths);
    }
    
    if (m_outputWidthSpin) { QSignalBlocker block(m_outputWidthSpin); m_outputWidthSpin->setValue(outputWidth); }
    if (m_outputHeightSpin) { QSignalBlocker block(m_outputHeightSpin); m_outputHeightSpin->setValue(outputHeight); }
    if (m_outputFormatCombo) {
        QSignalBlocker block(m_outputFormatCombo);
        const int formatIndex = m_outputFormatCombo->findData(outputFormat);
        if (formatIndex >= 0) m_outputFormatCombo->setCurrentIndex(formatIndex);
    }
    m_lastRenderOutputPath = lastRenderOutputPath;
    if (m_renderUseProxiesCheckBox) {
        QSignalBlocker block(m_renderUseProxiesCheckBox);
        m_renderUseProxiesCheckBox->setChecked(renderUseProxies);
    }
    m_autosaveIntervalMinutes = autosaveIntervalMinutes;
    m_autosaveMaxBackups = autosaveMaxBackups;
    if (m_autosaveIntervalMinutesSpin) {
        QSignalBlocker block(m_autosaveIntervalMinutesSpin);
        m_autosaveIntervalMinutesSpin->setValue(m_autosaveIntervalMinutes);
    }
    if (m_autosaveMaxBackupsSpin) {
        QSignalBlocker block(m_autosaveMaxBackupsSpin);
        m_autosaveMaxBackupsSpin->setValue(m_autosaveMaxBackups);
    }
    m_autosaveTimer.setInterval(m_autosaveIntervalMinutes * 60 * 1000);
    if (m_previewHideOutsideOutputCheckBox) {
        QSignalBlocker block(m_previewHideOutsideOutputCheckBox);
        m_previewHideOutsideOutputCheckBox->setChecked(previewHideOutsideOutput);
    }
    if (m_previewShowSpeakerTrackPointsCheckBox) {
        QSignalBlocker block(m_previewShowSpeakerTrackPointsCheckBox);
        m_previewShowSpeakerTrackPointsCheckBox->setChecked(previewShowSpeakerTrackPoints);
    }
    if (m_speakerShowBoxStreamBoxesCheckBox) {
        QSignalBlocker block(m_speakerShowBoxStreamBoxesCheckBox);
        m_speakerShowBoxStreamBoxesCheckBox->setChecked(previewShowSpeakerTrackBoxes);
    }
    if (m_previewPlaybackCacheFallbackCheckBox) {
        QSignalBlocker block(m_previewPlaybackCacheFallbackCheckBox);
        m_previewPlaybackCacheFallbackCheckBox->setChecked(previewPlaybackCacheFallback);
    }
    if (m_previewLeadPrefetchEnabledCheckBox) {
        QSignalBlocker block(m_previewLeadPrefetchEnabledCheckBox);
        m_previewLeadPrefetchEnabledCheckBox->setChecked(previewLeadPrefetchEnabled);
    }
    if (m_previewLeadPrefetchCountSpin) {
        QSignalBlocker block(m_previewLeadPrefetchCountSpin);
        m_previewLeadPrefetchCountSpin->setValue(previewLeadPrefetchCount);
        m_previewLeadPrefetchCountSpin->setEnabled(previewLeadPrefetchEnabled);
    }
    if (m_previewPlaybackWindowAheadSpin) {
        QSignalBlocker block(m_previewPlaybackWindowAheadSpin);
        m_previewPlaybackWindowAheadSpin->setValue(previewPlaybackWindowAhead);
    }
    if (m_previewVisibleQueueReserveSpin) {
        QSignalBlocker block(m_previewVisibleQueueReserveSpin);
        m_previewVisibleQueueReserveSpin->setValue(previewVisibleQueueReserve);
    }
    if (m_outputPlaybackCacheFallbackCheckBox) {
        QSignalBlocker block(m_outputPlaybackCacheFallbackCheckBox);
        m_outputPlaybackCacheFallbackCheckBox->setChecked(previewPlaybackCacheFallback);
    }
    if (m_outputLeadPrefetchEnabledCheckBox) {
        QSignalBlocker block(m_outputLeadPrefetchEnabledCheckBox);
        m_outputLeadPrefetchEnabledCheckBox->setChecked(previewLeadPrefetchEnabled);
    }
    if (m_outputLeadPrefetchCountSpin) {
        QSignalBlocker block(m_outputLeadPrefetchCountSpin);
        m_outputLeadPrefetchCountSpin->setValue(previewLeadPrefetchCount);
        m_outputLeadPrefetchCountSpin->setEnabled(previewLeadPrefetchEnabled);
    }
    if (m_outputPlaybackWindowAheadSpin) {
        QSignalBlocker block(m_outputPlaybackWindowAheadSpin);
        m_outputPlaybackWindowAheadSpin->setValue(previewPlaybackWindowAhead);
    }
    if (m_outputVisibleQueueReserveSpin) {
        QSignalBlocker block(m_outputVisibleQueueReserveSpin);
        m_outputVisibleQueueReserveSpin->setValue(previewVisibleQueueReserve);
    }
    if (m_outputPrefetchMaxQueueDepthSpin) {
        QSignalBlocker block(m_outputPrefetchMaxQueueDepthSpin);
        m_outputPrefetchMaxQueueDepthSpin->setValue(debugPrefetchMaxQueueDepth);
    }
    if (m_outputPrefetchMaxInflightSpin) {
        QSignalBlocker block(m_outputPrefetchMaxInflightSpin);
        m_outputPrefetchMaxInflightSpin->setValue(debugPrefetchMaxInflight);
    }
    if (m_outputPrefetchMaxPerTickSpin) {
        QSignalBlocker block(m_outputPrefetchMaxPerTickSpin);
        m_outputPrefetchMaxPerTickSpin->setValue(debugPrefetchMaxPerTick);
    }
    if (m_outputPrefetchSkipVisiblePendingThresholdSpin) {
        QSignalBlocker block(m_outputPrefetchSkipVisiblePendingThresholdSpin);
        m_outputPrefetchSkipVisiblePendingThresholdSpin->setValue(debugPrefetchSkipVisiblePendingThreshold);
    }
    if (m_outputDecoderLaneCountSpin) {
        QSignalBlocker block(m_outputDecoderLaneCountSpin);
        m_outputDecoderLaneCountSpin->setValue(debugDecoderLaneCount);
    }
    if (m_outputDecodeModeCombo) {
        QSignalBlocker block(m_outputDecodeModeCombo);
        const int decodeModeIndex =
            m_outputDecodeModeCombo->findData(editor::decodePreferenceToString(debugDecodePreference));
        if (decodeModeIndex >= 0) {
            m_outputDecodeModeCombo->setCurrentIndex(decodeModeIndex);
        }
    }
    if (m_outputDeterministicPipelineCheckBox) {
        QSignalBlocker block(m_outputDeterministicPipelineCheckBox);
        m_outputDeterministicPipelineCheckBox->setChecked(debugDeterministicPipeline);
    }
    if (m_speechFilterEnabledCheckBox) { QSignalBlocker block(m_speechFilterEnabledCheckBox); m_speechFilterEnabledCheckBox->setChecked(speechFilterEnabled); }
    
    m_transcriptPrependMs = transcriptPrependMs;
    m_transcriptPostpendMs = transcriptPostpendMs;
    m_speechFilterFadeSamples = qMax(0, speechFilterFadeSamples);
    m_speechFilterRangeCrossfade = speechFilterRangeCrossfade;
    
    if (m_transcriptPrependMsSpin) { QSignalBlocker block(m_transcriptPrependMsSpin); m_transcriptPrependMsSpin->setValue(m_transcriptPrependMs); }
    if (m_transcriptPostpendMsSpin) { QSignalBlocker block(m_transcriptPostpendMsSpin); m_transcriptPostpendMsSpin->setValue(m_transcriptPostpendMs); }
    if (m_speechFilterFadeSamplesSpin) { QSignalBlocker block(m_speechFilterFadeSamplesSpin); m_speechFilterFadeSamplesSpin->setValue(m_speechFilterFadeSamples); }
    if (m_speechFilterRangeCrossfadeCheckBox) {
        QSignalBlocker block(m_speechFilterRangeCrossfadeCheckBox);
        m_speechFilterRangeCrossfadeCheckBox->setChecked(m_speechFilterRangeCrossfade);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptUnifiedEditModeCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptUnifiedEditModeCheckBox());
        m_inspectorPane->transcriptUnifiedEditModeCheckBox()->setChecked(transcriptUnifiedEditColors);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptShowExcludedLinesCheckBox()) {
        QSignalBlocker block(m_inspectorPane->transcriptShowExcludedLinesCheckBox());
        m_inspectorPane->transcriptShowExcludedLinesCheckBox()->setChecked(transcriptShowExcludedLines);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptSpeakerFilterCombo()) {
        m_inspectorPane->transcriptSpeakerFilterCombo()->setProperty(
            "pendingSpeakerFilterValue", transcriptSpeakerFilterValue);
    }
    if (m_inspectorPane && m_inspectorPane->transcriptScriptVersionCombo()) {
        m_inspectorPane->transcriptScriptVersionCombo()->setProperty(
            "pendingCutPath", transcriptActiveCutPath);
    }
    if (m_transcriptTable && !transcriptColumnHidden.isEmpty()) {
        const int limit = qMin(m_transcriptTable->columnCount(), transcriptColumnHidden.size());
        for (int i = 0; i < limit; ++i) {
            if (i == 5) { // Text column must always be visible
                m_transcriptTable->setColumnHidden(i, false);
                continue;
            }
            m_transcriptTable->setColumnHidden(i, transcriptColumnHidden.at(i).toBool(false));
        }
    }
    
    if (m_transcriptFollowCurrentWordCheckBox) { QSignalBlocker block(m_transcriptFollowCurrentWordCheckBox); m_transcriptFollowCurrentWordCheckBox->setChecked(transcriptFollowCurrentWord); }
    if (m_gradingFollowCurrentCheckBox) { QSignalBlocker block(m_gradingFollowCurrentCheckBox); m_gradingFollowCurrentCheckBox->setChecked(gradingFollowCurrent); }
    if (m_gradingAutoScrollCheckBox) { QSignalBlocker block(m_gradingAutoScrollCheckBox); m_gradingAutoScrollCheckBox->setChecked(gradingAutoScroll); }
    if (m_bypassGradingCheckBox) { QSignalBlocker block(m_bypassGradingCheckBox); m_bypassGradingCheckBox->setChecked(gradingPreview); }
    if (m_keyframesFollowCurrentCheckBox) { QSignalBlocker block(m_keyframesFollowCurrentCheckBox); m_keyframesFollowCurrentCheckBox->setChecked(keyframesFollowCurrent); }
    if (m_keyframesAutoScrollCheckBox) { QSignalBlocker block(m_keyframesAutoScrollCheckBox); m_keyframesAutoScrollCheckBox->setChecked(keyframesAutoScroll); }
    if (m_inspectorPane && m_inspectorPane->correctionsEnabledCheck()) {
        QSignalBlocker block(m_inspectorPane->correctionsEnabledCheck());
        m_inspectorPane->correctionsEnabledCheck()->setChecked(correctionsEnabled);
    }
    m_correctionsEnabled = correctionsEnabled;
    if (m_preview) {
        m_preview->setCorrectionsEnabled(m_correctionsEnabled);
    }
    
    if (m_inspectorTabs && m_inspectorTabs->count() > 0) {
        const auto isTabNamed = [this](const QString& name) -> bool {
            if (!m_inspectorTabs) {
                return false;
            }
            const int index = m_inspectorTabs->currentIndex();
            return index >= 0 && m_inspectorTabs->tabText(index).compare(name, Qt::CaseInsensitive) == 0;
        };
        QSignalBlocker block(m_inspectorTabs);
        m_inspectorTabs->setCurrentIndex(qBound(0, selectedInspectorTab, m_inspectorTabs->count() - 1));
        if (m_preview) {
            const bool showCorrectionOverlays = isTabNamed(QStringLiteral("Corrections"));
            const bool transcriptOverlayInteractive = isTabNamed(QStringLiteral("Transcript"));
            const bool titleOverlayOnly = isTabNamed(QStringLiteral("Titles"));
            m_preview->setShowCorrectionOverlays(showCorrectionOverlays);
            m_preview->setTranscriptOverlayInteractionEnabled(transcriptOverlayInteractive);
            m_preview->setTitleOverlayInteractionOnly(titleOverlayOnly);
            if (!showCorrectionOverlays && m_correctionsTab) {
                m_correctionsTab->stopDrawing();
            }
        }
    }
    applyPlaybackRuntimeConfig(
        PlaybackRuntimeConfig{playbackSpeed, playbackClockSource, playbackAudioWarpMode});
    
    if (m_preview) {
        m_preview->setOutputSize(QSize(outputWidth, outputHeight));
        m_preview->setHideOutsideOutputWindow(previewHideOutsideOutput);
        m_preview->setShowSpeakerTrackPoints(previewShowSpeakerTrackPoints);
        m_preview->setShowSpeakerTrackBoxes(previewShowSpeakerTrackBoxes);
        m_preview->setBypassGrading(!gradingPreview);
    }
    editor::setDebugPlaybackCacheFallbackEnabled(previewPlaybackCacheFallback);
    editor::setDebugLeadPrefetchEnabled(previewLeadPrefetchEnabled);
    editor::setDebugLeadPrefetchCount(previewLeadPrefetchCount);
    editor::setDebugPlaybackWindowAhead(previewPlaybackWindowAhead);
    editor::setDebugVisibleQueueReserve(previewVisibleQueueReserve);
    editor::setDebugPrefetchMaxQueueDepth(debugPrefetchMaxQueueDepth);
    editor::setDebugPrefetchMaxInflight(debugPrefetchMaxInflight);
    editor::setDebugPrefetchMaxPerTick(debugPrefetchMaxPerTick);
    editor::setDebugPrefetchSkipVisiblePendingThreshold(debugPrefetchSkipVisiblePendingThreshold);
    editor::setDebugDecoderLaneCount(debugDecoderLaneCount);
    editor::setDebugDecodePreference(debugDecodePreference);
    editor::setDebugH26xSoftwareThreadingMode(debugH26xSoftwareThreadingMode);
    editor::setDebugDeterministicPipelineEnabled(debugDeterministicPipeline);

    m_timeline->setTracks(loadedTracks);
    m_timeline->setClips(loadedClips);
    m_timeline->setTimelineZoom(timelineZoom);
    m_timeline->setVerticalScrollOffset(timelineVerticalScroll);

    if (!loadedExportRanges.isEmpty()) {
        m_timeline->setExportRanges(loadedExportRanges);
    }

    m_timeline->setRenderSyncMarkers(loadedRenderSyncMarkers);
    m_timeline->setSelectedClipId(selectedClipId);
    syncSliderRange();

    const QVector<ExportRangeSegment> playbackRanges = effectivePlaybackRanges();
    setTransformSkipAwareTimelineRanges(
        speechFilterPlaybackEnabled() ? playbackRanges : QVector<ExportRangeSegment>{});
    
    m_preview->beginBulkUpdate();
    m_preview->setClipCount(m_timeline->clips().size());
    m_preview->setTimelineTracks(m_timeline->tracks());
    m_preview->setTimelineClips(m_timeline->clips());
    m_preview->setExportRanges(playbackRanges);
    m_preview->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
    m_preview->setSelectedClipId(selectedClipId);
    m_preview->endBulkUpdate();
    
    if (m_audioEngine) {
        m_audioEngine->setTimelineClips(m_timeline->clips());
        m_audioEngine->setExportRanges(playbackRanges);
        m_audioEngine->setRenderSyncMarkers(m_timeline->renderSyncMarkers());
        m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
        m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
        m_audioEngine->setPlaybackWarpMode(m_playbackAudioWarpMode);
        m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        m_audioEngine->seek(currentFrame);
    }
    
    setCurrentFrame(currentFrame);

    m_playbackTimer.stop();
    m_fastPlaybackActive.store(false);
    m_preview->setPlaybackState(false);
    if (m_audioEngine) {
        m_audioEngine->stop();
    }
    updateTransportLabels();

    m_loadingState = false;
    
    // Use the projects root from editor.config if available, otherwise use the saved explorer root
    const QString projectsRoot = rootDirPath();
    const QString explorerRoot = (!projectsRoot.isEmpty() && QDir(projectsRoot).exists()) 
        ? projectsRoot 
        : resolvedRootPath;
    
    QTimer::singleShot(0, this, [this, explorerRoot]() {
        if (m_explorerPane) {
            m_explorerPane->setInitialRootPath(explorerRoot);
        }
        // Ensure projects root is set to match explorer root
        setRootDirPath(explorerRoot);
        loadProjectsFromFolders();
        refreshProjectsList();
        m_inspectorPane->refresh();
    });
}



bool EditorWindow::parseSyncActionText(const QString &text, RenderSyncAction *actionOut) const
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("duplicate") || normalized == QStringLiteral("dup")) {
        *actionOut = RenderSyncAction::DuplicateFrame;
        return true;
    }
    if (normalized == QStringLiteral("skip")) {
        *actionOut = RenderSyncAction::SkipFrame;
        return true;
    }
    return false;
}

void EditorWindow::refreshSyncInspector()
{
    if (m_syncTab) {
        m_syncTab->refresh();
    }
}

void EditorWindow::onSyncTableSelectionChanged()
{
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableItemDoubleClicked(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::onSyncTableCustomContextMenu(const QPoint& pos)
{
    Q_UNUSED(pos);
    // Sync table interactions are handled by SyncTab.
}

void EditorWindow::refreshClipInspector()
{
    if (m_propertiesTab) {
        m_propertiesTab->refresh();
    }
}

void EditorWindow::refreshTracksTab()
{
    if (m_tracksTab) {
        m_tracksTab->refresh();
    }
}

void EditorWindow::onTrackTableItemChanged(QTableWidgetItem* item)
{
    Q_UNUSED(item);
    // Tracks table interactions are handled by TracksTab.
}

void EditorWindow::refreshOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->refresh();
    }
}

void EditorWindow::applyOutputRangeFromInspector()
{
    if (m_outputTab) {
        m_outputTab->applyRangeFromInspector();
    }
}

void EditorWindow::renderFromOutputInspector()
{
    if (m_outputTab) {
        m_outputTab->renderFromInspector();
    }
}

RenderRequest EditorWindow::buildRenderRequestFromOutputControls() const
{
    RenderRequest request;
    request.outputFormat = m_outputFormatCombo
        ? m_outputFormatCombo->currentData().toString()
        : QStringLiteral("mp4");
    if (request.outputFormat.isEmpty()) {
        request.outputFormat = QStringLiteral("mp4");
    }

    request.outputSize = QSize(
        m_outputWidthSpin ? m_outputWidthSpin->value() : 1080,
        m_outputHeightSpin ? m_outputHeightSpin->value() : 1920);
    request.useProxyMedia = m_renderUseProxiesCheckBox &&
                            m_renderUseProxiesCheckBox->isChecked();
    request.createVideoFromImageSequence = m_createImageSequenceCheckBox &&
                                           m_createImageSequenceCheckBox->isChecked();
    if (request.createVideoFromImageSequence && m_imageSequenceFormatCombo) {
        request.imageSequenceFormat = m_imageSequenceFormatCombo->currentData().toString();
        if (request.imageSequenceFormat.isEmpty()) {
            request.imageSequenceFormat = QStringLiteral("jpeg");
        }
    }
    return request;
}

void EditorWindow::renderTimelineFromOutputRequest(const RenderRequest &request)
{
    RenderRequest effectiveRequest = request;
    effectiveRequest.correctionsEnabled = m_correctionsEnabled;
    if (effectiveRequest.useProxyMedia)
    {
        for (TimelineClip &clip : effectiveRequest.clips)
        {
            const QString proxyPath = playbackProxyPathForClip(clip);
            if (!proxyPath.isEmpty())
            {
                clip.filePath = proxyPath;
            }
        }
    }

    int64_t totalFramesToRender = 0;
    for (const ExportRangeSegment &range : std::as_const(effectiveRequest.exportRanges))
    {
        totalFramesToRender += qMax<int64_t>(0, range.endFrame - range.startFrame + 1);
    }
    if (totalFramesToRender <= 0)
    {
        totalFramesToRender = qMax<int64_t>(1, effectiveRequest.exportEndFrame - effectiveRequest.exportStartFrame + 1);
    }

    const bool verticalRenderOutput =
        effectiveRequest.outputSize.height() > effectiveRequest.outputSize.width();

    QDialog progressDialog(this);
    progressDialog.setWindowTitle(QStringLiteral("Render Export"));
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumWidth(verticalRenderOutput ? 920 : 560);
    progressDialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f3ee; }"
        "QLabel { color: #1f2430; font-size: 13px; }"
        "QProgressBar { border: 1px solid #c9c2b8; border-radius: 6px; text-align: center; background: #ffffff; min-height: 20px; }"
        "QProgressBar::chunk { background: #2f7d67; border-radius: 5px; }"
        "QPushButton { min-width: 96px; padding: 6px 14px; }"));
    auto *progressLayout = new QVBoxLayout(&progressDialog);
    progressLayout->setContentsMargins(16, 16, 16, 16);
    progressLayout->setSpacing(10);

    auto *renderPreviewLabel = new QLabel(&progressDialog);
    renderPreviewLabel->setAlignment(Qt::AlignCenter);
    renderPreviewLabel->setMinimumSize(360, 202);
    renderPreviewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #11151c; color: #d9e1ea; border: 1px solid #c9c2b8; border-radius: 6px; }"));
    renderPreviewLabel->setText(QStringLiteral("Waiting for first rendered frame..."));

    auto *renderStatusLabel = new QLabel(QStringLiteral("Preparing render..."), &progressDialog);
    renderStatusLabel->setWordWrap(true);
    renderStatusLabel->setAlignment(Qt::AlignCenter);

    auto *showRenderPreviewCheckBox = new QCheckBox(QStringLiteral("Show Visual Preview"), &progressDialog);
    showRenderPreviewCheckBox->setChecked(true);

    auto *renderSourcesLabel = new QLabel(QStringLiteral("Sources In Use (Current Frame)"), &progressDialog);
    renderSourcesLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto *renderSourcesList = new QPlainTextEdit(&progressDialog);
    renderSourcesList->setReadOnly(true);
    renderSourcesList->setMinimumHeight(140);
    renderSourcesList->setPlainText(QStringLiteral("Waiting for first rendered frame..."));

    if (verticalRenderOutput) {
        auto *contentRow = new QHBoxLayout;
        contentRow->setSpacing(12);

        auto *leftColumn = new QVBoxLayout;
        leftColumn->setSpacing(10);
        leftColumn->addWidget(renderStatusLabel);
        leftColumn->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        leftColumn->addWidget(renderSourcesLabel);
        leftColumn->addWidget(renderSourcesList, 1);

        auto *rightColumn = new QVBoxLayout;
        rightColumn->setSpacing(10);
        rightColumn->addWidget(renderPreviewLabel, 1);

        contentRow->addLayout(leftColumn, 3);
        contentRow->addLayout(rightColumn, 2);
        progressLayout->addLayout(contentRow);
    } else {
        progressLayout->addWidget(renderStatusLabel);
        progressLayout->addWidget(showRenderPreviewCheckBox, 0, Qt::AlignLeft);
        progressLayout->addWidget(renderPreviewLabel);
        progressLayout->addWidget(renderSourcesLabel);
        progressLayout->addWidget(renderSourcesList);
    }

    auto *renderProgressBar = new QProgressBar(&progressDialog);
    renderProgressBar->setRange(0, static_cast<int>(qMin<int64_t>(totalFramesToRender, std::numeric_limits<int>::max())));
    renderProgressBar->setValue(0);
    progressLayout->addWidget(renderProgressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto *cancelRenderButton = new QPushButton(QStringLiteral("Cancel"), &progressDialog);
    buttonRow->addWidget(cancelRenderButton);
    progressLayout->addLayout(buttonRow);

    bool renderCancelled = false;
    QObject::connect(cancelRenderButton, &QPushButton::clicked, &progressDialog, [&renderCancelled, cancelRenderButton]() {
        renderCancelled = true;
        cancelRenderButton->setEnabled(false);
    });
    QObject::connect(showRenderPreviewCheckBox, &QCheckBox::toggled, &progressDialog, [renderPreviewLabel](bool checked) {
        renderPreviewLabel->setVisible(checked);
    });
    progressDialog.show();

    const QString outputPath = effectiveRequest.outputPath;
    const auto formatEta = [](qint64 remainingMs) -> QString
    {
        if (remainingMs <= 0)
        {
            return QStringLiteral("calculating...");
        }
        const qint64 totalSeconds = remainingMs / 1000;
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
        {
            return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
        }
        if (minutes > 0)
        {
            return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
        }
        return QStringLiteral("%1s").arg(seconds);
    };
    const auto stageSummary = [](qint64 stageMs, int64_t completedFrames) -> QString
    {
        if (stageMs <= 0 || completedFrames <= 0)
        {
            return QStringLiteral("0 ms");
        }
        return QStringLiteral("%1 ms total (%2 ms/frame)")
            .arg(stageMs)
            .arg(QString::number(static_cast<double>(stageMs) / static_cast<double>(completedFrames), 'f', 2));
    };
    const auto renderProfileFromProgress = [&formatEta](const RenderProgress &progress) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, progress.framesCompleted);
        const double fps = progress.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(progress.framesCompleted)) / static_cast<double>(progress.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("output_path"), QString()},
            {QStringLiteral("frames_completed"), static_cast<qint64>(progress.framesCompleted)},
            {QStringLiteral("total_frames"), static_cast<qint64>(progress.totalFrames)},
            {QStringLiteral("segment_index"), progress.segmentIndex},
            {QStringLiteral("segment_count"), progress.segmentCount},
            {QStringLiteral("timeline_frame"), static_cast<qint64>(progress.timelineFrame)},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(progress.segmentStartFrame)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(progress.segmentEndFrame)},
            {QStringLiteral("using_gpu"), progress.usingGpu},
            {QStringLiteral("using_hardware_encode"), progress.usingHardwareEncode},
            {QStringLiteral("encoder_label"), progress.encoderLabel},
            {QStringLiteral("elapsed_ms"), progress.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), progress.estimatedRemainingMs},
            {QStringLiteral("eta_text"), formatEta(progress.estimatedRemainingMs)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), progress.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), progress.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), progress.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), progress.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), progress.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), progress.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), progress.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), progress.convertStageMs},
            {QStringLiteral("encode_stage_ms"), progress.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), progress.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), progress.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), progress.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), progress.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), progress.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), progress.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), progress.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), progress.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), progress.renderStageTable},
            {QStringLiteral("worst_frame_table"), progress.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(progress.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(progress.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(progress.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(progress.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(progress.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(progress.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(progress.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(progress.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(progress.encodeStageMs) / static_cast<double>(completedFrames)}};
    };
    const auto renderProfileFromResult = [&formatEta, &outputPath](const RenderResult &result) -> QJsonObject
    {
        const qint64 completedFrames = qMax<int64_t>(1, result.framesRendered);
        const double fps = result.elapsedMs > 0
                               ? (1000.0 * static_cast<double>(result.framesRendered)) / static_cast<double>(result.elapsedMs)
                               : 0.0;
        return QJsonObject{
            {QStringLiteral("status"), result.success ? QStringLiteral("completed")
                                                      : (result.cancelled ? QStringLiteral("cancelled")
                                                                          : QStringLiteral("failed"))},
            {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
            {QStringLiteral("frames_completed"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("total_frames"), static_cast<qint64>(result.framesRendered)},
            {QStringLiteral("segment_index"), 0},
            {QStringLiteral("segment_count"), 0},
            {QStringLiteral("segment_start_frame"), static_cast<qint64>(0)},
            {QStringLiteral("segment_end_frame"), static_cast<qint64>(0)},
            {QStringLiteral("using_gpu"), result.usedGpu},
            {QStringLiteral("using_hardware_encode"), result.usedHardwareEncode},
            {QStringLiteral("encoder_label"), result.encoderLabel},
            {QStringLiteral("elapsed_ms"), result.elapsedMs},
            {QStringLiteral("estimated_remaining_ms"), static_cast<qint64>(0)},
            {QStringLiteral("eta_text"), formatEta(0)},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("render_stage_ms"), result.renderStageMs},
            {QStringLiteral("render_decode_stage_ms"), result.renderDecodeStageMs},
            {QStringLiteral("render_texture_stage_ms"), result.renderTextureStageMs},
            {QStringLiteral("render_composite_stage_ms"), result.renderCompositeStageMs},
            {QStringLiteral("render_nv12_stage_ms"), result.renderNv12StageMs},
            {QStringLiteral("gpu_readback_ms"), result.gpuReadbackMs},
            {QStringLiteral("overlay_stage_ms"), result.overlayStageMs},
            {QStringLiteral("convert_stage_ms"), result.convertStageMs},
            {QStringLiteral("encode_stage_ms"), result.encodeStageMs},
            {QStringLiteral("audio_stage_ms"), result.audioStageMs},
            {QStringLiteral("max_frame_render_stage_ms"), result.maxFrameRenderStageMs},
            {QStringLiteral("max_frame_decode_stage_ms"), result.maxFrameDecodeStageMs},
            {QStringLiteral("max_frame_texture_stage_ms"), result.maxFrameTextureStageMs},
            {QStringLiteral("max_frame_readback_stage_ms"), result.maxFrameReadbackStageMs},
            {QStringLiteral("max_frame_convert_stage_ms"), result.maxFrameConvertStageMs},
            {QStringLiteral("skipped_clips"), result.skippedClips},
            {QStringLiteral("skipped_clip_reason_counts"), result.skippedClipReasonCounts},
            {QStringLiteral("render_stage_table"), result.renderStageTable},
            {QStringLiteral("worst_frame_table"), result.worstFrameTable},
            {QStringLiteral("render_stage_per_frame_ms"), static_cast<double>(result.renderStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_decode_stage_per_frame_ms"), static_cast<double>(result.renderDecodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_texture_stage_per_frame_ms"), static_cast<double>(result.renderTextureStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_composite_stage_per_frame_ms"), static_cast<double>(result.renderCompositeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("render_nv12_stage_per_frame_ms"), static_cast<double>(result.renderNv12StageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("gpu_readback_per_frame_ms"), static_cast<double>(result.gpuReadbackMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("overlay_stage_per_frame_ms"), static_cast<double>(result.overlayStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("convert_stage_per_frame_ms"), static_cast<double>(result.convertStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("encode_stage_per_frame_ms"), static_cast<double>(result.encodeStageMs) / static_cast<double>(completedFrames)},
            {QStringLiteral("message"), result.message}};
    };
    m_renderInProgress = true;
    m_liveRenderProfile = QJsonObject{
        {QStringLiteral("status"), QStringLiteral("starting")},
        {QStringLiteral("output_path"), QDir::toNativeSeparators(outputPath)},
        {QStringLiteral("frames_completed"), static_cast<qint64>(0)},
        {QStringLiteral("total_frames"), static_cast<qint64>(totalFramesToRender)}};
    refreshProfileInspector();

    const auto activeRenderSourcesText = [&effectiveRequest](int64_t timelineFrame) -> QString {
        QStringList lines;
        lines.reserve(effectiveRequest.clips.size() * 2);
        for (const TimelineClip& clip : effectiveRequest.clips) {
            if (timelineFrame < clip.startFrame ||
                timelineFrame >= clip.startFrame + qMax<int64_t>(1, clip.durationFrames)) {
                continue;
            }
            const QString clipLabel = clip.label.isEmpty() ? QStringLiteral("(unnamed clip)") : clip.label;
            if (clipVisualPlaybackEnabled(clip, effectiveRequest.tracks) && !clip.filePath.isEmpty()) {
                lines.push_back(QStringLiteral("V | %1 | %2")
                                    .arg(clipLabel, QDir::toNativeSeparators(clip.filePath)));
            }
            if (clipAudioPlaybackEnabled(clip)) {
                const QString audioPath = playbackAudioPathForClip(clip);
                if (!audioPath.isEmpty()) {
                    lines.push_back(QStringLiteral("A | %1 | %2")
                                        .arg(clipLabel, QDir::toNativeSeparators(audioPath)));
                }
            }
        }
        if (lines.isEmpty()) {
            return QStringLiteral("No active clip sources at this frame.");
        }
        lines.removeDuplicates();
        std::sort(lines.begin(), lines.end());
        return lines.join(QLatin1Char('\n'));
    };

    const RenderResult result = renderTimelineToFile(
        effectiveRequest,
        [this, &progressDialog, renderStatusLabel, renderProgressBar, renderPreviewLabel,
         renderSourcesList, showRenderPreviewCheckBox, &renderCancelled,
         formatEta, stageSummary, renderProfileFromProgress, outputPath,
         activeRenderSourcesText](const RenderProgress &progress)
        {
            renderProgressBar->setMaximum(qMax(1, static_cast<int>(qMin<int64_t>(progress.totalFrames, std::numeric_limits<int>::max()))));
            renderProgressBar->setValue(static_cast<int>(qMin<int64_t>(progress.framesCompleted, std::numeric_limits<int>::max())));
            const QString rendererMode = progress.usingGpu ? QStringLiteral("GPU render") : QStringLiteral("CPU render");
            const QString encoderMode = progress.usingHardwareEncode
                                            ? QStringLiteral("Hardware encode")
                                            : QStringLiteral("Software encode");
            const QString encoderLabel = progress.encoderLabel.isEmpty()
                                             ? QStringLiteral("unknown")
                                             : progress.encoderLabel;
            m_liveRenderProfile = renderProfileFromProgress(progress);
            m_liveRenderProfile[QStringLiteral("output_path")] = QDir::toNativeSeparators(outputPath);
            refreshProfileInspector();
            const QString metricsTable = QStringLiteral(
                "<table cellspacing='0' cellpadding='2' style='margin: 0 auto;'>"
                "<tr>"
                "<td align='right'><b>Render</b></td><td>%1</td>"
                "<td align='right'><b>Decode</b></td><td>%2</td>"
                "<td align='right'><b>Texture</b></td><td>%3</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Composite</b></td><td>%4</td>"
                "<td align='right'><b>GPU NV12</b></td><td>%5</td>"
                "<td align='right'><b>Readback</b></td><td>%6</td>"
                "</tr>"
                "<tr>"
                "<td align='right'><b>Convert</b></td><td>%7</td>"
                "<td align='right'><b>Encode</b></td><td>%8</td>"
                "<td></td><td></td>"
                "</tr>"
                "</table>")
                .arg(stageSummary(progress.renderStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderDecodeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderTextureStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderCompositeStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.renderNv12StageMs, progress.framesCompleted))
                .arg(stageSummary(progress.gpuReadbackMs, progress.framesCompleted))
                .arg(stageSummary(progress.convertStageMs, progress.framesCompleted))
                .arg(stageSummary(progress.encodeStageMs, progress.framesCompleted));
            renderStatusLabel->setText(
                QStringLiteral("<b>Rendering frame %1 of %2</b><br>"
                               "Segment %3/%4: %5-%6<br>"
                               "%7 | %8 (%9)<br>"
                               "ETA: %10<br>%11")
                    .arg(progress.framesCompleted + 1)
                    .arg(qMax<int64_t>(1, progress.totalFrames))
                    .arg(progress.segmentIndex)
                    .arg(progress.segmentCount)
                    .arg(progress.segmentStartFrame)
                    .arg(progress.segmentEndFrame)
                    .arg(rendererMode)
                    .arg(encoderMode)
                    .arg(encoderLabel)
                    .arg(formatEta(progress.estimatedRemainingMs))
                    .arg(metricsTable));
            if (showRenderPreviewCheckBox->isChecked() && !progress.previewFrame.isNull())
            {
                const QPixmap pixmap = QPixmap::fromImage(progress.previewFrame).scaled(
                    renderPreviewLabel->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                renderPreviewLabel->setPixmap(pixmap);
                renderPreviewLabel->setText(QString());
            }
            renderSourcesList->setPlainText(activeRenderSourcesText(progress.timelineFrame));
            QCoreApplication::processEvents();
            return !renderCancelled;
        });
    renderProgressBar->setValue(renderProgressBar->maximum());
    progressDialog.close();
    m_renderInProgress = false;
    m_lastRenderProfile = renderProfileFromResult(result);
    m_liveRenderProfile = QJsonObject{};
    refreshProfileInspector();

    if (result.success)
    {
        QMessageBox::information(this, QStringLiteral("Render Complete"), result.message);
        return;
    }

    const QString message = result.message.isEmpty()
                                ? QStringLiteral("Render failed.")
                                : result.message;
    QMessageBox::warning(this,
                         result.cancelled ? QStringLiteral("Render Cancelled") : QStringLiteral("Render Failed"),
                         message);
}

void EditorWindow::exportVideoForSpeakersOnSelectedClip(const QStringList& speakerIds)
{
    if (!m_timeline || speakerIds.isEmpty()) {
        return;
    }
    const TimelineClip* clip = m_timeline->selectedClip();
    if (!clip || clip->durationFrames <= 0) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("Select a clip first."));
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip->filePath);
    QFile transcriptFile(transcriptPath);
    if (!transcriptFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript not found for the selected clip."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument transcriptDoc =
        QJsonDocument::fromJson(transcriptFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !transcriptDoc.isObject()) {
        QMessageBox::warning(this,
                             QStringLiteral("Export Video"),
                             QStringLiteral("Transcript JSON is invalid for the selected clip."));
        return;
    }

    auto appendMergedRange = [](QVector<ExportRangeSegment>& ranges, int64_t startFrame, int64_t endFrame) {
        if (endFrame < startFrame) {
            return;
        }
        if (ranges.isEmpty() || startFrame > ranges.constLast().endFrame + 1) {
            ranges.push_back(ExportRangeSegment{startFrame, endFrame});
            return;
        }
        ranges.last().endFrame = qMax(ranges.last().endFrame, endFrame);
    };

    QSet<QString> selectedSpeakerSet;
    selectedSpeakerSet.reserve(speakerIds.size());
    for (const QString& speakerId : speakerIds) {
        const QString trimmed = speakerId.trimmed();
        if (!trimmed.isEmpty()) {
            selectedSpeakerSet.insert(trimmed);
        }
    }
    if (selectedSpeakerSet.isEmpty()) {
        return;
    }

    QVector<ExportRangeSegment> sourceWordRanges;
    const QJsonArray segments = transcriptDoc.object().value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segmentValue : segments) {
        const QJsonObject segmentObj = segmentValue.toObject();
        const QString segmentSpeaker =
            segmentObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segmentObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            const QString wordText = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (wordText.isEmpty()) {
                continue;
            }
            QString wordSpeaker = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (wordSpeaker.isEmpty()) {
                wordSpeaker = segmentSpeaker;
            }
            if (!selectedSpeakerSet.contains(wordSpeaker)) {
                continue;
            }
            const double startSeconds = wordObj.value(QStringLiteral("start")).toDouble(-1.0);
            const double endSeconds = wordObj.value(QStringLiteral("end")).toDouble(-1.0);
            if (startSeconds < 0.0 || endSeconds < startSeconds) {
                continue;
            }
            const int64_t startFrame =
                qMax<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * kTimelineFps)));
            const int64_t endFrame =
                qMax<int64_t>(startFrame, static_cast<int64_t>(std::ceil(endSeconds * kTimelineFps)) - 1);
            appendMergedRange(sourceWordRanges, startFrame, endFrame);
        }
    }
    if (sourceWordRanges.isEmpty()) {
        QMessageBox::information(this,
                                 QStringLiteral("Export Video"),
                                 QStringLiteral("No spoken words found for the selected speakers in this clip."));
        return;
    }

    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    QVector<ExportRangeSegment> timelineRanges;
    timelineRanges.reserve(sourceWordRanges.size());
    int sourceRangeIndex = 0;
    const int64_t clipStartFrame = clip->startFrame;
    const int64_t clipEndFrame = clip->startFrame + clip->durationFrames - 1;
    for (int64_t timelineFrame = clipStartFrame; timelineFrame <= clipEndFrame; ++timelineFrame) {
        const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(
            *clip, frameToSamples(timelineFrame), markers);
        while (sourceRangeIndex < sourceWordRanges.size() &&
               sourceWordRanges.at(sourceRangeIndex).endFrame < transcriptFrame) {
            ++sourceRangeIndex;
        }
        if (sourceRangeIndex >= sourceWordRanges.size()) {
            break;
        }
        const ExportRangeSegment& sourceRange = sourceWordRanges.at(sourceRangeIndex);
        if (transcriptFrame < sourceRange.startFrame || transcriptFrame > sourceRange.endFrame) {
            continue;
        }
        appendMergedRange(timelineRanges, timelineFrame, timelineFrame);
    }
    if (timelineRanges.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Export Video"),
            QStringLiteral("Could not map selected speaker words to timeline frames."));
        return;
    }

    setPlaybackActive(false);

    RenderRequest request = buildRenderRequestFromOutputControls();
    const QString suggestedBase = QStringLiteral("speaker_export_%1")
        .arg(selectedSpeakerSet.size() == 1 ? *selectedSpeakerSet.constBegin()
                                            : QStringLiteral("multi"));
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Video"),
        QDir::current().filePath(QStringLiteral("%1.%2").arg(suggestedBase, request.outputFormat)),
        QStringLiteral("Video Files (*.%1);;All Files (*)").arg(request.outputFormat));
    if (selectedPath.isEmpty()) {
        return;
    }

    request.outputPath = selectedPath;
    m_lastRenderOutputPath = selectedPath;
    scheduleSaveState();

    request.clips = m_timeline->clips();
    request.tracks = m_timeline->tracks();
    request.renderSyncMarkers = markers;
    request.exportRanges = timelineRanges;
    request.exportStartFrame = timelineRanges.constFirst().startFrame;
    request.exportEndFrame = timelineRanges.constLast().endFrame;
    renderTimelineFromOutputRequest(request);
}


namespace {

bool zeroCopyPreferredEnvironmentDetected() {
#if defined(Q_OS_LINUX)
    return QFile::exists(QStringLiteral("/proc/driver/nvidia/version")) ||
           QFile::exists(QStringLiteral("/sys/module/nvidia")) ||
           QFile::exists(QStringLiteral("/dev/dri/renderD128"));
#else
    return false;
#endif
}

}

int main(int argc, char **argv)
{
    bool runHeadlessSpeakerHarness = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--speaker-export-harness") == 0) {
            runHeadlessSpeakerHarness = true;
            break;
        }
    }
    if (runHeadlessSpeakerHarness &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PanelTalkEditor"));
    qRegisterMetaType<editor::FrameHandle>();

    // Single instance enforcement via lock file
    const QString lockPath = QDir::tempPath() + QStringLiteral("/PanelTalkEditor.lock");
    QLockFile lockFile(lockPath);
    lockFile.setStaleLockTime(0);
    if (!lockFile.tryLock(100)) {
        qint64 pid = 0;
        QString hostname, appname;
        lockFile.getLockInfo(&pid, &hostname, &appname);
        fprintf(stderr, "Another instance is already running (pid %lld). Exiting.\n",
                static_cast<long long>(pid));
        return 1;
    }

    if (!zeroCopyPreferredEnvironmentDetected()) {
        qWarning().noquote() << QStringLiteral(
            "[STARTUP][WARN] Preferred zero-copy decode path requires Linux GPU interop "
            "(CUDA or VAAPI render node); falling back to hardware CPU-upload or software decode.");
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PanelVid2TikTok editor"));
    parser.addHelpOption();
    QCommandLineOption debugPlaybackOption(QStringLiteral("debug-playback"),
                                           QStringLiteral("Enable playback debug logging"));
    QCommandLineOption debugCacheOption(QStringLiteral("debug-cache"),
                                        QStringLiteral("Enable cache debug logging"));
    QCommandLineOption debugDecodeOption(QStringLiteral("debug-decode"),
                                         QStringLiteral("Enable decode debug logging"));
    QCommandLineOption debugAllOption(QStringLiteral("debug-all"),
                                      QStringLiteral("Enable all debug logging"));
    QCommandLineOption controlPortOption(
        QStringList{QStringLiteral("control-port")},
        QStringLiteral("Control server port."),
        QStringLiteral("port"));
    QCommandLineOption noRestOption(
        QStringList{QStringLiteral("no-rest")},
        QStringLiteral("Disable the local REST/control server."));
    QCommandLineOption speakerHarnessOption(
        QStringList{QStringLiteral("speaker-export-harness")},
        QStringLiteral("Run speaker export harness without showing the main window."));
    QCommandLineOption stateOption(
        QStringList{QStringLiteral("state")},
        QStringLiteral("Path to state JSON for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption outputOption(
        QStringList{QStringLiteral("output")},
        QStringLiteral("Output path for harness mode."),
        QStringLiteral("path"));
    QCommandLineOption speakerOption(
        QStringList{QStringLiteral("speaker-id")},
        QStringLiteral("Speaker id(s) for harness mode. Repeat or use comma-separated values."),
        QStringLiteral("id"));
    QCommandLineOption clipOption(
        QStringList{QStringLiteral("clip-id")},
        QStringLiteral("Clip id override for harness mode."),
        QStringLiteral("id"));
    QCommandLineOption formatOption(
        QStringList{QStringLiteral("format")},
        QStringLiteral("Output format override for harness mode."),
        QStringLiteral("format"));
    QCommandLineOption widthOption(
        QStringList{QStringLiteral("width")},
        QStringLiteral("Output width override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption heightOption(
        QStringList{QStringLiteral("height")},
        QStringLiteral("Output height override for harness mode."),
        QStringLiteral("pixels"));
    QCommandLineOption useProxyOption(
        QStringList{QStringLiteral("use-proxy")},
        QStringLiteral("Force proxy rendering in harness mode."));
    QCommandLineOption noProxyOption(
        QStringList{QStringLiteral("no-proxy")},
        QStringLiteral("Disable proxy rendering in harness mode."));
    parser.addOption(debugPlaybackOption);
    parser.addOption(debugCacheOption);
    parser.addOption(debugDecodeOption);
    parser.addOption(debugAllOption);
    parser.addOption(controlPortOption);
    parser.addOption(noRestOption);
    parser.addOption(speakerHarnessOption);
    parser.addOption(stateOption);
    parser.addOption(outputOption);
    parser.addOption(speakerOption);
    parser.addOption(clipOption);
    parser.addOption(formatOption);
    parser.addOption(widthOption);
    parser.addOption(heightOption);
    parser.addOption(useProxyOption);
    parser.addOption(noProxyOption);
    parser.process(app);

    if (parser.isSet(debugAllOption)) {
        editor::setDebugPlaybackEnabled(true);
        editor::setDebugCacheEnabled(true);
        editor::setDebugDecodeEnabled(true);
    } else {
        if (parser.isSet(debugPlaybackOption)) {
            editor::setDebugPlaybackEnabled(true);
        }
        if (parser.isSet(debugCacheOption)) {
            editor::setDebugCacheEnabled(true);
        }
        if (parser.isSet(debugDecodeOption)) {
            editor::setDebugDecodeEnabled(true);
        }
    }

    bool portOk = false;
    quint16 controlPort = 40130;
    const QString optionValue = parser.value(controlPortOption);
    if (!optionValue.isEmpty()) {
        const uint parsed = optionValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    } else {
        const QString envValue = qEnvironmentVariable("EDITOR_CONTROL_PORT");
        const uint parsed = envValue.toUInt(&portOk);
        if (portOk && parsed <= std::numeric_limits<quint16>::max()) {
            controlPort = static_cast<quint16>(parsed);
        }
    }

    if (parser.isSet(noRestOption)) {
        controlPort = 0;
    }

    if (parser.isSet(speakerHarnessOption)) {
        SpeakerExportHarnessConfig config;
        config.statePath = parser.value(stateOption);
        config.outputPath = parser.value(outputOption);
        config.outputFormat = parser.value(formatOption);
        config.clipId = parser.value(clipOption);
        config.speakerIds = parser.values(speakerOption);
        if (parser.isSet(widthOption) || parser.isSet(heightOption)) {
            bool widthOk = false;
            bool heightOk = false;
            const int parsedWidth = parser.value(widthOption).toInt(&widthOk);
            const int parsedHeight = parser.value(heightOption).toInt(&heightOk);
            config.outputSize = QSize(widthOk ? parsedWidth : 1080,
                                      heightOk ? parsedHeight : 1920);
            config.outputSizeOverride = true;
        }
        if (parser.isSet(useProxyOption) || parser.isSet(noProxyOption)) {
            config.useProxyOverride = true;
            config.useProxyMedia = parser.isSet(useProxyOption) && !parser.isSet(noProxyOption);
        }
        return runSpeakerExportHarness(config);
    }

    EditorWindow window(controlPort);
    window.show();
    return app.exec();
}
