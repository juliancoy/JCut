#include <QtTest/QtTest>

#include "../editor_runtime.h"
#include "../editor_document_core_json.h"
#include "../editor_document_render_bridge.h"
#include "../imgui_project_io.h"
#include "../editor_runtime_qt_bridge.h"
#include "../editor_timeline_types.h"
#include "../clip_serialization.h"
#include "../editor_shared_render_sync.h"
#include "../editor_shared_timing.h"
#include "../render.h"
#include "../render_qt_compat.h"

#include <nlohmann/json.hpp>

#include <QString>
#include <QFile>
#include <QTemporaryDir>
#include <utility>

class TestEditorRuntime : public QObject {
    Q_OBJECT

private slots:
    void testPlaybackCommandsUpdateTransport();
    void testSeekAndStepCommandsClampToTimeline();
    void testTickAdvancesPlaybackAndStopsAtEnd();
    void testTrackAndClipLifecycleCommandsUpdateDocument();
    void testMediaImportAndInsertionCommandsUpdateDocument();
    void testSplitClipCommandUpdatesTimeline();
    void testTrimClipCommandsUpdateTimeline();
    void testProjectAndClipEditCommandsUpdateDocument();
    void testSelectionCommandsSwitchSingleSelection();
    void testExportCommandsUpdateCoreRequest();
    void testQtRenderRequestPreservesPlaybackSpeed();
    void testQtBridgeBuildsDocumentCore();
    void testDocumentBuildsTimelineRenderData();
    void testCoreDocumentJsonRoundTrips();
    void testLegacyStateJsonBuildsDocumentCore();
    void testLegacyStateJsonPreservesQtOnlyArtifactFields();
    void testCoreDocumentFileRoundTrips();
    void testImGuiProjectSessionLoadsActiveQtProjectFiles();
    void testImGuiProjectSessionSaveWritesQtStateFiles();
    void testAiLoginPersistsRefreshTokenContract();
    void testAudioClipDurationPreservesSubframeSamples();
};

namespace {

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
    QCOMPARE(runtime.snapshot().transport.playbackSpeed, 2.0f);

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
    QCOMPARE(runtime.snapshot().mediaItems.size(), std::size_t(5));

    applyCommand(runtime, jcut::DeleteTrackCommand{runtime.snapshot().tracks.back().id});
    QCOMPARE(runtime.snapshot().tracks.size(), std::size_t(4));

    const jcut::CommandResult missingClip =
        runtime.execute(jcut::EditorCommand{jcut::DeleteClipCommand{999}});
    QCOMPARE(missingClip.applied, false);
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

    const jcut::CommandResult missingMedia =
        runtime.execute(jcut::EditorCommand{jcut::InsertClipFromMediaCommand{"missing", 1, 0, 10}});
    QCOMPARE(missingMedia.applied, false);
}

