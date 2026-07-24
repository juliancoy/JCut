#include <QtTest/QtTest>

#include "../editor_runtime.h"
#include "../editor_document_core_json.h"
#include "../editor_scale_to_fill.h"
#include "../editor_document_render_bridge.h"
#include "../imgui_project_io.h"
#include "../editor_runtime_qt_bridge.h"
#include "../editor_timeline_types.h"
#include "../clip_serialization.h"
#include "../editor_shared_render_sync.h"
#include "../editor_shared_timing.h"
#include "../render.h"
#include "../render_qt_compat.h"
#include "../preview_resize_core.h"
#include "../timeline_snap_core.h"
#include "../editor_media_presence_core.h"
#include "../editor_auto_oppose_core.h"

#include <nlohmann/json.hpp>

#include <QString>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

class TestEditorRuntime : public QObject {
    Q_OBJECT

private slots:
    void testPlaybackCommandsUpdateTransport();
    void testSeekAndStepCommandsClampToTimeline();
    void testTickAdvancesPlaybackAndStopsAtEnd();
    void testPlaybackRangesAndLoopMatchQtTransport();
    void testTrackAndClipLifecycleCommandsUpdateDocument();
    void testCreateTitleClipCommandMatchesQtDefaultsAndIsAtomic();
    void testReorderTrackCommandPreservesAssignmentsAndIsUndoable();
    void testCrossfadeTrackCommandMatchesQtTimelineSemantics();
    void testCrossfadeTrackCommandKeepsPositionsAndRejectsInvalidTargets();
    void testMediaImportAndInsertionCommandsUpdateDocument();
    void testMediaRemovalRejectsReferencedItemsAndIsUndoable();
    void testSplitClipCommandUpdatesTimeline();
    void testSplitSelectedClipsCommandMatchesRazorSemantics();
    void testTrimClipCommandsUpdateTimeline();
    void testProjectAndClipEditCommandsUpdateDocument();
    void testSelectionCommandsSwitchSingleSelection();
    void testClipSelectionSupportsAdditiveToggleAndSelectAll();
    void testClipboardPasteRemapsMarkersAndSurvivesUndo();
    void testCutAndDuplicateClipsAreUndoable();
    void testDeleteSelectedClipsIsAtomicAndUndoable();
    void testMoveSelectedClipsPreservesLayoutAndRejectsInvalidGroups();
    void testUndoRedoRestoresDeterministicSnapshots();
    void testCrossShellCommandScriptProducesEquivalentSnapshots();
    void testHistoryTransactionCoalescesContinuousEdits();
    void testNavigationAndPanelCommandsStayOutsideUndoHistory();
    void testMutationAfterUndoInvalidatesRedo();
    void testTranscriptDocumentsShareGlobalHistoryWithoutProjectSerialization();
    void testNudgeSelectedClipCommandUpdatesTimeline();
    void testClipLockAndPlaybackRateCommandsMatchTimelinePolicies();
    void testInspectorCommandsUpdateSharedClipState();
    void testCorrectionPolygonCommandNormalizesAndIsUndoable();
    void testPreviewTransformCommandMatchesQtKeyframeSemantics();
    void testSourceTransformLockMatchesQtChainSemantics();
    void testMaterializeMaskMatteMatchesQtOwnershipDefaults();
    void testResetClipGradingMatchesQtContextSemantics();
    void testResetClipGradingClearsQtKnownLegacyFields();
    void testScaleToFillHelperReusesUndoableTransformCommand();
    void testEffectsInspectorUsesCompleteNeutralPresetCatalogAndQtBounds();
    void testTrackPropertiesCommandNormalizesLabelAndHeight();
    void testTrackMediaPresenceMatchesQtEnablement();
    void testAutoOpposeAnalysisCoreMatchesQtThresholds();
    void testTitleKeyframeFieldsRoundTripThroughRenderBridge();
    void testTranscriptOverlayInspectorStateRoundTripsAndRenders();
    void testClipKeyframeRemovalIsChannelScopedAndUndoable();
    void testKeyframeFrameAndValueEditsAreAtomicAndChannelScoped();
    void testGradingCurveNormalizationIsUndoable();
    void testRenderSyncMarkerActionReplacementIsUniqueAndUndoable();
    void testRenderSyncMarkerLoadNormalizationIsLastWins();
    void testRenderSyncMarkerCountClampsToQtRange();
    void testRenderSyncMarkerRemovalIsScopedAndUndoable();
    void testMaskMatteRenderSyncOwnershipIsCanonical();
    void testGeneratedChildTrackRoundTripsAcrossNeutralAndQtBridges();
    void testTranscriptGeneratedTitlesUseImmutableChildTrack();
    void testGeneratedChildTrackReconciliationAndPoliciesAreUndoable();
    void testMaskMatteCloneAndSplitRelationshipsStayValid();
    void testExtendedClipStateRoundTripsIntoRenderTimeline();
    void testExportCommandsUpdateCoreRequest();
    void testExportRangeContextActionsMatchQtSemantics();
    void testQtRenderRequestPreservesPlaybackSpeed();
    void testQtBridgeBuildsDocumentCore();
    void testDocumentBuildsTimelineRenderData();
    void testExplicitAudioSourceBuildsPlayableTimelineState();
    void testMediaProbeRefreshesAudioSourceStatus();
    void testAudioPresenceSurvivesPlaybackToggle();
    void testExplicitAudioSourceWorksWithoutPrimaryMedia();
    void testCoreDocumentJsonRoundTrips();
    void testLegacyStateJsonBuildsDocumentCore();
    void testLegacyStateJsonPreservesQtOnlyArtifactFields();
    void testLegacySaveRemapsGappedTrackIds();
    void testCoreDocumentFileRoundTrips();
    void testImGuiProjectSessionLoadsActiveQtProjectFiles();
    void testImGuiProjectSessionSaveWritesQtStateFiles();
    void testAiLoginPersistsRefreshTokenContract();
    void testAudioClipDurationPreservesSubframeSamples();
};

namespace {

bool writeSilentPcmWav(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    constexpr quint16 channels = 1;
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 sampleFrames = 480;
    constexpr quint16 blockAlign = channels * (bitsPerSample / 8);
    constexpr quint32 dataBytes = sampleFrames * blockAlign;

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    if (stream.writeRawData("RIFF", 4) != 4) {
        return false;
    }
    stream << quint32{36 + dataBytes};
    if (stream.writeRawData("WAVEfmt ", 8) != 8) {
        return false;
    }
    stream << quint32{16} << quint16{1} << channels << sampleRate
           << quint32{sampleRate * blockAlign} << blockAlign << bitsPerSample;
    if (stream.writeRawData("data", 4) != 4) {
        return false;
    }
    stream << dataBytes;
    const QByteArray silence(static_cast<qsizetype>(dataBytes), '\0');
    return stream.writeRawData(silence.constData(), silence.size()) ==
            silence.size() &&
        stream.status() == QDataStream::Ok;
}

template <typename Command>
void applyCommand(jcut::EditorRuntime& runtime, Command&& command)
{
    [[maybe_unused]] const jcut::CommandResult result =
        runtime.execute(jcut::EditorCommand{std::forward<Command>(command)});
}

} // namespace

void TestEditorRuntime::testPlaybackCommandsUpdateTransport()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    QCOMPARE(runtime.snapshot().transport.playbackActive, false);
    applyCommand(runtime, jcut::TogglePlaybackCommand{});
    QCOMPARE(runtime.snapshot().transport.playbackActive, true);

    applyCommand(runtime, jcut::SetPlaybackSpeedCommand{4.0f});
    QCOMPARE(runtime.snapshot().transport.playbackSpeed, 3.0f);
    applyCommand(
        runtime, jcut::SetPlaybackSpeedCommand{0.01f});
    QCOMPARE(runtime.snapshot().transport.playbackSpeed, 0.1f);
    applyCommand(
        runtime, jcut::SetPlaybackLoopEnabledCommand{true});
    QCOMPARE(
        runtime.snapshot().transport.playbackLoopEnabled, true);
    applyCommand(
        runtime, jcut::SetPreviewViewModeCommand{"audio"});
    QCOMPARE(
        QString::fromStdString(
            runtime.snapshot().transport.previewViewMode),
        QStringLiteral("audio"));
    applyCommand(
        runtime, jcut::SetTransportAudioCommand{true, 2.0f});
    QCOMPARE(runtime.snapshot().transport.audioMuted, true);
    QCOMPARE(runtime.snapshot().transport.audioVolume, 1.0f);

    applyCommand(runtime, jcut::SetPreviewZoomCommand{0.1f});
    QCOMPARE(runtime.snapshot().transport.previewZoom, 0.5f);
}

void TestEditorRuntime::testSeekAndStepCommandsClampToTimeline()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SeekToFrameCommand{50});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 50);

    applyCommand(runtime, jcut::StepFrameCommand{25});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 75);

    applyCommand(runtime, jcut::StepFrameCommand{-100});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 0);

    applyCommand(runtime, jcut::SeekToFrameCommand{5000});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 432);
}

void TestEditorRuntime::testTickAdvancesPlaybackAndStopsAtEnd()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SeekToFrameCommand{0});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{true});
    runtime.tick({0.5});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 15);
    QCOMPARE(runtime.snapshot().transport.playbackActive, true);

    applyCommand(runtime, jcut::SeekToFrameCommand{430});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{true});
    runtime.tick({1.0});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 432);
    QCOMPARE(runtime.snapshot().transport.playbackActive, false);
}

void TestEditorRuntime::testPlaybackRangesAndLoopMatchQtTransport()
{
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.durationFrames = 30;
    document.clips.push_back(clip);
    document.exportRequest.outputFps = 10.0;
    document.exportRanges = {{5, 9}, {20, 24}};
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(document);

    applyCommand(runtime, jcut::SeekToFrameCommand{9});
    applyCommand(runtime, jcut::StepFrameCommand{1});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 20);
    applyCommand(runtime, jcut::StepFrameCommand{-1});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 9);

    applyCommand(runtime, jcut::SeekToFrameCommand{8});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{true});
    runtime.tick({0.2});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 20);
    QCOMPARE(runtime.snapshot().transport.playbackActive, true);

    applyCommand(
        runtime, jcut::SetPlaybackLoopEnabledCommand{true});
    applyCommand(runtime, jcut::SeekToFrameCommand{23});
    runtime.tick({0.2});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 5);
    QCOMPARE(runtime.snapshot().transport.playbackActive, true);

    applyCommand(
        runtime, jcut::SetPlaybackLoopEnabledCommand{false});
    applyCommand(runtime, jcut::SeekToFrameCommand{23});
    runtime.tick({0.2});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 24);
    QCOMPARE(runtime.snapshot().transport.playbackActive, false);

    applyCommand(runtime, jcut::SeekToFrameCommand{30});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{true});
    QCOMPARE(runtime.snapshot().transport.currentFrame, 5);
    QCOMPARE(runtime.snapshot().transport.playbackActive, true);
}

void TestEditorRuntime::testTrackAndClipLifecycleCommandsUpdateDocument()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::AddTrackCommand{"Captions"});
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(5));
    QCOMPARE(QString::fromStdString(runtime.snapshot().tracks.back().label), QStringLiteral("Captions"));
    QCOMPARE(runtime.snapshot().tracks.back().selected, true);

    applyCommand(runtime, jcut::AddClipCommand{
        runtime.snapshot().tracks.back().id,
        "Title Card",
        240,
        72,
        "/tmp/title-card.png",
        "image"});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(5));
    QCOMPARE(QString::fromStdString(runtime.snapshot().clips.back().label), QStringLiteral("Title Card"));
    QCOMPARE(runtime.snapshot().clips.back().trackId, runtime.snapshot().tracks.back().id);
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(6));

    applyCommand(runtime, jcut::DeleteClipCommand{runtime.snapshot().clips.back().id});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(4));
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(6));

    applyCommand(runtime, jcut::DeleteTrackCommand{runtime.snapshot().tracks.back().id});
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(4));

    const jcut::CommandResult missingClip =
        runtime.execute(jcut::EditorCommand{jcut::DeleteClipCommand{999}});
    QCOMPARE(missingClip.applied, false);
}

void TestEditorRuntime::testCreateTitleClipCommandMatchesQtDefaultsAndIsAtomic()
{
    jcut::EditorDocumentCore document;
    document.mediaItems.push_back({
        "/tmp/library.mov", "Library Clip", "video", true, true});
    document.tracks.push_back({7, "Video", true});

    jcut::EditorClip sourceClip;
    sourceClip.id = 11;
    sourceClip.trackId = 7;
    sourceClip.label = "Source";
    sourceClip.startFrame = 0;
    sourceClip.durationFrames = 90;
    sourceClip.selected = true;
    sourceClip.sourcePath = "/tmp/library.mov";
    sourceClip.persistentId = "source-clip";
    sourceClip.mediaKind = "video";
    document.clips.push_back(std::move(sourceClip));

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const jcut::CommandResult created = runtime.execute(
        jcut::EditorCommand{jcut::CreateTitleClipCommand{150}});
    QVERIFY2(created.applied, created.message.c_str());
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorDocumentCore createdSnapshot = runtime.snapshot();
    QCOMPARE(createdSnapshot.tracks.size(), std::size_t(2));
    QCOMPARE(createdSnapshot.clips.size(), std::size_t(2));
    QCOMPARE(createdSnapshot.mediaItems.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(createdSnapshot.tracks.back().label),
             QStringLiteral("Titles"));
    QCOMPARE(createdSnapshot.tracks.back().audioEnabled, false);
    QCOMPARE(createdSnapshot.tracks.back().selected, false);
    QCOMPARE(createdSnapshot.tracks.front().selected, true);
    const auto title = std::find_if(
        createdSnapshot.clips.begin(), createdSnapshot.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.mediaKind == "title";
        });
    QVERIFY(title != createdSnapshot.clips.end());
    QCOMPARE(title->trackId, createdSnapshot.tracks.back().id);
    QCOMPARE(QString::fromStdString(title->label), QStringLiteral("Title"));
    QCOMPARE(title->startFrame, 150);
    QCOMPARE(title->durationFrames, jcut::kEditorDefaultTitleDurationFrames);
    QCOMPARE(title->sourceDurationFrames,
             std::int64_t{jcut::kEditorDefaultTitleDurationFrames});
    QVERIFY(title->sourcePath.empty());
    QVERIFY(!title->persistentId.empty());
    QCOMPARE(title->selected, true);
    QCOMPARE(title->videoEnabled, true);
    QCOMPARE(title->audioEnabled, false);
    QCOMPARE(title->audioPresenceKnown, true);
    QCOMPARE(title->hasAudio, false);
    QCOMPARE(title->titleKeyframes.size(), std::size_t(1));
    const jcut::EditorTitleKeyframe& initialTitle =
        title->titleKeyframes.front();
    QCOMPARE(initialTitle.frame, std::int64_t{0});
    QCOMPARE(QString::fromStdString(initialTitle.text), QStringLiteral("Title"));
    QCOMPARE(initialTitle.translationX, 0.0);
    QCOMPARE(initialTitle.translationY, 0.0);
    QCOMPARE(initialTitle.fontSize, 48.0);
    QCOMPARE(initialTitle.opacity, 1.0);
    QCOMPARE(QString::fromStdString(initialTitle.fontFamily),
             QStringLiteral("DejaVu Sans"));
    QCOMPARE(initialTitle.bold, true);
    QCOMPARE(initialTitle.italic, false);
    QCOMPARE(QString::fromStdString(initialTitle.color),
             QStringLiteral("#ffffff"));

    const std::string titlePersistentId = title->persistentId;
    const int titleTrackId = title->trackId;
    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(createdSnapshot);
    const auto renderTitle = std::find_if(
        renderData.clips.begin(), renderData.clips.end(),
        [&](const TimelineClip& clip) {
            return clip.id.toStdString() == titlePersistentId;
        });
    QVERIFY(renderTitle != renderData.clips.end());
    QCOMPARE(renderTitle->mediaType, ClipMediaType::Title);
    QVERIFY(renderTitle->filePath.isEmpty());
    QCOMPARE(renderTitle->titleKeyframes.size(), 1);
    QCOMPARE(renderTitle->titleKeyframes.front().text, QStringLiteral("Title"));

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(1));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    const jcut::EditorDocumentCore redoneSnapshot = runtime.snapshot();
    const auto redoneTitle = std::find_if(
        redoneSnapshot.clips.begin(), redoneSnapshot.clips.end(),
        [&](const jcut::EditorClip& clip) {
            return clip.persistentId == titlePersistentId;
        });
    QVERIFY(redoneTitle != redoneSnapshot.clips.end());
    QCOMPARE(redoneTitle->trackId, titleTrackId);
    QVERIFY(std::any_of(
        redoneSnapshot.tracks.begin(), redoneSnapshot.tracks.end(),
        [&](const jcut::EditorTrack& track) {
            return track.id == titleTrackId && track.label == "Titles";
        }));

    // The canonical Titles lane is occupied here. Match the Qt policy by
    // creating a muted numbered lane and the clip in the same undoable edit.
    const jcut::CommandResult conflictTitle = runtime.execute(
        jcut::EditorCommand{jcut::CreateTitleClipCommand{180}});
    QVERIFY2(conflictTitle.applied, conflictTitle.message.c_str());
    const jcut::EditorDocumentCore conflictSnapshot = runtime.snapshot();
    QCOMPARE(conflictSnapshot.tracks.size(), std::size_t(3));
    QCOMPARE(conflictSnapshot.clips.size(), std::size_t(3));
    QCOMPARE(conflictSnapshot.mediaItems.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(conflictSnapshot.tracks.back().label),
             QStringLiteral("Titles 2"));
    QCOMPARE(conflictSnapshot.tracks.back().audioEnabled, false);
    QCOMPARE(conflictSnapshot.tracks.back().selected, false);
    const auto conflictClip = std::find_if(
        conflictSnapshot.clips.begin(), conflictSnapshot.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.mediaKind == "title" && clip.startFrame == 180;
        });
    QVERIFY(conflictClip != conflictSnapshot.clips.end());
    QCOMPARE(conflictClip->trackId, conflictSnapshot.tracks.back().id);
    QCOMPARE(conflictClip->selected, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(2));

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(1));
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    // Even when a free prefixed lane already exists, Qt first creates and
    // targets the exact unnumbered Titles lane. Later overlaps reuse the
    // prefixed lane before creating a numbered lane based on prefix count.
    jcut::EditorDocumentCore routingDocument;
    routingDocument.tracks.push_back({3, "Video", true});
    routingDocument.tracks.push_back({8, "  tItLeS 9  ", false});
    jcut::EditorRuntime routingRuntime =
        jcut::EditorRuntime::fromDocument(std::move(routingDocument));
    const jcut::CommandResult canonical = routingRuntime.execute(
        jcut::EditorCommand{jcut::CreateTitleClipCommand{-20, 0}});
    QVERIFY2(canonical.applied, canonical.message.c_str());
    const jcut::EditorDocumentCore canonicalSnapshot = routingRuntime.snapshot();
    QCOMPARE(canonicalSnapshot.tracks.size(), std::size_t(3));
    QCOMPARE(QString::fromStdString(canonicalSnapshot.tracks.back().label),
             QStringLiteral("Titles"));
    QCOMPARE(canonicalSnapshot.clips.front().trackId,
             canonicalSnapshot.tracks.back().id);
    QCOMPARE(canonicalSnapshot.clips.front().startFrame, 0);
    QCOMPARE(canonicalSnapshot.clips.front().durationFrames, 1);
    QCOMPARE(canonicalSnapshot.clips.front().sourceDurationFrames,
             std::int64_t{1});

    const jcut::CommandResult reused = routingRuntime.execute(
        jcut::EditorCommand{jcut::CreateTitleClipCommand{0, 1}});
    QVERIFY2(reused.applied, reused.message.c_str());
    const jcut::EditorDocumentCore reusedSnapshot = routingRuntime.snapshot();
    QCOMPARE(reusedSnapshot.tracks.size(), std::size_t(3));
    const auto reusedTitle = std::find_if(
        reusedSnapshot.clips.begin(),
        reusedSnapshot.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.mediaKind == "title" && clip.id == 2;
        });
    QVERIFY(reusedTitle != reusedSnapshot.clips.end());
    QCOMPARE(reusedTitle->trackId, 8);

    const jcut::CommandResult numbered = routingRuntime.execute(
        jcut::EditorCommand{jcut::CreateTitleClipCommand{0, 1}});
    QVERIFY2(numbered.applied, numbered.message.c_str());
    QCOMPARE(routingRuntime.snapshot().tracks.size(), std::size_t(4));
    QCOMPARE(QString::fromStdString(routingRuntime.snapshot().tracks.back().label),
             QStringLiteral("Titles 3"));
    QCOMPARE(routingRuntime.snapshot().tracks.back().audioEnabled, false);
    QCOMPARE(routingRuntime.undoDepth(), std::size_t(3));
    QVERIFY(routingRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(routingRuntime.snapshot().tracks.size(), std::size_t(3));
    QCOMPARE(routingRuntime.snapshot().clips.size(), std::size_t(2));

    QFile imguiSource(QStringLiteral(JCUT_SOURCE_DIR "/jcut_imgui_main.cpp"));
    QVERIFY2(imguiSource.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(imguiSource.fileName()));
    const QString imguiCode = QString::fromUtf8(imguiSource.readAll());
    QVERIFY(imguiCode.contains(QStringLiteral("Create Title At Playhead")));
    QVERIFY(imguiCode.contains(QStringLiteral("jcut::CreateTitleClipCommand")));
    QVERIFY(imguiCode.contains(QStringLiteral(
        "Uses the unnumbered Titles track first")));
    QVERIFY(!imguiCode.contains(QStringLiteral("titleTargetTrackId")));
}

void TestEditorRuntime::testReorderTrackCommandPreservesAssignmentsAndIsUndoable()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const jcut::EditorDocumentCore before = runtime.snapshot();
    const std::size_t undoDepth = runtime.undoDepth();

    const jcut::CommandResult moved = runtime.execute(
        jcut::EditorCommand{jcut::ReorderTrackCommand{2, 99}});
    QCOMPARE(moved.applied, true);
    const jcut::EditorDocumentCore reordered = runtime.snapshot();
    QCOMPARE(reordered.tracks.size(), before.tracks.size());
    QCOMPARE(reordered.tracks[0].id, 1);
    QCOMPARE(reordered.tracks[1].id, 3);
    QCOMPARE(reordered.tracks[2].id, 4);
    QCOMPARE(reordered.tracks[3].id, 2);
    QCOMPARE(reordered.tracks[3].selected, before.tracks[1].selected);
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);

    for (const jcut::EditorClip& originalClip : before.clips) {
        const auto clip = std::find_if(
            reordered.clips.cbegin(), reordered.clips.cend(),
            [&](const jcut::EditorClip& candidate) {
                return candidate.id == originalClip.id;
            });
        QVERIFY(clip != reordered.clips.cend());
        QCOMPARE(clip->trackId, originalClip.trackId);
    }

    const jcut::CommandResult noOp = runtime.execute(
        jcut::EditorCommand{jcut::ReorderTrackCommand{2, 99}});
    QCOMPARE(noOp.applied, false);
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);

    const jcut::CommandResult missing = runtime.execute(
        jcut::EditorCommand{jcut::ReorderTrackCommand{999, 0}});
    QCOMPARE(missing.applied, false);
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);

    const jcut::CommandResult undone =
        runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}});
    QCOMPARE(undone.applied, true);
    const jcut::EditorDocumentCore restored = runtime.snapshot();
    QCOMPARE(restored.tracks.size(), before.tracks.size());
    for (std::size_t index = 0; index < before.tracks.size(); ++index) {
        QCOMPARE(restored.tracks[index].id, before.tracks[index].id);
    }

    const jcut::CommandResult clampedFirst = runtime.execute(
        jcut::EditorCommand{jcut::ReorderTrackCommand{4, -5}});
    QCOMPARE(clampedFirst.applied, true);
    QCOMPARE(runtime.snapshot().tracks.front().id, 4);
}

void TestEditorRuntime::testCrossfadeTrackCommandMatchesQtTimelineSemantics()
{
    jcut::EditorDocumentCore document;
    jcut::EditorTrack track;
    track.id = 10;
    track.label = "Main";
    document.tracks.push_back(track);
    jcut::EditorTrack childTrack;
    childTrack.id = 11;
    childTrack.generatedChildTrack = true;
    childTrack.parentClipId = "middle";
    childTrack.childClipId = "middle-matte";
    document.tracks.push_back(childTrack);

    jcut::EditorClip left;
    left.id = 1;
    left.persistentId = "left";
    left.trackId = track.id;
    left.label = "Left";
    left.startFrame = 0;
    left.durationFrames = 30;
    left.mediaKind = "video";
    left.hasAudio = true;
    left.opacity = 0.8;

    jcut::EditorClip middle;
    middle.id = 2;
    middle.persistentId = "middle";
    middle.trackId = track.id;
    middle.label = "Middle";
    middle.startFrame = 60;
    middle.durationFrames = 20;
    middle.mediaKind = "video";
    middle.opacity = 0.6;

    jcut::EditorClip right;
    right.id = 3;
    right.persistentId = "right";
    right.trackId = track.id;
    right.label = "Right";
    right.startFrame = 120;
    right.durationFrames = 10;
    right.mediaKind = "audio";
    right.hasAudio = true;

    jcut::EditorClip middleMatte = middle;
    middleMatte.id = 4;
    middleMatte.persistentId = "middle-matte";
    middleMatte.trackId = childTrack.id;
    middleMatte.clipRole = "mask_matte";
    middleMatte.linkedSourceClipId = "middle";
    middleMatte.locked = true;

    document.clips = {right, middleMatte, middle, left};
    document.renderSyncMarkers.push_back({"middle", 7, false, 2});
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const jcut::EditorDocumentCore before = runtime.snapshot();
    const nlohmann::json beforeJson = jcut::toJson(before);
    const std::size_t undoDepth = runtime.undoDepth();

    const jcut::CommandResult result = runtime.execute(
        jcut::EditorCommand{jcut::CrossfadeTrackCommand{10, 0.05, true}});
    QVERIFY2(result.applied, result.message.c_str());
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);

    const jcut::EditorDocumentCore after = runtime.snapshot();
    const auto clipById = [&](int id) -> const jcut::EditorClip* {
        const auto found = std::find_if(
            after.clips.cbegin(), after.clips.cend(),
            [&](const jcut::EditorClip& clip) { return clip.id == id; });
        return found == after.clips.cend() ? nullptr : &*found;
    };
    const jcut::EditorClip* fadedLeft = clipById(1);
    const jcut::EditorClip* fadedMiddle = clipById(2);
    const jcut::EditorClip* fadedRight = clipById(3);
    QVERIFY(fadedLeft);
    QVERIFY(fadedMiddle);
    QVERIFY(fadedRight);

    // 0.05 seconds is 2,400 audio samples and rounds to two visual frames.
    // The move policy cascades from each already-moved predecessor.
    QCOMPARE(fadedLeft->startFrame, 0);
    QCOMPARE(fadedLeft->startSubframeSamples, std::int64_t{0});
    QCOMPARE(fadedMiddle->startFrame, 28);
    QCOMPARE(fadedMiddle->startSubframeSamples, std::int64_t{800});
    QCOMPARE(fadedRight->startFrame, 47);
    QCOMPARE(fadedRight->startSubframeSamples, std::int64_t{0});
    const jcut::EditorClip* fadedMiddleMatte = clipById(4);
    QVERIFY(fadedMiddleMatte);
    QCOMPARE(fadedMiddleMatte->startFrame, fadedMiddle->startFrame);
    QCOMPARE(fadedMiddleMatte->startSubframeSamples,
             fadedMiddle->startSubframeSamples);
    QCOMPARE(fadedLeft->fadeSamples, 2400);
    QCOMPARE(fadedMiddle->fadeSamples, 250);
    QCOMPARE(fadedRight->fadeSamples, 2400);

    QCOMPARE(fadedLeft->opacityKeyframes.size(), std::size_t{3});
    QCOMPARE(fadedLeft->opacityKeyframes[0].frame, std::int64_t{0});
    QCOMPARE(fadedLeft->opacityKeyframes[0].opacity, 0.8);
    QCOMPARE(fadedLeft->opacityKeyframes[1].frame, std::int64_t{28});
    QCOMPARE(fadedLeft->opacityKeyframes[1].opacity, 0.8);
    QCOMPARE(fadedLeft->opacityKeyframes[2].frame, std::int64_t{29});
    QCOMPARE(fadedLeft->opacityKeyframes[2].opacity, 0.0);
    QCOMPARE(fadedMiddle->opacity, 0.0);
    QCOMPARE(fadedMiddle->opacityKeyframes.size(), std::size_t{4});
    QCOMPARE(fadedMiddle->opacityKeyframes[0].frame, std::int64_t{0});
    QCOMPARE(fadedMiddle->opacityKeyframes[0].opacity, 0.0);
    QCOMPARE(fadedMiddle->opacityKeyframes[1].frame, std::int64_t{2});
    QCOMPARE(fadedMiddle->opacityKeyframes[1].opacity, 0.6);
    QCOMPARE(fadedMiddle->opacityKeyframes[2].frame, std::int64_t{18});
    QCOMPARE(fadedMiddle->opacityKeyframes[2].opacity, 0.6);
    QCOMPARE(fadedMiddle->opacityKeyframes[3].frame, std::int64_t{19});
    QCOMPARE(fadedMiddle->opacityKeyframes[3].opacity, 0.0);
    QVERIFY(fadedRight->opacityKeyframes.empty());

    // Timeline movement must not reinterpret source-relative sync decisions.
    QCOMPARE(after.renderSyncMarkers.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(after.renderSyncMarkers[0].clipId),
             QStringLiteral("middle"));
    QCOMPARE(after.renderSyncMarkers[0].frame, std::int64_t{7});
    QCOMPARE(after.renderSyncMarkers[0].skipFrame, false);
    QCOMPARE(after.renderSyncMarkers[0].count, 2);

    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == beforeJson);
    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.size(), after.clips.size());
}

void TestEditorRuntime::testCrossfadeTrackCommandKeepsPositionsAndRejectsInvalidTargets()
{
    jcut::EditorDocumentCore document;
    jcut::EditorTrack track;
    track.id = 1;
    track.label = "Audio";
    document.tracks.push_back(track);

    jcut::EditorClip first;
    first.id = 1;
    first.persistentId = "first";
    first.trackId = track.id;
    first.startFrame = 4;
    first.startSubframeSamples = 300;
    first.durationFrames = 30;
    first.mediaKind = "audio";
    first.hasAudio = true;

    jcut::EditorClip second = first;
    second.id = 2;
    second.persistentId = "second";
    second.startFrame = 90;
    second.startSubframeSamples = 700;
    document.clips = {first, second};

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(document);
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::CrossfadeTrackCommand{1, 0.25, false}}).applied);
    const jcut::EditorDocumentCore kept = runtime.snapshot();
    QCOMPARE(kept.clips[0].startFrame, 4);
    QCOMPARE(kept.clips[0].startSubframeSamples, std::int64_t{300});
    QCOMPARE(kept.clips[1].startFrame, 90);
    QCOMPARE(kept.clips[1].startSubframeSamples, std::int64_t{700});
    QCOMPARE(kept.clips[0].fadeSamples, 12000);
    QCOMPARE(kept.clips[1].fadeSamples, 12000);
    QVERIFY(kept.clips[0].opacityKeyframes.empty());
    QVERIFY(kept.clips[1].opacityKeyframes.empty());

    jcut::EditorDocumentCore lockedDocument = document;
    lockedDocument.clips[1].locked = true;
    jcut::EditorRuntime lockedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(lockedDocument));
    const nlohmann::json beforeLocked =
        jcut::toJson(lockedRuntime.snapshot());
    const jcut::CommandResult lockedResult = lockedRuntime.execute(
        jcut::EditorCommand{jcut::CrossfadeTrackCommand{1, 0.5, true}});
    QCOMPARE(lockedResult.applied, false);
    QVERIFY(jcut::toJson(lockedRuntime.snapshot()) == beforeLocked);
    QCOMPARE(lockedRuntime.undoDepth(), std::size_t{0});

    jcut::EditorDocumentCore generatedDocument;
    jcut::EditorTrack sourceTrack;
    sourceTrack.id = 1;
    sourceTrack.label = "Source";
    jcut::EditorTrack generatedTrack;
    generatedTrack.id = 2;
    generatedTrack.generatedChildTrack = true;
    generatedTrack.parentClipId = "source";
    generatedTrack.childClipId = "matte";
    generatedDocument.tracks = {sourceTrack, generatedTrack};

    jcut::EditorClip source;
    source.id = 1;
    source.trackId = 1;
    source.persistentId = "source";
    source.mediaKind = "video";
    source.durationFrames = 30;
    jcut::EditorClip matte = source;
    matte.id = 2;
    matte.trackId = 2;
    matte.persistentId = "matte";
    matte.clipRole = "mask_matte";
    matte.linkedSourceClipId = "source";
    generatedDocument.clips = {source, matte};

    jcut::EditorRuntime generatedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(generatedDocument));
    const nlohmann::json beforeGenerated =
        jcut::toJson(generatedRuntime.snapshot());
    const jcut::CommandResult generatedResult = generatedRuntime.execute(
        jcut::EditorCommand{jcut::CrossfadeTrackCommand{2, 0.5, true}});
    QCOMPARE(generatedResult.applied, false);
    QVERIFY(jcut::toJson(generatedRuntime.snapshot()) == beforeGenerated);
    QCOMPARE(generatedRuntime.undoDepth(), std::size_t{0});

    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::CrossfadeTrackCommand{999, 0.5, true}}).applied, false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::CrossfadeTrackCommand{1, 0.0, true}}).applied, false);

    jcut::EditorDocumentCore overflowDocument;
    overflowDocument.tracks.push_back({1, "Overflow", true});
    jcut::EditorClip overflowLeft = first;
    overflowLeft.startFrame = std::numeric_limits<int>::max();
    overflowLeft.durationFrames = std::numeric_limits<int>::max();
    jcut::EditorClip overflowRight = second;
    overflowRight.startFrame = std::numeric_limits<int>::max();
    overflowDocument.clips = {overflowLeft, overflowRight};
    jcut::EditorRuntime overflowRuntime =
        jcut::EditorRuntime::fromDocument(std::move(overflowDocument));
    const nlohmann::json beforeOverflow =
        jcut::toJson(overflowRuntime.snapshot());
    QCOMPARE(overflowRuntime.execute(jcut::EditorCommand{
        jcut::CrossfadeTrackCommand{1, 0.5, true}}).applied, false);
    QVERIFY(jcut::toJson(overflowRuntime.snapshot()) == beforeOverflow);
    QCOMPARE(overflowRuntime.undoDepth(), std::size_t{0});
}