void TestEditorRuntime::testSplitClipCommandUpdatesTimeline()
{
    jcut::EditorRuntime runtime = jcut::EditorRuntime::createDemo();

    applyCommand(runtime, jcut::SplitClipCommand{1, 120});
    QCOMPARE(runtime.snapshot().clips.size(), std::size_t(5));
    QCOMPARE(runtime.snapshot().clips.at(0).id, 1);
    QCOMPARE(runtime.snapshot().clips.at(0).durationFrames, 120);
    QCOMPARE(runtime.snapshot().clips.at(1).trackId, 1);
    QCOMPARE(runtime.snapshot().clips.at(1).startFrame, 120);
    QCOMPARE(runtime.snapshot().clips.at(1).durationFrames, 300);
    QCOMPARE(runtime.snapshot().clips.at(1).selected, true);

    const jcut::CommandResult invalidSplit =
        runtime.execute(jcut::EditorCommand{jcut::SplitClipCommand{1, 0}});
    QCOMPARE(invalidSplit.applied, false);

    const jcut::CommandResult missingClip =
        runtime.execute(jcut::EditorCommand{jcut::SplitClipCommand{999, 12}});
    QCOMPARE(missingClip.applied, false);
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

void TestEditorRuntime::testAiLoginPersistsRefreshTokenContract()
{
    QFile source(QStringLiteral(JCUT_SOURCE_DIR "/editor_ai_integration.cpp"));
    QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(source.fileName()));
    const QString code = QString::fromUtf8(source.readAll());

    QVERIFY(code.contains(QStringLiteral("loginResult.value().refreshToken.trimmed()")));
    QVERIFY(code.contains(QStringLiteral("cppmonetize::loadStoredAuthSession(*store)")));
    QVERIFY(code.contains(QStringLiteral("cppmonetize::storeStoredAuthSession")));
    QVERIFY(code.contains(QStringLiteral("OAuthDesktopFlow oauthFlow")));
    QVERIFY(code.contains(QStringLiteral("oauthFlow.refreshWithToken")));
    QVERIFY(code.contains(QStringLiteral("refreshAiAuthTokenFromSecureStore()")));
    QVERIFY(!code.contains(QStringLiteral("Q_UNUSED(refreshToken);")));
    QVERIFY(!code.contains(QStringLiteral("Q_UNUSED(this);\n            return false;")));
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

    const jcut::render::RenderRequestCore core = jcut::render::toCoreRenderRequest(request);

    QCOMPARE(core.outputFps, 60.0);
    QCOMPARE(core.playbackSpeed, 1.75);
    QCOMPARE(core.backgroundFillOpacity, 0.42);
    QCOMPARE(core.backgroundFillBrightness, -0.15);
    QCOMPARE(core.backgroundFillSaturation, 1.25);
    QCOMPARE(core.backgroundFillEdgePixels, 32);
    QCOMPARE(core.backgroundFillEdgeProgressive, true);
    QCOMPARE(core.backgroundFillEdgePower, 3.5);
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

    TimelineClip clipB;
    clipB.filePath = QStringLiteral("/tmp/vo.wav");
    clipB.mediaType = ClipMediaType::Audio;
    clipB.trackIndex = 1;
    clipB.startFrame = 24;
    clipB.durationFrames = 120;

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

void TestEditorRuntime::testCoreDocumentJsonRoundTrips()
{
    const jcut::EditorDocumentCore original = jcut::EditorRuntime::createDemo().snapshot();

    std::string error;
    const nlohmann::json json = jcut::toJson(original);
    const std::optional<jcut::EditorDocumentCore> reparsed =
        jcut::editorDocumentCoreFromJson(json, &error);

    QVERIFY2(reparsed.has_value(), error.c_str());
    QCOMPARE(QString::fromStdString(reparsed->projectName), QString::fromStdString(original.projectName));
    QCOMPARE(reparsed->tracks.size(), original.tracks.size());
    QCOMPARE(reparsed->clips.size(), original.clips.size());
    QCOMPARE(reparsed->mediaItems.size(), original.mediaItems.size());
    QCOMPARE(reparsed->exportRequest.outputSize.width, original.exportRequest.outputSize.width);
    QCOMPARE(reparsed->exportRequest.outputSize.height, original.exportRequest.outputSize.height);
    QCOMPARE(reparsed->exportRequest.playbackSpeed, original.exportRequest.playbackSpeed);
    QCOMPARE(reparsed->transport.currentFrame, original.transport.currentFrame);
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
    QCOMPARE(QString::fromStdString(parsed->clips.back().label), QStringLiteral("b.wav"));
    QCOMPARE(parsed->mediaItems.size(), std::size_t(2));
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
                {"gradingKeyframes", nlohmann::json::array({{{"frame", 10}, {"brightness", 0.25}}})},
                {"transcriptPath", "transcripts/interview.json"},
                {"speakerTrackingEnabled", true},
                {"correctionPolygons", nlohmann::json::array({{{"enabled", true}}})}
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

    const nlohmann::json saved = jcut::toLegacyStateJson(updated, &base);
    QCOMPARE(QString::fromStdString(saved.value("mediaRoot", std::string{})), QStringLiteral("/media/root"));
    const nlohmann::json track = saved.at("tracks").at(0);
    QCOMPARE(track.value("height", 0), 72);
    QCOMPARE(track.value("visualsEnabled", true), false);
    const nlohmann::json clip = saved.at("timeline").at(0);
    QCOMPARE(QString::fromStdString(clip.value("id", std::string{})), QStringLiteral("clip-a"));
    QCOMPARE(clip.value("startFrame", 0), 12);
    QVERIFY(clip.contains("gradingKeyframes"));
    QVERIFY(clip.contains("transcriptPath"));
    QVERIFY(clip.contains("speakerTrackingEnabled"));
    QVERIFY(clip.contains("correctionPolygons"));
    QCOMPARE(QString::fromStdString(saved.value("selectedClipId", std::string{})), QStringLiteral("clip-a"));
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
    const QByteArray historyBytes = historyFile.readAll();
    QVERIFY(historyBytes.contains("\"entries\""));
    QVERIFY(historyBytes.contains("Saved From ImGui"));
}

QTEST_MAIN(TestEditorRuntime)
#include "test_editor_runtime.moc"