void TestEditorRuntime::testMediaImportAndInsertionCommandsUpdateDocument()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/new-broll.mov",
        "New Broll",
        "video"});
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(6));
    QCOMPARE(QString::fromStdString(runtime.snapshot().mediaItems.back().label), QStringLiteral("New Broll"));

    applyCommand(runtime, jcut::InsertClipFromMediaCommand{
        "/tmp/new-broll.mov",
        1,
        210,
        48});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(5));
    QCOMPARE(QString::fromStdString(runtime.snapshot().clips.back().label), QStringLiteral("New Broll"));
    QCOMPARE(runtime.snapshot().clips.back().trackId, 1);
    QCOMPARE(runtime.snapshot().clips.back().startFrame, 210);
    QCOMPARE(runtime.snapshot().clips.back().durationFrames, 48);
    QCOMPARE(runtime.snapshot().clips.back().hasAudio, true);
    QCOMPARE(runtime.snapshot().clips.back().audioPresenceKnown, false);

    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/new-broll.mov", "New Broll", "video", true, false});
    QCOMPARE(runtime.snapshot().clips.back().audioPresenceKnown, true);
    QCOMPARE(runtime.snapshot().clips.back().hasAudio, false);

    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/silent-broll.mov",
        "Silent Broll",
        "video",
        true,
        false});
    applyCommand(runtime, jcut::InsertClipFromMediaCommand{
        "/tmp/silent-broll.mov", 1, 260, 48});
    QCOMPARE(runtime.snapshot().clips.back().audioPresenceKnown, true);
    QCOMPARE(runtime.snapshot().clips.back().hasAudio, false);

    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/legacy-unknown.WAV", "Unknown Voice", "unknown"});
    applyCommand(runtime, jcut::InsertClipFromMediaCommand{
        "/tmp/legacy-unknown.WAV", 1, 320, 48});
    QCOMPARE(runtime.snapshot().clips.back().audioPresenceKnown, false);
    QCOMPARE(runtime.snapshot().clips.back().hasAudio, true);

    const jcut::CommandResult missingMedia =
        runtime.execute(jcut::EditorCommand{jcut::InsertClipFromMediaCommand{"missing", 1, 0, 10}});
    QCOMPARE(missingMedia.applied, false);

    jcut::EditorDocumentCore routingDocument;
    routingDocument.tracks.push_back({1, "Video 1"});
    routingDocument.tracks.push_back({2, "Video 2"});
    jcut::EditorClip firstVisual;
    firstVisual.id = 1;
    firstVisual.trackId = 1;
    firstVisual.startFrame = 0;
    firstVisual.durationFrames = 100;
    firstVisual.mediaKind = "video";
    routingDocument.clips.push_back(firstVisual);
    jcut::EditorClip secondVisual = firstVisual;
    secondVisual.id = 2;
    secondVisual.trackId = 2;
    routingDocument.clips.push_back(secondVisual);
    QCOMPARE(jcut::firstNonConflictingTrackIndex(
                 routingDocument, 0, "video", 50, 10),
             -1);
    QCOMPARE(jcut::firstNonConflictingTrackIndex(
                 routingDocument, 0, "audio", 50, 10),
             0);
    QCOMPARE(jcut::firstNonConflictingTrackIndex(
                 routingDocument, 0, "image", 100, 10),
             0);
    routingDocument.clips.pop_back();
    QCOMPARE(jcut::firstNonConflictingTrackIndex(
                 routingDocument, 0, "graphics", 50, 10),
             1);

    jcut::EditorDocumentCore droppedDocument;
    droppedDocument.tracks.push_back({1, "Video"});
    jcut::EditorRuntime droppedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(droppedDocument));
    const std::size_t droppedUndoDepth = droppedRuntime.undoDepth();
    const jcut::CommandResult dropped = droppedRuntime.execute(
        jcut::EditorCommand{jcut::AddClipCommand{
            1,
            "Dropped Clip",
            24,
            120,
            "/tmp/dropped.mov",
            "video",
            true,
            true}});
    QVERIFY(dropped.applied);
    QCOMPARE(droppedRuntime.snapshot().mediaItems.size(), std::size_t(1));
    QCOMPARE(droppedRuntime.snapshot().clips.size(), std::size_t(1));
    QVERIFY(droppedRuntime.snapshot().mediaItems.front().audioPresenceKnown);
    QVERIFY(droppedRuntime.snapshot().mediaItems.front().hasAudio);
    QVERIFY(droppedRuntime.snapshot().clips.front().audioPresenceKnown);
    QVERIFY(droppedRuntime.snapshot().clips.front().hasAudio);
    QCOMPARE(droppedRuntime.undoDepth(), droppedUndoDepth + 1);

    QVERIFY(droppedRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(droppedRuntime.snapshot().mediaItems.empty());
    QVERIFY(droppedRuntime.snapshot().clips.empty());
}

void TestEditorRuntime::testMediaRemovalRejectsReferencedItemsAndIsUndoable()
{
    jcut::EditorDocumentCore document;
    document.mediaItems = {
        {"/tmp/used.mov", "Used", "video"},
        {"/tmp/unused.wav", "Unused", "audio"},
    };
    document.tracks.push_back({1, "Video"});
    jcut::EditorClip referencedClip;
    referencedClip.id = 1;
    referencedClip.trackId = 1;
    referencedClip.sourcePath = "/tmp/used.mov";
    document.clips.push_back(std::move(referencedClip));

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json initialSnapshot = jcut::toJson(runtime.snapshot());
    const std::size_t initialUndoDepth = runtime.undoDepth();

    const jcut::CommandResult referenced = runtime.execute(
        jcut::EditorCommand{jcut::RemoveMediaCommand{"/tmp/used.mov"}});
    QCOMPARE(referenced.applied, false);
    QCOMPARE(QString::fromStdString(referenced.message),
             QStringLiteral("media is used by timeline clips"));
    QVERIFY(jcut::toJson(runtime.snapshot()) == initialSnapshot);
    QCOMPARE(runtime.undoDepth(), initialUndoDepth);

    const jcut::CommandResult missing = runtime.execute(
        jcut::EditorCommand{jcut::RemoveMediaCommand{"/tmp/missing.mov"}});
    QCOMPARE(missing.applied, false);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initialSnapshot);
    QCOMPARE(runtime.undoDepth(), initialUndoDepth);

    const jcut::CommandResult removed = runtime.execute(
        jcut::EditorCommand{jcut::RemoveMediaCommand{"/tmp/unused.wav"}});
    QCOMPARE(removed.applied, true);
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().mediaItems.front().id,
             std::string("/tmp/used.mov"));
    QCOMPARE(runtime.undoDepth(), initialUndoDepth + 1);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initialSnapshot);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().mediaItems.front().id,
             std::string("/tmp/used.mov"));
}

void TestEditorRuntime::testSplitClipCommandUpdatesTimeline()
{
    jcut::EditorDocumentCore document = jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& sourceClip = document.clips.front();
    sourceClip.mediaKind = "video";
    sourceClip.sourceDurationFrames = 900;
    sourceClip.sourceFps = 30.0;
    sourceClip.sourceInFrame = 10;
    sourceClip.transformKeyframes = {
        {30, "Left", 1.0, 2.0, 0.0, 1.0, 1.0, true},
        {150, "Right", 3.0, 4.0, 0.0, 1.0, 1.0, true}
    };
    sourceClip.gradingKeyframes = {{150, 0.2, 1.0, 1.0, 1.0, true}};
    sourceClip.opacityKeyframes = {{150, 0.5, true}};
    sourceClip.titleKeyframes = {{150, "Right title", 0.0, 0.0, 48.0, 1.0,
                                  "DejaVu Sans", true, false, "#ffffffff", true}};
    document.renderSyncMarkers = {
        {sourceClip.persistentId, 60, false, 1},
        {sourceClip.persistentId, 200, true, 1}
    };
    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(std::move(document));

    applyCommand(runtime, jcut::SplitClipCommand{1, 120});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(5));
    QCOMPARE(runtime.snapshot().clips.at(0).id, 1);
    QCOMPARE(runtime.snapshot().clips.at(0).durationFrames, 120);
    QCOMPARE(runtime.snapshot().clips.at(1).trackId, 1);
    QCOMPARE(runtime.snapshot().clips.at(1).startFrame, 120);
    QCOMPARE(runtime.snapshot().clips.at(1).durationFrames, 300);
    QCOMPARE(runtime.snapshot().clips.at(1).selected, true);
    QVERIFY(runtime.snapshot().clips.at(0).persistentId !=
            runtime.snapshot().clips.at(1).persistentId);
    QCOMPARE(runtime.snapshot().clips.at(0).transformKeyframes.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().clips.at(0).transformKeyframes.front().frame, std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.at(1).transformKeyframes.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().clips.at(1).transformKeyframes.front().frame, std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.at(1).gradingKeyframes.front().frame, std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.at(1).opacityKeyframes.front().frame, std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.at(1).titleKeyframes.front().frame, std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.at(1).sourceInFrame, std::int64_t(130));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.at(0).clipId,
             runtime.snapshot().clips.at(0).persistentId);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.at(1).clipId,
             runtime.snapshot().clips.at(1).persistentId);

    const jcut::CommandResult invalidSplit =
        runtime.execute(jcut::EditorCommand{jcut::SplitClipCommand{1, 0}});
    QCOMPARE(invalidSplit.applied, false);

    const jcut::CommandResult missingClip =
        runtime.execute(jcut::EditorCommand{jcut::SplitClipCommand{999, 12}});
    QCOMPARE(missingClip.applied, false);
}

void TestEditorRuntime::testSplitSelectedClipsCommandMatchesRazorSemantics()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    for (jcut::EditorClip& clip : document.clips) {
        clip.selected = true;
    }
    document.clips.at(3).locked = true;
    document.renderSyncMarkers = {
        {document.clips.at(0).persistentId, 40, false, 1},
        {document.clips.at(0).persistentId, 160, true, 1},
        {document.clips.at(1).persistentId, 180, false, 1},
        {document.clips.at(3).persistentId, 200, false, 1}
    };
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const jcut::EditorDocumentCore before = runtime.snapshot();

    const jcut::CommandResult result = runtime.execute(
        jcut::EditorCommand{jcut::SplitSelectedClipsCommand{100}});
    QCOMPARE(result.applied, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorDocumentCore split = runtime.snapshot();
    QCOMPARE(split.clips.size(), std::size_t(6));
    const auto clipById = [&](int id) -> const jcut::EditorClip& {
        const auto clip = std::find_if(
            split.clips.cbegin(), split.clips.cend(),
            [&](const jcut::EditorClip& candidate) {
                return candidate.id == id;
            });
        Q_ASSERT(clip != split.clips.cend());
        return *clip;
    };

    QCOMPARE(clipById(1).durationFrames, 100);
    QCOMPARE(clipById(2).durationFrames, 64);
    QCOMPARE(clipById(3).durationFrames, 96);
    QCOMPARE(clipById(4).durationFrames, 420);
    QCOMPARE(clipById(5).startFrame, 100);
    QCOMPARE(clipById(5).durationFrames, 320);
    QCOMPARE(clipById(6).startFrame, 100);
    QCOMPARE(clipById(6).durationFrames, 332);

    for (const jcut::EditorClip& clip : split.clips) {
        QCOMPARE(clip.selected, clip.id == 5 || clip.id == 6);
    }
    QCOMPARE(split.renderSyncMarkers.at(0).clipId,
             clipById(1).persistentId);
    QCOMPARE(split.renderSyncMarkers.at(1).clipId,
             clipById(5).persistentId);
    QCOMPARE(split.renderSyncMarkers.at(2).clipId,
             clipById(6).persistentId);
    QCOMPARE(split.renderSyncMarkers.at(3).clipId,
             clipById(4).persistentId);

    const jcut::CommandResult undone = runtime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}});
    QCOMPARE(undone.applied, true);
    QVERIFY(jcut::toJson(runtime.snapshot()) == jcut::toJson(before));

    jcut::EditorDocumentCore ineligible = before;
    for (jcut::EditorClip& clip : ineligible.clips) {
        clip.selected = clip.id == 3 || clip.id == 4;
    }
    jcut::EditorRuntime ineligibleRuntime =
        jcut::EditorRuntime::fromDocument(std::move(ineligible));
    const nlohmann::json beforeIneligible =
        jcut::toJson(ineligibleRuntime.snapshot());
    const jcut::CommandResult noSplit = ineligibleRuntime.execute(
        jcut::EditorCommand{jcut::SplitSelectedClipsCommand{100}});
    QCOMPARE(noSplit.applied, false);
    QVERIFY(jcut::toJson(ineligibleRuntime.snapshot()) == beforeIneligible);
    QCOMPARE(ineligibleRuntime.canUndo(), false);
}

void TestEditorRuntime::testTrimClipCommandsUpdateTimeline()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::TrimClipStartCommand{1, 24});
    QCOMPARE(runtime.snapshot().clips.at(0).startFrame, 24);
    QCOMPARE(runtime.snapshot().clips.at(0).durationFrames, 396);

    applyCommand(runtime, jcut::TrimClipEndCommand{1, 300});
    QCOMPARE(runtime.snapshot().clips.at(0).startFrame, 24);
    QCOMPARE(runtime.snapshot().clips.at(0).durationFrames, 276);

    const jcut::CommandResult invalidStart =
        runtime.execute(jcut::EditorCommand{jcut::TrimClipStartCommand{1, 400}});
    QCOMPARE(invalidStart.applied, false);

    const jcut::CommandResult invalidEnd =
        runtime.execute(jcut::EditorCommand{jcut::TrimClipEndCommand{1, 24}});
    QCOMPARE(invalidEnd.applied, false);
}

void TestEditorRuntime::testAudioClipDurationPreservesSubframeSamples()
{
    TimelineClip clip;
    clip.id = QStringLiteral("audio-a");
    clip.filePath = QStringLiteral("/tmp/audio.wav");
    clip.mediaType = ClipMediaType::Audio;
    clip.sourceFps = static_cast<qreal>(kTimelineFps);
    clip.sourceDurationFrames = 120;
    clip.startFrame = 10;
    clip.startSubframeSamples = kSamplesPerFrame / 4;
    clip.durationFrames = 2;
    clip.durationSubframeSamples = kSamplesPerFrame / 2;

    QCOMPARE(clipTimelineDurationSamples(clip),
             frameToSamples(2) + (kSamplesPerFrame / 2));
    QCOMPARE(clipTimelineEndSamples(clip),
             clipTimelineStartSamples(clip) + frameToSamples(2) + (kSamplesPerFrame / 2));

    const int64_t lastTimelineSample = clipTimelineEndSamples(clip) - 1;
    const int64_t lastSourceSample =
        sourceSampleForClipAtTimelineSample(clip, lastTimelineSample, {});
    QCOMPARE(lastSourceSample, clipTimelineDurationSamples(clip) - 1);

    const TimelineClip loaded = editor::clipFromJson(editor::clipToJson(clip));
    QCOMPARE(loaded.durationFrames, clip.durationFrames);
    QCOMPARE(loaded.durationSubframeSamples, clip.durationSubframeSamples);
    QCOMPARE(clipTimelineDurationSamples(loaded), clipTimelineDurationSamples(clip));
}

void TestEditorRuntime::testProjectAndClipEditCommandsUpdateDocument()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SetProjectNameCommand{"Client Cut"});
    QCOMPARE(QString::fromStdString(runtime.snapshot().projectName), QStringLiteral("Client Cut"));

    applyCommand(runtime, jcut::SetClipLabelCommand{2, "Interview B Tight"});
    QCOMPARE(QString::fromStdString(runtime.snapshot().clips.at(1).label),
             QStringLiteral("Interview B Tight"));

    applyCommand(runtime, jcut::MoveClipCommand{2, 3, 84});
    QCOMPARE(runtime.snapshot().clips.at(1).trackId, 3);
    QCOMPARE(runtime.snapshot().clips.at(1).startFrame, 84);

    applyCommand(runtime, jcut::ResizeClipCommand{2, 180});
    QCOMPARE(runtime.snapshot().clips.at(1).durationFrames, 180);

    applyCommand(runtime, jcut::ResizeClipCommand{2, -5});
    QCOMPARE(runtime.snapshot().clips.at(1).durationFrames, 1);

    const jcut::CommandResult moveMissingTrack =
        runtime.execute(jcut::EditorCommand{jcut::MoveClipCommand{2, 99, 20}});
    QCOMPARE(moveMissingTrack.applied, false);
}

void TestEditorRuntime::testSelectionCommandsSwitchSingleSelection()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SelectTrackCommand{3});
    int selectedTracks = 0;
    for (const jcut::EditorTrack& track : runtime.snapshot().tracks) {
        if (track.selected) {
            ++selectedTracks;
            QCOMPARE(track.id, 3);
        }
    }
    QCOMPARE(selectedTracks, 1);

    applyCommand(runtime, jcut::SelectClipCommand{2});
    int selectedClips = 0;
    for (const jcut::EditorClip& clip : runtime.snapshot().clips) {
        if (clip.selected) {
            ++selectedClips;
            QCOMPARE(clip.id, 2);
        }
    }
    QCOMPARE(selectedClips, 1);
}

void TestEditorRuntime::testClipSelectionSupportsAdditiveToggleAndSelectAll()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SelectClipCommand{2, true});
    jcut::EditorDocumentCore snapshot = runtime.snapshot();
    QVERIFY(snapshot.clips.at(0).selected);
    QVERIFY(snapshot.clips.at(1).selected);

    applyCommand(runtime, jcut::SelectClipCommand{1, false, true});
    snapshot = runtime.snapshot();
    QVERIFY(!snapshot.clips.at(0).selected);
    QVERIFY(snapshot.clips.at(1).selected);

    applyCommand(runtime, jcut::SelectClipCommand{3, true});
    snapshot = runtime.snapshot();
    QVERIFY(snapshot.clips.at(1).selected);
    QVERIFY(snapshot.clips.at(2).selected);

    applyCommand(runtime, jcut::SelectClipCommand{4});
    snapshot = runtime.snapshot();
    QCOMPARE(static_cast<int>(std::count_if(
                 snapshot.clips.begin(), snapshot.clips.end(),
                 [](const jcut::EditorClip& clip) { return clip.selected; })),
             1);
    QVERIFY(snapshot.clips.at(3).selected);

    const jcut::CommandResult selectAll = runtime.execute(
        jcut::EditorCommand{jcut::SelectAllClipsCommand{}});
    QCOMPARE(selectAll.applied, true);
    snapshot = runtime.snapshot();
    QVERIFY(std::all_of(
        snapshot.clips.begin(), snapshot.clips.end(),
        [](const jcut::EditorClip& clip) { return clip.selected; }));
    QCOMPARE(runtime.canUndo(), false);
}

void TestEditorRuntime::testClipboardPasteRemapsMarkersAndSurvivesUndo()
{
    jcut::EditorDocumentCore document;
    document.tracks = {
        {10, "Track A", true},
        {20, "Track B", false},
        {30, "Track C", false},
    };

    jcut::EditorClip first;
    first.id = 1;
    first.trackId = 10;
    first.label = "First";
    first.startFrame = 100;
    first.durationFrames = 20;
    first.selected = true;
    first.persistentId = "clip-a";
    jcut::EditorClip second;
    second.id = 2;
    second.trackId = 30;
    second.label = "Second";
    second.startFrame = 130;
    second.durationFrames = 10;
    second.selected = true;
    second.persistentId = "clip-b";
    jcut::EditorClip unselected;
    unselected.id = 3;
    unselected.trackId = 20;
    unselected.label = "Unselected";
    unselected.startFrame = 0;
    unselected.durationFrames = 10;
    unselected.persistentId = "clip-c";
    document.clips = {first, second, unselected};
    document.renderSyncMarkers = {
        {"clip-a", 105, false, 1},
        {"clip-b", 135, true, 2},
        {"clip-c", 5, false, 1},
    };

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json beforeCopy = jcut::toJson(runtime.snapshot());
    const jcut::CommandResult copied = runtime.execute(
        jcut::EditorCommand{jcut::CopySelectedClipsCommand{}});
    QCOMPARE(copied.applied, true);
    QVERIFY(jcut::toJson(runtime.snapshot()) == beforeCopy);
    QCOMPARE(runtime.canUndo(), false);

    const jcut::CommandResult pasted = runtime.execute(
        jcut::EditorCommand{jcut::PasteClipsCommand{200, 20}});
    QCOMPARE(pasted.applied, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    jcut::EditorDocumentCore pastedDocument = runtime.snapshot();
    QCOMPARE(pastedDocument.tracks.size(), std::size_t(4));
    QCOMPARE(pastedDocument.clips.size(), std::size_t(5));
    QCOMPARE(pastedDocument.renderSyncMarkers.size(), std::size_t(5));

    std::vector<jcut::EditorClip> pastedClips;
    for (const jcut::EditorClip& clip : pastedDocument.clips) {
        if (clip.selected) {
            pastedClips.push_back(clip);
        }
    }
    std::sort(pastedClips.begin(), pastedClips.end(),
              [](const jcut::EditorClip& left,
                 const jcut::EditorClip& right) {
                  return left.startFrame < right.startFrame;
              });
    QCOMPARE(pastedClips.size(), std::size_t(2));
    QCOMPARE(pastedClips.at(0).startFrame, 200);
    QCOMPARE(pastedClips.at(0).trackId, 20);
    QCOMPARE(pastedClips.at(1).startFrame, 230);
    QCOMPARE(pastedClips.at(1).trackId, 31);
    QVERIFY(pastedClips.at(0).persistentId != "clip-a");
    QVERIFY(pastedClips.at(1).persistentId != "clip-b");

    const auto hasRemappedMarker = [&](const jcut::EditorClip& clip,
                                       std::int64_t frame,
                                       bool skipFrame,
                                       int count) {
        return std::any_of(
            pastedDocument.renderSyncMarkers.begin(),
            pastedDocument.renderSyncMarkers.end(),
            [&](const jcut::EditorRenderSyncMarker& marker) {
                return marker.clipId == clip.persistentId &&
                    marker.frame == frame && marker.skipFrame == skipFrame &&
                    marker.count == count;
            });
    };
    QVERIFY(hasRemappedMarker(pastedClips.at(0), 205, false, 1));
    QVERIFY(hasRemappedMarker(pastedClips.at(1), 235, true, 2));

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(3));
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(3));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(3));
    QCOMPARE(runtime.canRedo(), true);

    applyCommand(runtime, jcut::RedoCommand{});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(5));
    applyCommand(runtime, jcut::UndoCommand{});

    // Clipboard state is intentionally outside document history and remains
    // available after undoing the paste that consumed it.
    const jcut::CommandResult pastedAgain = runtime.execute(
        jcut::EditorCommand{jcut::PasteClipsCommand{300, 10}});
    QCOMPARE(pastedAgain.applied, true);
    QCOMPARE(runtime.canRedo(), false);
    pastedDocument = runtime.snapshot();
    std::vector<int> selectedStarts;
    for (const jcut::EditorClip& clip : pastedDocument.clips) {
        if (clip.selected) {
            selectedStarts.push_back(clip.startFrame);
        }
    }
    std::sort(selectedStarts.begin(), selectedStarts.end());
    QCOMPARE(selectedStarts.size(), std::size_t(2));
    QCOMPARE(selectedStarts.at(0), 300);
    QCOMPARE(selectedStarts.at(1), 330);
}

void TestEditorRuntime::testCutAndDuplicateClipsAreUndoable()
{
    jcut::EditorDocumentCore document;
    document.tracks = {{1, "Video", true}};
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.label = "Source";
    clip.startFrame = 10;
    clip.durationFrames = 20;
    clip.selected = true;
    clip.persistentId = "source-clip";
    document.clips = {clip};
    document.renderSyncMarkers = {{"source-clip", 15, false, 1}};

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const jcut::CommandResult duplicated = runtime.execute(
        jcut::EditorCommand{jcut::DuplicateSelectedClipsCommand{}});
    QCOMPARE(duplicated.applied, true);
    const jcut::EditorDocumentCore duplicatedDocument = runtime.snapshot();
    QCOMPARE(duplicatedDocument.clips.size(), std::size_t(2));
    QCOMPARE(duplicatedDocument.renderSyncMarkers.size(), std::size_t(2));
    const jcut::EditorClip& duplicate = duplicatedDocument.clips.back();
    QCOMPARE(duplicate.startFrame, 30);
    QVERIFY(duplicate.persistentId != "source-clip");
    QVERIFY(std::any_of(
        duplicatedDocument.renderSyncMarkers.begin(),
        duplicatedDocument.renderSyncMarkers.end(),
        [&](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == duplicate.persistentId &&
                marker.frame == 35;
        }));

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));

    const jcut::CommandResult cut = runtime.execute(
        jcut::EditorCommand{jcut::CutSelectedClipsCommand{}});
    QCOMPARE(cut.applied, true);
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(0));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(0));
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));
    QVERIFY(runtime.snapshot().clips.front().selected);
}

void TestEditorRuntime::testDeleteSelectedClipsIsAtomicAndUndoable()
{
    jcut::EditorDocumentCore document;
    document.tracks = {
        {10, "Upper", true},
        {20, "Lower", false},
    };

    jcut::EditorClip remainingLower;
    remainingLower.id = 4;
    remainingLower.trackId = 20;
    remainingLower.startFrame = 5;
    remainingLower.durationFrames = 10;
    remainingLower.persistentId = "remaining-lower";
    jcut::EditorClip selectedUpper;
    selectedUpper.id = 1;
    selectedUpper.trackId = 10;
    selectedUpper.startFrame = 10;
    selectedUpper.durationFrames = 10;
    selectedUpper.selected = true;
    selectedUpper.persistentId = "selected-upper";
    jcut::EditorClip remainingUpper;
    remainingUpper.id = 3;
    remainingUpper.trackId = 10;
    remainingUpper.startFrame = 40;
    remainingUpper.durationFrames = 10;
    remainingUpper.persistentId = "remaining-upper";
    jcut::EditorClip selectedLower;
    selectedLower.id = 2;
    selectedLower.trackId = 20;
    selectedLower.startFrame = 20;
    selectedLower.durationFrames = 10;
    selectedLower.selected = true;
    selectedLower.persistentId = "selected-lower";
    document.clips = {
        remainingLower, selectedUpper, remainingUpper, selectedLower};
    document.renderSyncMarkers = {
        {"selected-upper", 12, false, 1},
        {"selected-lower", 22, true, 2},
        {"remaining-upper", 42, false, 1},
        {"remaining-lower", 7, false, 1},
    };

    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(document);
    const nlohmann::json original = jcut::toJson(runtime.snapshot());
    const jcut::CommandResult deleted = runtime.execute(
        jcut::EditorCommand{jcut::DeleteSelectedClipsCommand{}});
    QCOMPARE(deleted.applied, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorDocumentCore afterDelete = runtime.snapshot();
    QCOMPARE(afterDelete.clips.size(), std::size_t(2));
    QCOMPARE(afterDelete.renderSyncMarkers.size(), std::size_t(2));
    const auto selected = std::find_if(
        afterDelete.clips.begin(), afterDelete.clips.end(),
        [](const jcut::EditorClip& clip) { return clip.selected; });
    QVERIFY(selected != afterDelete.clips.end());
    // Track row, start frame, then numeric ID define the fallback selection;
    // it is independent of the clips' insertion order.
    QCOMPARE(selected->id, 3);
    QCOMPARE(static_cast<int>(std::count_if(
                 afterDelete.clips.begin(), afterDelete.clips.end(),
                 [](const jcut::EditorClip& clip) { return clip.selected; })),
             1);
    QVERIFY(std::none_of(
        afterDelete.renderSyncMarkers.begin(),
        afterDelete.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "selected-upper" ||
                marker.clipId == "selected-lower";
        }));

    applyCommand(runtime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(runtime.snapshot()) == original);

    document.clips.back().locked = true;
    jcut::EditorRuntime lockedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json beforeLockedDelete =
        jcut::toJson(lockedRuntime.snapshot());
    const jcut::CommandResult lockedDelete = lockedRuntime.execute(
        jcut::EditorCommand{jcut::DeleteSelectedClipsCommand{}});
    QCOMPARE(lockedDelete.applied, false);
    QVERIFY(jcut::toJson(lockedRuntime.snapshot()) == beforeLockedDelete);
    QCOMPARE(lockedRuntime.canUndo(), false);
}

void TestEditorRuntime::
    testMoveSelectedClipsPreservesLayoutAndRejectsInvalidGroups()
{
    const std::vector<jcut::timeline::SnapClip> snapClips = {
        {1, 10, 10, true},
        {2, 30, 10, true},
        {3, 51, 10, false}};
    const jcut::timeline::GroupMoveSnap companionEdgeSnap =
        jcut::timeline::snapSelectedGroupMove(
            snapClips, 1, 20, 2);
    QCOMPARE(companionEdgeSnap.anchorStartFrame, std::int64_t(21));
    QCOMPARE(companionEdgeSnap.boundaryFrame, std::int64_t(51));

    const std::vector<jcut::timeline::SnapClip> zeroBoundClips = {
        {1, 20, 10, true},
        {2, 5, 10, true}};
    const jcut::timeline::GroupMoveSnap zeroBoundSnap =
        jcut::timeline::snapSelectedGroupMove(
            zeroBoundClips, 1, 10, 2);
    QCOMPARE(zeroBoundSnap.anchorStartFrame, std::int64_t(15));
    QCOMPARE(zeroBoundSnap.boundaryFrame, std::int64_t(0));

    jcut::EditorDocumentCore document;
    document.tracks = {
        {10, "Track A", true},
        {20, "Track B", false},
        {30, "Track C", false},
    };

    jcut::EditorClip anchor;
    anchor.id = 1;
    anchor.trackId = 20;
    anchor.startFrame = 10;
    anchor.durationFrames = 20;
    anchor.selected = true;
    anchor.persistentId = "anchor";
    jcut::EditorClip companion;
    companion.id = 2;
    companion.trackId = 30;
    companion.startFrame = 2;
    companion.durationFrames = 10;
    companion.selected = true;
    companion.persistentId = "companion";
    jcut::EditorClip untouched;
    untouched.id = 3;
    untouched.trackId = 10;
    untouched.startFrame = 50;
    untouched.durationFrames = 10;
    untouched.persistentId = "untouched";
    document.clips = {anchor, companion, untouched};
    document.renderSyncMarkers = {
        {"anchor", 12, false, 1},
        {"companion", 3, true, 2},
        {"untouched", 51, false, 1},
    };

    jcut::EditorRuntime groupLockRuntime =
        jcut::EditorRuntime::fromDocument(document);
    QVERIFY(groupLockRuntime.execute(jcut::EditorCommand{
        jcut::SetSelectedClipsLockedCommand{true}}).applied);
    const jcut::EditorDocumentCore lockedSelection =
        groupLockRuntime.snapshot();
    QVERIFY(lockedSelection.clips.at(0).locked);
    QVERIFY(lockedSelection.clips.at(1).locked);
    QVERIFY(!lockedSelection.clips.at(2).locked);
    QVERIFY(groupLockRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(!groupLockRuntime.snapshot().clips.at(0).locked);
    QVERIFY(!groupLockRuntime.snapshot().clips.at(1).locked);

    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(document);
    const nlohmann::json original = jcut::toJson(runtime.snapshot());
    const jcut::CommandResult moved = runtime.execute(
        jcut::EditorCommand{jcut::MoveSelectedClipsCommand{1, 10, -50}});
    QCOMPARE(moved.applied, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorDocumentCore afterMove = runtime.snapshot();
    QCOMPARE(afterMove.clips.at(0).trackId, 10);
    QCOMPARE(afterMove.clips.at(0).startFrame, 8);
    QCOMPARE(afterMove.clips.at(1).trackId, 20);
    QCOMPARE(afterMove.clips.at(1).startFrame, 0);
    QCOMPARE(afterMove.clips.at(2).trackId, 10);
    QCOMPARE(afterMove.clips.at(2).startFrame, 50);
    QVERIFY(std::any_of(
        afterMove.renderSyncMarkers.begin(),
        afterMove.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "anchor" && marker.frame == 10;
        }));
    QVERIFY(std::any_of(
        afterMove.renderSyncMarkers.begin(),
        afterMove.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "companion" && marker.frame == 1;
        }));
    QVERIFY(std::any_of(
        afterMove.renderSyncMarkers.begin(),
        afterMove.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "untouched" && marker.frame == 51;
        }));

    applyCommand(runtime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(runtime.snapshot()) == original);
    QCOMPARE(runtime.canRedo(), true);

    const nlohmann::json beforeOutOfRangeMove =
        jcut::toJson(runtime.snapshot());
    const jcut::CommandResult outOfRangeTrack = runtime.execute(
        jcut::EditorCommand{jcut::MoveSelectedClipsCommand{1, 30, 50}});
    QCOMPARE(outOfRangeTrack.applied, false);
    QVERIFY(jcut::toJson(runtime.snapshot()) == beforeOutOfRangeMove);
    QCOMPARE(runtime.canRedo(), true);

    jcut::EditorDocumentCore lockedDocument = document;
    lockedDocument.clips.at(1).locked = true;
    jcut::EditorRuntime lockedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(lockedDocument));
    const nlohmann::json beforeLockedMove =
        jcut::toJson(lockedRuntime.snapshot());
    const jcut::CommandResult lockedMove = lockedRuntime.execute(
        jcut::EditorCommand{jcut::MoveSelectedClipsCommand{1, 10, 0}});
    QCOMPARE(lockedMove.applied, false);
    QVERIFY(jcut::toJson(lockedRuntime.snapshot()) == beforeLockedMove);
    QCOMPARE(lockedRuntime.canUndo(), false);

    jcut::EditorDocumentCore overflowDocument = document;
    overflowDocument.clips.at(1).trackId = 20;
    overflowDocument.clips.at(1).startFrame = 20;
    jcut::EditorRuntime overflowRuntime =
        jcut::EditorRuntime::fromDocument(std::move(overflowDocument));
    const nlohmann::json beforeOverflowMove =
        jcut::toJson(overflowRuntime.snapshot());
    const jcut::CommandResult overflowMove = overflowRuntime.execute(
        jcut::EditorCommand{jcut::MoveSelectedClipsCommand{
            1, 20, std::numeric_limits<int>::max()}});
    QCOMPARE(overflowMove.applied, false);
    QVERIFY(jcut::toJson(overflowRuntime.snapshot()) == beforeOverflowMove);
    QCOMPARE(overflowRuntime.canUndo(), false);
}

void TestEditorRuntime::testUndoRedoRestoresDeterministicSnapshots()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const nlohmann::json initialSnapshot = jcut::toJson(runtime.snapshot());

    QCOMPARE(runtime.canUndo(), false);
    QCOMPARE(runtime.canRedo(), false);

    applyCommand(runtime, jcut::SetProjectNameCommand{"Client Cut"});
    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/history-broll.mov",
        "History Broll",
        "video"});
    applyCommand(runtime, jcut::SetExportFpsCommand{48.0});
    const nlohmann::json editedSnapshot = jcut::toJson(runtime.snapshot());

    QCOMPARE(runtime.canUndo(), true);
    QCOMPARE(runtime.canRedo(), false);

    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied, true);
    QCOMPARE(runtime.snapshot().exportRequest.outputFps, 30.0);
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied, true);
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(5));
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied, true);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initialSnapshot);
    QCOMPARE(runtime.canUndo(), false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied, false);

    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied, true);
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied, true);
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied, true);
    QVERIFY(jcut::toJson(runtime.snapshot()) == editedSnapshot);
    QCOMPARE(runtime.canRedo(), false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied, false);
}

void TestEditorRuntime::testCrossShellCommandScriptProducesEquivalentSnapshots()
{
    const jcut::EditorDocumentCore seed =
        jcut::EditorRuntime::createDemo().snapshot();
    std::string error;
    const std::optional<jcut::EditorDocumentCore> imguiLoaded =
        jcut::editorDocumentCoreFromJson(jcut::toJson(seed), &error);
    QVERIFY2(imguiLoaded.has_value(), error.c_str());
    error.clear();
    const nlohmann::json qtState = jcut::toLegacyStateJson(seed);
    const std::optional<jcut::EditorDocumentCore> qtLoaded =
        jcut::editorDocumentCoreFromJson(qtState, &error);
    QVERIFY2(qtLoaded.has_value(), error.c_str());

    jcut::EditorRuntime imguiRuntime =
        jcut::EditorRuntime::fromDocument(*imguiLoaded);
    jcut::EditorRuntime qtRuntime =
        jcut::EditorRuntime::fromDocument(*qtLoaded);
    const nlohmann::json imguiBaseline =
        jcut::toJson(imguiRuntime.snapshot());
    const nlohmann::json qtBaseline =
        jcut::toJson(qtRuntime.snapshot());
    const std::string baselineDifference =
        nlohmann::json::diff(imguiBaseline, qtBaseline).dump();
    QVERIFY2(
        imguiBaseline == qtBaseline,
        baselineDifference.c_str());

    const std::vector<jcut::EditorCommand> script = {
        jcut::SetProjectNameCommand{"Cross-shell edit"},
        jcut::SelectClipCommand{2},
        jcut::SetClipLabelCommand{2, "Interview B revised"},
        jcut::SetClipGradingCommand{2, 0.35, 1.2, 0.85, true},
        jcut::UpsertGradingKeyframeCommand{
            2, {72, 0.1, 1.1, 0.9, 0.75, false}},
        jcut::SetClipOpacityCommand{2, 0.8},
        jcut::SetClipTransformCommand{
            2, 24.0, -12.0, 7.5, 1.1, 0.95},
        jcut::SetTrackPropertiesCommand{
            2, "Interview alternate", 84},
        jcut::AddRenderSyncMarkerCommand{2, 96, true, 2},
        jcut::SetExportRangeCommand{24, 360},
        jcut::SetExportSizeCommand{1920, 1080},
        jcut::SetExportFpsCommand{24.0},
        jcut::MoveClipCommand{2, 2, 48},
        jcut::SplitClipCommand{2, 192},
        jcut::UndoCommand{},
        jcut::RedoCommand{},
        jcut::CreateTitleClipCommand{500, 75},
    };

    for (std::size_t index = 0; index < script.size(); ++index) {
        const jcut::CommandResult imguiResult =
            imguiRuntime.execute(script[index]);
        const jcut::CommandResult qtResult =
            qtRuntime.execute(script[index]);
        QCOMPARE(imguiResult.applied, qtResult.applied);
        QCOMPARE(
            QByteArray::fromStdString(
                jcut::toJson(imguiRuntime.snapshot()).dump()),
            QByteArray::fromStdString(
                jcut::toJson(qtRuntime.snapshot()).dump()));
    }

    const nlohmann::json savedFromImGui =
        jcut::toLegacyStateJson(imguiRuntime.snapshot(), &qtState);
    const nlohmann::json savedFromQt =
        jcut::toLegacyStateJson(qtRuntime.snapshot(), &qtState);
    QCOMPARE(
        QByteArray::fromStdString(savedFromImGui.dump()),
        QByteArray::fromStdString(savedFromQt.dump()));
}

void TestEditorRuntime::testHistoryTransactionCoalescesContinuousEdits()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const double initialOpacity = runtime.snapshot().clips.front().opacity;

    runtime.beginHistoryTransaction();
    applyCommand(runtime, jcut::SetClipOpacityCommand{1, 0.8});
    applyCommand(runtime, jcut::SetClipOpacityCommand{1, 0.5});
    applyCommand(runtime, jcut::SetClipOpacityCommand{1, 0.2});
    runtime.endHistoryTransaction();

    QCOMPARE(runtime.snapshot().clips.front().opacity, 0.2);
    QCOMPARE(runtime.canUndo(), true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().clips.front().opacity, initialOpacity);
    QCOMPARE(runtime.canUndo(), false);
    QCOMPARE(runtime.canRedo(), true);
    QCOMPARE(runtime.redoDepth(), std::size_t(1));
    applyCommand(runtime, jcut::RedoCommand{});
    QCOMPARE(runtime.snapshot().clips.front().opacity, 0.2);
}

void TestEditorRuntime::testNavigationAndPanelCommandsStayOutsideUndoHistory()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SelectTrackCommand{3});
    applyCommand(runtime, jcut::SelectClipCommand{2});
    applyCommand(runtime, jcut::SeekToFrameCommand{120});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{true});
    applyCommand(runtime, jcut::SetPreviewZoomCommand{1.75f});
    applyCommand(runtime, jcut::SetScopesVisibleCommand{true});
    QCOMPARE(runtime.canUndo(), false);

    applyCommand(runtime, jcut::SetProjectNameCommand{"Navigation History"});
    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.canRedo(), true);

    applyCommand(runtime, jcut::SelectClipCommand{3});
    applyCommand(runtime, jcut::SeekToFrameCommand{72});
    applyCommand(runtime, jcut::SetPlaybackActiveCommand{false});
    applyCommand(runtime, jcut::SetWaveformVisibleCommand{false});
    QCOMPARE(runtime.canRedo(), true);

    applyCommand(runtime, jcut::RedoCommand{});
    QCOMPARE(QString::fromStdString(runtime.snapshot().projectName),
             QStringLiteral("Navigation History"));
    QCOMPARE(runtime.snapshot().transport.currentFrame, 72);
    QCOMPARE(runtime.snapshot().transport.playbackActive, false);
    QCOMPARE(runtime.snapshot().panels.showWaveform, false);
}

void TestEditorRuntime::testMutationAfterUndoInvalidatesRedo()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SetProjectNameCommand{"First Name"});
    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.canRedo(), true);

    const jcut::CommandResult noOpName =
        runtime.execute(jcut::EditorCommand{jcut::SetProjectNameCommand{"Demo Session"}});
    QCOMPARE(noOpName.applied, true);
    QCOMPARE(runtime.canRedo(), true);

    const jcut::CommandResult failedDelete =
        runtime.execute(jcut::EditorCommand{jcut::DeleteClipCommand{999}});
    QCOMPARE(failedDelete.applied, false);
    QCOMPARE(runtime.canRedo(), true);

    applyCommand(runtime, jcut::SetExportFpsCommand{60.0});
    QCOMPARE(runtime.canRedo(), false);
    QCOMPARE(runtime.canUndo(), true);
    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().exportRequest.outputFps, 30.0);
}

void TestEditorRuntime::testTranscriptDocumentsShareGlobalHistoryWithoutProjectSerialization()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const std::string originalName = runtime.snapshot().projectName;
    const std::string path = "/tmp/jcut-transcript-history.json";
    const std::string beforePayload = R"({"segments":[{"words":[{"word":"before"}]}]})";
    const std::string afterPayload = R"({"segments":[{"words":[{"word":"after"}]}]})";

    QVERIFY(runtime.execute(jcut::SeedTranscriptHistoryDocumentCommand{
        path, beforePayload}).applied);
    QCOMPARE(runtime.undoDepth(), std::size_t{0});
    QVERIFY(runtime.execute(jcut::SetTranscriptHistoryDocumentCommand{
        path, afterPayload}).applied);
    QCOMPARE(runtime.undoDepth(), std::size_t{1});
    QVERIFY(runtime.execute(jcut::SetProjectNameCommand{"After Transcript"}).applied);
    QCOMPARE(runtime.undoDepth(), std::size_t{2});

    QVERIFY(runtime.execute(jcut::UndoCommand{}).applied);
    QCOMPARE(runtime.snapshot().projectName, originalName);
    QCOMPARE(runtime.snapshot().transcriptHistoryDocuments.front().jsonPayload,
             afterPayload);
    QVERIFY(runtime.execute(jcut::UndoCommand{}).applied);
    QCOMPARE(runtime.snapshot().transcriptHistoryDocuments.front().jsonPayload,
             beforePayload);
    QVERIFY(runtime.execute(jcut::RedoCommand{}).applied);
    QCOMPARE(runtime.snapshot().transcriptHistoryDocuments.front().jsonPayload,
             afterPayload);
    QVERIFY(runtime.execute(jcut::RedoCommand{}).applied);
    QCOMPARE(runtime.snapshot().projectName, std::string("After Transcript"));

    const nlohmann::json neutralJson = jcut::toJson(runtime.snapshot());
    const nlohmann::json legacyJson = jcut::toLegacyStateJson(runtime.snapshot());
    QVERIFY(!neutralJson.contains("transcriptHistoryDocuments"));
    QVERIFY(!neutralJson.contains("__historyTranscriptDocuments"));
    QVERIFY(!legacyJson.contains("transcriptHistoryDocuments"));
    QVERIFY(!legacyJson.contains("__historyTranscriptDocuments"));

    runtime.clearHistory();
    QVERIFY(!runtime.canUndo());
    QVERIFY(!runtime.canRedo());
}

void TestEditorRuntime::testNudgeSelectedClipCommandUpdatesTimeline()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    const jcut::CommandResult nudgeRight =
        runtime.execute(jcut::EditorCommand{jcut::NudgeSelectedClipCommand{7}});
    QCOMPARE(nudgeRight.applied, true);
    QCOMPARE(runtime.snapshot().clips.at(0).startFrame, 7);
    QCOMPARE(runtime.canUndo(), true);

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().clips.at(0).startFrame, 0);

    const jcut::CommandResult boundaryNudge =
        runtime.execute(jcut::EditorCommand{jcut::NudgeSelectedClipCommand{-1}});
    QCOMPARE(boundaryNudge.applied, false);
    QCOMPARE(runtime.canRedo(), true);

    applyCommand(runtime, jcut::SelectClipCommand{2});
    const jcut::CommandResult nudgeToStart =
        runtime.execute(jcut::EditorCommand{jcut::NudgeSelectedClipCommand{-100}});
    QCOMPARE(nudgeToStart.applied, true);
    QCOMPARE(runtime.snapshot().clips.at(1).startFrame, 0);
    QCOMPARE(runtime.canRedo(), false);

    jcut::EditorDocumentCore noSelectionDocument = runtime.snapshot();
    for (jcut::EditorClip& clip : noSelectionDocument.clips) {
        clip.selected = false;
    }
    jcut::EditorRuntime noSelectionRuntime =
        jcut::EditorRuntime::fromDocument(std::move(noSelectionDocument));
    const jcut::CommandResult noSelection =
        noSelectionRuntime.execute(jcut::EditorCommand{jcut::NudgeSelectedClipCommand{1}});
    QCOMPARE(noSelection.applied, false);
    QCOMPARE(noSelectionRuntime.canUndo(), false);

    jcut::EditorDocumentCore groupDocument;
    groupDocument.tracks = {{1, "Video", true}};
    jcut::EditorClip groupA;
    groupA.id = 1;
    groupA.trackId = 1;
    groupA.startFrame = 5;
    groupA.durationFrames = 10;
    groupA.selected = true;
    groupA.persistentId = "group-a";
    jcut::EditorClip groupB;
    groupB.id = 2;
    groupB.trackId = 1;
    groupB.startFrame = 1;
    groupB.durationFrames = 10;
    groupB.selected = true;
    groupB.persistentId = "group-b";
    jcut::EditorClip unselected;
    unselected.id = 3;
    unselected.trackId = 1;
    unselected.startFrame = 30;
    unselected.durationFrames = 10;
    unselected.persistentId = "unselected";
    groupDocument.clips = {groupA, groupB, unselected};
    groupDocument.renderSyncMarkers = {
        {"group-a", 6, false, 1},
        {"group-b", 2, false, 1},
        {"unselected", 31, false, 1},
    };

    jcut::EditorRuntime groupRuntime =
        jcut::EditorRuntime::fromDocument(groupDocument);
    const nlohmann::json originalGroup =
        jcut::toJson(groupRuntime.snapshot());
    const jcut::CommandResult groupNudge = groupRuntime.execute(
        jcut::EditorCommand{jcut::NudgeSelectedClipCommand{-50}});
    QCOMPARE(groupNudge.applied, true);
    const jcut::EditorDocumentCore nudgedGroup = groupRuntime.snapshot();
    QCOMPARE(nudgedGroup.clips.at(0).startFrame, 4);
    QCOMPARE(nudgedGroup.clips.at(1).startFrame, 0);
    QCOMPARE(nudgedGroup.clips.at(2).startFrame, 30);
    QVERIFY(std::any_of(
        nudgedGroup.renderSyncMarkers.begin(),
        nudgedGroup.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "group-a" && marker.frame == 5;
        }));
    QVERIFY(std::any_of(
        nudgedGroup.renderSyncMarkers.begin(),
        nudgedGroup.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "group-b" && marker.frame == 1;
        }));
    QVERIFY(std::any_of(
        nudgedGroup.renderSyncMarkers.begin(),
        nudgedGroup.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "unselected" && marker.frame == 31;
        }));
    QCOMPARE(groupRuntime.undoDepth(), std::size_t(1));
    applyCommand(groupRuntime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(groupRuntime.snapshot()) == originalGroup);

    groupDocument.clips.at(1).locked = true;
    jcut::EditorRuntime lockedGroupRuntime =
        jcut::EditorRuntime::fromDocument(std::move(groupDocument));
    const nlohmann::json beforeLockedNudge =
        jcut::toJson(lockedGroupRuntime.snapshot());
    const jcut::CommandResult lockedNudge = lockedGroupRuntime.execute(
        jcut::EditorCommand{jcut::NudgeSelectedClipCommand{1}});
    QCOMPARE(lockedNudge.applied, false);
    QVERIFY(jcut::toJson(lockedGroupRuntime.snapshot()) ==
            beforeLockedNudge);
    QCOMPARE(lockedGroupRuntime.canUndo(), false);
}

void TestEditorRuntime::testClipLockAndPlaybackRateCommandsMatchTimelinePolicies()
{
    jcut::EditorDocumentCore document;
    document.tracks = {
        {1, "Video", true},
        {2, "Overlay", false},
    };

    jcut::EditorClip target;
    target.id = 1;
    target.trackId = 1;
    target.label = "Target";
    target.startFrame = 10;
    target.durationFrames = 120;
    target.selected = true;
    target.persistentId = "target";

    jcut::EditorClip later;
    later.id = 2;
    later.trackId = 1;
    later.label = "Later";
    later.startFrame = 150;
    later.durationFrames = 20;
    later.persistentId = "later";

    jcut::EditorClip otherTrack;
    otherTrack.id = 3;
    otherTrack.trackId = 2;
    otherTrack.label = "Other Track";
    otherTrack.startFrame = 150;
    otherTrack.durationFrames = 20;
    otherTrack.persistentId = "other-track";

    document.clips = {target, later, otherTrack};
    document.renderSyncMarkers = {
        {"target", 50, false, 1},
        {"later", 155, false, 1},
        {"other-track", 155, false, 1},
    };

    jcut::EditorRuntime lockRuntime =
        jcut::EditorRuntime::fromDocument(document);
    const nlohmann::json beforeLock = jcut::toJson(lockRuntime.snapshot());
    const jcut::CommandResult locked = lockRuntime.execute(
        jcut::EditorCommand{jcut::SetClipLockedCommand{1, true}});
    QCOMPARE(locked.applied, true);
    QCOMPARE(lockRuntime.snapshot().clips.front().locked, true);
    QCOMPARE(lockRuntime.undoDepth(), std::size_t(1));

    const nlohmann::json lockedSnapshot = jcut::toJson(lockRuntime.snapshot());
    const std::array<jcut::CommandResult, 6> rejected = {
        lockRuntime.execute(jcut::EditorCommand{jcut::SplitClipCommand{1, 50}}),
        lockRuntime.execute(jcut::EditorCommand{jcut::TrimClipStartCommand{1, 20}}),
        lockRuntime.execute(jcut::EditorCommand{jcut::TrimClipEndCommand{1, 100}}),
        lockRuntime.execute(jcut::EditorCommand{jcut::MoveClipCommand{1, 2, 20}}),
        lockRuntime.execute(jcut::EditorCommand{jcut::ResizeClipCommand{1, 60}}),
        lockRuntime.execute(jcut::EditorCommand{jcut::DeleteClipCommand{1}}),
    };
    for (const jcut::CommandResult& result : rejected) {
        QCOMPARE(result.applied, false);
    }
    const jcut::CommandResult lockedRate = lockRuntime.execute(
        jcut::EditorCommand{jcut::SetClipPlaybackRateCommand{1, 2.0}});
    QCOMPARE(lockedRate.applied, false);
    QVERIFY(jcut::toJson(lockRuntime.snapshot()) == lockedSnapshot);
    QCOMPARE(lockRuntime.undoDepth(), std::size_t(1));

    applyCommand(lockRuntime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(lockRuntime.snapshot()) == beforeLock);
    applyCommand(lockRuntime, jcut::RedoCommand{});
    QCOMPARE(lockRuntime.snapshot().clips.front().locked, true);
    const jcut::CommandResult unlocked = lockRuntime.execute(
        jcut::EditorCommand{jcut::SetClipLockedCommand{1, false}});
    QCOMPARE(unlocked.applied, true);
    QCOMPARE(lockRuntime.snapshot().clips.front().locked, false);

    jcut::EditorRuntime rateRuntime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json beforeRate = jcut::toJson(rateRuntime.snapshot());
    const jcut::CommandResult invalidRate = rateRuntime.execute(
        jcut::EditorCommand{jcut::SetClipPlaybackRateCommand{
            1, std::numeric_limits<double>::quiet_NaN()}});
    QCOMPARE(invalidRate.applied, false);
    QCOMPARE(rateRuntime.canUndo(), false);

    const jcut::CommandResult rateChanged = rateRuntime.execute(
        jcut::EditorCommand{jcut::SetClipPlaybackRateCommand{1, 2.0}});
    QCOMPARE(rateChanged.applied, true);
    const jcut::EditorDocumentCore changed = rateRuntime.snapshot();
    QCOMPARE(changed.clips.at(0).playbackRate, 2.0);
    QCOMPARE(changed.clips.at(0).durationFrames, 60);
    QCOMPARE(changed.clips.at(1).startFrame, 90);
    QCOMPARE(changed.clips.at(2).startFrame, 150);
    QCOMPARE(changed.renderSyncMarkers.at(0).frame, std::int64_t(50));
    QCOMPARE(changed.renderSyncMarkers.at(1).frame, std::int64_t(95));
    QCOMPARE(changed.renderSyncMarkers.at(2).frame, std::int64_t(155));
    QCOMPARE(rateRuntime.undoDepth(), std::size_t(1));

    const jcut::CommandResult unchangedRate = rateRuntime.execute(
        jcut::EditorCommand{jcut::SetClipPlaybackRateCommand{1, 2.0}});
    QCOMPARE(unchangedRate.applied, false);
    QCOMPARE(rateRuntime.undoDepth(), std::size_t(1));

    applyCommand(rateRuntime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(rateRuntime.snapshot()) == beforeRate);
    applyCommand(rateRuntime, jcut::RedoCommand{});
    QCOMPARE(rateRuntime.snapshot().clips.at(0).playbackRate, 2.0);
    QCOMPARE(rateRuntime.snapshot().clips.at(1).startFrame, 90);
}

void TestEditorRuntime::testAiLoginPersistsRefreshTokenContract()
{
    QFile source(QStringLiteral(JCUT_SOURCE_DIR "/editor_ai_integration.cpp"));
    QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(source.fileName()));
    const QString code = QString::fromUtf8(source.readAll());

    QVERIFY(code.contains(QStringLiteral("loginResult.value().refreshToken.trimmed()")));
    QVERIFY(code.contains(QStringLiteral("jcut::ai::loadStoredCredentialsCore(config)")));
    QVERIFY(code.contains(QStringLiteral("jcut::ai::storeCredentialsCore(")));
    QVERIFY(!code.contains(QStringLiteral("cppmonetize::loadStoredAuthSession(*store)")));
    QVERIFY(!code.contains(QStringLiteral("cppmonetize::storeStoredAuthSession")));
    QVERIFY(code.contains(QStringLiteral("OAuthDesktopFlow oauthFlow")));
    QVERIFY(code.contains(QStringLiteral("oauthFlow.refreshWithToken")));
    QVERIFY(code.contains(QStringLiteral("refreshAiAuthTokenFromSecureStore()")));
    QVERIFY(!code.contains(QStringLiteral("Q_UNUSED(refreshToken);")));
    QVERIFY(!code.contains(QStringLiteral("Q_UNUSED(this);\n            return false;")));
}

void TestEditorRuntime::testInspectorCommandsUpdateSharedClipState()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SetClipGradingCommand{1, 0.25, 1.4, 0.8, false});
    applyCommand(runtime, jcut::UpsertGradingKeyframeCommand{
        1, {30, 0.25, 1.4, 0.8, 0.9, true}});
    applyCommand(runtime, jcut::SetClipOpacityCommand{1, 0.72});
    applyCommand(runtime, jcut::UpsertOpacityKeyframeCommand{1, {30, 0.72, true}});
    applyCommand(runtime, jcut::SetClipTransformCommand{1, 42.0, -18.0, 12.0, -1.25, 1.1});
    applyCommand(runtime, jcut::UpsertTransformKeyframeCommand{
        1, {30, "Push", 42.0, -18.0, 12.0, -1.25, 1.1, true}});
    applyCommand(runtime, jcut::SetClipSpeakerFramingCommand{
        1,
        true,
        0.45,
        0.40,
        0.22,
        0.31,
        7,
        " T7 ",
        9,
        13,
        2,
        0.75,
        1.25,
        17});
    applyCommand(
        runtime,
        jcut::SetClipSpeakerSectionMinimumWordsCommand{
            1, 24});
    applyCommand(
        runtime,
        jcut::UpsertSpeakerFramingEnabledKeyframeCommand{
            1, {30, true}});
    applyCommand(
        runtime,
        jcut::UpsertSpeakerFramingKeyframeCommand{
            1,
            {30, "Baked", -20.0, 15.0, 3.0, 1.2, 1.1, true}});
    applyCommand(
        runtime,
        jcut::UpsertSpeakerFramingTargetKeyframeCommand{
            1,
            {30, "Target", 0.55, 0.42, 12.0, 0.28, 0.9, true}});
    applyCommand(runtime, jcut::SetClipMaskEffectCommand{
        1, true, 18.0, 1.4, 3, true, true, 120.0, 24.0,
        "kaleidoscope", 48, 1.5, 0.8, false});

    jcut::EditorTranscriptOverlayState overlay;
    overlay.enabled = true;
    overlay.maxLines = 3;
    overlay.maxCharsPerLine = 36;
    overlay.boxWidth = 720.0;
    overlay.boxHeight = 180.0;
    applyCommand(runtime, jcut::SetClipTranscriptOverlayCommand{1, overlay});

    jcut::EditorTitleKeyframe title;
    title.frame = 30;
    title.text = "Lower third";
    title.translationX = 64.0;
    title.translationY = 96.0;
    title.fontSize = 54.0;
    applyCommand(runtime, jcut::UpsertTitleKeyframeCommand{1, title});
    applyCommand(runtime, jcut::SetClipAudioCommand{1, true, 1.5, -0.25, true});
    applyCommand(runtime, jcut::SetTrackStateCommand{
        1, 2, false, 0.75, true, false, false});
    applyCommand(runtime, jcut::AddRenderSyncMarkerCommand{1, 45, true, 2});
    applyCommand(runtime, jcut::SetExportRangeCommand{15, 180});

    const jcut::EditorDocumentCore snapshot = runtime.snapshot();
    const jcut::EditorClip& clip = snapshot.clips.front();
    QCOMPARE(clip.brightness, 0.25);
    QCOMPARE(clip.contrast, 1.4);
    QCOMPARE(clip.saturation, 0.8);
    QCOMPARE(clip.gradingPreviewEnabled, false);
    QCOMPARE(clip.gradingKeyframes.size(), std::size_t(1));
    QCOMPARE(clip.opacity, 0.72);
    QCOMPARE(clip.speakerFramingEnabled, true);
    QCOMPARE(clip.speakerFramingBakedTargetXNorm, 0.45);
    QCOMPARE(clip.speakerFramingBakedTargetYNorm, 0.40);
    QCOMPARE(clip.speakerFramingBakedTargetBoxNorm, 0.22);
    QCOMPARE(clip.speakerFramingMinConfidence, 0.31);
    QCOMPARE(clip.speakerFramingManualTrackId, 7);
    QCOMPARE(QString::fromStdString(clip.speakerFramingManualStreamId),
             QStringLiteral("T7"));
    QCOMPARE(clip.speakerFramingCenterSmoothingFrames, 9);
    QCOMPARE(clip.speakerFramingZoomSmoothingFrames, 13);
    QCOMPARE(clip.speakerFramingSmoothingMode, 2);
    QCOMPARE(clip.speakerFramingCenterSmoothingStrength, 0.75);
    QCOMPARE(clip.speakerFramingZoomSmoothingStrength, 1.25);
    QCOMPARE(clip.speakerFramingGapHoldFrames, 17);
    QCOMPARE(clip.speakerSectionMinimumWords, 24);
    QCOMPARE(clip.speakerFramingEnabledKeyframes.size(), std::size_t(1));
    QCOMPARE(clip.speakerFramingKeyframes.size(), std::size_t(1));
    QCOMPARE(clip.speakerFramingTargetKeyframes.size(), std::size_t(1));
    QCOMPARE(clip.speakerFramingTargetKeyframes.front().rotation, 0.0);
    QCOMPARE(clip.speakerFramingTargetKeyframes.front().scaleX, 0.28);
    QCOMPARE(clip.speakerFramingTargetKeyframes.front().scaleY, 0.28);
    QCOMPARE(clip.opacityKeyframes.size(), std::size_t(1));
    QCOMPARE(clip.baseTranslationX, 42.0);
    QCOMPARE(clip.baseScaleX, -1.25);
    QCOMPARE(clip.transformKeyframes.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(clip.effectPreset), QStringLiteral("kaleidoscope"));
    QCOMPARE(clip.maskEnabled, true);
    QCOMPARE(clip.transcriptOverlay.enabled, true);
    QCOMPARE(clip.transcriptOverlay.maxLines, 3);
    QCOMPARE(clip.titleKeyframes.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(clip.titleKeyframes.front().text), QStringLiteral("Lower third"));
    QCOMPARE(clip.audioGain, 1.5);
    QCOMPARE(clip.audioPan, -0.25);
    QCOMPARE(snapshot.tracks.front().visualMode, 2);
    QCOMPARE(snapshot.tracks.front().audioEnabled, false);
    QCOMPARE(snapshot.tracks.front().audioGain, 0.75);
    QCOMPARE(snapshot.tracks.front().audioMuted, true);
    QCOMPARE(snapshot.tracks.front().audioSolo, false);
    QCOMPARE(snapshot.tracks.front().gradingPreviewEnabled, false);
    QCOMPARE(snapshot.renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(snapshot.renderSyncMarkers.front().skipFrame, true);
    QCOMPARE(snapshot.renderSyncMarkers.front().count, 2);
    QCOMPARE(snapshot.exportRanges.size(), std::size_t(1));
    QCOMPARE(snapshot.exportRanges.front().startFrame, std::int64_t(15));
    QCOMPARE(snapshot.exportRanges.front().endFrame, std::int64_t(180));

    const jcut::EditorTrack hiddenTrack = snapshot.tracks.front();
    applyCommand(runtime, jcut::SetTrackStateCommand{
        hiddenTrack.id, 1, hiddenTrack.audioEnabled, hiddenTrack.audioGain,
        hiddenTrack.audioMuted, hiddenTrack.audioSolo, true});
    const jcut::EditorTrack forceOpaqueTrack = runtime.snapshot().tracks.front();
    QCOMPARE(forceOpaqueTrack.visualMode, 1);
    QCOMPARE(forceOpaqueTrack.audioEnabled, hiddenTrack.audioEnabled);
    QCOMPARE(forceOpaqueTrack.audioGain, hiddenTrack.audioGain);
    QCOMPARE(forceOpaqueTrack.audioMuted, hiddenTrack.audioMuted);
    QCOMPARE(forceOpaqueTrack.audioSolo, hiddenTrack.audioSolo);
    QCOMPARE(forceOpaqueTrack.gradingPreviewEnabled, true);

    applyCommand(runtime, jcut::SetTrackStateCommand{
        forceOpaqueTrack.id, 0, forceOpaqueTrack.audioEnabled,
        forceOpaqueTrack.audioGain, forceOpaqueTrack.audioMuted,
        forceOpaqueTrack.audioSolo, false});
    const jcut::EditorTrack enabledTrack = runtime.snapshot().tracks.front();
    QCOMPARE(enabledTrack.visualMode, 0);
    QCOMPARE(enabledTrack.audioEnabled, forceOpaqueTrack.audioEnabled);
    QCOMPARE(enabledTrack.audioGain, forceOpaqueTrack.audioGain);
    QCOMPARE(enabledTrack.audioMuted, forceOpaqueTrack.audioMuted);
    QCOMPARE(enabledTrack.audioSolo, forceOpaqueTrack.audioSolo);
    QCOMPARE(enabledTrack.gradingPreviewEnabled, false);

    applyCommand(runtime, jcut::RefreshClipMetadataCommand{{
        {1, "video", false, 60.0, 600, 200}}});
    const jcut::EditorClip refreshedClip =
        runtime.snapshot().clips.front();
    QCOMPARE(QString::fromStdString(refreshedClip.mediaKind),
             QStringLiteral("video"));
    QCOMPARE(refreshedClip.hasAudio, false);
    QCOMPARE(refreshedClip.sourceFps, 60.0);
    QCOMPARE(refreshedClip.sourceDurationFrames, std::int64_t(600));
    QCOMPARE(refreshedClip.durationFrames, 200);

    applyCommand(runtime, jcut::SetClipGradingCommand{
        1, 8.0, -7.5, 9.0, true});
    applyCommand(runtime, jcut::UpsertGradingKeyframeCommand{
        1, {31, -8.0, 7.5, -9.0, 1.0, true}});
    const jcut::EditorDocumentCore wideGradeSnapshot =
        runtime.snapshot();
    const jcut::EditorClip& wideGradeClip =
        wideGradeSnapshot.clips.front();
    QCOMPARE(wideGradeClip.brightness, 8.0);
    QCOMPARE(wideGradeClip.contrast, -7.5);
    QCOMPARE(wideGradeClip.saturation, 9.0);
    const auto wideGradeKey = std::find_if(
        wideGradeClip.gradingKeyframes.begin(),
        wideGradeClip.gradingKeyframes.end(),
        [](const jcut::EditorGradingKeyframe& keyframe) {
            return keyframe.frame == 31;
        });
    QVERIFY(wideGradeKey != wideGradeClip.gradingKeyframes.end());
    QCOMPARE(wideGradeKey->brightness, -8.0);
    QCOMPARE(wideGradeKey->contrast, 7.5);
    QCOMPARE(wideGradeKey->saturation, -9.0);
    QCOMPARE(runtime.canUndo(), true);
}

void TestEditorRuntime::testCorrectionPolygonCommandNormalizesAndIsUndoable()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    jcut::EditorCorrectionPolygon polygon;
    polygon.enabled = true;
    polygon.startFrame = -12;
    polygon.endFrame = -2;
    polygon.pointsNormalized = {{-0.2, 0.25}, {0.5, 1.4}, {1.2, -0.1}};

    const jcut::CommandResult result = runtime.execute(
        jcut::EditorCommand{jcut::SetClipCorrectionPolygonsCommand{1, {polygon}}});
    QVERIFY(result.applied);
    const jcut::EditorCorrectionPolygon stored =
        runtime.snapshot().clips.front().correctionPolygons.front();
    QCOMPARE(stored.startFrame, std::int64_t(0));
    QCOMPARE(stored.endFrame, std::int64_t(-2));
    QCOMPARE(stored.pointsNormalized[0].x, 0.0);
    QCOMPARE(stored.pointsNormalized[1].y, 1.0);
    QCOMPARE(stored.pointsNormalized[2].x, 1.0);
    QCOMPARE(stored.pointsNormalized[2].y, 0.0);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(runtime.snapshot().clips.front().correctionPolygons.empty());
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.front().correctionPolygons.size(), std::size_t(1));
}

void TestEditorRuntime::testPreviewTransformCommandMatchesQtKeyframeSemantics()
{
    QCOMPARE(
        jcut::preview::rotationDeltaDegrees(
            {0.0, 0.0}, {0.0, -10.0}, {10.0, 0.0}),
        90.0);
    QVERIFY(std::abs(
        jcut::preview::rotationDeltaDegrees(
            {0.0, 0.0}, {-10.0, -0.1}, {-10.0, 0.1}) +
        1.1459) < 0.001);
    QCOMPARE(
        jcut::preview::rotationForPointerDrag(
            7.0,
            {0.0, 0.0},
            {0.0, -10.0},
            {10.0, 0.0},
            15.0),
        90.0);

    const jcut::preview::PointD anchored =
        jcut::preview::translationForAnchoredResize(
            {10.0, 20.0}, {1.0, 1.0}, {2.0, 1.0},
            {0.0, 0.0, 100.0, 80.0},
            jcut::preview::ResizeAnchor::Left,
            {0.5, 0.5});
    QCOMPARE(anchored.x, 110.0);
    QCOMPARE(anchored.y, 20.0);

    const auto translatedQuad =
        jcut::preview::transformedPresentationQuad(
            {0.0, 0.0, 100.0, 50.0},
            {100.0, 50.0},
            {10.0, 5.0},
            {1.0, 1.0},
            0.0);
    QCOMPARE(translatedQuad[0].x, 10.0);
    QCOMPARE(translatedQuad[0].y, 5.0);
    QCOMPARE(translatedQuad[2].x, 110.0);
    QCOMPARE(translatedQuad[2].y, 55.0);
    const auto rotatedQuad =
        jcut::preview::transformedPresentationQuad(
            {0.0, 0.0, 100.0, 50.0},
            {100.0, 50.0},
            {},
            {1.0, 1.0},
            90.0);
    QVERIFY(std::abs(rotatedQuad[0].x - 75.0) < 1.0e-9);
    QVERIFY(std::abs(rotatedQuad[0].y + 25.0) < 1.0e-9);
    QVERIFY(std::abs(rotatedQuad[2].x - 25.0) < 1.0e-9);
    QVERIFY(std::abs(rotatedQuad[2].y - 75.0) < 1.0e-9);
    const auto fittedRect =
        jcut::preview::fittedPresentationRect(
            {0.0, 0.0, 200.0, 100.0},
            {100.0, 50.0},
            {100.0, 100.0});
    QCOMPARE(fittedRect.left, 50.0);
    QCOMPARE(fittedRect.top, 0.0);
    QCOMPARE(fittedRect.width, 100.0);
    QCOMPARE(fittedRect.height, 100.0);
    const auto fittedTranslatedQuad =
        jcut::preview::transformedPresentationQuad(
            fittedRect,
            {0.0, 0.0, 200.0, 100.0},
            {100.0, 50.0},
            {10.0, 0.0},
            {1.0, 1.0},
            0.0);
    QCOMPARE(fittedTranslatedQuad[0].x, 70.0);
    QCOMPARE(fittedTranslatedQuad[2].x, 170.0);

    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    clip.baseTranslationX = 12.0;
    clip.baseTranslationY = -8.0;
    clip.baseRotation = 5.0;
    clip.baseScaleX = 2.0;
    clip.baseScaleY = 0.5;
    clip.transformKeyframes = {{40, "later", 4.0, 6.0, 3.0, 1.2, 0.8, false}};
    const int clipId = clip.id;
    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(std::move(document));

    const jcut::CommandResult result = runtime.execute(jcut::EditorCommand{
        jcut::CommitPreviewTransformCommand{
            clipId, 20, 112.0, 42.0, 25.0, 3.0, -1.0}});
    QVERIFY(result.applied);
    const jcut::EditorClip storedClip = runtime.snapshot().clips.front();
    QCOMPARE(storedClip.transformKeyframes.size(), std::size_t(3));
    QCOMPARE(storedClip.transformKeyframes[0].frame, std::int64_t(0));
    QCOMPARE(storedClip.transformKeyframes[1].frame, std::int64_t(20));
    QCOMPARE(storedClip.transformKeyframes[1].translationX, 100.0);
    QCOMPARE(storedClip.transformKeyframes[1].translationY, 50.0);
    QCOMPARE(storedClip.transformKeyframes[1].rotation, 20.0);
    QCOMPARE(storedClip.transformKeyframes[1].scaleX, 1.5);
    QCOMPARE(storedClip.transformKeyframes[1].scaleY, -2.0);
    QCOMPARE(storedClip.transformKeyframes[1].linearInterpolation, false);
    const jcut::EditorTransformKeyframe evaluated =
        jcut::evaluateEditorClipTransformAtLocalFrame(storedClip, 20);
    QCOMPARE(evaluated.translationX, 112.0);
    QCOMPARE(evaluated.translationY, 42.0);
    QCOMPARE(evaluated.rotation, 25.0);
    QCOMPARE(evaluated.scaleX, 3.0);
    QCOMPARE(evaluated.scaleY, -1.0);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.front().transformKeyframes.size(), std::size_t(1));

    jcut::EditorDocumentCore ownedDocument =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& owner = ownedDocument.clips.front();
    owner.persistentId = "preview-owner";
    owner.clipRole = "media";
    owner.mediaKind = "video";
    jcut::EditorClip child = owner;
    child.id = 901;
    child.persistentId = "preview-child";
    child.clipRole = "mask_matte";
    child.linkedSourceClipId = owner.persistentId;
    ownedDocument.clips.push_back(child);
    jcut::EditorRuntime ownedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(ownedDocument));
    QVERIFY(ownedRuntime.execute(jcut::EditorCommand{
        jcut::CommitPreviewTransformCommand{
            child.id, 10, 55.0, -35.0, 0.0, 1.25, 0.75}}).applied);
    const jcut::EditorDocumentCore ownedSnapshot = ownedRuntime.snapshot();
    const auto findPersistent = [&](const std::string& id) -> const jcut::EditorClip& {
        return *std::find_if(
            ownedSnapshot.clips.begin(), ownedSnapshot.clips.end(),
            [&](const jcut::EditorClip& candidate) {
                return candidate.persistentId == id;
            });
    };
    const jcut::EditorClip& storedOwner = findPersistent("preview-owner");
    const jcut::EditorClip& storedChild = findPersistent("preview-child");
    const jcut::EditorTransformKeyframe ownerTransform =
        jcut::evaluateEditorClipTransformAtLocalFrame(storedOwner, 10);
    const jcut::EditorTransformKeyframe childTransform =
        jcut::evaluateEditorClipTransformAtLocalFrame(storedChild, 10);
    QCOMPARE(ownerTransform.translationX, 55.0);
    QCOMPARE(ownerTransform.translationY, -35.0);
    QCOMPARE(childTransform.translationX, ownerTransform.translationX);
    QCOMPARE(childTransform.translationY, ownerTransform.translationY);
    QCOMPARE(childTransform.scaleX, ownerTransform.scaleX);
    QCOMPARE(childTransform.scaleY, ownerTransform.scaleY);
}

void TestEditorRuntime::testSourceTransformLockMatchesQtChainSemantics()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    document.clips.resize(1);
    jcut::EditorClip& source = document.clips.front();
    source.id = 101;
    source.persistentId = "transform-source";
    source.clipRole = "media";
    source.mediaKind = "video";
    source.startFrame = 10;
    source.durationFrames = 120;
    source.baseTranslationX = 35.0;
    source.baseTranslationY = -12.0;
    source.baseScaleX = 1.5;
    source.baseScaleY = 0.75;
    source.transformKeyframes.clear();

    jcut::EditorClip middle = source;
    middle.id = 102;
    middle.persistentId = "transform-middle";
    middle.linkedSourceClipId = source.persistentId;
    middle.sourceTransformLocked = true;
    middle.baseTranslationX = 90.0;

    jcut::EditorClip follower = source;
    follower.id = 103;
    follower.persistentId = "transform-follower";
    follower.linkedSourceClipId = middle.persistentId;
    follower.sourceTransformLocked = true;
    follower.startFrame = 20;
    follower.baseTranslationX = -50.0;
    follower.baseScaleX = 0.5;
    document.clips.push_back(middle);
    document.clips.push_back(follower);

    const jcut::EditorTransformKeyframe inherited =
        jcut::evaluateEditorClipRenderTransformAtTimelineFrame(
            document, document.clips.back(), 40);
    QCOMPARE(inherited.translationX, 35.0);
    QCOMPARE(inherited.translationY, -12.0);
    QCOMPARE(inherited.scaleX, 1.5);
    QCOMPARE(inherited.scaleY, 0.75);
    QCOMPARE(inherited.frame, std::int64_t(20));

    document.clips[1].linkedSourceClipId =
        document.clips[2].persistentId;
    const jcut::EditorTransformKeyframe cycleFallback =
        jcut::evaluateEditorClipRenderTransformAtTimelineFrame(
            document, document.clips[2], 40);
    QCOMPARE(cycleFallback.translationX, -50.0);
    QCOMPARE(cycleFallback.scaleX, 0.5);

    document.clips[1].linkedSourceClipId =
        document.clips[0].persistentId;
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(document);
    QVERIFY(runtime.execute(
        jcut::SetClipSourceTransformLockedCommand{
            follower.id, false}).applied);
    QCOMPARE(
        runtime.snapshot().clips[2].sourceTransformLocked,
        false);
    QVERIFY(runtime.execute(
        jcut::SetClipSourceTransformLockedCommand{
            follower.id, true}).applied);
    QCOMPARE(
        runtime.snapshot().clips[2].sourceTransformLocked,
        true);
    QVERIFY(runtime.execute(jcut::UndoCommand{}).applied);
    QCOMPARE(
        runtime.snapshot().clips[2].sourceTransformLocked,
        false);
}

void TestEditorRuntime::testMaterializeMaskMatteMatchesQtOwnershipDefaults()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& source = document.clips.front();
    source.persistentId = "mask-source";
    source.clipRole = "media";
    source.mediaKind = "video";
    source.selected = true;
    source.maskGradeEnabled = true;
    source.maskGradeBrightness = 0.2;
    source.maskGradeContrast = 1.3;
    source.maskGradeSaturation = 0.7;
    const int sourceId = source.id;
    const std::size_t initialClipCount = document.clips.size();
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));

    const jcut::CommandResult result = runtime.execute(jcut::EditorCommand{
        jcut::MaterializeMaskMatteCommand{
            sourceId, "/tmp/mask-source_alpha_masks", "a1b2c3d4", "Person"}});
    QVERIFY(result.applied);
    const jcut::EditorDocumentCore snapshot = runtime.snapshot();
    QCOMPARE(snapshot.clips.size(), initialClipCount + 1);
    const auto childIt = std::find_if(
        snapshot.clips.begin(), snapshot.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.clipRole == "mask_matte" &&
                clip.generatedFromMaskId == "a1b2c3d4";
        });
    QVERIFY(childIt != snapshot.clips.end());
    const jcut::EditorClip& child = *childIt;
    QCOMPARE(QString::fromStdString(child.linkedSourceClipId), QStringLiteral("mask-source"));
    QCOMPARE(QString::fromStdString(child.generatedFromMaskId), QStringLiteral("a1b2c3d4"));
    QCOMPARE(QString::fromStdString(child.maskFramesDir),
             QStringLiteral("/tmp/mask-source_alpha_masks"));
    QCOMPARE(child.syncLockedToSource, true);
    QCOMPARE(child.sourceTransformLocked, true);
    QCOMPARE(child.maskEnabled, true);
    QCOMPARE(child.hasAudio, false);
    QCOMPARE(child.audioEnabled, false);
    QCOMPARE(child.locked, true);
    QCOMPARE(child.selected, true);
    QCOMPARE(child.brightness, 0.2);
    QCOMPARE(child.contrast, 1.3);
    QCOMPARE(child.saturation, 0.7);
    const auto childTrack = std::find_if(
        snapshot.tracks.begin(), snapshot.tracks.end(),
        [&](const jcut::EditorTrack& track) { return track.id == child.trackId; });
    QVERIFY(childTrack != snapshot.tracks.end());
    QCOMPARE(childTrack->generatedChildTrack, true);
    QCOMPARE(QString::fromStdString(childTrack->parentClipId), QStringLiteral("mask-source"));
    QCOMPARE(QString::fromStdString(childTrack->childClipId),
             QString::fromStdString(child.persistentId));

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::SetClipZLevelCommand{child.id, 42, false}}).applied);
    // Own the snapshot before dereferencing; runtime.snapshot() returns by value.
    const jcut::EditorDocumentCore zSnapshot = runtime.snapshot();
    const auto stableZChild = std::find_if(
        zSnapshot.clips.begin(), zSnapshot.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.generatedFromMaskId == "a1b2c3d4";
        });
    QVERIFY(stableZChild != zSnapshot.clips.end());
    QCOMPARE(stableZChild->zLevel, 42);
    QCOMPARE(stableZChild->zLevelUserSet, true);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);

    const nlohmann::json serialized = jcut::toJson(snapshot);
    const auto reparsed = jcut::editorDocumentCoreFromJson(serialized);
    QVERIFY(reparsed.has_value());
    const auto reparsedChild = std::find_if(
        reparsed->clips.begin(), reparsed->clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.clipRole == "mask_matte" &&
                clip.generatedFromMaskId == "a1b2c3d4";
        });
    QVERIFY(reparsedChild != reparsed->clips.end());
    QCOMPARE(QString::fromStdString(reparsedChild->generatedFromMaskId),
             QStringLiteral("a1b2c3d4"));
    QCOMPARE(reparsedChild->syncLockedToSource, true);
    QCOMPARE(reparsedChild->sourceTransformLocked, true);
    QCOMPARE(reparsedChild->zLevel, child.zLevel);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.size(), initialClipCount);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.size(), initialClipCount + 1);
}

void TestEditorRuntime::testResetClipGradingMatchesQtContextSemantics()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    clip.mediaKind = "video";
    clip.videoEnabled = false;
    clip.locked = true;
    clip.brightness = -0.4;
    clip.contrast = 2.25;
    clip.saturation = 1.75;
    clip.opacity = 0.35;
    clip.gradingPreviewEnabled = false;
    clip.gradingKeyframes = {
        {0, -0.4, 2.25, 1.75, 0.35, false},
        {60, 0.3, 0.75, 0.5, 0.9, true},
    };
    clip.opacityKeyframes = {
        {0, 0.35, false},
        {90, 0.8, true},
    };
    clip.baseTranslationX = 48.0;
    clip.baseScaleY = 1.4;
    clip.transformKeyframes = {
        {15, "Preserved", 48.0, -12.0, 8.0, 1.2, 1.4, false},
    };
    clip.maskEnabled = true;
    clip.effectPreset = "kaleidoscope";
    jcut::EditorTitleKeyframe titleKeyframe;
    titleKeyframe.frame = 20;
    titleKeyframe.text = "Preserved title";
    clip.titleKeyframes = {titleKeyframe};
    const int clipId = clip.id;

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json initial = jcut::toJson(runtime.snapshot());
    const jcut::CommandResult missing = runtime.execute(
        jcut::EditorCommand{jcut::ResetClipGradingCommand{999}});
    QCOMPARE(missing.applied, false);
    QCOMPARE(runtime.canUndo(), false);

    const jcut::CommandResult reset = runtime.execute(
        jcut::EditorCommand{jcut::ResetClipGradingCommand{clipId}});
    QCOMPARE(reset.applied, true);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    const jcut::EditorDocumentCore neutralSnapshot = runtime.snapshot();
    const jcut::EditorClip& neutral = neutralSnapshot.clips.front();
    QCOMPARE(neutral.brightness, 0.0);
    QCOMPARE(neutral.contrast, 1.0);
    QCOMPARE(neutral.saturation, 1.0);
    QCOMPARE(neutral.opacity, 1.0);
    QCOMPARE(neutral.gradingPreviewEnabled, false);
    QCOMPARE(neutral.gradingKeyframes.size(), std::size_t(1));
    QCOMPARE(neutral.gradingKeyframes.front().frame, std::int64_t(0));
    QCOMPARE(neutral.gradingKeyframes.front().brightness, 0.0);
    QCOMPARE(neutral.gradingKeyframes.front().contrast, 1.0);
    QCOMPARE(neutral.gradingKeyframes.front().saturation, 1.0);
    QCOMPARE(neutral.gradingKeyframes.front().opacity, 1.0);
    QCOMPARE(neutral.gradingKeyframes.front().linearInterpolation, true);
    QCOMPARE(neutral.opacityKeyframes.size(), std::size_t(1));
    QCOMPARE(neutral.opacityKeyframes.front().frame, std::int64_t(0));
    QCOMPARE(neutral.opacityKeyframes.front().opacity, 1.0);
    QCOMPARE(neutral.opacityKeyframes.front().linearInterpolation, true);
    QCOMPARE(neutral.locked, true);
    QCOMPARE(neutral.videoEnabled, false);
    QCOMPARE(neutral.baseTranslationX, 48.0);
    QCOMPARE(neutral.baseScaleY, 1.4);
    QCOMPARE(neutral.transformKeyframes.size(), std::size_t(1));
    QCOMPARE(neutral.maskEnabled, true);
    QCOMPARE(QString::fromStdString(neutral.effectPreset),
             QStringLiteral("kaleidoscope"));
    QCOMPARE(neutral.titleKeyframes.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(neutral.titleKeyframes.front().text),
             QStringLiteral("Preserved title"));
    const nlohmann::json resetSnapshot = jcut::toJson(runtime.snapshot());

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initial);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == resetSnapshot);

    jcut::EditorDocumentCore legacyDocument;
    legacyDocument.tracks.push_back({1, "Legacy", true});
    jcut::EditorClip legacyVisual;
    legacyVisual.id = 1;
    legacyVisual.trackId = 1;
    legacyVisual.durationFrames = 30;
    legacyVisual.videoEnabled = true;
    legacyVisual.gradingKeyframes = {{10, 0.2, 1.2, 0.8, 0.6, true}};
    legacyVisual.opacityKeyframes = {{10, 0.6, true}};
    legacyDocument.clips = {legacyVisual};
    jcut::EditorRuntime legacyRuntime =
        jcut::EditorRuntime::fromDocument(std::move(legacyDocument));
    QVERIFY(legacyRuntime.execute(
        jcut::EditorCommand{jcut::ResetClipGradingCommand{1}}).applied);
    QCOMPARE(legacyRuntime.snapshot().clips.front().gradingKeyframes.size(),
             std::size_t(1));
    QCOMPARE(legacyRuntime.snapshot().clips.front().opacityKeyframes.size(),
             std::size_t(1));

    jcut::EditorDocumentCore audioDocument = legacyRuntime.snapshot();
    audioDocument.clips.front().mediaKind = "audio";
    audioDocument.clips.front().gradingKeyframes = {
        {10, 0.2, 1.2, 0.8, 0.6, true}};
    audioDocument.clips.front().opacityKeyframes = {{10, 0.6, true}};
    jcut::EditorRuntime audioRuntime =
        jcut::EditorRuntime::fromDocument(std::move(audioDocument));
    QVERIFY(audioRuntime.execute(
        jcut::EditorCommand{jcut::ResetClipGradingCommand{1}}).applied);
    QVERIFY(audioRuntime.snapshot().clips.front().gradingKeyframes.empty());
    QVERIFY(audioRuntime.snapshot().clips.front().opacityKeyframes.empty());
}

void TestEditorRuntime::testResetClipGradingClearsQtKnownLegacyFields()
{
    const nlohmann::json base = {
        {"projectName", "Advanced grade"},
        {"selectedClipId", "clip-graded"},
        {"tracks", nlohmann::json::array({{{"name", "Video"}}})},
        {"timeline", nlohmann::json::array({{
            {"id", "clip-graded"},
            {"mediaType", "video"},
            {"trackIndex", 0},
            {"startFrame", 0},
            {"durationFrames", 90},
            {"brightness", 0.25},
            {"contrast", 1.4},
            {"saturation", 0.7},
            {"opacity", 0.6},
            {"gradingKeyframes", nlohmann::json::array({{
                {"frame", 0},
                {"brightness", 0.25},
                {"contrast", 1.4},
                {"saturation", 0.7},
                {"opacity", 0.6},
                {"linearInterpolation", false},
                {"shadowsR", 0.11},
                {"shadowsG", 0.12},
                {"shadowsB", 0.13},
                {"midtonesR", 0.21},
                {"midtonesG", 0.22},
                {"midtonesB", 0.23},
                {"highlightsR", 0.31},
                {"highlightsG", 0.32},
                {"highlightsB", 0.33},
                {"curvePointsR", nlohmann::json::array({
                    {{"x", 0.0}, {"y", 0.2}},
                    {{"x", 1.0}, {"y", 0.8}}
                })},
                {"curvePointsG", nlohmann::json::array({
                    {{"x", 0.0}, {"y", 0.1}},
                    {{"x", 1.0}, {"y", 0.9}}
                })},
                {"curvePointsB", nlohmann::json::array({
                    {{"x", 0.0}, {"y", 0.3}},
                    {{"x", 1.0}, {"y", 0.7}}
                })},
                {"curvePointsLuma", nlohmann::json::array({
                    {{"x", 0.0}, {"y", 0.4}},
                    {{"x", 1.0}, {"y", 0.6}}
                })},
                {"curveThreePointLock", true},
                {"curveSmoothingEnabled", false},
                {"futureCurveMode", "spline_v2"}
            }})},
            {"opacityKeyframes", nlohmann::json::array({{
                {"frame", 0}, {"opacity", 0.6}
            }})}
        }})}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> parsed =
        jcut::editorDocumentCoreFromJson(base, &error);
    QVERIFY2(parsed.has_value(), error.c_str());
    QCOMPARE(parsed->clips.front().gradingKeyframes.front().shadowsR, 0.11);
    QCOMPARE(parsed->clips.front().gradingKeyframes.front().curvePointsLuma.size(),
             std::size_t(2));

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(*parsed);
    const nlohmann::json initialCore = jcut::toJson(runtime.snapshot());
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::ResetClipGradingCommand{runtime.snapshot().clips.front().id}}).applied);

    const nlohmann::json saved =
        jcut::toLegacyStateJson(runtime.snapshot(), &base);
    const nlohmann::json& grade =
        saved.at("timeline").at(0).at("gradingKeyframes").at(0);
    QCOMPARE(grade.value("brightness", -1.0), 0.0);
    QCOMPARE(grade.value("contrast", -1.0), 1.0);
    QCOMPARE(grade.value("saturation", -1.0), 1.0);
    QCOMPARE(grade.value("opacity", -1.0), 1.0);
    for (const char* channel : {
             "shadowsR", "shadowsG", "shadowsB",
             "midtonesR", "midtonesG", "midtonesB",
             "highlightsR", "highlightsG", "highlightsB"}) {
        QCOMPARE(grade.value(channel, -1.0), 0.0);
    }
    for (const char* curve : {
             "curvePointsR", "curvePointsG",
             "curvePointsB", "curvePointsLuma"}) {
        const nlohmann::json& points = grade.at(curve);
        QCOMPARE(points.size(), std::size_t(2));
        QCOMPARE(points.at(0).value("x", -1.0), 0.0);
        QCOMPARE(points.at(0).value("y", -1.0), 0.0);
        QCOMPARE(points.at(1).value("x", -1.0), 1.0);
        QCOMPARE(points.at(1).value("y", -1.0), 1.0);
    }
    QCOMPARE(grade.value("curveThreePointLock", true), false);
    QCOMPARE(grade.value("curveSmoothingEnabled", false), true);
    QCOMPARE(QString::fromStdString(
                 grade.value("futureCurveMode", std::string{})),
             QStringLiteral("spline_v2"));

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initialCore);
    const nlohmann::json undoSaved =
        jcut::toLegacyStateJson(runtime.snapshot(), &base);
    const nlohmann::json& restored =
        undoSaved.at("timeline").at(0).at("gradingKeyframes").at(0);
    QCOMPARE(restored.value("shadowsR", 0.0), 0.11);
    QCOMPARE(restored.value("curveThreePointLock", false), true);
    QCOMPARE(restored.at("curvePointsLuma").at(0).value("y", 0.0), 0.4);
}

void TestEditorRuntime::testScaleToFillHelperReusesUndoableTransformCommand()
{
    const std::optional<double> portraitFill = jcut::scaleToFillFactor(
        {1920, 1080}, {1080, 1920});
    QVERIFY(portraitFill.has_value());
    QVERIFY(std::abs(*portraitFill - (256.0 / 81.0)) < 0.000000001);

    const std::optional<double> matchingAspect = jcut::scaleToFillFactor(
        {1920, 1080}, {1280, 720});
    QVERIFY(matchingAspect.has_value());
    QCOMPARE(*matchingAspect, 1.0);
    QVERIFY(!jcut::scaleToFillFactor({0, 1080}, {1080, 1920}).has_value());
    QVERIFY(!jcut::scaleToFillFactor({1920, 1080}, {1080, 0}).has_value());

    jcut::EditorDocumentCore document = jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    clip.baseTranslationX = 80.0;
    clip.baseTranslationY = -40.0;
    clip.baseRotation = 27.0;
    clip.baseScaleX = -1.25;
    clip.baseScaleY = 0.75;
    clip.transformKeyframes = {
        {0, "Preserved", 12.0, -6.0, 3.0, 1.1, 0.9, false}};
    const int clipId = clip.id;
    const double rotation = clip.baseRotation;

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json before = jcut::toJson(runtime.snapshot());
    const jcut::CommandResult applied = runtime.execute(jcut::EditorCommand{
        jcut::SetClipTransformCommand{
            clipId, 0.0, 0.0, rotation,
            *portraitFill, *portraitFill}});
    QVERIFY2(applied.applied, applied.message.c_str());
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorClip filled = runtime.snapshot().clips.front();
    QCOMPARE(filled.baseTranslationX, 0.0);
    QCOMPARE(filled.baseTranslationY, 0.0);
    QCOMPARE(filled.baseRotation, 27.0);
    QCOMPARE(filled.baseScaleX, *portraitFill);
    QCOMPARE(filled.baseScaleY, *portraitFill);
    QCOMPARE(filled.transformKeyframes.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(filled.transformKeyframes.front().title),
             QStringLiteral("Preserved"));
    QCOMPARE(filled.transformKeyframes.front().linearInterpolation, false);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == before);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.front().baseScaleX, *portraitFill);
}

void TestEditorRuntime::testEffectsInspectorUsesCompleteNeutralPresetCatalogAndQtBounds()
{
    QCOMPARE(jcut::kEditorEffectPresetIds.size(), std::size_t(35));

    std::vector<std::string> neutralPresetIds;
    neutralPresetIds.reserve(jcut::kEditorEffectPresetIds.size());
    for (const std::string_view presetId : jcut::kEditorEffectPresetIds) {
        neutralPresetIds.emplace_back(presetId);
    }
    std::sort(neutralPresetIds.begin(), neutralPresetIds.end());
    QVERIFY(std::adjacent_find(neutralPresetIds.begin(), neutralPresetIds.end()) ==
            neutralPresetIds.end());

    std::vector<std::string> qtPresetIds;
    const int lastQtPreset = static_cast<int>(ClipEffectPreset::SpeakerMaskDilationRings);
    qtPresetIds.reserve(static_cast<std::size_t>(lastQtPreset + 1));
    for (int value = 0; value <= lastQtPreset; ++value) {
        qtPresetIds.push_back(
            editor::effectPresetToJson(static_cast<ClipEffectPreset>(value)).toStdString());
    }
    std::sort(qtPresetIds.begin(), qtPresetIds.end());
    QVERIFY(neutralPresetIds == qtPresetIds);

    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const jcut::CommandResult reverseMotion = runtime.execute(jcut::EditorCommand{
        jcut::SetClipMaskEffectCommand{
            1, false, 0.0, 1.0, 0, false, false, 160.0, 0.0,
            "progressive_edge_stretch", 999, -3.25, 0.01, true}});
    QVERIFY2(reverseMotion.applied, reverseMotion.message.c_str());
    const jcut::EditorClip reverseClip = runtime.snapshot().clips.front();
    QCOMPARE(reverseClip.effectRows, jcut::kEditorEffectProgressiveEdgeMaxRows);
    QCOMPARE(reverseClip.effectSpeed, -3.25);
    QCOMPARE(reverseClip.effectScale, jcut::kEditorEffectMinScale);

    const jcut::CommandResult stationaryMotion = runtime.execute(jcut::EditorCommand{
        jcut::SetClipMaskEffectCommand{
            1, false, 0.0, 1.0, 0, false, false, 160.0, 0.0,
            "neon_glow", 999, 0.0, 99.0, false}});
    QVERIFY2(stationaryMotion.applied, stationaryMotion.message.c_str());
    const jcut::EditorClip stationaryClip = runtime.snapshot().clips.front();
    QCOMPARE(stationaryClip.effectRows, jcut::kEditorEffectDefaultMaxRows);
    QCOMPARE(stationaryClip.effectSpeed, 0.0);
    QCOMPARE(stationaryClip.effectScale, jcut::kEditorEffectMaxScale);

    // A mask-only edit must not normalize fields belonging to an unknown
    // future effect preset.
    jcut::EditorDocumentCore futureDocument = runtime.snapshot();
    jcut::EditorClip& futureClip = futureDocument.clips.front();
    futureClip.effectPreset = "future_effect_v2";
    futureClip.effectRows = 777;
    futureClip.effectSpeed = -42.5;
    futureClip.effectScale = 123.25;
    futureClip.effectAlternateDirection = true;
    jcut::EditorRuntime futureRuntime =
        jcut::EditorRuntime::fromDocument(std::move(futureDocument));
    const jcut::CommandResult maskOnly = futureRuntime.execute(
        jcut::EditorCommand{jcut::SetClipMaskCommand{
            1, true, 24.0, 2.0, 4, true, true, 88.0, -12.0}});
    QVERIFY2(maskOnly.applied, maskOnly.message.c_str());
    const jcut::EditorClip preservedFutureClip =
        futureRuntime.snapshot().clips.front();
    QCOMPARE(QString::fromStdString(preservedFutureClip.effectPreset),
             QStringLiteral("future_effect_v2"));
    QCOMPARE(preservedFutureClip.effectRows, 777);
    QCOMPARE(preservedFutureClip.effectSpeed, -42.5);
    QCOMPARE(preservedFutureClip.effectScale, 123.25);
    QCOMPARE(preservedFutureClip.effectAlternateDirection, true);
    QCOMPARE(preservedFutureClip.maskEnabled, true);
    QCOMPARE(preservedFutureClip.maskFeather, 24.0);

    QFile imguiSource(QStringLiteral(JCUT_SOURCE_DIR "/jcut_imgui_main.cpp"));
    QVERIFY2(imguiSource.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(imguiSource.fileName()));
    const QString imguiCode = QString::fromUtf8(imguiSource.readAll());
    QVERIFY(imguiCode.contains(QStringLiteral("jcut::kEditorEffectPresetIds")));
    QVERIFY(imguiCode.contains(QStringLiteral("jcut::kEditorEffectMinSpeed")));
    QVERIFY(imguiCode.contains(QStringLiteral("effectPresetUsesCommonNeutralParameters")));
    QVERIFY(imguiCode.contains(QStringLiteral("jcut::SetClipMaskCommand")));
    QVERIFY(imguiCode.contains(QStringLiteral("Enable Mask Grade")));
    QVERIFY(imguiCode.contains(QStringLiteral("Mask Brightness")));
    QVERIFY(imguiCode.contains(QStringLiteral("Mask Contrast")));
    QVERIFY(imguiCode.contains(QStringLiteral("Mask Saturation")));
    QVERIFY(imguiCode.contains(QStringLiteral("Enable Mask Shadow")));
    QVERIFY(imguiCode.contains(QStringLiteral("Shadow Radius")));
    QVERIFY(imguiCode.contains(QStringLiteral("Show Mask Only")));
    QVERIFY(imguiCode.contains(QStringLiteral("command.gradeCurvePointsR")));
    QVERIFY(imguiCode.contains(QStringLiteral("command.dropShadowOpacity")));
    QVERIFY(imguiCode.contains(QStringLiteral("Difference Reference Frames")));
    QVERIFY(imguiCode.contains(QStringLiteral("Echo Spacing")));
    QVERIFY(imguiCode.contains(QStringLiteral("Tiling Pattern")));
    QVERIFY(imguiCode.contains(QStringLiteral("Synchronize motion with Speech Filter")));
    QVERIFY(imguiCode.contains(QStringLiteral("command.temporalEchoDecay")));
    QVERIFY(imguiCode.contains(QStringLiteral("command.tilingPattern")));
    QVERIFY(!imguiCode.contains(QStringLiteral("std::array<const char*, 12> presets")));
}

void TestEditorRuntime::testTrackPropertiesCommandNormalizesLabelAndHeight()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const jcut::EditorTrack original = runtime.snapshot().tracks.at(1);

    const jcut::CommandResult missing = runtime.execute(
        jcut::EditorCommand{jcut::SetTrackPropertiesCommand{
            999, "Missing", 72}});
    QCOMPARE(missing.applied, false);
    QCOMPARE(runtime.canUndo(), false);

    const jcut::CommandResult renamed = runtime.execute(
        jcut::EditorCommand{jcut::SetTrackPropertiesCommand{
            original.id, "  Dialogue & Effects \n", 999}});
    QVERIFY2(renamed.applied, renamed.message.c_str());
    const jcut::EditorTrack edited = runtime.snapshot().tracks.at(1);
    QCOMPARE(QString::fromStdString(edited.label),
             QStringLiteral("Dialogue & Effects"));
    QCOMPARE(edited.height, jcut::kEditorTrackMaxHeight);
    QCOMPARE(edited.visualMode, original.visualMode);
    QCOMPARE(edited.audioEnabled, original.audioEnabled);
    QCOMPARE(edited.audioGain, original.audioGain);
    QCOMPARE(edited.audioMuted, original.audioMuted);
    QCOMPARE(edited.audioSolo, original.audioSolo);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(QString::fromStdString(runtime.snapshot().tracks.at(1).label),
             QString::fromStdString(original.label));
    QCOMPARE(runtime.snapshot().tracks.at(1).height, original.height);

    const jcut::CommandResult fallback = runtime.execute(
        jcut::EditorCommand{jcut::SetTrackPropertiesCommand{
            original.id, " \t\n ", -100}});
    QVERIFY2(fallback.applied, fallback.message.c_str());
    const jcut::EditorTrack normalized = runtime.snapshot().tracks.at(1);
    QCOMPARE(QString::fromStdString(normalized.label), QStringLiteral("Track 2"));
    QCOMPARE(normalized.height, jcut::kEditorTrackMinHeight);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(QString::fromStdString(runtime.snapshot().tracks.at(1).label),
             QString::fromStdString(original.label));
    QCOMPARE(runtime.snapshot().tracks.at(1).height, original.height);
}

void TestEditorRuntime::testTrackMediaPresenceMatchesQtEnablement()
{
    jcut::EditorDocumentCore document;
    document.tracks = {
        {1, "Empty", false},
        {2, "Audio", false},
        {3, "Image", false},
        {4, "Video", false},
        {5, "Legacy", false},
    };

    jcut::EditorClip audio;
    audio.id = 1;
    audio.trackId = 2;
    audio.mediaKind = "audio";
    audio.videoEnabled = true;
    audio.hasAudio = true;
    document.clips.push_back(audio);

    jcut::EditorClip image;
    image.id = 2;
    image.trackId = 3;
    image.mediaKind = "image";
    image.videoEnabled = false;
    document.clips.push_back(image);

    jcut::EditorClip video;
    video.id = 3;
    video.trackId = 4;
    video.mediaKind = "video";
    video.hasAudio = true;
    document.clips.push_back(video);

    jcut::EditorClip legacy;
    legacy.id = 4;
    legacy.trackId = 5;
    legacy.mediaKind.clear();
    legacy.videoEnabled = true;
    document.clips.push_back(legacy);

    const auto emptyPresence =
        jcut::editorTrackMediaPresenceCore(document, 1);
    QCOMPARE(emptyPresence.hasVisual, false);
    QCOMPARE(emptyPresence.hasAudio, false);

    const auto audioPresence =
        jcut::editorTrackMediaPresenceCore(document, 2);
    QCOMPARE(audioPresence.hasVisual, false);
    QCOMPARE(audioPresence.hasAudio, true);

    const auto imagePresence =
        jcut::editorTrackMediaPresenceCore(document, 3);
    QCOMPARE(imagePresence.hasVisual, true);
    QCOMPARE(imagePresence.hasAudio, false);

    const auto videoPresence =
        jcut::editorTrackMediaPresenceCore(document, 4);
    QCOMPARE(videoPresence.hasVisual, true);
    QCOMPARE(videoPresence.hasAudio, true);

    const auto legacyPresence =
        jcut::editorTrackMediaPresenceCore(document, 5);
    QCOMPARE(legacyPresence.hasVisual, true);
    QCOMPARE(legacyPresence.hasAudio, false);
}

void TestEditorRuntime::testAutoOpposeAnalysisCoreMatchesQtThresholds()
{
    const std::array<std::uint8_t, 8> rgba{
        255, 0, 0, 255,
        0, 255, 0, 255};
    jcut::EditorGradeProbeSampleCore probed;
    QVERIFY(jcut::probeEditorGradeStatsRgba(
        rgba.data(), 2, 1, 8, &probed));
    QVERIFY(std::abs(probed.lumaMean - 0.4639) < 0.0001);
    QCOMPARE(probed.saturationMean, 1.0);

    std::vector<jcut::EditorGradeProbeSampleCore> samples{
        {0, 0.40, 0.20, 0.10},
        {12, 0.60, 0.20, 0.10},
        {24, 0.60, 0.10, 0.05},
    };
    const std::vector<jcut::EditorOpposeGradeEventCore> events =
        jcut::detectEditorOpposeGradeEvents(
            samples, jcut::EditorAutoOpposeSettingsCore{});
    QCOMPARE(events.size(), std::size_t(2));
    QCOMPARE(events.front().localFrame, std::int64_t(12));
    QVERIFY(std::abs(events.front().brightnessDelta + 0.48) < 0.000001);
    QCOMPARE(events.front().contrastMul, 1.0);
    QCOMPARE(events.front().saturationMul, 1.0);
    QCOMPARE(events.back().localFrame, std::int64_t(24));
    QCOMPARE(events.back().contrastMul, 2.0);
    QCOMPARE(events.back().saturationMul, 2.0);
}

void TestEditorRuntime::testTitleKeyframeFieldsRoundTripThroughRenderBridge()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    jcut::EditorTitleKeyframe keyframe;
    keyframe.frame = 42;
    keyframe.text = "A fully styled title";
    keyframe.translationX = -123.5;
    keyframe.translationY = 87.25;
    keyframe.fontSize = 73.0;
    keyframe.opacity = 0.625;
    keyframe.fontFamily = "Liberation Serif";
    keyframe.bold = false;
    keyframe.italic = true;
    keyframe.color = "#44cc88";
    keyframe.linearInterpolation = false;
    keyframe.autoFitToOutput = true;
    keyframe.logoPath = "assets/logo.png";
    keyframe.textMaterialStyle = "diagonal_stripes";
    keyframe.textPatternImagePath = "assets/text-pattern.png";
    keyframe.textPatternScale = 2.25;
    keyframe.dropShadowEnabled = false;
    keyframe.dropShadowColor = "#112233";
    keyframe.dropShadowOpacity = 0.45;
    keyframe.dropShadowOffsetX = -12.0;
    keyframe.dropShadowOffsetY = 14.0;
    keyframe.windowEnabled = true;
    keyframe.windowColor = "#223344";
    keyframe.windowOpacity = 0.35;
    keyframe.windowPadding = 26.0;
    keyframe.windowWidth = 620.0;
    keyframe.windowFrameEnabled = true;
    keyframe.windowFrameColor = "#334455";
    keyframe.windowFrameOpacity = 0.75;
    keyframe.windowFrameWidth = 6.0;
    keyframe.windowFrameGap = 8.0;
    keyframe.windowFrameMaterialStyle = "grid";
    keyframe.windowFramePatternImagePath = "assets/frame-pattern.png";
    keyframe.windowFramePatternScale = 1.75;
    keyframe.vulkan3DEnabled = true;
    keyframe.vulkan3DExtrudeEnabled = true;
    keyframe.textExtrudeMode = "eroded_solid";
    keyframe.vulkan3DExtrudeDepth = 0.8;
    keyframe.vulkan3DBevelScale = 1.2;
    keyframe.vulkan3DYawDegrees = 21.0;
    keyframe.vulkan3DPitchDegrees = -17.0;
    keyframe.vulkan3DRollDegrees = 9.0;
    keyframe.vulkan3DDepth = 1.5;
    keyframe.vulkan3DScale = 1.3;

    const jcut::CommandResult upsertResult = runtime.execute(
        jcut::EditorCommand{jcut::UpsertTitleKeyframeCommand{1, keyframe}});
    QVERIFY2(upsertResult.applied, upsertResult.message.c_str());

    const jcut::EditorDocumentCore snapshot = runtime.snapshot();
    QCOMPARE(snapshot.clips.front().titleKeyframes.size(), std::size_t(1));
    const jcut::EditorTitleKeyframe& stored =
        snapshot.clips.front().titleKeyframes.front();
    QCOMPARE(stored.frame, std::int64_t(42));
    QCOMPARE(QString::fromStdString(stored.text), QStringLiteral("A fully styled title"));
    QCOMPARE(stored.translationX, -123.5);
    QCOMPARE(stored.translationY, 87.25);
    QCOMPARE(stored.fontSize, 73.0);
    QCOMPARE(stored.opacity, 0.625);
    QCOMPARE(QString::fromStdString(stored.fontFamily), QStringLiteral("Liberation Serif"));
    QCOMPARE(stored.bold, false);
    QCOMPARE(stored.italic, true);
    QCOMPARE(QString::fromStdString(stored.color), QStringLiteral("#44cc88"));
    QCOMPARE(stored.linearInterpolation, false);
    QCOMPARE(stored.autoFitToOutput, true);
    QCOMPARE(QString::fromStdString(stored.logoPath), QStringLiteral("assets/logo.png"));
    QCOMPARE(QString::fromStdString(stored.textMaterialStyle), QStringLiteral("diagonal_stripes"));
    QCOMPARE(stored.textPatternScale, 2.25);
    QCOMPARE(stored.dropShadowEnabled, false);
    QCOMPARE(stored.dropShadowOpacity, 0.45);
    QCOMPARE(stored.windowEnabled, true);
    QCOMPARE(stored.windowWidth, 620.0);
    QCOMPARE(stored.windowFrameEnabled, true);
    QCOMPARE(QString::fromStdString(stored.windowFrameMaterialStyle), QStringLiteral("grid"));
    QCOMPARE(stored.vulkan3DEnabled, true);
    QCOMPARE(stored.vulkan3DExtrudeEnabled, true);
    QCOMPARE(QString::fromStdString(stored.textExtrudeMode), QStringLiteral("eroded_solid"));
    QCOMPARE(stored.vulkan3DYawDegrees, 21.0);
    QCOMPARE(stored.vulkan3DScale, 1.3);

    const nlohmann::json serialized = jcut::toJson(snapshot);
    std::string parseError;
    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(serialized, &parseError);
    QVERIFY2(reparsed.has_value(), parseError.c_str());
    const jcut::EditorTitleKeyframe& reparsedTitle =
        reparsed->clips.front().titleKeyframes.front();
    QCOMPARE(reparsedTitle.frame, stored.frame);
    QCOMPARE(QString::fromStdString(reparsedTitle.text), QString::fromStdString(stored.text));
    QCOMPARE(reparsedTitle.translationX, stored.translationX);
    QCOMPARE(reparsedTitle.translationY, stored.translationY);
    QCOMPARE(reparsedTitle.fontSize, stored.fontSize);
    QCOMPARE(reparsedTitle.opacity, stored.opacity);
    QCOMPARE(QString::fromStdString(reparsedTitle.fontFamily),
             QString::fromStdString(stored.fontFamily));
    QCOMPARE(reparsedTitle.bold, stored.bold);
    QCOMPARE(reparsedTitle.italic, stored.italic);
    QCOMPARE(QString::fromStdString(reparsedTitle.color),
             QString::fromStdString(stored.color));
    QCOMPARE(reparsedTitle.linearInterpolation, stored.linearInterpolation);
    QCOMPARE(reparsedTitle.autoFitToOutput, stored.autoFitToOutput);
    QCOMPARE(QString::fromStdString(reparsedTitle.logoPath),
             QString::fromStdString(stored.logoPath));
    QCOMPARE(QString::fromStdString(reparsedTitle.textMaterialStyle),
             QString::fromStdString(stored.textMaterialStyle));
    QCOMPARE(reparsedTitle.textPatternScale, stored.textPatternScale);
    QCOMPARE(reparsedTitle.dropShadowOpacity, stored.dropShadowOpacity);
    QCOMPARE(reparsedTitle.windowPadding, stored.windowPadding);
    QCOMPARE(reparsedTitle.windowFrameWidth, stored.windowFrameWidth);
    QCOMPARE(QString::fromStdString(reparsedTitle.windowFrameMaterialStyle),
             QString::fromStdString(stored.windowFrameMaterialStyle));
    QCOMPARE(QString::fromStdString(reparsedTitle.textExtrudeMode),
             QString::fromStdString(stored.textExtrudeMode));
    QCOMPARE(reparsedTitle.vulkan3DExtrudeDepth, stored.vulkan3DExtrudeDepth);
    QCOMPARE(reparsedTitle.vulkan3DRollDegrees, stored.vulkan3DRollDegrees);

    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(*reparsed);
    QCOMPARE(renderData.clips.front().titleKeyframes.size(), 1);
    const TimelineClip::TitleKeyframe& renderTitle =
        renderData.clips.front().titleKeyframes.front();
    QCOMPARE(renderTitle.frame, std::int64_t(42));
    QCOMPARE(renderTitle.text, QStringLiteral("A fully styled title"));
    QCOMPARE(renderTitle.translationX, -123.5);
    QCOMPARE(renderTitle.translationY, 87.25);
    QCOMPARE(renderTitle.fontSize, 73.0);
    QCOMPARE(renderTitle.opacity, 0.625);
    QCOMPARE(renderTitle.fontFamily, QStringLiteral("Liberation Serif"));
    QCOMPARE(renderTitle.bold, false);
    QCOMPARE(renderTitle.italic, true);
    QCOMPARE(renderTitle.color.name(), QStringLiteral("#44cc88"));
    QCOMPARE(renderTitle.linearInterpolation, false);
    QCOMPARE(renderTitle.autoFitToOutput, true);
    QCOMPARE(renderTitle.logoPath, QStringLiteral("assets/logo.png"));
    QCOMPARE(renderTitle.textMaterialStyle,
             TimelineClip::TitleKeyframe::MaterialStyle::DiagonalStripes);
    QCOMPARE(renderTitle.textPatternImagePath,
             QStringLiteral("assets/text-pattern.png"));
    QCOMPARE(renderTitle.textPatternScale, 2.25);
    QCOMPARE(renderTitle.dropShadowEnabled, false);
    QCOMPARE(renderTitle.dropShadowColor, QColor(QStringLiteral("#112233")));
    QCOMPARE(renderTitle.dropShadowOffsetX, -12.0);
    QCOMPARE(renderTitle.windowEnabled, true);
    QCOMPARE(renderTitle.windowColor, QColor(QStringLiteral("#223344")));
    QCOMPARE(renderTitle.windowWidth, 620.0);
    QCOMPARE(renderTitle.windowFrameEnabled, true);
    QCOMPARE(renderTitle.windowFrameMaterialStyle,
             TimelineClip::TitleKeyframe::MaterialStyle::Grid);
    QCOMPARE(renderTitle.windowFramePatternScale, 1.75);
    QCOMPARE(renderTitle.vulkan3DEnabled, true);
    QCOMPARE(renderTitle.vulkan3DExtrudeEnabled, true);
    QCOMPARE(renderTitle.textExtrudeMode,
             TimelineClip::TitleKeyframe::TextExtrudeMode::ErodedSolid);
    QCOMPARE(renderTitle.vulkan3DExtrudeDepth, 0.8);
    QCOMPARE(renderTitle.vulkan3DBevelScale, 1.2);
    QCOMPARE(renderTitle.vulkan3DYawDegrees, 21.0);
    QCOMPARE(renderTitle.vulkan3DPitchDegrees, -17.0);
    QCOMPARE(renderTitle.vulkan3DRollDegrees, 9.0);
    QCOMPARE(renderTitle.vulkan3DDepth, 1.5);
    QCOMPARE(renderTitle.vulkan3DScale, 1.3);

    const jcut::CommandResult removeResult = runtime.execute(
        jcut::EditorCommand{jcut::RemoveTitleKeyframeCommand{1, keyframe.frame}});
    QVERIFY2(removeResult.applied, removeResult.message.c_str());
    QVERIFY(runtime.snapshot().clips.front().titleKeyframes.empty());
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == serialized);
}

void TestEditorRuntime::testTranscriptOverlayInspectorStateRoundTripsAndRenders()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const nlohmann::json before = jcut::toJson(runtime.snapshot());
    const int clipId = runtime.snapshot().clips.front().id;

    jcut::EditorTranscriptOverlayState overlay;
    overlay.enabled = true;
    overlay.showBackground = false;
    overlay.backgroundOpacity = 0.375;
    overlay.backgroundCornerRadius = 23.0;
    overlay.backgroundPadding = 31.0;
    overlay.backgroundFrameEnabled = true;
    overlay.backgroundFrameColor = "#112233";
    overlay.backgroundFrameOpacity = 0.55;
    overlay.backgroundFrameWidth = 7.0;
    overlay.backgroundFrameGap = 9.0;
    overlay.showShadow = false;
    overlay.shadowColor = "#223344";
    overlay.shadowOpacity = 0.45;
    overlay.shadowOffsetX = -11.0;
    overlay.shadowOffsetY = 13.0;
    overlay.textOutlineEnabled = true;
    overlay.textOutlineWidth = 3.5;
    overlay.textOutlineColor = "#334455";
    overlay.textOutlineOpacity = 0.65;
    overlay.textExtrudeMode = "eroded_solid";
    overlay.textExtrudeDepth = 0.85;
    overlay.textExtrudeBevelScale = 1.25;
    overlay.showSpeakerTitle = true;
    overlay.highlightCurrentWord = false;
    overlay.autoScroll = true;
    overlay.useManualPlacement = true;
    overlay.translationX = -0.375;
    overlay.translationY = 0.625;
    overlay.boxWidth = 760.0;
    overlay.boxHeight = 190.0;
    overlay.maxLines = 4;
    overlay.maxCharsPerLine = 48;
    overlay.fontFamily = "Liberation Sans";
    overlay.fontPointSize = 58;
    overlay.bold = false;
    overlay.italic = true;
    overlay.textColor = "#123456";
    overlay.textOpacity = 0.625;
    overlay.backgroundColor = "#abcdef";
    overlay.highlightColor = "#fedcba";
    overlay.highlightTextColor = "#0f1e2d";

    const jcut::CommandResult result = runtime.execute(
        jcut::EditorCommand{jcut::SetClipTranscriptOverlayCommand{clipId, overlay}});
    QVERIFY(result.applied);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));

    const jcut::EditorTranscriptOverlayState stored =
        runtime.snapshot().clips.front().transcriptOverlay;
    QCOMPARE(stored.enabled, true);
    QCOMPARE(stored.showBackground, false);
    QCOMPARE(stored.backgroundOpacity, 0.375);
    QCOMPARE(stored.backgroundCornerRadius, 23.0);
    QCOMPARE(stored.backgroundPadding, 31.0);
    QCOMPARE(stored.backgroundFrameEnabled, true);
    QCOMPARE(QString::fromStdString(stored.backgroundFrameColor), QStringLiteral("#112233"));
    QCOMPARE(stored.backgroundFrameOpacity, 0.55);
    QCOMPARE(stored.backgroundFrameWidth, 7.0);
    QCOMPARE(stored.backgroundFrameGap, 9.0);
    QCOMPARE(stored.showShadow, false);
    QCOMPARE(QString::fromStdString(stored.shadowColor), QStringLiteral("#223344"));
    QCOMPARE(stored.shadowOpacity, 0.45);
    QCOMPARE(stored.shadowOffsetX, -11.0);
    QCOMPARE(stored.shadowOffsetY, 13.0);
    QCOMPARE(stored.textOutlineEnabled, true);
    QCOMPARE(stored.textOutlineWidth, 3.5);
    QCOMPARE(QString::fromStdString(stored.textOutlineColor), QStringLiteral("#334455"));
    QCOMPARE(stored.textOutlineOpacity, 0.65);
    QCOMPARE(QString::fromStdString(stored.textExtrudeMode), QStringLiteral("eroded_solid"));
    QCOMPARE(stored.textExtrudeDepth, 0.85);
    QCOMPARE(stored.textExtrudeBevelScale, 1.25);
    QCOMPARE(stored.showSpeakerTitle, true);
    QCOMPARE(stored.highlightCurrentWord, false);
    QCOMPARE(stored.autoScroll, true);
    QCOMPARE(stored.useManualPlacement, true);
    QCOMPARE(stored.translationX, -0.375);
    QCOMPARE(stored.translationY, 0.625);
    QCOMPARE(stored.boxWidth, 760.0);
    QCOMPARE(stored.boxHeight, 190.0);
    QCOMPARE(stored.maxLines, 4);
    QCOMPARE(stored.maxCharsPerLine, 48);
    QCOMPARE(QString::fromStdString(stored.fontFamily), QStringLiteral("Liberation Sans"));
    QCOMPARE(stored.fontPointSize, 58);
    QCOMPARE(stored.bold, false);
    QCOMPARE(stored.italic, true);
    QCOMPARE(QString::fromStdString(stored.textColor), QStringLiteral("#123456"));
    QCOMPARE(stored.textOpacity, 0.625);
    QCOMPARE(QString::fromStdString(stored.backgroundColor), QStringLiteral("#abcdef"));
    QCOMPARE(QString::fromStdString(stored.highlightColor), QStringLiteral("#fedcba"));
    QCOMPARE(QString::fromStdString(stored.highlightTextColor), QStringLiteral("#0f1e2d"));

    std::string error;
    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(jcut::toJson(runtime.snapshot()), &error);
    QVERIFY2(reparsed.has_value(), error.c_str());
    const jcut::EditorTranscriptOverlayState& roundTripped =
        reparsed->clips.front().transcriptOverlay;
    QCOMPARE(roundTripped.showShadow, stored.showShadow);
    QCOMPARE(roundTripped.backgroundCornerRadius, stored.backgroundCornerRadius);
    QCOMPARE(roundTripped.backgroundPadding, stored.backgroundPadding);
    QCOMPARE(roundTripped.backgroundFrameEnabled, stored.backgroundFrameEnabled);
    QCOMPARE(QString::fromStdString(roundTripped.backgroundFrameColor),
             QString::fromStdString(stored.backgroundFrameColor));
    QCOMPARE(roundTripped.shadowOpacity, stored.shadowOpacity);
    QCOMPARE(roundTripped.shadowOffsetX, stored.shadowOffsetX);
    QCOMPARE(roundTripped.shadowOffsetY, stored.shadowOffsetY);
    QCOMPARE(roundTripped.textOutlineEnabled, stored.textOutlineEnabled);
    QCOMPARE(roundTripped.textOutlineWidth, stored.textOutlineWidth);
    QCOMPARE(QString::fromStdString(roundTripped.textOutlineColor),
             QString::fromStdString(stored.textOutlineColor));
    QCOMPARE(QString::fromStdString(roundTripped.textExtrudeMode),
             QString::fromStdString(stored.textExtrudeMode));
    QCOMPARE(roundTripped.textExtrudeDepth, stored.textExtrudeDepth);
    QCOMPARE(roundTripped.textExtrudeBevelScale, stored.textExtrudeBevelScale);
    QCOMPARE(roundTripped.showSpeakerTitle, stored.showSpeakerTitle);
    QCOMPARE(roundTripped.useManualPlacement, stored.useManualPlacement);
    QCOMPARE(roundTripped.translationX, stored.translationX);
    QCOMPARE(roundTripped.translationY, stored.translationY);
    QCOMPARE(QString::fromStdString(roundTripped.fontFamily),
             QString::fromStdString(stored.fontFamily));
    QCOMPARE(roundTripped.fontPointSize, stored.fontPointSize);
    QCOMPARE(roundTripped.bold, stored.bold);
    QCOMPARE(roundTripped.italic, stored.italic);
    QCOMPARE(QString::fromStdString(roundTripped.textColor),
             QString::fromStdString(stored.textColor));
    QCOMPARE(QString::fromStdString(roundTripped.backgroundColor),
             QString::fromStdString(stored.backgroundColor));
    QCOMPARE(QString::fromStdString(roundTripped.highlightColor),
             QString::fromStdString(stored.highlightColor));
    QCOMPARE(QString::fromStdString(roundTripped.highlightTextColor),
             QString::fromStdString(stored.highlightTextColor));

    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(*reparsed, false);
    const TimelineClip::TranscriptOverlaySettings& renderOverlay =
        renderData.clips.front().transcriptOverlay;
    QCOMPARE(renderOverlay.showShadow, false);
    QCOMPARE(renderOverlay.backgroundCornerRadius, 23.0);
    QCOMPARE(renderOverlay.backgroundPadding, 31.0);
    QCOMPARE(renderOverlay.backgroundFrameEnabled, true);
    QCOMPARE(renderOverlay.backgroundFrameColor, QColor(QStringLiteral("#112233")));
    QCOMPARE(renderOverlay.shadowColor, QColor(QStringLiteral("#223344")));
    QCOMPARE(renderOverlay.shadowOffsetX, -11.0);
    QCOMPARE(renderOverlay.shadowOffsetY, 13.0);
    QCOMPARE(renderOverlay.textOutlineEnabled, true);
    QCOMPARE(renderOverlay.textOutlineWidth, 3.5);
    QCOMPARE(renderOverlay.textOutlineColor, QColor(QStringLiteral("#334455")));
    QCOMPARE(renderOverlay.textExtrudeMode,
             TimelineClip::TitleKeyframe::TextExtrudeMode::ErodedSolid);
    QCOMPARE(renderOverlay.textExtrudeDepth, 0.85);
    QCOMPARE(renderOverlay.textExtrudeBevelScale, 1.25);
    QCOMPARE(renderOverlay.showSpeakerTitle, true);
    QCOMPARE(renderOverlay.useManualPlacement, true);
    QCOMPARE(renderOverlay.translationX, -0.375);
    QCOMPARE(renderOverlay.translationY, 0.625);
    QCOMPARE(renderOverlay.fontFamily, QStringLiteral("Liberation Sans"));
    QCOMPARE(renderOverlay.fontPointSize, 58);
    QCOMPARE(renderOverlay.bold, false);
    QCOMPARE(renderOverlay.italic, true);
    QCOMPARE(renderOverlay.textColor, QColor(QStringLiteral("#123456")));
    QCOMPARE(renderOverlay.backgroundColor, QColor(QStringLiteral("#abcdef")));
    QCOMPARE(renderOverlay.highlightColor, QColor(QStringLiteral("#fedcba")));
    QCOMPARE(renderOverlay.highlightTextColor, QColor(QStringLiteral("#0f1e2d")));

    applyCommand(runtime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(runtime.snapshot()) == before);
    applyCommand(runtime, jcut::RedoCommand{});
    QCOMPARE(runtime.snapshot().clips.front().transcriptOverlay.fontPointSize, 58);
}

void TestEditorRuntime::testClipKeyframeRemovalIsChannelScopedAndUndoable()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    clip.gradingKeyframes = {{30, 0.1, 1.2, 0.8, 1.0, true}};
    clip.opacityKeyframes = {{30, 0.7, false}};
    clip.transformKeyframes = {
        {30, "Move", 12.0, -8.0, 5.0, 1.1, 0.9, true}};
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json initial = jcut::toJson(runtime.snapshot());

    const jcut::CommandResult missing = runtime.execute(
        jcut::EditorCommand{jcut::RemoveClipKeyframeCommand{
            1, jcut::EditorKeyframeChannel::Grading, 99}});
    QVERIFY(!missing.applied);
    QVERIFY(!runtime.canUndo());

    runtime.beginHistoryTransaction();
    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::RemoveClipKeyframeCommand{
            1, jcut::EditorKeyframeChannel::Grading, 30}}).applied);
    QVERIFY(runtime.snapshot().clips.front().gradingKeyframes.empty());
    QCOMPARE(runtime.snapshot().clips.front().opacityKeyframes.size(),
             std::size_t(1));
    QCOMPARE(runtime.snapshot().clips.front().transformKeyframes.size(),
             std::size_t(1));
    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::RemoveClipKeyframeCommand{
            1, jcut::EditorKeyframeChannel::Opacity, 30}}).applied);
    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::RemoveClipKeyframeCommand{
            1, jcut::EditorKeyframeChannel::Transform, 30}}).applied);
    runtime.endHistoryTransaction();

    QVERIFY(runtime.snapshot().clips.front().gradingKeyframes.empty());
    QVERIFY(runtime.snapshot().clips.front().opacityKeyframes.empty());
    QVERIFY(runtime.snapshot().clips.front().transformKeyframes.empty());
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initial);
}

void TestEditorRuntime::testKeyframeFrameAndValueEditsAreAtomicAndChannelScoped()
{
    jcut::EditorDocumentCore document =
        jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    jcut::EditorGradingKeyframe originalGrade{
        30, 0.1, 1.2, 0.8, 0.9, true};
    originalGrade.shadowsR = -0.4;
    originalGrade.midtonesG = 0.25;
    originalGrade.highlightsB = 0.6;
    originalGrade.curvePointsR = {{0.0, 0.15}, {1.0, 0.85}};
    originalGrade.curvePointsG = {{0.0, 0.05}, {1.0, 0.95}};
    originalGrade.curveThreePointLock = false;
    originalGrade.curveSmoothingEnabled = false;
    clip.gradingKeyframes = {originalGrade};
    clip.opacityKeyframes = {{30, 0.7, false}};
    clip.transformKeyframes = {
        {30, "Original", 12.0, -8.0, 5.0, 1.1, 0.9, true}};
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const nlohmann::json initial = jcut::toJson(runtime.snapshot());

    const auto replaceKeyframe = [&runtime](
                                     jcut::EditorKeyframeChannel channel,
                                     std::int64_t originalFrame,
                                     jcut::EditorCommand replacement) {
        runtime.beginHistoryTransaction();
        const jcut::CommandResult removed = runtime.execute(
            jcut::EditorCommand{jcut::RemoveClipKeyframeCommand{
                1, channel, originalFrame}});
        const jcut::CommandResult upserted = removed.applied
            ? runtime.execute(std::move(replacement))
            : jcut::CommandResult{};
        runtime.endHistoryTransaction();
        return removed.applied && upserted.applied;
    };

    jcut::EditorGradingKeyframe editedGrade =
        runtime.snapshot().clips.front().gradingKeyframes.front();
    editedGrade.frame = 40;
    editedGrade.brightness = -0.25;
    editedGrade.contrast = 1.75;
    editedGrade.saturation = 1.5;
    editedGrade.opacity = 0.45;
    editedGrade.linearInterpolation = false;
    editedGrade.shadowsR = -1.25;
    editedGrade.midtonesG = 0.75;
    editedGrade.highlightsB = 1.5;
    QVERIFY(replaceKeyframe(
        jcut::EditorKeyframeChannel::Grading,
        30,
        jcut::EditorCommand{jcut::UpsertGradingKeyframeCommand{
            1, editedGrade}}));
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    QCOMPARE(runtime.snapshot().clips.front().opacityKeyframes.front().frame,
             std::int64_t(30));
    QCOMPARE(runtime.snapshot().clips.front().transformKeyframes.front().frame,
             std::int64_t(30));

    QVERIFY(replaceKeyframe(
        jcut::EditorKeyframeChannel::Opacity,
        30,
        jcut::EditorCommand{jcut::UpsertOpacityKeyframeCommand{
            1, {50, 0.35, true}}}));
    QCOMPARE(runtime.undoDepth(), std::size_t(2));

    QVERIFY(replaceKeyframe(
        jcut::EditorKeyframeChannel::Transform,
        30,
        jcut::EditorCommand{jcut::UpsertTransformKeyframeCommand{
            1, {60, "Reframed", 44.0, -22.0, 35.0, -1.25, 1.5, false}}}));
    QCOMPARE(runtime.undoDepth(), std::size_t(3));

    const jcut::EditorDocumentCore editedSnapshot = runtime.snapshot();
    const jcut::EditorClip& edited = editedSnapshot.clips.front();
    QCOMPARE(edited.gradingKeyframes.size(), std::size_t(1));
    QCOMPARE(edited.gradingKeyframes.front().frame, std::int64_t(40));
    QCOMPARE(edited.gradingKeyframes.front().brightness, -0.25);
    QCOMPARE(edited.gradingKeyframes.front().contrast, 1.75);
    QCOMPARE(edited.gradingKeyframes.front().saturation, 1.5);
    QCOMPARE(edited.gradingKeyframes.front().opacity, 0.45);
    QCOMPARE(edited.gradingKeyframes.front().linearInterpolation, false);
    QCOMPARE(edited.gradingKeyframes.front().shadowsR, -1.25);
    QCOMPARE(edited.gradingKeyframes.front().midtonesG, 0.75);
    QCOMPARE(edited.gradingKeyframes.front().highlightsB, 1.5);
    QCOMPARE(edited.gradingKeyframes.front().curvePointsR.front().y, 0.15);
    QCOMPARE(edited.gradingKeyframes.front().curvePointsG.back().y, 0.95);
    QCOMPARE(edited.gradingKeyframes.front().curveThreePointLock, false);
    QCOMPARE(edited.gradingKeyframes.front().curveSmoothingEnabled, false);
    QCOMPARE(edited.opacityKeyframes.size(), std::size_t(1));
    QCOMPARE(edited.opacityKeyframes.front().frame, std::int64_t(50));
    QCOMPARE(edited.opacityKeyframes.front().opacity, 0.35);
    QCOMPARE(edited.opacityKeyframes.front().linearInterpolation, true);
    QCOMPARE(edited.transformKeyframes.size(), std::size_t(1));
    QCOMPARE(edited.transformKeyframes.front().frame, std::int64_t(60));
    QCOMPARE(QString::fromStdString(edited.transformKeyframes.front().title),
             QStringLiteral("Reframed"));
    QCOMPARE(edited.transformKeyframes.front().translationX, 44.0);
    QCOMPARE(edited.transformKeyframes.front().translationY, -22.0);
    QCOMPARE(edited.transformKeyframes.front().rotation, 35.0);
    QCOMPARE(edited.transformKeyframes.front().scaleX, -1.25);
    QCOMPARE(edited.transformKeyframes.front().scaleY, 1.5);
    QCOMPARE(edited.transformKeyframes.front().linearInterpolation, false);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.front().transformKeyframes.front().frame,
             std::int64_t(30));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().clips.front().opacityKeyframes.front().frame,
             std::int64_t(30));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initial);
}

void TestEditorRuntime::testGradingCurveNormalizationIsUndoable()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    const nlohmann::json initial = jcut::toJson(runtime.snapshot());
    const auto nearlyEqual = [](double left, double right) {
        return std::abs(left - right) <= 0.000001;
    };

    jcut::EditorGradingKeyframe locked;
    locked.frame = 999;
    locked.shadowsR = 7.0;
    locked.shadowsG = -7.0;
    locked.shadowsB = 1.0;
    locked.midtonesR = -3.0;
    locked.midtonesG = 3.0;
    locked.midtonesB = -1.0;
    locked.highlightsR = 2.5;
    locked.highlightsG = -2.5;
    locked.highlightsB = -1.0;
    locked.curvePointsR = {{0.0, 0.9}, {1.0, 0.1}};
    locked.curvePointsG = {};
    locked.curvePointsB = {{0.75, 0.33}};
    locked.curvePointsLuma = {
        {1.4, 3.0}, {0.25, -3.0}, {0.2500005, 0.4}};
    locked.curveThreePointLock = true;

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::UpsertGradingKeyframeCommand{1, locked}}).applied);
    QCOMPARE(runtime.undoDepth(), std::size_t(1));
    const jcut::EditorGradingKeyframe normalized =
        runtime.snapshot().clips.front().gradingKeyframes.front();
    QCOMPARE(normalized.frame, std::int64_t(419));
    QCOMPARE(normalized.shadowsR, 2.0);
    QCOMPARE(normalized.shadowsG, -2.0);
    QCOMPARE(normalized.shadowsB, 1.0);
    QCOMPARE(normalized.midtonesR, -2.0);
    QCOMPARE(normalized.midtonesG, 2.0);
    QCOMPARE(normalized.midtonesB, -1.0);
    QCOMPARE(normalized.highlightsR, 2.0);
    QCOMPARE(normalized.highlightsG, -2.0);
    QCOMPARE(normalized.highlightsB, -1.0);
    QCOMPARE(normalized.curvePointsR.size(), std::size_t(3));
    QVERIFY(nearlyEqual(normalized.curvePointsR.at(0).y, 0.5));
    QVERIFY(nearlyEqual(normalized.curvePointsR.at(1).y, 0.1));
    QVERIFY(nearlyEqual(normalized.curvePointsR.at(2).y, 1.0));
    QCOMPARE(normalized.curvePointsG.size(), std::size_t(3));
    QVERIFY(nearlyEqual(normalized.curvePointsG.at(0).y, 0.0));
    QVERIFY(nearlyEqual(normalized.curvePointsG.at(1).y, 0.9));
    QVERIFY(nearlyEqual(normalized.curvePointsG.at(2).y, 0.5));
    QCOMPARE(normalized.curvePointsB.size(), std::size_t(3));
    QVERIFY(nearlyEqual(normalized.curvePointsB.at(0).y, 0.25));
    QVERIFY(nearlyEqual(normalized.curvePointsB.at(1).y, 0.3));
    QVERIFY(nearlyEqual(normalized.curvePointsB.at(2).y, 0.75));
    QCOMPARE(normalized.curvePointsLuma.size(), std::size_t(2));
    QCOMPARE(normalized.curvePointsLuma.at(0).x, 0.25);
    QCOMPARE(normalized.curvePointsLuma.at(0).y, 0.4);
    QCOMPARE(normalized.curvePointsLuma.at(1).x, 1.0);
    QCOMPARE(normalized.curvePointsLuma.at(1).y, 2.0);

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initial);

    jcut::EditorGradingKeyframe unlocked;
    unlocked.frame = 10;
    unlocked.shadowsR = 2.0;
    unlocked.midtonesR = 2.0;
    unlocked.highlightsR = 2.0;
    unlocked.curvePointsR = {
        {1.0, 0.8}, {0.0, 0.2}, {0.5, 0.6}};
    unlocked.curvePointsG = {};
    unlocked.curvePointsB = {{0.75, 0.33}};
    unlocked.curvePointsLuma = {{0.0, -5.0}, {1.0, 5.0}};
    unlocked.curveThreePointLock = false;

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::UpsertGradingKeyframeCommand{1, unlocked}}).applied);
    const jcut::EditorGradingKeyframe preserved =
        runtime.snapshot().clips.front().gradingKeyframes.front();
    QCOMPARE(preserved.curvePointsR.size(), std::size_t(3));
    QCOMPARE(preserved.curvePointsR.at(0).y, 0.2);
    QCOMPARE(preserved.curvePointsR.at(1).y, 0.6);
    QCOMPARE(preserved.curvePointsR.at(2).y, 0.8);
    QCOMPARE(preserved.curvePointsG.size(), std::size_t(2));
    QCOMPARE(preserved.curvePointsG.front().x, 0.0);
    QCOMPARE(preserved.curvePointsG.back().y, 1.0);
    QCOMPARE(preserved.curvePointsB.size(), std::size_t(2));
    QCOMPARE(preserved.curvePointsB.front().x, 0.0);
    QCOMPARE(preserved.curvePointsB.front().y, 0.33);
    QCOMPARE(preserved.curvePointsB.back().x, 0.75);
    QCOMPARE(preserved.curvePointsB.back().y, 0.33);
    QCOMPARE(preserved.curvePointsLuma.front().y, -1.0);
    QCOMPARE(preserved.curvePointsLuma.back().y, 2.0);
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == initial);
}

void TestEditorRuntime::testRenderSyncMarkerActionReplacementIsUniqueAndUndoable()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{1, 45, false, 2}}).applied);
    const jcut::EditorDocumentCore duplicateSnapshot = runtime.snapshot();
    QCOMPARE(duplicateSnapshot.renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(duplicateSnapshot.renderSyncMarkers.front().skipFrame, false);
    QCOMPARE(duplicateSnapshot.renderSyncMarkers.front().count, 2);
    const std::size_t undoDepth = runtime.undoDepth();

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{1, 45, true, 7}}).applied);
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);
    const jcut::EditorDocumentCore skipSnapshot = runtime.snapshot();
    QCOMPARE(skipSnapshot.renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(skipSnapshot.renderSyncMarkers.front().frame, std::int64_t(45));
    QCOMPARE(skipSnapshot.renderSyncMarkers.front().skipFrame, true);
    QCOMPARE(skipSnapshot.renderSyncMarkers.front().count, 7);
    QCOMPARE(skipSnapshot.exportRequest.renderSyncMarkerCount, std::size_t(1));

    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == jcut::toJson(duplicateSnapshot));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::RedoCommand{}}).applied);
    QVERIFY(jcut::toJson(runtime.snapshot()) == jcut::toJson(skipSnapshot));
}

void TestEditorRuntime::testRenderSyncMarkerLoadNormalizationIsLastWins()
{
    jcut::EditorDocumentCore document = jcut::EditorRuntime::createDemo().snapshot();
    const std::string clipId = document.clips.front().persistentId;
    document.renderSyncMarkers = {
        {clipId, 60, false, 3},
        {clipId, 45, false, 2},
        {clipId, 45, true, 9},
    };
    document.exportRequest.renderSyncMarkerCount = 99;

    const jcut::EditorDocumentCore normalized =
        jcut::EditorRuntime::fromDocument(std::move(document)).snapshot();
    QCOMPARE(normalized.renderSyncMarkers.size(), std::size_t(2));
    QCOMPARE(normalized.exportRequest.renderSyncMarkerCount, std::size_t(2));
    QCOMPARE(normalized.renderSyncMarkers.at(0).frame, std::int64_t(45));
    QCOMPARE(normalized.renderSyncMarkers.at(0).skipFrame, true);
    QCOMPARE(normalized.renderSyncMarkers.at(0).count, 9);
    QCOMPARE(normalized.renderSyncMarkers.at(1).frame, std::int64_t(60));
}

void TestEditorRuntime::testRenderSyncMarkerCountClampsToQtRange()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{1, 45, false, -20}}).applied);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.front().count,
             jcut::kEditorRenderSyncMinCount);

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{1, 45, false, 999}}).applied);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.front().count,
             jcut::kEditorRenderSyncMaxCount);

    jcut::EditorDocumentCore document = runtime.snapshot();
    const std::string clipId = document.clips.front().persistentId;
    document.renderSyncMarkers = {
        {clipId, 30, false, 0},
        {clipId, 60, true, 121},
    };
    const jcut::EditorDocumentCore normalized =
        jcut::EditorRuntime::fromDocument(std::move(document)).snapshot();
    QCOMPARE(normalized.renderSyncMarkers.at(0).count,
             jcut::kEditorRenderSyncMinCount);
    QCOMPARE(normalized.renderSyncMarkers.at(1).count,
             jcut::kEditorRenderSyncMaxCount);
}

void TestEditorRuntime::testRenderSyncMarkerRemovalIsScopedAndUndoable()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    applyCommand(runtime, jcut::AddRenderSyncMarkerCommand{1, 45, true, 2});
    applyCommand(runtime, jcut::AddRenderSyncMarkerCommand{1, 60, false, 3});

    const jcut::EditorDocumentCore initial = runtime.snapshot();
    QCOMPARE(initial.renderSyncMarkers.size(), std::size_t(2));
    const std::string clipId = initial.renderSyncMarkers.front().clipId;
    const std::size_t undoDepth = runtime.undoDepth();

    const jcut::CommandResult missing = runtime.execute(jcut::EditorCommand{
        jcut::RemoveRenderSyncMarkerCommand{clipId, 999, true}});
    QCOMPARE(missing.applied, false);
    QCOMPARE(runtime.undoDepth(), undoDepth);

    runtime.beginHistoryTransaction();
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::RemoveRenderSyncMarkerCommand{clipId, 45, true}}).applied);
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{1, 75, false, 4}}).applied);
    runtime.endHistoryTransaction();
    QCOMPARE(runtime.undoDepth(), undoDepth + 1);
    const jcut::EditorDocumentCore replacedSnapshot = runtime.snapshot();
    QCOMPARE(replacedSnapshot.renderSyncMarkers.size(), std::size_t(2));
    const auto reframed = std::find_if(
        replacedSnapshot.renderSyncMarkers.begin(),
        replacedSnapshot.renderSyncMarkers.end(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.frame == 75;
        });
    QVERIFY(reframed != replacedSnapshot.renderSyncMarkers.end());
    QCOMPARE(reframed->skipFrame, false);
    QCOMPARE(reframed->count, 4);

    applyCommand(runtime, jcut::UndoCommand{});
    QVERIFY(jcut::toJson(runtime.snapshot()) == jcut::toJson(initial));
    applyCommand(runtime, jcut::RedoCommand{});

    const jcut::CommandResult removed = runtime.execute(jcut::EditorCommand{
        jcut::RemoveRenderSyncMarkerCommand{clipId, 75, false}});
    QCOMPARE(removed.applied, true);
    QCOMPARE(runtime.undoDepth(), undoDepth + 2);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.front().frame, std::int64_t(60));
    QCOMPARE(runtime.snapshot().exportRequest.renderSyncMarkerCount, std::size_t(1));

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(2));
    applyCommand(runtime, jcut::RedoCommand{});
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));
}

void TestEditorRuntime::testMaskMatteRenderSyncOwnershipIsCanonical()
{
    const nlohmann::json state = {
        {"projectName", "Mask ownership"},
        {"selectedClipId", "mask-child"},
        {"tracks", nlohmann::json::array({
            {{"name", "Video"}}, {{"name", "Masks"}}
        })},
        {"timeline", nlohmann::json::array({
            {
                {"id", "source"}, {"clipRole", "media"},
                {"mediaType", "video"}, {"trackIndex", 0},
                {"startFrame", 0}, {"durationFrames", 90}
            },
            {
                {"id", "mask-child"}, {"clipRole", " MASK "},
                {"linkedSourceClipId", " source "},
                {"mediaType", "video"}, {"trackIndex", 1},
                {"startFrame", 0}, {"durationFrames", 90}
            },
            {
                {"id", "future"}, {"clipRole", "FutureCompositeV2"},
                {"mediaType", "video"}, {"trackIndex", 0},
                {"startFrame", 100}, {"durationFrames", 30}
            },
            {
                {"id", "orphan-mask"}, {"clipRole", "mask_matte"},
                {"linkedSourceClipId", " missing-source "},
                {"mediaType", "video"}, {"trackIndex", 1},
                {"startFrame", 100}, {"durationFrames", 30}
            }
        })},
        {"renderSyncMarkers", nlohmann::json::array({
            {{"clipId", "source"}, {"frame", 45},
             {"action", "duplicate"}, {"count", 2}},
            {{"clipId", "mask-child"}, {"frame", 45},
             {"action", "skip"}, {"count", 7}},
            {{"clipId", "orphan-mask"}, {"frame", 110},
             {"action", "duplicate"}, {"count", 3}}
        })}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> parsed =
        jcut::editorDocumentCoreFromJson(state, &error);
    QVERIFY2(parsed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(parsed->clips.at(1).clipRole),
             QStringLiteral("mask_matte"));
    QCOMPARE(QString::fromStdString(parsed->clips.at(1).linkedSourceClipId),
             QStringLiteral("source"));
    QCOMPARE(QString::fromStdString(parsed->clips.at(2).clipRole),
             QStringLiteral("FutureCompositeV2"));

    const nlohmann::json preserved =
        jcut::toLegacyStateJson(*parsed, &state);
    QCOMPARE(QString::fromStdString(
                 preserved.at("timeline").at(1).value(
                     "clipRole", std::string{})),
             QStringLiteral("mask_matte"));
    QCOMPARE(QString::fromStdString(
                 preserved.at("timeline").at(2).value(
                     "clipRole", std::string{})),
             QStringLiteral("FutureCompositeV2"));

    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(*parsed, false);
    QCOMPARE(renderData.clips.at(1).clipRole, ClipRole::MaskMatte);
    QCOMPARE(renderData.clips.at(1).linkedSourceClipId,
             QStringLiteral("source"));
    QVector<TimelineClip> bridgedClips;
    bridgedClips.reserve(static_cast<qsizetype>(renderData.clips.size()));
    for (const TimelineClip& clip : renderData.clips) {
        bridgedClips.push_back(clip);
    }
    QVector<TimelineTrack> bridgedTracks;
    bridgedTracks.reserve(static_cast<qsizetype>(renderData.tracks.size()));
    for (const TimelineTrack& track : renderData.tracks) {
        bridgedTracks.push_back(track);
    }
    const jcut::EditorDocumentCore bridgedBack =
        jcut::buildEditorDocumentCore(
            QStringLiteral("Mask ownership"),
            bridgedClips,
            bridgedTracks);
    QCOMPARE(QString::fromStdString(bridgedBack.clips.at(1).clipRole),
             QStringLiteral("mask_matte"));
    QCOMPARE(QString::fromStdString(
                 bridgedBack.clips.at(1).linkedSourceClipId),
             QStringLiteral("source"));

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(*parsed);
    const jcut::EditorDocumentCore normalized = runtime.snapshot();
    QCOMPARE(normalized.renderSyncMarkers.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(
                 normalized.renderSyncMarkers.at(0).clipId),
             QStringLiteral("source"));
    QCOMPARE(normalized.renderSyncMarkers.at(0).skipFrame, true);
    QCOMPARE(normalized.renderSyncMarkers.at(0).count, 7);
    // Match Qt exactly: an orphan mask still canonicalizes to its nonempty
    // linked owner even when that source is not present in the current model.
    QCOMPARE(QString::fromStdString(
                 normalized.renderSyncMarkers.at(1).clipId),
             QStringLiteral("missing-source"));

    const int childId = normalized.clips.at(1).id;
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::AddRenderSyncMarkerCommand{childId, 45, false, 9}}).applied);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().renderSyncMarkers.at(0).skipFrame, false);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.at(0).count, 9);
    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::ClearRenderSyncMarkersCommand{childId}}).applied);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(
                 runtime.snapshot().renderSyncMarkers.front().clipId),
             QStringLiteral("missing-source"));
    QVERIFY(runtime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(runtime.snapshot().renderSyncMarkers.size(), std::size_t(2));
}

void TestEditorRuntime::testGeneratedChildTrackRoundTripsAcrossNeutralAndQtBridges()
{
    const nlohmann::json legacy = {
        {"projectName", "Generated child tracks"},
        {"selectedTrackIndex", 0},
        {"tracks", nlohmann::json::array({
            {{"name", "Source"}, {"height", 72}},
            {
                {"name", "Persisted mask row"},
                {"generatedChildTrack", true},
                {"parentClipId", "source"},
                {"childClipId", "mask"},
                {"height", 48},
                {"visualMode", 2},
                {"gradingPreviewEnabled", false},
                {"audioEnabled", true},
                {"qtOnlyTrackMetadata", "preserve-me"}
            }
        })},
        {"timeline", nlohmann::json::array({
            {
                {"id", "source"}, {"clipRole", "media"},
                {"label", "Source"}, {"filePath", "/tmp/source.mov"},
                {"mediaType", "video"}, {"trackIndex", 0},
                {"startFrame", 0}, {"durationFrames", 90}
            },
            {
                {"id", "mask"}, {"clipRole", "mask_matte"},
                {"linkedSourceClipId", "source"},
                {"label", "Person Mask"},
                {"filePath", "/tmp/source.mov"},
                {"mediaType", "video"}, {"trackIndex", 1},
                {"startFrame", 0}, {"durationFrames", 90}
            }
        })}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> parsed =
        jcut::editorDocumentCoreFromJson(legacy, &error);
    QVERIFY2(parsed.has_value(), error.c_str());
    QCOMPARE(parsed->tracks.size(), std::size_t(2));
    const jcut::EditorTrack& parsedLane = parsed->tracks.at(1);
    QVERIFY(jcut::isGeneratedEditorChildTrack(parsedLane));
    QCOMPARE(QString::fromStdString(parsedLane.parentClipId),
             QStringLiteral("source"));
    QCOMPARE(QString::fromStdString(parsedLane.childClipId),
             QStringLiteral("mask"));

    const nlohmann::json coreJson = jcut::toJson(*parsed);
    QCOMPARE(coreJson.at("tracks").at(1).value(
                 "generatedChildTrack", false),
             true);
    const std::optional<jcut::EditorDocumentCore> coreRoundTrip =
        jcut::editorDocumentCoreFromJson(coreJson, &error);
    QVERIFY2(coreRoundTrip.has_value(), error.c_str());
    QVERIFY(coreRoundTrip->tracks.at(1).generatedChildTrack);
    QCOMPARE(QString::fromStdString(coreRoundTrip->tracks.at(1).childClipId),
             QStringLiteral("mask"));

    const nlohmann::json savedLegacy =
        jcut::toLegacyStateJson(*parsed, &legacy);
    const nlohmann::json& savedLane = savedLegacy.at("tracks").at(1);
    QCOMPARE(savedLane.value("generatedChildTrack", false), true);
    QCOMPARE(QString::fromStdString(
                 savedLane.value("parentClipId", std::string{})),
             QStringLiteral("source"));
    QCOMPARE(QString::fromStdString(
                 savedLane.value("childClipId", std::string{})),
             QStringLiteral("mask"));
    QCOMPARE(QString::fromStdString(
                 savedLane.value("qtOnlyTrackMetadata", std::string{})),
             QStringLiteral("preserve-me"));

    const jcut::EditorDocumentCore normalized =
        jcut::EditorRuntime::fromDocument(*parsed).snapshot();
    QCOMPARE(normalized.tracks.size(), std::size_t(2));
    const jcut::EditorTrack& normalizedLane = normalized.tracks.at(1);
    QVERIFY(normalizedLane.generatedChildTrack);
    QCOMPARE(QString::fromStdString(normalizedLane.label),
             QStringLiteral("↳ Person Mask"));
    QCOMPARE(normalizedLane.height, 48);
    QCOMPARE(normalizedLane.visualMode, 2);
    QCOMPARE(normalizedLane.gradingPreviewEnabled, false);
    QCOMPARE(normalizedLane.audioEnabled, false);
    QCOMPARE(normalizedLane.audioWaveformVisible, false);

    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(normalized, false);
    QCOMPARE(renderData.tracks.size(), std::size_t(2));
    QVERIFY(renderData.tracks.at(1).generatedChildTrack);
    QCOMPARE(renderData.tracks.at(1).parentClipId,
             QStringLiteral("source"));
    QCOMPARE(renderData.tracks.at(1).childClipId,
             QStringLiteral("mask"));

    QVector<TimelineClip> qtClips;
    for (const TimelineClip& clip : renderData.clips) {
        qtClips.push_back(clip);
    }
    QVector<TimelineTrack> qtTracks;
    for (const TimelineTrack& track : renderData.tracks) {
        qtTracks.push_back(track);
    }
    const jcut::EditorDocumentCore bridgedBack =
        jcut::buildEditorDocumentCore(
            QStringLiteral("Generated child tracks"), qtClips, qtTracks);
    QVERIFY(bridgedBack.tracks.at(1).generatedChildTrack);
    QCOMPARE(QString::fromStdString(bridgedBack.tracks.at(1).parentClipId),
             QStringLiteral("source"));
    QCOMPARE(QString::fromStdString(bridgedBack.tracks.at(1).childClipId),
             QStringLiteral("mask"));
}

void TestEditorRuntime::testTranscriptGeneratedTitlesUseImmutableChildTrack()
{
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Transcript", true});
    jcut::EditorClip source;
    source.id = 1;
    source.trackId = 1;
    source.persistentId = "transcript-source";
    source.label = "Interview";
    source.mediaKind = "video";
    source.startFrame = 100;
    source.durationFrames = 300;
    source.sourceDurationFrames = 300;
    source.selected = true;
    document.clips.push_back(source);

    const auto generatedTitle = [](int startFrame,
                                   const std::string& text) {
        jcut::EditorClip title;
        title.label = "Speaker: " + text;
        title.mediaKind = "title";
        title.startFrame = startFrame;
        title.durationFrames = 90;
        jcut::EditorTitleKeyframe keyframe;
        keyframe.text = text;
        title.titleKeyframes.push_back(std::move(keyframe));
        return title;
    };

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    const jcut::CommandResult generated = runtime.execute(
        jcut::EditorCommand{
            jcut::ReplaceSpeakerTitleClipsCommand{
                1,
                {generatedTitle(120, "Alice"),
                 generatedTitle(150, "Bob")}}});
    QVERIFY2(generated.applied, generated.message.c_str());

    jcut::EditorDocumentCore snapshot = runtime.snapshot();
    QCOMPARE(snapshot.tracks.size(), std::size_t(2));
    const jcut::EditorTrack& childTrack = snapshot.tracks.at(1);
    QVERIFY(childTrack.generatedChildTrack);
    QCOMPARE(QString::fromStdString(childTrack.parentClipId),
             QStringLiteral("transcript-source"));
    QCOMPARE(QString::fromStdString(childTrack.label),
             QStringLiteral("↳ Transcript • Speaker Introductions"));

    std::vector<const jcut::EditorClip*> titles;
    for (const jcut::EditorClip& clip : snapshot.clips) {
        if (jcut::isTranscriptGeneratedEditorTitle(clip)) {
            titles.push_back(&clip);
        }
    }
    QCOMPARE(titles.size(), std::size_t(2));
    QCOMPARE(titles.at(0)->trackId, childTrack.id);
    QCOMPARE(titles.at(1)->trackId, childTrack.id);
    QVERIFY(titles.at(0)->locked);
    QVERIFY(titles.at(1)->locked);
    QCOMPARE(QString::fromStdString(childTrack.childClipId),
             QString::fromStdString(titles.at(0)->persistentId));

    jcut::EditorTitleKeyframe directEdit =
        titles.at(0)->titleKeyframes.front();
    directEdit.text = "Manual override";
    const int generatedClipId = titles.at(0)->id;
    QCOMPARE(runtime.execute(jcut::EditorCommand{
                 jcut::UpsertTitleKeyframeCommand{
                     generatedClipId, directEdit}})
                 .applied,
             false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
                 jcut::MoveClipCommand{
                     generatedClipId, childTrack.id, 200}})
                 .applied,
             false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
                 jcut::DeleteClipCommand{generatedClipId}})
                 .applied,
             false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
                 jcut::SetClipLockedCommand{
                     generatedClipId, false}})
                 .applied,
             false);

    const jcut::CommandResult regenerated = runtime.execute(
        jcut::EditorCommand{
            jcut::ReplaceSpeakerTitleClipsCommand{
                1, {generatedTitle(130, "Alice Updated")}}});
    QVERIFY2(regenerated.applied, regenerated.message.c_str());
    snapshot = runtime.snapshot();
    QCOMPARE(std::count_if(
                 snapshot.clips.cbegin(),
                 snapshot.clips.cend(),
                 [](const jcut::EditorClip& clip) {
                     return jcut::isTranscriptGeneratedEditorTitle(
                         clip);
                 }),
             1);
    QCOMPARE(snapshot.tracks.size(), std::size_t(2));
    QVERIFY(snapshot.tracks.at(1).generatedChildTrack);
}

void TestEditorRuntime::testGeneratedChildTrackReconciliationAndPoliciesAreUndoable()
{
    const auto malformedDocument = [] {
        jcut::EditorDocumentCore document;
        document.mediaItems.push_back(
            {"media-new", "New Media", "video", true, true});

        jcut::EditorTrack sourceATrack{1, "Source A", true};
        jcut::EditorTrack childATrack{2, "Old child row", false};
        childATrack.generatedChildTrack = true;
        childATrack.parentClipId = "stale-parent";
        childATrack.childClipId = "mask-a";
        childATrack.height = 40;
        childATrack.visualMode = 2;
        childATrack.gradingPreviewEnabled = false;
        childATrack.audioEnabled = true;
        jcut::EditorTrack neutralTrack{3, "Neutral", false};
        jcut::EditorTrack sourceBTrack{4, "Source B", false};
        jcut::EditorTrack staleEmptyTrack{7, "Stale child", false};
        staleEmptyTrack.generatedChildTrack = true;
        staleEmptyTrack.parentClipId = "missing-source";
        staleEmptyTrack.childClipId = "missing-child";
        jcut::EditorTrack opaqueFutureTrack{8, "Future child", false};
        opaqueFutureTrack.generatedChildTrack = true;
        opaqueFutureTrack.parentClipId = "future-parent";
        opaqueFutureTrack.childClipId = "future-child";
        document.tracks = {
            sourceATrack,
            childATrack,
            neutralTrack,
            sourceBTrack,
            staleEmptyTrack,
            opaqueFutureTrack,
        };

        jcut::EditorClip sourceA;
        sourceA.id = 1;
        sourceA.trackId = 1;
        sourceA.persistentId = "source-a";
        sourceA.label = "Source A";
        sourceA.mediaKind = "video";
        sourceA.durationFrames = 90;
        sourceA.selected = true;
        sourceA.clipRole = "media";
        jcut::EditorClip maskA = sourceA;
        maskA.id = 2;
        maskA.trackId = 2;
        maskA.persistentId = "mask-a";
        maskA.label = "Person Mask";
        maskA.selected = false;
        maskA.clipRole = "mask_matte";
        maskA.linkedSourceClipId = "source-a";
        jcut::EditorClip foreign = sourceA;
        foreign.id = 3;
        foreign.trackId = 2;
        foreign.persistentId = "foreign";
        foreign.label = "Recovered Media";
        foreign.startFrame = 200;
        foreign.durationFrames = 30;
        foreign.selected = false;
        jcut::EditorClip sourceB = sourceA;
        sourceB.id = 4;
        sourceB.trackId = 4;
        sourceB.persistentId = "source-b";
        sourceB.label = "Source B";
        sourceB.startFrame = 100;
        sourceB.selected = false;
        jcut::EditorClip maskB = sourceB;
        maskB.id = 5;
        maskB.persistentId = "mask-b";
        maskB.label = "Alpha Mask";
        maskB.clipRole = "mask_matte";
        maskB.linkedSourceClipId = "source-b";
        // Deliberately shares its parent's base row; reconciliation must give
        // it a dedicated generated lane.
        maskB.trackId = 4;
        jcut::EditorClip future = sourceA;
        future.id = 6;
        future.trackId = 8;
        future.persistentId = "future-child";
        future.label = "Future Child";
        future.startFrame = 300;
        future.durationFrames = 30;
        future.selected = false;
        future.clipRole = "future_composite_v2";
        document.clips = {
            sourceA, maskA, foreign, sourceB, maskB, future};
        return document;
    };

    const auto trackById = [](const jcut::EditorDocumentCore& document,
                              int trackId) -> const jcut::EditorTrack* {
        const auto track = std::find_if(
            document.tracks.cbegin(), document.tracks.cend(),
            [&](const jcut::EditorTrack& candidate) {
                return candidate.id == trackId;
            });
        return track == document.tracks.cend() ? nullptr : &*track;
    };
    const auto clipByPersistentId = [](
        const jcut::EditorDocumentCore& document,
        const std::string& clipId) -> const jcut::EditorClip* {
        const auto clip = std::find_if(
            document.clips.cbegin(), document.clips.cend(),
            [&](const jcut::EditorClip& candidate) {
                return candidate.persistentId == clipId;
            });
        return clip == document.clips.cend() ? nullptr : &*clip;
    };

    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(malformedDocument());
    jcut::EditorDocumentCore normalized = runtime.snapshot();
    const jcut::EditorTrack* maskALane = trackById(normalized, 2);
    QVERIFY(maskALane);
    QVERIFY(maskALane->generatedChildTrack);
    QCOMPARE(QString::fromStdString(maskALane->parentClipId),
             QStringLiteral("source-a"));
    QCOMPARE(QString::fromStdString(maskALane->label),
             QStringLiteral("↳ Person Mask"));
    QCOMPARE(maskALane->height, 40);
    QCOMPARE(maskALane->visualMode, 2);
    QCOMPARE(maskALane->gradingPreviewEnabled, false);
    QCOMPARE(maskALane->audioEnabled, false);
    QVERIFY(!trackById(normalized, 7));
    const jcut::EditorTrack* opaqueFutureTrack = trackById(normalized, 8);
    QVERIFY(opaqueFutureTrack);
    QVERIFY(opaqueFutureTrack->generatedChildTrack);

    const jcut::EditorClip* maskB =
        clipByPersistentId(normalized, "mask-b");
    const jcut::EditorClip* foreign =
        clipByPersistentId(normalized, "foreign");
    QVERIFY(maskB);
    QVERIFY(foreign);
    QVERIFY(maskB->trackId != 4);
    const jcut::EditorTrack* maskBLane = trackById(normalized, maskB->trackId);
    QVERIFY(maskBLane);
    QVERIFY(maskBLane->generatedChildTrack);
    QCOMPARE(QString::fromStdString(maskBLane->parentClipId),
             QStringLiteral("source-b"));
    QVERIFY(foreign->trackId != 2);
    const jcut::EditorTrack* recoveredTrack =
        trackById(normalized, foreign->trackId);
    QVERIFY(recoveredTrack);
    QVERIFY(!recoveredTrack->generatedChildTrack);
    QCOMPARE(QString::fromStdString(recoveredTrack->label),
             QStringLiteral("Recovered Media"));

    const auto indexForTrackId = [](const jcut::EditorDocumentCore& document,
                                    int trackId) {
        const auto found = std::find_if(
            document.tracks.cbegin(), document.tracks.cend(),
            [&](const jcut::EditorTrack& track) {
                return track.id == trackId;
            });
        return found == document.tracks.cend()
            ? -1
            : static_cast<int>(std::distance(document.tracks.cbegin(), found));
    };
    QCOMPARE(indexForTrackId(normalized, 2),
             indexForTrackId(normalized, 1) + 1);
    QCOMPARE(indexForTrackId(normalized, maskB->trackId),
             indexForTrackId(normalized, 4) + 1);

    const int maskALaneIndex = indexForTrackId(normalized, 2);
    const int routedTrackIndex = jcut::firstNonConflictingTrackIndex(
        normalized, maskALaneIndex, "video", 400, 30);
    QVERIFY(routedTrackIndex >= 0);
    QVERIFY(!normalized.tracks.at(
        static_cast<std::size_t>(routedTrackIndex)).generatedChildTrack);
    QVERIFY(routedTrackIndex != maskALaneIndex);

    jcut::EditorRuntime insertionRuntime =
        jcut::EditorRuntime::fromDocument(malformedDocument());
    const int generatedDropIndex =
        indexForTrackId(insertionRuntime.snapshot(), 2);
    QVERIFY(generatedDropIndex >= 0);
    QVERIFY(insertionRuntime.execute(jcut::EditorCommand{
        jcut::AddTrackCommand{"Dropped Video", generatedDropIndex}}).applied);
    jcut::EditorDocumentCore insertedLaneDocument =
        insertionRuntime.snapshot();
    QCOMPARE(QString::fromStdString(
                 insertedLaneDocument.tracks.at(
                     static_cast<std::size_t>(generatedDropIndex)).label),
             QStringLiteral("Dropped Video"));
    QVERIFY(!insertedLaneDocument.tracks.at(
        static_cast<std::size_t>(generatedDropIndex)).generatedChildTrack);
    QCOMPARE(insertedLaneDocument.tracks.at(
                 static_cast<std::size_t>(generatedDropIndex + 1)).id,
             2);
    const int insertedTrackId = insertedLaneDocument.tracks.at(
        static_cast<std::size_t>(generatedDropIndex)).id;
    QVERIFY(insertionRuntime.execute(jcut::EditorCommand{
        jcut::InsertClipFromMediaCommand{
            "media-new", insertedTrackId, 400, 30}}).applied);
    const jcut::EditorDocumentCore afterDropInsert =
        insertionRuntime.snapshot();
    QVERIFY(std::any_of(
        afterDropInsert.clips.cbegin(), afterDropInsert.clips.cend(),
        [&](const jcut::EditorClip& clip) {
            return clip.sourcePath == "media-new" &&
                clip.trackId == insertedTrackId;
        }));

    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::InsertClipFromMediaCommand{"media-new", 2, 400, 30}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::AddClipCommand{2, "Rejected", 400, 30, {}, "video"}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::MoveClipCommand{1, 2, 10}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::MoveSelectedClipsCommand{1, 2, 10}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::DeleteTrackCommand{2}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::ReorderTrackCommand{2, 0}}).applied,
        false);
    QCOMPARE(runtime.execute(jcut::EditorCommand{
        jcut::ReorderTrackCommand{1, maskALaneIndex}}).applied,
        false);

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::SetTrackPropertiesCommand{2, "Hacked label", 55}}).applied);
    normalized = runtime.snapshot();
    maskALane = trackById(normalized, 2);
    QVERIFY(maskALane);
    QCOMPARE(QString::fromStdString(maskALane->label),
             QStringLiteral("↳ Person Mask"));
    QCOMPARE(maskALane->height, 55);
    QVERIFY(runtime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    normalized = runtime.snapshot();
    maskALane = trackById(normalized, 2);
    QVERIFY(maskALane);
    QCOMPARE(maskALane->height, 40);

    QVERIFY(runtime.execute(jcut::EditorCommand{
        jcut::SetTrackStateCommand{2, 1, true, 2.0, true, true, true}}).applied);
    normalized = runtime.snapshot();
    maskALane = trackById(normalized, 2);
    QVERIFY(maskALane);
    QCOMPARE(maskALane->visualMode, 1);
    QCOMPARE(maskALane->audioEnabled, false);
    QCOMPARE(maskALane->audioMuted, false);
    QCOMPARE(maskALane->audioSolo, false);
    QCOMPARE(maskALane->gradingPreviewEnabled, true);

    jcut::EditorRuntime reorderRuntime =
        jcut::EditorRuntime::fromDocument(malformedDocument());
    const nlohmann::json beforeReorder =
        jcut::toJson(reorderRuntime.snapshot());
    const int sourceBIndex = indexForTrackId(reorderRuntime.snapshot(), 4);
    QVERIFY(sourceBIndex >= 0);
    QVERIFY(reorderRuntime.execute(jcut::EditorCommand{
        jcut::ReorderTrackCommand{4, 0}}).applied);
    const jcut::EditorDocumentCore reordered = reorderRuntime.snapshot();
    QCOMPARE(reordered.tracks.front().id, 4);
    const jcut::EditorClip* reorderedMaskB =
        clipByPersistentId(reordered, "mask-b");
    QVERIFY(reorderedMaskB);
    QCOMPARE(reordered.tracks.at(1).id, reorderedMaskB->trackId);
    QVERIFY(reorderRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(reorderRuntime.snapshot()) == beforeReorder);

    jcut::EditorRuntime deleteRuntime =
        jcut::EditorRuntime::fromDocument(malformedDocument());
    QVERIFY(deleteRuntime.execute(jcut::EditorCommand{
        jcut::DeleteTrackCommand{1}}).applied);
    const jcut::EditorDocumentCore deleted = deleteRuntime.snapshot();
    QVERIFY(!clipByPersistentId(deleted, "source-a"));
    QVERIFY(!clipByPersistentId(deleted, "mask-a"));
    QVERIFY(!trackById(deleted, 2));
    QVERIFY(deleteRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    const jcut::EditorDocumentCore restoredDelete = deleteRuntime.snapshot();
    QVERIFY(clipByPersistentId(restoredDelete, "source-a"));
    QVERIFY(clipByPersistentId(restoredDelete, "mask-a"));
    QVERIFY(trackById(restoredDelete, 2));
}

void TestEditorRuntime::testMaskMatteCloneAndSplitRelationshipsStayValid()
{
    const auto relatedDocument = [] {
        jcut::EditorDocumentCore document;
        document.tracks = {
            {1, "Video", true},
            {2, "Masks", false},
        };
        jcut::EditorClip source;
        source.id = 1;
        source.trackId = 1;
        source.persistentId = "source";
        source.label = "Source";
        source.mediaKind = "video";
        source.startFrame = 0;
        source.durationFrames = 90;
        source.selected = true;
        source.clipRole = "media";
        jcut::EditorClip mask = source;
        mask.id = 2;
        mask.trackId = 2;
        mask.persistentId = "mask";
        mask.label = "Mask";
        mask.selected = true;
        mask.clipRole = "mask_matte";
        mask.linkedSourceClipId = "source";
        document.clips = {source, mask};
        document.renderSyncMarkers = {{"source", 60, false, 2}};
        return document;
    };

    jcut::EditorDocumentCore parentOnlyCopy = relatedDocument();
    parentOnlyCopy.clips.back().selected = false;
    jcut::EditorRuntime cloneRuntime =
        jcut::EditorRuntime::fromDocument(std::move(parentOnlyCopy));
    QVERIFY(cloneRuntime.execute(
        jcut::EditorCommand{jcut::CopySelectedClipsCommand{}}).applied);
    QVERIFY(cloneRuntime.execute(jcut::EditorCommand{
        jcut::PasteClipsCommand{100, 1}}).applied);
    const jcut::EditorDocumentCore pasted = cloneRuntime.snapshot();
    const auto pastedSource = std::find_if(
        pasted.clips.begin(), pasted.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.selected &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "media";
        });
    const auto pastedMask = std::find_if(
        pasted.clips.begin(), pasted.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.selected &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte";
        });
    QVERIFY(pastedSource != pasted.clips.end());
    QVERIFY(pastedMask != pasted.clips.end());
    QCOMPARE(QString::fromStdString(pastedMask->linkedSourceClipId),
             QString::fromStdString(pastedSource->persistentId));
    QVERIFY(pastedMask->linkedSourceClipId != "source");
    const auto pastedMarker = std::find_if(
        pasted.renderSyncMarkers.begin(), pasted.renderSyncMarkers.end(),
        [&](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == pastedSource->persistentId;
        });
    QVERIFY(pastedMarker != pasted.renderSyncMarkers.end());

    jcut::EditorDocumentCore childOnly = relatedDocument();
    childOnly.clips.front().selected = false;
    jcut::EditorClip siblingMask = childOnly.clips.back();
    siblingMask.id = 3;
    siblingMask.persistentId = "mask-sibling";
    siblingMask.label = "Mask Sibling";
    siblingMask.selected = false;
    childOnly.clips.push_back(std::move(siblingMask));
    jcut::EditorRuntime childOnlyRuntime =
        jcut::EditorRuntime::fromDocument(std::move(childOnly));
    QVERIFY(childOnlyRuntime.execute(
        jcut::EditorCommand{jcut::CopySelectedClipsCommand{}}).applied);
    QVERIFY(childOnlyRuntime.execute(jcut::EditorCommand{
        jcut::PasteClipsCommand{200, 1}}).applied);
    const jcut::EditorDocumentCore pastedFromChild =
        childOnlyRuntime.snapshot();
    QCOMPARE(pastedFromChild.clips.size(), std::size_t(6));
    const auto childCopiedSource = std::find_if(
        pastedFromChild.clips.begin(), pastedFromChild.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.selected &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "media";
        });
    const auto childCopiedMask = std::find_if(
        pastedFromChild.clips.begin(), pastedFromChild.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.selected &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte";
        });
    QVERIFY(childCopiedSource != pastedFromChild.clips.end());
    QVERIFY(childCopiedMask != pastedFromChild.clips.end());
    QCOMPARE(std::count_if(
                 pastedFromChild.clips.begin(), pastedFromChild.clips.end(),
                 [](const jcut::EditorClip& clip) {
                     return clip.selected &&
                         jcut::canonicalEditorClipRole(clip.clipRole) ==
                             "mask_matte";
                 }),
             std::ptrdiff_t(2));
    for (const jcut::EditorClip& clip : pastedFromChild.clips) {
        if (!clip.selected ||
            jcut::canonicalEditorClipRole(clip.clipRole) != "mask_matte") {
            continue;
        }
        QCOMPARE(QString::fromStdString(clip.linkedSourceClipId),
                 QString::fromStdString(childCopiedSource->persistentId));
    }
    QCOMPARE(QString::fromStdString(childCopiedMask->linkedSourceClipId),
             QString::fromStdString(childCopiedSource->persistentId));
    const auto childCopiedMarker = std::find_if(
        pastedFromChild.renderSyncMarkers.begin(),
        pastedFromChild.renderSyncMarkers.end(),
        [&](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == childCopiedSource->persistentId;
        });
    QVERIFY(childCopiedMarker != pastedFromChild.renderSyncMarkers.end());

    jcut::EditorDocumentCore childOnlyMutation = relatedDocument();
    childOnlyMutation.clips.front().selected = false;
    jcut::EditorRuntime childOnlyMutationRuntime =
        jcut::EditorRuntime::fromDocument(std::move(childOnlyMutation));
    const nlohmann::json beforeChildOnlyMutation =
        jcut::toJson(childOnlyMutationRuntime.snapshot());
    QCOMPARE(childOnlyMutationRuntime.execute(jcut::EditorCommand{
        jcut::CutSelectedClipsCommand{}}).applied, false);
    QCOMPARE(childOnlyMutationRuntime.execute(jcut::EditorCommand{
        jcut::DeleteSelectedClipsCommand{}}).applied, false);
    QCOMPARE(childOnlyMutationRuntime.execute(jcut::EditorCommand{
        jcut::DeleteClipCommand{2}}).applied, false);
    QCOMPARE(childOnlyMutationRuntime.execute(jcut::EditorCommand{
        jcut::DeleteTrackCommand{2}}).applied, false);
    QVERIFY(jcut::toJson(childOnlyMutationRuntime.snapshot()) ==
            beforeChildOnlyMutation);

    jcut::EditorDocumentCore parentOnlyCut = relatedDocument();
    parentOnlyCut.clips.back().selected = false;
    jcut::EditorRuntime cutRuntime =
        jcut::EditorRuntime::fromDocument(std::move(parentOnlyCut));
    QVERIFY(cutRuntime.execute(jcut::EditorCommand{
        jcut::CutSelectedClipsCommand{}}).applied);
    QCOMPARE(cutRuntime.snapshot().clips.size(), std::size_t(0));
    QCOMPARE(cutRuntime.snapshot().renderSyncMarkers.size(), std::size_t(0));
    QVERIFY(cutRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(cutRuntime.snapshot().clips.size(), std::size_t(2));
    QCOMPARE(cutRuntime.snapshot().renderSyncMarkers.size(), std::size_t(1));

    jcut::EditorRuntime deleteRuntime =
        jcut::EditorRuntime::fromDocument(relatedDocument());
    QVERIFY(deleteRuntime.execute(jcut::EditorCommand{
        jcut::DeleteClipCommand{1}}).applied);
    QCOMPARE(deleteRuntime.snapshot().clips.size(), std::size_t(0));
    QCOMPARE(deleteRuntime.snapshot().renderSyncMarkers.size(),
             std::size_t(0));
    QVERIFY(deleteRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QCOMPARE(deleteRuntime.snapshot().clips.size(), std::size_t(2));

    jcut::EditorDocumentCore parentOnlyDelete = relatedDocument();
    parentOnlyDelete.clips.back().selected = false;
    jcut::EditorRuntime deleteSelectedRuntime =
        jcut::EditorRuntime::fromDocument(std::move(parentOnlyDelete));
    QVERIFY(deleteSelectedRuntime.execute(jcut::EditorCommand{
        jcut::DeleteSelectedClipsCommand{}}).applied);
    QCOMPARE(deleteSelectedRuntime.snapshot().clips.size(), std::size_t(0));
    QCOMPARE(deleteSelectedRuntime.snapshot().renderSyncMarkers.size(),
             std::size_t(0));

    jcut::EditorRuntime deleteTrackRuntime =
        jcut::EditorRuntime::fromDocument(relatedDocument());
    QVERIFY(deleteTrackRuntime.execute(jcut::EditorCommand{
        jcut::DeleteTrackCommand{1}}).applied);
    QCOMPARE(deleteTrackRuntime.snapshot().tracks.size(), std::size_t(1));
    QCOMPARE(deleteTrackRuntime.snapshot().clips.size(), std::size_t(0));
    QCOMPARE(deleteTrackRuntime.snapshot().renderSyncMarkers.size(),
             std::size_t(0));

    jcut::EditorRuntime splitRuntime =
        jcut::EditorRuntime::fromDocument(relatedDocument());
    QVERIFY(splitRuntime.execute(jcut::EditorCommand{
        jcut::SplitClipCommand{1, 45}}).applied);
    const jcut::EditorDocumentCore split = splitRuntime.snapshot();
    QCOMPARE(split.clips.size(), std::size_t(4));
    const auto trailingSource = std::find_if(
        split.clips.begin(), split.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.startFrame == 45 &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "media";
        });
    const auto trailingMask = std::find_if(
        split.clips.begin(), split.clips.end(),
        [](const jcut::EditorClip& clip) {
            return clip.startFrame == 45 &&
                jcut::canonicalEditorClipRole(clip.clipRole) == "mask_matte";
        });
    QVERIFY(trailingSource != split.clips.end());
    QVERIFY(trailingMask != split.clips.end());
    QCOMPARE(QString::fromStdString(trailingMask->linkedSourceClipId),
             QString::fromStdString(trailingSource->persistentId));
    QCOMPARE(QString::fromStdString(split.renderSyncMarkers.front().clipId),
             QString::fromStdString(trailingSource->persistentId));
    QCOMPARE(split.renderSyncMarkers.front().frame, std::int64_t(60));

    jcut::EditorRuntime directChildSplit =
        jcut::EditorRuntime::fromDocument(relatedDocument());
    const nlohmann::json beforeChildSplit =
        jcut::toJson(directChildSplit.snapshot());
    QCOMPARE(directChildSplit.execute(jcut::EditorCommand{
        jcut::SplitClipCommand{2, 45}}).applied, false);
    QVERIFY(jcut::toJson(directChildSplit.snapshot()) == beforeChildSplit);

    const auto clipWithPersistentId = [](
        const jcut::EditorDocumentCore& document,
        const std::string& persistentId) -> const jcut::EditorClip* {
        const auto clip = std::find_if(
            document.clips.cbegin(), document.clips.cend(),
            [&](const jcut::EditorClip& candidate) {
                return candidate.persistentId == persistentId;
            });
        return clip == document.clips.cend() ? nullptr : &*clip;
    };

    jcut::EditorDocumentCore staleChild = relatedDocument();
    staleChild.clips.back().startFrame = 600;
    staleChild.clips.back().durationFrames = 3;
    staleChild.clips.back().sourceInFrame = 44;
    staleChild.clips.back().playbackRate = 3.0;
    staleChild.clips.back().baseTranslationX = 999.0;
    const jcut::EditorDocumentCore normalizedChild =
        jcut::EditorRuntime::fromDocument(std::move(staleChild)).snapshot();
    const jcut::EditorClip* normalizedSource =
        clipWithPersistentId(normalizedChild, "source");
    const jcut::EditorClip* normalizedMask =
        clipWithPersistentId(normalizedChild, "mask");
    QVERIFY(normalizedSource);
    QVERIFY(normalizedMask);
    QCOMPARE(normalizedMask->startFrame, normalizedSource->startFrame);
    QCOMPARE(normalizedMask->durationFrames, normalizedSource->durationFrames);
    QCOMPARE(normalizedMask->sourceInFrame, normalizedSource->sourceInFrame);
    QCOMPARE(normalizedMask->playbackRate, normalizedSource->playbackRate);
    QCOMPARE(normalizedMask->baseTranslationX,
             normalizedSource->baseTranslationX);

    jcut::EditorDocumentCore childOnlyTiming = relatedDocument();
    childOnlyTiming.clips.front().selected = false;
    childOnlyTiming.clips.back().locked = true;
    jcut::EditorRuntime childTimingRuntime =
        jcut::EditorRuntime::fromDocument(std::move(childOnlyTiming));
    const nlohmann::json beforeChildTiming =
        jcut::toJson(childTimingRuntime.snapshot());
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::MoveClipCommand{2, 1, 20}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::MoveSelectedClipsCommand{2, 1, 20}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::NudgeSelectedClipCommand{1}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::TrimClipStartCommand{2, 10}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::TrimClipEndCommand{2, 80}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::ResizeClipCommand{2, 60}}).applied, false);
    QCOMPARE(childTimingRuntime.execute(jcut::EditorCommand{
        jcut::SetClipPlaybackRateCommand{2, 2.0}}).applied, false);
    QVERIFY(jcut::toJson(childTimingRuntime.snapshot()) == beforeChildTiming);
    QCOMPARE(childTimingRuntime.canUndo(), false);

    jcut::EditorDocumentCore parentOnlyMove = relatedDocument();
    parentOnlyMove.tracks.push_back({3, "Destination", false});
    parentOnlyMove.clips.front().selected = true;
    parentOnlyMove.clips.back().selected = false;
    parentOnlyMove.clips.back().locked = true;
    jcut::EditorRuntime aggregateMoveRuntime =
        jcut::EditorRuntime::fromDocument(parentOnlyMove);
    const nlohmann::json beforeAggregateMove =
        jcut::toJson(aggregateMoveRuntime.snapshot());
    QVERIFY(aggregateMoveRuntime.execute(jcut::EditorCommand{
        jcut::MoveClipCommand{1, 3, 20}}).applied);
    jcut::EditorDocumentCore aggregateMoved = aggregateMoveRuntime.snapshot();
    const jcut::EditorClip* movedSource =
        clipWithPersistentId(aggregateMoved, "source");
    const jcut::EditorClip* movedMask =
        clipWithPersistentId(aggregateMoved, "mask");
    QVERIFY(movedSource);
    QVERIFY(movedMask);
    QCOMPARE(movedSource->trackId, 3);
    QCOMPARE(movedSource->startFrame, 20);
    QCOMPARE(movedMask->startFrame, 20);
    QCOMPARE(aggregateMoved.renderSyncMarkers.front().frame,
             std::int64_t(80));
    QVERIFY(aggregateMoveRuntime.execute(
        jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(aggregateMoveRuntime.snapshot()) ==
            beforeAggregateMove);

    QVERIFY(aggregateMoveRuntime.execute(jcut::EditorCommand{
        jcut::MoveSelectedClipsCommand{1, 3, 10}}).applied);
    aggregateMoved = aggregateMoveRuntime.snapshot();
    movedSource = clipWithPersistentId(aggregateMoved, "source");
    movedMask = clipWithPersistentId(aggregateMoved, "mask");
    QVERIFY(movedSource);
    QVERIFY(movedMask);
    QCOMPARE(movedSource->trackId, 3);
    QCOMPARE(movedSource->startFrame, 10);
    QCOMPARE(movedMask->startFrame, 10);
    QCOMPARE(aggregateMoved.renderSyncMarkers.front().frame,
             std::int64_t(70));

    jcut::EditorRuntime aggregateNudgeRuntime =
        jcut::EditorRuntime::fromDocument(parentOnlyMove);
    QVERIFY(aggregateNudgeRuntime.execute(jcut::EditorCommand{
        jcut::NudgeSelectedClipCommand{7}}).applied);
    const jcut::EditorDocumentCore nudgedAggregate =
        aggregateNudgeRuntime.snapshot();
    QCOMPARE(clipWithPersistentId(nudgedAggregate, "source")->startFrame, 7);
    QCOMPARE(clipWithPersistentId(nudgedAggregate, "mask")->startFrame, 7);
    QCOMPARE(nudgedAggregate.renderSyncMarkers.front().frame,
             std::int64_t(67));

    jcut::EditorRuntime aggregateTrimRuntime =
        jcut::EditorRuntime::fromDocument(parentOnlyMove);
    QVERIFY(aggregateTrimRuntime.execute(jcut::EditorCommand{
        jcut::TrimClipStartCommand{1, 15}}).applied);
    jcut::EditorDocumentCore trimmedAggregate =
        aggregateTrimRuntime.snapshot();
    QCOMPARE(clipWithPersistentId(trimmedAggregate, "mask")->startFrame, 15);
    QCOMPARE(clipWithPersistentId(trimmedAggregate, "mask")->durationFrames,
             75);
    QCOMPARE(clipWithPersistentId(trimmedAggregate, "mask")->sourceInFrame,
             std::int64_t(15));
    QVERIFY(aggregateTrimRuntime.execute(jcut::EditorCommand{
        jcut::TrimClipEndCommand{1, 70}}).applied);
    trimmedAggregate = aggregateTrimRuntime.snapshot();
    QCOMPARE(clipWithPersistentId(trimmedAggregate, "mask")->durationFrames,
             55);
    QVERIFY(aggregateTrimRuntime.execute(jcut::EditorCommand{
        jcut::ResizeClipCommand{1, 40}}).applied);
    trimmedAggregate = aggregateTrimRuntime.snapshot();
    QCOMPARE(clipWithPersistentId(trimmedAggregate, "mask")->durationFrames,
             40);

    jcut::EditorDocumentCore rateDocument = parentOnlyMove;
    jcut::EditorClip laterSource = rateDocument.clips.front();
    laterSource.id = 3;
    laterSource.persistentId = "later-source";
    laterSource.label = "Later Source";
    laterSource.startFrame = 120;
    laterSource.durationFrames = 30;
    laterSource.selected = false;
    jcut::EditorClip laterMask = rateDocument.clips.back();
    laterMask.id = 4;
    laterMask.persistentId = "later-mask";
    laterMask.linkedSourceClipId = "later-source";
    laterMask.startFrame = 999;
    laterMask.durationFrames = 1;
    laterMask.selected = false;
    rateDocument.clips.push_back(laterSource);
    rateDocument.clips.push_back(laterMask);
    rateDocument.renderSyncMarkers.push_back(
        {"later-source", 125, false, 1});
    jcut::EditorRuntime rateRuntime =
        jcut::EditorRuntime::fromDocument(std::move(rateDocument));
    const nlohmann::json beforeAggregateRate =
        jcut::toJson(rateRuntime.snapshot());
    QVERIFY(rateRuntime.execute(jcut::EditorCommand{
        jcut::SetClipPlaybackRateCommand{1, 2.0}}).applied);
    const jcut::EditorDocumentCore rated = rateRuntime.snapshot();
    QCOMPARE(clipWithPersistentId(rated, "source")->durationFrames, 45);
    QCOMPARE(clipWithPersistentId(rated, "mask")->durationFrames, 45);
    QCOMPARE(clipWithPersistentId(rated, "mask")->playbackRate, 2.0);
    QCOMPARE(clipWithPersistentId(rated, "later-source")->startFrame, 75);
    QCOMPARE(clipWithPersistentId(rated, "later-mask")->startFrame, 75);
    const auto laterMarker = std::find_if(
        rated.renderSyncMarkers.cbegin(), rated.renderSyncMarkers.cend(),
        [](const jcut::EditorRenderSyncMarker& marker) {
            return marker.clipId == "later-source";
        });
    QVERIFY(laterMarker != rated.renderSyncMarkers.cend());
    QCOMPARE(laterMarker->frame, std::int64_t(80));
    QVERIFY(rateRuntime.execute(jcut::EditorCommand{jcut::UndoCommand{}}).applied);
    QVERIFY(jcut::toJson(rateRuntime.snapshot()) == beforeAggregateRate);
}

void TestEditorRuntime::testExtendedClipStateRoundTripsIntoRenderTimeline()
{
    jcut::EditorDocumentCore document = jcut::EditorRuntime::createDemo().snapshot();
    jcut::EditorClip& clip = document.clips.front();
    clip.persistentId = "clip-persistent-a";
    clip.mediaKind = "video";
    clip.sourcePath = "/definitely/missing/authoritative-metadata.mov";
    clip.sourceFps = 23.976;
    clip.sourceDurationFrames = 777;
    clip.videoEnabled = false;
    clip.audioEnabled = false;
    clip.hasAudio = false;
    clip.brightness = -0.2;
    clip.contrast = 1.3;
    clip.saturation = 1.2;
    clip.opacity = 0.65;
    clip.baseTranslationX = 24.0;
    clip.baseScaleX = -1.0;
    clip.maskEnabled = true;
    clip.maskFeather = 12.0;
    clip.maskGradeEnabled = true;
    clip.maskGradeBrightness = 0.25;
    clip.maskGradeContrast = 1.5;
    clip.maskGradeSaturation = 0.75;
    clip.maskGradeCurvePointsR = {{0.0, 0.1}, {1.0, 0.9}};
    clip.maskGradeCurvePointsLuma = {{0.0, 0.2}, {1.0, 0.8}};
    clip.maskGradeCurveSmoothingEnabled = false;
    clip.maskDropShadowEnabled = true;
    clip.maskDropShadowRadius = 18.0;
    clip.maskDropShadowOffsetX = 7.0;
    clip.maskDropShadowOffsetY = 9.0;
    clip.maskDropShadowOpacity = 0.6;
    clip.effectPreset = "neon_glow";
    clip.effectSkipAwareTiming = false;
    clip.differenceReferenceFrames = 17;
    clip.differenceThreshold = 0.23;
    clip.differenceSoftness = 0.11;
    clip.temporalEchoCount = 7;
    clip.temporalEchoSpacingFrames = 13;
    clip.temporalEchoDecay = 0.42;
    clip.tilingPattern = "spiral_yz";
    clip.tilingSpacing = 2.5;
    clip.tilingWrap = false;
    clip.gradingKeyframes.push_back({12, -0.2, 1.3, 1.2, 0.65, true});
    clip.gradingKeyframes.back().shadowsR = -0.15;
    clip.gradingKeyframes.back().midtonesG = 0.25;
    clip.gradingKeyframes.back().highlightsB = 0.45;
    clip.gradingKeyframes.back().curvePointsR = {{0.0, 0.2}, {1.0, 0.8}};
    clip.gradingKeyframes.back().curvePointsG = {{0.0, 0.1}, {1.0, 0.9}};
    clip.gradingKeyframes.back().curvePointsB = {{0.0, 0.3}, {1.0, 0.7}};
    clip.gradingKeyframes.back().curvePointsLuma = {{0.0, 0.4}, {1.0, 0.6}};
    clip.gradingKeyframes.back().curveThreePointLock = true;
    clip.gradingKeyframes.back().curveSmoothingEnabled = false;
    clip.opacityKeyframes.push_back({12, 0.65, true});
    clip.transformKeyframes.push_back({12, "Start", 24.0, 0.0, 0.0, -1.0, 1.0, true});
    clip.titleKeyframes.push_back({12, "Title", 20.0, 40.0, 48.0, 1.0,
                                   "DejaVu Sans", true, false, "#ffffffff", true});
    clip.transcriptOverlay.enabled = true;
    clip.speakerFramingEnabled = true;
    clip.speakerFramingBakedTargetXNorm = 0.42;
    clip.speakerFramingBakedTargetYNorm = 0.38;
    clip.speakerFramingBakedTargetBoxNorm = 0.24;
    clip.speakerFramingMinConfidence = 0.31;
    clip.speakerFramingManualTrackId = 7;
    clip.speakerFramingManualStreamId = "stream-a";
    clip.speakerFramingCenterSmoothingFrames = 9;
    clip.speakerFramingZoomSmoothingFrames = 13;
    clip.speakerFramingSmoothingMode = 2;
    clip.speakerFramingCenterSmoothingStrength = 0.75;
    clip.speakerFramingZoomSmoothingStrength = 1.25;
    clip.speakerFramingGapHoldFrames = 17;
    clip.speakerFramingEnabledKeyframes.push_back({0, true});
    clip.speakerFramingKeyframes.push_back(
        {0, "Framing", -24.0, 15.0, 3.0, 1.25, 1.15, true});
    clip.speakerFramingTargetKeyframes.push_back(
        {0, "Target", 0.55, 0.45, 0.0, 0.30, 0.30, true});
    clip.correctionPolygons.push_back({
        {{0.1, 0.1}, {0.8, 0.1}, {0.5, 0.8}}, true, 0, 90});
    document.renderSyncMarkers.push_back({"clip-persistent-a", 24, true, 2});
    document.exportRanges.push_back({12, 96});

    const nlohmann::json json = jcut::toJson(document);
    std::string error;
    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(json, &error);
    QVERIFY2(reparsed.has_value(), error.c_str());

    const jcut::EditorClip& reparsedClip = reparsed->clips.front();
    QCOMPARE(QString::fromStdString(reparsedClip.persistentId), QStringLiteral("clip-persistent-a"));
    QCOMPARE(reparsedClip.brightness, -0.2);
    QCOMPARE(reparsedClip.gradingKeyframes.size(), std::size_t(1));
    QCOMPARE(reparsedClip.gradingKeyframes.front().shadowsR, -0.15);
    QCOMPARE(reparsedClip.gradingKeyframes.front().midtonesG, 0.25);
    QCOMPARE(reparsedClip.gradingKeyframes.front().highlightsB, 0.45);
    QCOMPARE(reparsedClip.gradingKeyframes.front().curvePointsLuma.front().y,
             0.4);
    QCOMPARE(reparsedClip.gradingKeyframes.front().curveThreePointLock, true);
    QCOMPARE(reparsedClip.gradingKeyframes.front().curveSmoothingEnabled, false);
    QCOMPARE(reparsedClip.opacityKeyframes.size(), std::size_t(1));
    QCOMPARE(reparsedClip.transformKeyframes.size(), std::size_t(1));
    QCOMPARE(reparsedClip.titleKeyframes.size(), std::size_t(1));
    QCOMPARE(reparsedClip.speakerFramingEnabled, true);
    QCOMPARE(reparsedClip.speakerFramingBakedTargetXNorm, 0.42);
    QCOMPARE(reparsedClip.speakerFramingBakedTargetBoxNorm, 0.24);
    QCOMPARE(reparsedClip.speakerFramingMinConfidence, 0.31);
    QCOMPARE(reparsedClip.speakerFramingManualTrackId, 7);
    QCOMPARE(QString::fromStdString(
                 reparsedClip.speakerFramingManualStreamId),
             QStringLiteral("stream-a"));
    QCOMPARE(reparsedClip.speakerFramingCenterSmoothingFrames, 9);
    QCOMPARE(reparsedClip.speakerFramingZoomSmoothingFrames, 13);
    QCOMPARE(reparsedClip.speakerFramingSmoothingMode, 2);
    QCOMPARE(reparsedClip.speakerFramingCenterSmoothingStrength, 0.75);
    QCOMPARE(reparsedClip.speakerFramingZoomSmoothingStrength, 1.25);
    QCOMPARE(reparsedClip.speakerFramingGapHoldFrames, 17);
    QCOMPARE(reparsedClip.speakerFramingEnabledKeyframes.size(),
             std::size_t(1));
    QCOMPARE(reparsedClip.speakerFramingKeyframes.size(), std::size_t(1));
    QCOMPARE(reparsedClip.speakerFramingTargetKeyframes.size(),
             std::size_t(1));
    QCOMPARE(reparsedClip.maskGradeEnabled, true);
    QCOMPARE(reparsedClip.maskGradeBrightness, 0.25);
    QCOMPARE(reparsedClip.maskGradeCurvePointsR.front().y, 0.1);
    QCOMPARE(reparsedClip.maskGradeCurvePointsLuma.back().y, 0.8);
    QCOMPARE(reparsedClip.maskGradeCurveSmoothingEnabled, false);
    QCOMPARE(reparsedClip.maskDropShadowEnabled, true);
    QCOMPARE(reparsedClip.maskDropShadowRadius, 18.0);
    QCOMPARE(reparsedClip.maskDropShadowOffsetX, 7.0);
    QCOMPARE(reparsedClip.maskDropShadowOpacity, 0.6);
    QCOMPARE(reparsedClip.effectSkipAwareTiming, false);
    QCOMPARE(reparsedClip.differenceReferenceFrames, 17);
    QCOMPARE(reparsedClip.differenceThreshold, 0.23);
    QCOMPARE(reparsedClip.temporalEchoCount, 7);
    QCOMPARE(reparsedClip.temporalEchoSpacingFrames, 13);
    QCOMPARE(reparsedClip.temporalEchoDecay, 0.42);
    QCOMPARE(QString::fromStdString(reparsedClip.tilingPattern), QStringLiteral("spiral_yz"));
    QCOMPARE(reparsedClip.tilingSpacing, 2.5);
    QCOMPARE(reparsedClip.tilingWrap, false);
    QCOMPARE(reparsedClip.correctionPolygons.size(), std::size_t(1));
    QCOMPARE(reparsedClip.correctionPolygons.front().pointsNormalized.size(), std::size_t(3));
    QCOMPARE(reparsed->renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(reparsed->exportRanges.size(), std::size_t(1));

    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(*reparsed);
    const TimelineClip& renderClip = renderData.clips.front();
    QCOMPARE(renderClip.id, QStringLiteral("clip-persistent-a"));
    QCOMPARE(renderClip.sourceFps, 23.976);
    QCOMPARE(renderClip.sourceDurationFrames, std::int64_t(777));
    QCOMPARE(renderClip.videoEnabled, false);
    QCOMPARE(renderClip.audioEnabled, false);
    QCOMPARE(renderClip.hasAudio, false);
    QCOMPARE(renderClip.brightness, -0.2);
    QCOMPARE(renderClip.opacity, 0.65);
    QCOMPARE(renderClip.baseTranslationX, 24.0);
    QCOMPARE(renderClip.baseScaleX, -1.0);
    QCOMPARE(renderClip.maskEnabled, true);
    QCOMPARE(renderClip.maskGradeEnabled, true);
    QCOMPARE(renderClip.maskGradeBrightness, 0.25);
    QCOMPARE(renderClip.maskGradeCurvePointsR.front(), QPointF(0.0, 0.1));
    QCOMPARE(renderClip.maskGradeCurvePointsLuma.back(), QPointF(1.0, 0.8));
    QCOMPARE(renderClip.maskGradeCurveSmoothingEnabled, false);
    QCOMPARE(renderClip.maskDropShadowEnabled, true);
    QCOMPARE(renderClip.maskDropShadowRadius, 18.0);
    QCOMPARE(renderClip.maskDropShadowOffsetY, 9.0);
    QCOMPARE(renderClip.maskDropShadowOpacity, 0.6);
    QCOMPARE(renderClip.effectPreset, ClipEffectPreset::NeonGlow);
    QCOMPARE(renderClip.effectSkipAwareTiming, false);
    QCOMPARE(renderClip.differenceReferenceFrames, 17);
    QCOMPARE(renderClip.differenceSoftness, 0.11);
    QCOMPARE(renderClip.temporalEchoCount, 7);
    QCOMPARE(renderClip.temporalEchoSpacingFrames, 13);
    QCOMPARE(renderClip.temporalEchoDecay, 0.42);
    QCOMPARE(renderClip.tilingPattern, ClipTilingPattern::SpiralYZ);
    QCOMPARE(renderClip.tilingSpacing, 2.5);
    QCOMPARE(renderClip.tilingWrap, false);
    QCOMPARE(renderClip.gradingKeyframes.size(), 1);
    QCOMPARE(renderClip.gradingKeyframes.front().shadowsR, -0.15);
    QCOMPARE(renderClip.gradingKeyframes.front().midtonesG, 0.25);
    QCOMPARE(renderClip.gradingKeyframes.front().highlightsB, 0.45);
    QCOMPARE(renderClip.gradingKeyframes.front().curvePointsR.front(),
             QPointF(0.0, 0.2));
    QCOMPARE(renderClip.gradingKeyframes.front().curvePointsLuma.back(),
             QPointF(1.0, 0.6));
    QCOMPARE(renderClip.gradingKeyframes.front().curveThreePointLock, true);
    QCOMPARE(renderClip.gradingKeyframes.front().curveSmoothingEnabled, false);
    QCOMPARE(renderClip.opacityKeyframes.size(), 1);
    QCOMPARE(renderClip.transformKeyframes.size(), 1);
    QCOMPARE(renderClip.titleKeyframes.size(), 1);
    QCOMPARE(renderClip.transcriptOverlay.enabled, true);
    QCOMPARE(renderClip.speakerFramingEnabled, true);
    QCOMPARE(renderClip.speakerFramingBakedTargetYNorm, 0.38);
    QCOMPARE(renderClip.speakerFramingManualTrackId, 7);
    QCOMPARE(renderClip.speakerFramingManualStreamId,
             QStringLiteral("stream-a"));
    QCOMPARE(renderClip.speakerFramingZoomSmoothingFrames, 13);
    QCOMPARE(renderClip.speakerFramingZoomSmoothingStrength, 1.25);
    QCOMPARE(renderClip.speakerFramingEnabledKeyframes.size(), 1);
    QCOMPARE(renderClip.speakerFramingKeyframes.size(), 1);
    QCOMPARE(renderClip.speakerFramingTargetKeyframes.size(), 1);
    QCOMPARE(renderClip.speakerFramingKeyframes.front().translationX,
             -24.0);
    QCOMPARE(renderClip.speakerFramingTargetKeyframes.front().scaleX,
             0.30);
    QCOMPARE(renderClip.correctionPolygons.size(), 1);
    QCOMPARE(renderData.renderSyncMarkers.size(), std::size_t(1));
    QCOMPARE(renderData.renderSyncMarkers.front().action, RenderSyncAction::SkipFrame);
    QCOMPARE(renderData.renderSyncMarkers.front().count, 2);
    QCOMPARE(renderData.exportRanges.size(), std::size_t(1));
    QCOMPARE(renderData.exportRanges.front().startFrame, std::int64_t(12));
    QCOMPARE(renderData.exportRanges.front().endFrame, std::int64_t(96));
}

void TestEditorRuntime::testExportCommandsUpdateCoreRequest()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SetExportFormatCommand{"mov"});
    QCOMPARE(QString::fromStdString(runtime.snapshot().exportRequest.outputFormat), QStringLiteral("mov"));

    applyCommand(runtime, jcut::SetExportUseProxyMediaCommand{true});
    QCOMPARE(runtime.snapshot().exportRequest.useProxyMedia, true);

    applyCommand(runtime, jcut::SetExportOutputPathCommand{"/tmp/export.mp4"});
    QCOMPARE(QString::fromStdString(runtime.snapshot().exportRequest.outputPath),
             QStringLiteral("/tmp/export.mp4"));

    applyCommand(runtime, jcut::SetExportImageSequenceCommand{true});
    QCOMPARE(runtime.snapshot().exportRequest.createVideoFromImageSequence, true);
    QCOMPARE(QString::fromStdString(runtime.snapshot().exportRequest.imageSequenceFormat), QStringLiteral("jpeg"));
    QCOMPARE(runtime.snapshot().exportRequest.outputMode,
             jcut::render::RenderOutputMode::EncodedFileAndImageSequence);

    applyCommand(runtime, jcut::SetExportImageSequenceFormatCommand{"png"});
    QCOMPARE(QString::fromStdString(runtime.snapshot().exportRequest.imageSequenceFormat),
             QStringLiteral("png"));
}

void TestEditorRuntime::testExportRangeContextActionsMatchQtSemantics()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();
    applyCommand(runtime, jcut::SetExportRangeCommand{10, 100});

    applyCommand(
        runtime,
        jcut::EditExportRangesCommand{
            jcut::ExportRangeEdit::SplitAtPlayhead, 50});
    QCOMPARE(runtime.snapshot().exportRanges.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().exportRanges[0].startFrame, std::int64_t(10));
    QCOMPARE(runtime.snapshot().exportRanges[0].endFrame, std::int64_t(49));
    QCOMPARE(runtime.snapshot().exportRanges[1].startFrame, std::int64_t(50));
    QCOMPARE(runtime.snapshot().exportRanges[1].endFrame, std::int64_t(100));

    applyCommand(
        runtime,
        jcut::EditExportRangesCommand{
            jcut::ExportRangeEdit::SetStartAtPlayhead, 20});
    applyCommand(
        runtime,
        jcut::EditExportRangesCommand{
            jcut::ExportRangeEdit::SetEndAtPlayhead, 80});
    QCOMPARE(runtime.snapshot().exportRanges[0].startFrame, std::int64_t(20));
    QCOMPARE(runtime.snapshot().exportRanges[1].endFrame, std::int64_t(80));
    QCOMPARE(runtime.snapshot().exportRequest.exportStartFrame, std::int64_t(20));
    QCOMPARE(runtime.snapshot().exportRequest.exportEndFrame, std::int64_t(80));
    QCOMPARE(runtime.snapshot().exportRequest.exportRangeCount, std::size_t(2));

    const jcut::CommandResult invalidSplit = runtime.execute(
        jcut::EditExportRangesCommand{
            jcut::ExportRangeEdit::SplitAtPlayhead, 20});
    QVERIFY(!invalidSplit.applied);
    QCOMPARE(runtime.snapshot().exportRanges.size(), std::size_t(2));

    applyCommand(
        runtime,
        jcut::SetExportRangesCommand{
            {{70, 75}, {10, 15}}});
    QCOMPARE(runtime.snapshot().exportRanges.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().exportRanges[0].startFrame,
             std::int64_t(10));
    QCOMPARE(runtime.snapshot().exportRanges[1].endFrame,
             std::int64_t(75));
    QCOMPARE(runtime.snapshot().exportRequest.exportStartFrame,
             std::int64_t(10));
    QCOMPARE(runtime.snapshot().exportRequest.exportEndFrame,
             std::int64_t(75));

    applyCommand(
        runtime,
        jcut::EditExportRangesCommand{
            jcut::ExportRangeEdit::Reset, 0});
    const jcut::EditorDocumentCore reset = runtime.snapshot();
    std::int64_t expectedExtent = 300;
    for (const jcut::EditorClip& clip : reset.clips) {
        expectedExtent = std::max(
            expectedExtent,
            static_cast<std::int64_t>(clip.startFrame) +
                clip.durationFrames + 30);
    }
    QCOMPARE(reset.exportRanges.size(), std::size_t(1));
    QCOMPARE(reset.exportRanges.front().startFrame, std::int64_t(0));
    QCOMPARE(reset.exportRanges.front().endFrame, expectedExtent);
    QCOMPARE(reset.exportRequest.exportRangeCount, std::size_t(1));

    applyCommand(runtime, jcut::UndoCommand{});
    QCOMPARE(runtime.snapshot().exportRanges.size(), std::size_t(2));
    QCOMPARE(runtime.snapshot().exportRanges[0].startFrame, std::int64_t(10));
    QCOMPARE(runtime.snapshot().exportRanges[1].endFrame, std::int64_t(75));
}

void TestEditorRuntime::testQtRenderRequestPreservesPlaybackSpeed()
{
    RenderRequest request;
    request.outputPath = QStringLiteral("/tmp/export.mp4");
    request.outputFormat = QStringLiteral("mp4");
    request.outputSize = QSize(1280, 720);
    request.outputFps = 60.0;
    request.playbackSpeed = 1.75;
    request.backgroundFillOpacity = 0.42;
    request.backgroundFillBrightness = -0.15;
    request.backgroundFillSaturation = 1.25;
    request.backgroundFillEdgePixels = 32;
    request.backgroundFillEdgeProgressive = true;
    request.backgroundFillEdgePower = 3.5;
    request.backgroundFillStretchSourceClipId = QStringLiteral("clip-selected");

    const jcut::render::RenderRequestCore core = jcut::render::toCoreRenderRequest(request);

    QCOMPARE(core.outputFps, 60.0);
    QCOMPARE(core.playbackSpeed, 1.75);
    QCOMPARE(core.backgroundFillOpacity, 0.42);
    QCOMPARE(core.backgroundFillBrightness, -0.15);
    QCOMPARE(core.backgroundFillSaturation, 1.25);
    QCOMPARE(core.backgroundFillEdgePixels, 32);
    QCOMPARE(core.backgroundFillEdgeProgressive, true);
    QCOMPARE(core.backgroundFillEdgePower, 3.5);
    QCOMPARE(QString::fromStdString(core.backgroundFillStretchSourceClipId),
             QStringLiteral("clip-selected"));
}

void TestEditorRuntime::testQtBridgeBuildsDocumentCore()
{
    TimelineTrack trackA;
    trackA.name = QStringLiteral("A Cam");
    TimelineTrack trackB;
    trackB.name = QStringLiteral("B Cam");

    TimelineClip clipA;
    clipA.label = QStringLiteral("Interview A");
    clipA.filePath = QStringLiteral("/tmp/interview_a.mov");
    clipA.mediaType = ClipMediaType::Video;
    clipA.trackIndex = 0;
    clipA.startFrame = 12;
    clipA.durationFrames = 240;
    clipA.speakerSectionMinimumWords = 31;
    TimelineClip::GradingKeyframe qtGrade;
    qtGrade.frame = 8;
    qtGrade.shadowsR = -0.1;
    qtGrade.shadowsG = -0.2;
    qtGrade.shadowsB = -0.3;
    qtGrade.midtonesR = 0.1;
    qtGrade.midtonesG = 0.2;
    qtGrade.midtonesB = 0.3;
    qtGrade.highlightsR = 0.4;
    qtGrade.highlightsG = 0.5;
    qtGrade.highlightsB = 0.6;
    qtGrade.curvePointsR = {{0.0, 0.1}, {1.0, 0.9}};
    qtGrade.curvePointsG = {{0.0, 0.2}, {1.0, 0.8}};
    qtGrade.curvePointsB = {{0.0, 0.3}, {1.0, 0.7}};
    qtGrade.curvePointsLuma = {{0.0, 0.4}, {1.0, 0.6}};
    qtGrade.curveThreePointLock = true;
    qtGrade.curveSmoothingEnabled = false;
    clipA.gradingKeyframes = {qtGrade};

    TimelineClip clipB;
    clipB.filePath = QStringLiteral("/tmp/vo.wav");
    clipB.mediaType = ClipMediaType::Audio;
    clipB.trackIndex = 1;
    clipB.startFrame = 24;
    clipB.durationFrames = 120;
    clipB.hasAudio = true;

    const jcut::EditorDocumentCore document = jcut::buildEditorDocumentCore(
        QStringLiteral("Runtime Bridge"),
        QVector<TimelineClip>{clipA, clipB},
        QVector<TimelineTrack>{trackA, trackB});

    QCOMPARE(QString::fromStdString(document.projectName), QStringLiteral("Runtime Bridge"));
    QCOMPARE(document.tracks.size(), std::size_t(2));
    QCOMPARE(document.clips.size(), std::size_t(2));
    QCOMPARE(document.mediaItems.size(), std::size_t(2));
    QCOMPARE(document.tracks.front().selected, true);
    QCOMPARE(document.clips.front().selected, true);
    QCOMPARE(QString::fromStdString(document.clips.front().label), QStringLiteral("Interview A"));
    QCOMPARE(QString::fromStdString(document.clips.back().label), QStringLiteral("vo.wav"));
    QCOMPARE(document.clips.back().trackId, 2);
    QCOMPARE(document.clips.front().audioPresenceKnown, true);
    QCOMPARE(document.clips.front().hasAudio, false);
    QCOMPARE(
        document.clips.front().speakerSectionMinimumWords,
        31);
    QCOMPARE(document.clips.back().audioPresenceKnown, true);
    QCOMPARE(document.clips.back().hasAudio, true);
    QCOMPARE(document.mediaItems.back().audioPresenceKnown, true);
    QCOMPARE(document.mediaItems.back().hasAudio, true);
    const jcut::EditorGradingKeyframe& coreGrade =
        document.clips.front().gradingKeyframes.front();
    QCOMPARE(coreGrade.shadowsR, -0.1);
    QCOMPARE(coreGrade.midtonesG, 0.2);
    QCOMPARE(coreGrade.highlightsB, 0.6);
    QCOMPARE(coreGrade.curvePointsR.size(), std::size_t(2));
    QCOMPARE(coreGrade.curvePointsG.front().y, 0.2);
    QCOMPARE(coreGrade.curvePointsB.back().y, 0.7);
    QCOMPARE(coreGrade.curvePointsLuma.front().y, 0.4);
    QCOMPARE(coreGrade.curveThreePointLock, true);
    QCOMPARE(coreGrade.curveSmoothingEnabled, false);
}

void TestEditorRuntime::testDocumentBuildsTimelineRenderData()
{
    const jcut::EditorDocumentCore document = jcut::EditorRuntime::createDemo().snapshot();
    const jcut::render::TimelineRenderData timelineData =
        jcut::render::buildTimelineRenderData(document);

    QCOMPARE(timelineData.tracks.size(), document.tracks.size());
    QCOMPARE(timelineData.clips.size(), document.clips.size());
    QCOMPARE(timelineData.exportRanges.size(), std::size_t(1));
    QCOMPARE(timelineData.clips.front().trackIndex, 0);
    QCOMPARE(timelineData.clips.front().durationFrames, 420);
}

void TestEditorRuntime::testExplicitAudioSourceBuildsPlayableTimelineState()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString audioPath = tempDir.filePath(QStringLiteral("voice.wav"));
    QVERIFY(writeSilentPcmWav(audioPath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Audio"});

    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "voice-clip";
    clip.trackId = 1;
    clip.label = "Voice";
    clip.durationFrames = 30;
    clip.sourcePath = audioPath.toStdString();
    clip.mediaKind = "audio";
    clip.audioEnabled = true;
    clip.hasAudio = true;
    clip.audioSourceMode = "explicit_file";
    clip.audioSourcePath = audioPath.toStdString();
    document.clips.push_back(clip);

    const jcut::render::TimelineRenderData playable =
        jcut::render::buildTimelineRenderData(document, false);
    QCOMPARE(playable.clips.size(), std::size_t(1));
    QCOMPARE(playable.clips.front().audioSourcePath, audioPath);
    QCOMPARE(playable.clips.front().audioSourceStatus, QStringLiteral("ok"));

    document.clips.front().audioSourcePath =
        tempDir.filePath(QStringLiteral("missing.wav")).toStdString();
    document.clips.front().sourcePath = document.clips.front().audioSourcePath;
    const jcut::render::TimelineRenderData missing =
        jcut::render::buildTimelineRenderData(document, false);
    QCOMPARE(missing.clips.size(), std::size_t(1));
    QCOMPARE(missing.clips.front().audioSourceStatus, QStringLiteral("missing"));
}

void TestEditorRuntime::testMediaProbeRefreshesAudioSourceStatus()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString audioPath = tempDir.filePath(QStringLiteral("probed.wav"));
    QVERIFY(writeSilentPcmWav(audioPath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Audio"});

    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.durationFrames = 30;
    clip.sourcePath = audioPath.toStdString();
    clip.mediaKind = "audio";
    clip.audioEnabled = true;
    clip.hasAudio = false;
    document.clips.push_back(clip);

    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(document, true);
    QCOMPARE(timeline.clips.size(), std::size_t(1));
    const TimelineClip& renderClip = timeline.clips.front();
    QVERIFY(renderClip.hasAudio);
    QCOMPARE(renderClip.audioSourceMode, QStringLiteral("explicit_file"));
    QCOMPARE(renderClip.audioSourcePath, audioPath);
    QCOMPARE(renderClip.audioSourceStatus, QStringLiteral("ok"));
}

void TestEditorRuntime::testAudioPresenceSurvivesPlaybackToggle()
{
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video"});
    jcut::EditorRuntime runtime =
        jcut::EditorRuntime::fromDocument(std::move(document));

    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/probed-video.mov", "Probed Video", "video"});
    applyCommand(runtime, jcut::InsertClipFromMediaCommand{
        "/tmp/probed-video.mov", 1, 0, 30});
    const int clipId = runtime.snapshot().clips.front().id;

    applyCommand(runtime, jcut::SetClipAudioCommand{
        clipId, false, 1.0, 0.0, false});
    QCOMPARE(runtime.snapshot().clips.front().audioEnabled, false);
    QCOMPARE(runtime.snapshot().clips.front().hasAudio, true);

    // This models an asynchronous media probe completing while playback for
    // the clip is disabled. Presence metadata must not inherit that toggle.
    applyCommand(runtime, jcut::ImportMediaCommand{
        "/tmp/probed-video.mov", "Probed Video", "video", true, true});
    QCOMPARE(runtime.snapshot().clips.front().audioPresenceKnown, true);
    QCOMPARE(runtime.snapshot().clips.front().audioEnabled, false);
    QCOMPARE(runtime.snapshot().clips.front().hasAudio, true);

    applyCommand(runtime, jcut::SetClipAudioCommand{
        clipId, true, 1.0, 0.0, false});
    QCOMPARE(runtime.snapshot().clips.front().audioEnabled, true);
    QCOMPARE(runtime.snapshot().clips.front().hasAudio, true);
}

void TestEditorRuntime::testExplicitAudioSourceWorksWithoutPrimaryMedia()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString audioPath = tempDir.filePath(QStringLiteral("external.wav"));
    QVERIFY(writeSilentPcmWav(audioPath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Audio"});

    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.durationFrames = 30;
    clip.mediaKind = "audio";
    clip.audioEnabled = true;
    clip.hasAudio = true;
    clip.audioSourceMode = "explicit_file";
    clip.audioSourcePath = audioPath.toStdString();
    document.clips.push_back(clip);

    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(document, false);
    QCOMPARE(timeline.clips.size(), std::size_t(1));
    const TimelineClip& renderClip = timeline.clips.front();
    QVERIFY(renderClip.filePath.isEmpty());
    QCOMPARE(renderClip.audioSourceMode, QStringLiteral("explicit_file"));
    QCOMPARE(renderClip.audioSourcePath, audioPath);
    QCOMPARE(renderClip.audioSourceStatus, QStringLiteral("ok"));
}

void TestEditorRuntime::testCoreDocumentJsonRoundTrips()
{
    jcut::EditorDocumentCore original = jcut::EditorRuntime::createDemo().snapshot();
    original.transport.playbackLoopEnabled = true;
    original.transport.previewViewMode = "audio";
    original.transport.audioMuted = true;
    original.transport.audioVolume = 0.35f;
    original.mediaItems.front().audioPresenceKnown = true;
    original.mediaItems.front().hasAudio = false;
    original.clips.front().audioPresenceKnown = true;
    original.clips.front().hasAudio = false;
    original.clips.front().speakerSectionMinimumWords = 37;
    original.exportRequest.backgroundFillEffect = "blur";
    original.exportRequest.backgroundFillOpacity = 0.42;
    original.exportRequest.backgroundFillBrightness = -0.2;
    original.exportRequest.backgroundFillSaturation = 1.4;
    original.exportRequest.backgroundFillEdgePixels = 24;
    original.exportRequest.backgroundFillEdgeProgressive = true;
    original.exportRequest.backgroundFillEdgePower = 3.5;
    original.exportRequest.backgroundFillStretchSourceClipId = "imgui-clip-1";
    original.exportRequest.transcriptPrependMs = 210;
    original.exportRequest.transcriptPostpendMs = 95;
    original.exportRequest.transcriptOffsetMs = -30;

    std::string error;
    const nlohmann::json json = jcut::toJson(original);
    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(json, &error);

    QVERIFY2(reparsed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(reparsed->projectName), QString::fromStdString(original.projectName));
    QCOMPARE(reparsed->tracks.size(), original.tracks.size());
    QCOMPARE(reparsed->clips.size(), original.clips.size());
    QCOMPARE(reparsed->mediaItems.size(), original.mediaItems.size());
    QCOMPARE(reparsed->mediaItems.front().audioPresenceKnown, true);
    QCOMPARE(reparsed->mediaItems.front().hasAudio, false);
    QCOMPARE(reparsed->clips.front().audioPresenceKnown, true);
    QCOMPARE(reparsed->clips.front().hasAudio, false);
    QCOMPARE(
        reparsed->clips.front().speakerSectionMinimumWords,
        37);
    QCOMPARE(reparsed->exportRequest.outputSize.width, original.exportRequest.outputSize.width);
    QCOMPARE(reparsed->exportRequest.outputSize.height, original.exportRequest.outputSize.height);
    QCOMPARE(reparsed->exportRequest.playbackSpeed, original.exportRequest.playbackSpeed);
    QCOMPARE(QString::fromStdString(reparsed->exportRequest.backgroundFillEffect),
             QStringLiteral("none"));
    QCOMPARE(reparsed->exportRequest.backgroundFillOpacity, 0.42);
    QCOMPARE(reparsed->exportRequest.backgroundFillBrightness, -0.2);
    QCOMPARE(reparsed->exportRequest.backgroundFillSaturation, 1.4);
    QCOMPARE(reparsed->exportRequest.backgroundFillEdgePixels, 24);
    QCOMPARE(reparsed->exportRequest.backgroundFillEdgeProgressive, false);
    QCOMPARE(reparsed->exportRequest.backgroundFillEdgePower, 3.5);
    QCOMPARE(QString::fromStdString(
                 reparsed->exportRequest.backgroundFillStretchSourceClipId),
             QStringLiteral("imgui-clip-1"));
    QCOMPARE(reparsed->exportRequest.transcriptPrependMs, 210);
    QCOMPARE(reparsed->exportRequest.transcriptPostpendMs, 95);
    QCOMPARE(reparsed->exportRequest.transcriptOffsetMs, -30);
    QCOMPARE(reparsed->transport.currentFrame, original.transport.currentFrame);
    QCOMPARE(
        reparsed->transport.playbackLoopEnabled,
        original.transport.playbackLoopEnabled);
    QCOMPARE(
        QString::fromStdString(reparsed->transport.previewViewMode),
        QString::fromStdString(original.transport.previewViewMode));
    QCOMPARE(
        reparsed->transport.audioMuted,
        original.transport.audioMuted);
    QCOMPARE(
        reparsed->transport.audioVolume,
        original.transport.audioVolume);

    const nlohmann::json wrongScalarTypes = {
        {"projectName", 42},
        {"mediaItems", nlohmann::json::array()},
        {"tracks", nlohmann::json::array({{
            {"id", "not-an-integer"},
            {"selected", nlohmann::json::object()},
            {"height", "not-an-integer"}
        }})},
        {"clips", nlohmann::json::array()},
        {"transport", {
            {"currentFrame", "not-an-integer"},
            {"playbackSpeed", nlohmann::json::array()}
        }},
        {"exportRequest", {
            {"outputFps", "not-a-number"},
            {"outputSize", {{"width", false}, {"height", "large"}}}
        }}
    };
    error.clear();
    const std::optional<jcut::EditorDocumentCore> safelyParsed =
        jcut::editorDocumentCoreFromJson(wrongScalarTypes, &error);
    QVERIFY2(safelyParsed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(safelyParsed->projectName), QStringLiteral("Untitled Project"));
    QCOMPARE(safelyParsed->tracks.front().id, 0);
    QCOMPARE(safelyParsed->tracks.front().height, 72);
    QCOMPARE(safelyParsed->transport.currentFrame, 0);
    QCOMPARE(safelyParsed->exportRequest.outputFps, 30.0);
    QCOMPARE(safelyParsed->exportRequest.outputSize.width, 1080);
    QCOMPARE(safelyParsed->exportRequest.outputSize.height, 1920);
}

void TestEditorRuntime::testLegacyStateJsonBuildsDocumentCore()
{
    const nlohmann::json state = {
        {"projectName", "Legacy Session"},
        {"currentFrame", 48},
        {"playing", true},
        {"selectedClipId", "clip-b"},
        {"selectedTrackIndex", 1},
        {"playbackSpeed", 1.5},
        {"exportPlaybackSpeed", 2.25},
        {"timelineZoom", 3.0},
        {"outputWidth", 1280},
        {"outputHeight", 720},
        {"outputFps", 24.0},
        {"outputFormat", "png"},
        {"renderUseProxies", true},
        {"correctionsEnabled", false},
        {"audioWaveformVisible", false},
        {"exportStartFrame", 10},
        {"exportEndFrame", 90},
        {"exportRanges", {{{"startFrame", 10}, {"endFrame", 90}}}},
        {"renderSyncMarkers", {{{"clipId", "clip-a"}, {"frame", 44}, {"action", "cut"}, {"count", 1}}}},
        {"mediaItems", {{
            {"id", "/tmp/library.wav"},
            {"label", "Library Voice"},
            {"kind", "audio"},
            {"hasAudio", true}
        }}},
        {"tracks", {
            {{"name", "Video 1"}},
            {{"name", "Video 2"}}
        }},
        {"timeline", {
            {
                {"id", "clip-a"},
                {"label", "Clip A"},
                {"filePath", "/tmp/a.mov"},
                {"mediaType", "video"},
                {"audioEnabled", false},
                {"trackIndex", 0},
                {"startFrame", 12},
                {"durationFrames", 40}
            },
            {
                {"id", "clip-b"},
                {"filePath", "/tmp/b.wav"},
                {"mediaType", "audio"},
                {"trackIndex", 1},
                {"startFrame", 20},
                {"durationFrames", 55}
            }
        }}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> parsed =
        jcut::editorDocumentCoreFromJson(state, &error);

    QVERIFY2(parsed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(parsed->projectName), QStringLiteral("Legacy Session"));
    QCOMPARE(parsed->transport.playbackActive, true);
    QCOMPARE(parsed->transport.currentFrame, 48);
    QCOMPARE(parsed->transport.playbackSpeed, 1.5f);
    QCOMPARE(parsed->transport.previewZoom, 3.0f);
    QCOMPARE(parsed->panels.showWaveform, false);
    QCOMPARE(parsed->tracks.size(), std::size_t(2));
    QCOMPARE(parsed->tracks.back().selected, true);
    QCOMPARE(parsed->clips.size(), std::size_t(2));
    QCOMPARE(parsed->clips.back().selected, true);
    QCOMPARE(parsed->clips.front().audioEnabled, false);
    QCOMPARE(parsed->clips.front().hasAudio, true);
    QCOMPARE(parsed->clips.back().hasAudio, true);
    QCOMPARE(QString::fromStdString(parsed->clips.back().label), QStringLiteral("b.wav"));
    QCOMPARE(parsed->mediaItems.size(), std::size_t(3));
    const auto persistedMedia = std::find_if(
        parsed->mediaItems.begin(), parsed->mediaItems.end(),
        [](const jcut::EditorMediaItem& item) {
            return item.id == "/tmp/library.wav";
        });
    QVERIFY(persistedMedia != parsed->mediaItems.end());
    QCOMPARE(persistedMedia->audioPresenceKnown, true);
    QCOMPARE(persistedMedia->hasAudio, true);
    QCOMPARE(parsed->exportRequest.outputSize.width, 1280);
    QCOMPARE(parsed->exportRequest.outputSize.height, 720);
    QCOMPARE(parsed->exportRequest.playbackSpeed, 2.25);
    QCOMPARE(parsed->exportRequest.useProxyMedia, true);
    QCOMPARE(parsed->exportRequest.correctionsEnabled, false);
    QCOMPARE(parsed->exportRequest.exportRangeCount, std::size_t(1));
    QCOMPARE(parsed->exportRequest.renderSyncMarkerCount, std::size_t(1));
    QCOMPARE(parsed->exportRequest.imageSequenceFormat, std::string("png"));
    QCOMPARE(parsed->exportRequest.outputMode,
             jcut::render::RenderOutputMode::EncodedFileAndImageSequence);
}

void TestEditorRuntime::testLegacyStateJsonPreservesQtOnlyArtifactFields()
{
    const nlohmann::json base = {
        {"projectName", "Artifact"},
        {"mediaRoot", "/media/root"},
        {"selectedClipId", "clip-a"},
        {"tracks", nlohmann::json::array({
            {
                {"name", "Video"},
                {"height", 72},
                {"visualsEnabled", false},
                {"audioEnabled", true}
            }
        })},
        {"timeline", nlohmann::json::array({
            {
                {"id", "clip-a"},
                {"label", "Interview"},
                {"filePath", "interview.mov"},
                {"proxyPath", "proxy/interview"},
                {"audioSourcePath", "audio/interview.wav"},
                {"mediaType", "video"},
                {"trackIndex", 0},
                {"startFrame", 10},
                {"durationFrames", 90},
                {"transformKeyframes", nlohmann::json::array({{
                    {"frame", 10},
                    {"translationX", 20.0},
                    {"maskRepeatDeltaX", 320.0}
                }})},
                {"gradingKeyframes", nlohmann::json::array({{
                    {"frame", 10},
                    {"brightness", 0.25},
                    {"shadowsR", 0.2},
                    {"curvePointsLuma", nlohmann::json::array({{{"x", 0.0}, {"y", 0.1}}})}
                }})},
                {"titleKeyframes", nlohmann::json::array({{
                    {"frame", 10},
                    {"text", "Original title"},
                    {"vulkan3DEnabled", true},
                    {"vulkan3DExtrudeDepth", 0.4}
                }})},
                {"transcriptOverlay", {
                    {"enabled", true},
                    {"backgroundCornerRadius", 22.0},
                    {"textOutlineEnabled", true},
                    {"textExtrudeMode", "mesh"}
                }},
                {"transcriptPath", "transcripts/interview.json"},
                {"speakerTrackingEnabled", true},
                {"correctionPolygons", nlohmann::json::array({{
                    {"enabled", true},
                    {"points", nlohmann::json::array({
                        {{"x", 0.1}, {"y", 0.1}},
                        {{"x", 0.9}, {"y", 0.1}},
                        {{"x", 0.5}, {"y", 0.9}}
                    })},
                    {"qtOnlyPolygonMetadata", "preserved"}
                }})}
            }
        })}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> core =
        jcut::editorDocumentCoreFromJson(base, &error);
    QVERIFY2(core.has_value(), error.c_str());

    jcut::EditorDocumentCore updated = *core;
    updated.projectName = "Artifact Edited";
    updated.transport.currentFrame = 42;
    updated.clips.front().startFrame = 12;
    updated.clips.front().selected = true;
    updated.clips.front().transformKeyframes.front().frame = 20;
    updated.clips.front().gradingKeyframes.front().frame = 21;
    updated.clips.front().titleKeyframes.front().frame = 22;

    const nlohmann::json saved = jcut::toLegacyStateJson(updated, &base);
    QCOMPARE(QString::fromStdString(saved.value("mediaRoot", std::string{})), QStringLiteral("/media/root"));
    const nlohmann::json track = saved.at("tracks").at(0);
    QCOMPARE(track.value("height", 0), 72);
    QCOMPARE(track.value("visualsEnabled", true), false);
    const nlohmann::json clip = saved.at("timeline").at(0);
    QCOMPARE(QString::fromStdString(clip.value("id", std::string{})), QStringLiteral("clip-a"));
    QCOMPARE(clip.value("startFrame", 0), 12);
    QVERIFY(clip.contains("gradingKeyframes"));
    QCOMPARE(clip.at("transformKeyframes").at(0).value("frame", 0), 20);
    QCOMPARE(clip.at("transformKeyframes").at(0).value("maskRepeatDeltaX", 0.0), 320.0);
    QCOMPARE(clip.at("gradingKeyframes").at(0).value("frame", 0), 21);
    QCOMPARE(clip.at("gradingKeyframes").at(0).value("shadowsR", 0.0), 0.2);
    QVERIFY(clip.at("gradingKeyframes").at(0).contains("curvePointsLuma"));
    QCOMPARE(clip.at("titleKeyframes").at(0).value("frame", 0), 22);
    QCOMPARE(clip.at("titleKeyframes").at(0).value("vulkan3DEnabled", false), true);
    QCOMPARE(clip.at("titleKeyframes").at(0).value("vulkan3DExtrudeDepth", 0.0), 0.4);
    QCOMPARE(clip.at("transcriptOverlay").value("backgroundCornerRadius", 0.0), 22.0);
    QCOMPARE(clip.at("transcriptOverlay").value("textOutlineEnabled", false), true);
    QCOMPARE(QString::fromStdString(
                 clip.at("transcriptOverlay").value("textExtrudeMode", std::string{})),
             QStringLiteral("mesh"));
    QVERIFY(clip.contains("transcriptPath"));
    QVERIFY(clip.contains("speakerTrackingEnabled"));
    QVERIFY(clip.contains("correctionPolygons"));
    QCOMPARE(QString::fromStdString(
                 clip.at("correctionPolygons").at(0).value(
                     "qtOnlyPolygonMetadata", std::string{})),
             QStringLiteral("preserved"));
    QCOMPARE(QString::fromStdString(saved.value("selectedClipId", std::string{})), QStringLiteral("clip-a"));
}

void TestEditorRuntime::testLegacySaveRemapsGappedTrackIds()
{
    const nlohmann::json base = {
        {"projectName", "Tracks"},
        {"selectedTrackIndex", 2},
        {"selectedClipId", "clip-c"},
        {"tracks", nlohmann::json::array({
            {{"name", "A"}, {"qtOnlyTrack", "a"}},
            {{"name", "B"}, {"qtOnlyTrack", "b"}},
            {{"name", "C"}, {"qtOnlyTrack", "c"}}
        })},
        {"timeline", nlohmann::json::array({
            {{"id", "clip-a"}, {"filePath", "a.mov"}, {"mediaType", "video"},
             {"trackIndex", 0}, {"startFrame", 0}, {"durationFrames", 30}},
            {{"id", "clip-c"}, {"filePath", "c.mov"}, {"mediaType", "video"},
             {"trackIndex", 2}, {"startFrame", 30}, {"durationFrames", 30},
             {"qtOnlyClip", "c"}}
        })}
    };

    std::string error;
    const std::optional<jcut::EditorDocumentCore> parsed =
        jcut::editorDocumentCoreFromJson(base, &error);
    QVERIFY2(parsed.has_value(), error.c_str());
    jcut::EditorRuntime runtime = jcut::EditorRuntime::fromDocument(*parsed);
    applyCommand(runtime, jcut::DeleteTrackCommand{2});

    const nlohmann::json saved = jcut::toLegacyStateJson(runtime.snapshot(), &base);
    QCOMPARE(saved.at("tracks").size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(
                 saved.at("tracks").at(1).value("qtOnlyTrack", std::string{})),
             QStringLiteral("c"));
    QCOMPARE(saved.at("timeline").at(1).value("trackIndex", -1), 1);
    QCOMPARE(QString::fromStdString(
                 saved.at("timeline").at(1).value("qtOnlyClip", std::string{})),
             QStringLiteral("c"));

    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(saved, &error);
    QVERIFY2(reparsed.has_value(), error.c_str());
    const jcut::render::TimelineRenderData renderData =
        jcut::render::buildTimelineRenderData(*reparsed);
    QCOMPARE(renderData.tracks.size(), std::size_t(2));
    QCOMPARE(renderData.clips.size(), std::size_t(2));
    QCOMPARE(renderData.clips.at(1).trackIndex, 1);
}

void TestEditorRuntime::testCoreDocumentFileRoundTrips()
{
    const jcut::EditorDocumentCore original = jcut::EditorRuntime::createDemo().snapshot();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString path = tempDir.filePath(QStringLiteral("runtime_core.json"));

    std::string saveError;
    QVERIFY2(jcut::saveEditorDocumentCoreToFile(original, path.toStdString(), &saveError),
             saveError.c_str());

    std::string loadError;
    const std::optional<jcut::EditorDocumentCore> loaded =
        jcut::loadEditorDocumentCoreFromFile(path.toStdString(), &loadError);

    QVERIFY2(loaded.has_value(), loadError.c_str());
    QCOMPARE(QString::fromStdString(loaded->projectName), QString::fromStdString(original.projectName));
    QCOMPARE(loaded->clips.size(), original.clips.size());
    QCOMPARE(loaded->tracks.size(), original.tracks.size());
    QCOMPARE(loaded->transport.currentFrame, original.transport.currentFrame);
}

void TestEditorRuntime::testImGuiProjectSessionLoadsActiveQtProjectFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray previousRoot = qgetenv("JCUT_PROJECT_ROOT");
    qputenv("JCUT_PROJECT_ROOT", tempDir.path().toUtf8());

    const QString projectsDir = tempDir.filePath(QStringLiteral("projects"));
    QVERIFY(QDir().mkpath(projectsDir));
    QVERIFY(QDir().mkpath(tempDir.filePath(QStringLiteral("projects/default"))));
    QVERIFY(QDir().mkpath(tempDir.filePath(QStringLiteral("projects/client-cut"))));

    QFile markerFile(tempDir.filePath(QStringLiteral("projects/.current_project")));
    QVERIFY(markerFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(markerFile.write("client-cut") > 0);
    markerFile.close();

    QFile stateFile(tempDir.filePath(QStringLiteral("projects/client-cut/state.json")));
    QVERIFY(stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(stateFile.write(R"({
  "projectName": "Client Cut",
  "mediaRoot": "media",
  "currentFrame": 64,
  "selectedTrackIndex": 0,
  "selectedClipId": "clip-1",
  "tracks": [{"name": "Video A"}],
  "timeline": [{
    "id": "clip-1",
    "label": "Interview",
    "filePath": "/tmp/interview.mov",
    "mediaType": "video",
    "trackIndex": 0,
    "startFrame": 12,
    "durationFrames": 80
  }]
})") > 0);
    stateFile.close();

    std::string error;
    const std::optional<jcut::ImGuiProjectSession> session =
        jcut::loadActiveImGuiProjectSession(&error);

    if (previousRoot.isEmpty()) {
        qunsetenv("JCUT_PROJECT_ROOT");
    } else {
        qputenv("JCUT_PROJECT_ROOT", previousRoot);
    }

    QVERIFY2(session.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(session->projectId), QStringLiteral("client-cut"));
    QCOMPARE(QString::fromStdString(session->rootDirPath), tempDir.path());
    QCOMPARE(QString::fromStdString(session->mediaRootPath),
             QDir(tempDir.path()).filePath(QStringLiteral("media")));
    QCOMPARE(QString::fromStdString(session->document.projectName), QStringLiteral("Client Cut"));
    QCOMPARE(session->document.transport.currentFrame, 64);
    QCOMPARE(session->document.clips.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(session->statePath),
             tempDir.filePath(QStringLiteral("projects/client-cut/state.json")));
}

void TestEditorRuntime::testImGuiProjectSessionSaveWritesQtStateFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray previousRoot = qgetenv("JCUT_PROJECT_ROOT");
    qputenv("JCUT_PROJECT_ROOT", tempDir.path().toUtf8());

    QVERIFY(QDir().mkpath(tempDir.filePath(QStringLiteral("projects/default"))));
    QFile markerFile(tempDir.filePath(QStringLiteral("projects/.current_project")));
    QVERIFY(markerFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(markerFile.write("default") > 0);
    markerFile.close();

    QFile stateFile(tempDir.filePath(QStringLiteral("projects/default/state.json")));
    QVERIFY(stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(stateFile.write(R"({
  "projectName": "Default Project",
  "tracks": [{"name": "Video 1"}],
  "timeline": []
})") > 0);
    stateFile.close();

    std::string loadError;
    const std::optional<jcut::ImGuiProjectSession> loaded =
        jcut::loadActiveImGuiProjectSession(&loadError);
    QVERIFY2(loaded.has_value(), loadError.c_str());

    jcut::EditorDocumentCore updated = loaded->document;
    updated.projectName = "Saved From ImGui";
    updated.transport.currentFrame = 91;
    updated.tracks = {{1, "Primary", true}};
    updated.clips = {{1, 1, "Clip One", 24, 72, true, "/tmp/clip-one.mov"}};
    updated.mediaItems.push_back({
        "/tmp/imported-but-unused.wav", "Unused Voiceover", "audio"});

    std::string saveError;
    QVERIFY2(jcut::saveImGuiProjectSession(*loaded, updated, &saveError), saveError.c_str());

    std::string rereadStateError;
    const std::optional<jcut::EditorDocumentCore> rereadState =
        jcut::loadEditorDocumentCoreFromFile(
            tempDir.filePath(QStringLiteral("projects/default/state.json")).toStdString(),
            &rereadStateError);

    QFile historyFile(tempDir.filePath(QStringLiteral("projects/default/history.json")));
    QVERIFY(historyFile.open(QIODevice::ReadOnly));

    if (previousRoot.isEmpty()) {
        qunsetenv("JCUT_PROJECT_ROOT");
    } else {
        qputenv("JCUT_PROJECT_ROOT", previousRoot);
    }

    QVERIFY2(rereadState.has_value(), rereadStateError.c_str());
    QCOMPARE(QString::fromStdString(rereadState->projectName), QStringLiteral("Saved From ImGui"));
    QCOMPARE(rereadState->transport.currentFrame, 91);
    QCOMPARE(rereadState->clips.size(), std::size_t(1));
    const auto importedMedia = std::find_if(
        rereadState->mediaItems.begin(), rereadState->mediaItems.end(),
        [](const jcut::EditorMediaItem& item) {
            return item.id == "/tmp/imported-but-unused.wav";
        });
    QVERIFY(importedMedia != rereadState->mediaItems.end());
    QCOMPARE(QString::fromStdString(importedMedia->label), QStringLiteral("Unused Voiceover"));
    const QByteArray historyBytes = historyFile.readAll();
    QVERIFY(historyBytes.contains("\"entries\""));
    QVERIFY(historyBytes.contains("Saved From ImGui"));
}

QTEST_MAIN(TestEditorRuntime)
#include "test_editor_runtime.moc"
