#include <QtTest/QtTest>

#include "../editor_runtime.h"
#include "../image_sequence_directory.h"
#include "../imgui_audio_runtime.h"
#include "../standalone_preview_renderer.h"
#include "../standalone_timeline_renderer.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <fstream>

class TestImGuiStandaloneRender : public QObject {
    Q_OBJECT

private slots:
    void testRenderPreviewFrameDecodesImageClip();
    void testLegacyClipWithoutMediaKindIsVisual();
    void testPreviewKeepsZeroCopyWithCpuFallbackContract();
    void testStandaloneImportProbeReportsAudioPresence();
    void testImageSequenceDirectoryProbeAndRender();
    void testLegacyUnknownAudioPresenceIsProbedOnLoad();
    void testAudioFacadeRefreshesIdleTimelineStatus();
    void testAudioFacadeTracksDerivedSidecarAvailability();
    void testAudioFacadeRefreshesReplacedSourceTopology();
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

bool writeSolidBmp(const QString& path,
                   quint8 red,
                   quint8 green,
                   quint8 blue,
                   int width = 2,
                   int height = 2)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int rowStride = ((width * 3) + 3) & ~3;
    const int pixelBytes = rowStride * height;
    QByteArray bytes(54 + pixelBytes, '\0');
    auto put16 = [&](int offset, quint16 value) {
        bytes[offset] = static_cast<char>(value & 0xff);
        bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
    };
    auto put32 = [&](int offset, quint32 value) {
        bytes[offset] = static_cast<char>(value & 0xff);
        bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
        bytes[offset + 2] = static_cast<char>((value >> 16) & 0xff);
        bytes[offset + 3] = static_cast<char>((value >> 24) & 0xff);
    };
    bytes[0] = 'B';
    bytes[1] = 'M';
    put32(2, static_cast<quint32>(bytes.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    put16(26, 1);
    put16(28, 24);
    put32(34, pixelBytes);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int offset = 54 + y * rowStride + x * 3;
            bytes[offset] = static_cast<char>(blue);
            bytes[offset + 1] = static_cast<char>(green);
            bytes[offset + 2] = static_cast<char>(red);
        }
    }

    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString readSourceFile(const QString& relativePath)
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

void TestImGuiStandaloneRender::testRenderPreviewFrameDecodesImageClip()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("frame.ppm"));
    {
        std::ofstream output(imagePath.toStdString(), std::ios::binary);
        QVERIFY(output.is_open());
        output << "P6\n2 2\n255\n";
        const unsigned char pixels[] = {
            255, 0, 0,   0, 255, 0,
            0, 0, 255,   255, 255, 0
        };
        output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
        QVERIFY(output.good());
    }

    jcut::EditorDocumentCore document;
    document.projectName = "Preview";
    document.tracks.push_back({1, "Video", true});
    document.mediaItems.push_back({imagePath.toStdString(), "frame", "image"});
    document.clips.push_back({1, 1, "frame", 0, 30, true, imagePath.toStdString()});
    document.transport.currentFrame = 0;
    document.exportRequest.outputSize = {320, 240};

    const jcut::standalone_render::PreviewRenderResult result =
        jcut::standalone_render::renderPreviewFrame({
            document,
            document.exportRequest.outputSize,
            0
        });

    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(!result.image.empty());
    QCOMPARE(result.image.size.width, 320);
    QCOMPARE(result.image.size.height, 240);

    const int centerX = result.image.size.width / 2;
    const int centerY = result.image.size.height / 2;
    const std::size_t offset =
        static_cast<std::size_t>(centerY * result.image.strideBytes + centerX * 4);
    const bool nonBlack =
        result.image.bytes[offset + 0] > 0 ||
        result.image.bytes[offset + 1] > 0 ||
        result.image.bytes[offset + 2] > 0;
    QVERIFY(nonBlack);
}

void TestImGuiStandaloneRender::testLegacyClipWithoutMediaKindIsVisual()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("legacy.ppm"));
    {
        std::ofstream output(imagePath.toStdString(), std::ios::binary);
        QVERIFY(output.is_open());
        output << "P6\n2 2\n255\n";
        const unsigned char pixels[] = {
            240, 32, 32,   240, 32, 32,
            240, 32, 32,   240, 32, 32
        };
        output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
        QVERIFY(output.good());
    }

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    document.clips.push_back({1, 1, "clip", 0, 30, true, imagePath.toStdString()});
    document.exportRequest.outputSize = {160, 120};

    const jcut::standalone_render::PreviewRenderResult result =
        jcut::standalone_render::renderPreviewFrame({
            document,
            document.exportRequest.outputSize,
            12
        });

    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(!result.image.empty());
}

void TestImGuiStandaloneRender::testPreviewKeepsZeroCopyWithCpuFallbackContract()
{
    const QString shell = readSourceFile(QStringLiteral("jcut_imgui_main.cpp"));
    const QString preview = readSourceFile(QStringLiteral("standalone_preview_renderer.cpp"));
    const QString previewHeader = readSourceFile(QStringLiteral("standalone_preview_renderer.h"));
    const QString audioRuntime = readSourceFile(QStringLiteral("imgui_audio_runtime.cpp"));
    const QString sequenceCore =
        readSourceFile(QStringLiteral("image_sequence_directory.cpp"));
    const QString qtMedia = readSourceFile(QStringLiteral("editor_shared_media.cpp"));
    const QString timelineRenderer =
        readSourceFile(QStringLiteral("standalone_timeline_renderer.cpp"));
    const QString qtEditorPane =
        readSourceFile(QStringLiteral("editor_editor_pane.cpp"));
    const QString scaleToFill =
        readSourceFile(QStringLiteral("editor_scale_to_fill.h"));
    const QString frameImporter = readSourceFile(QStringLiteral("imgui_vulkan_frame_importer.cpp"));
    const QString frameImporterHeader =
        readSourceFile(QStringLiteral("imgui_vulkan_frame_importer.h"));
    const QString frameImportCore =
        readSourceFile(QStringLiteral("vulkan_external_frame_import_core.cpp"));
    const QString frameImportCoreHeader =
        readSourceFile(QStringLiteral("vulkan_external_frame_import_core.h"));
    const QString legacyFrameHandoff =
        readSourceFile(QStringLiteral("vulkan_detector_frame_handoff.cpp"));
    QVERIFY2(!shell.isEmpty(), "jcut_imgui_main.cpp must be readable");
    QVERIFY2(!preview.isEmpty(), "standalone_preview_renderer.cpp must be readable");
    QVERIFY2(!previewHeader.isEmpty(), "standalone_preview_renderer.h must be readable");
    QVERIFY2(!audioRuntime.isEmpty(), "imgui_audio_runtime.cpp must be readable");
    QVERIFY2(!sequenceCore.isEmpty(), "image_sequence_directory.cpp must be readable");
    QVERIFY2(!qtMedia.isEmpty(), "editor_shared_media.cpp must be readable");
    QVERIFY2(!timelineRenderer.isEmpty(), "standalone_timeline_renderer.cpp must be readable");
    QVERIFY2(!qtEditorPane.isEmpty(), "editor_editor_pane.cpp must be readable");
    QVERIFY2(!scaleToFill.isEmpty(), "editor_scale_to_fill.h must be readable");
    QVERIFY2(!frameImporter.isEmpty(), "imgui_vulkan_frame_importer.cpp must be readable");
    QVERIFY2(!frameImporterHeader.isEmpty(), "imgui_vulkan_frame_importer.h must be readable");
    QVERIFY2(!frameImportCore.isEmpty(), "vulkan_external_frame_import_core.cpp must be readable");
    QVERIFY2(!frameImportCoreHeader.isEmpty(), "vulkan_external_frame_import_core.h must be readable");
    QVERIFY2(!legacyFrameHandoff.isEmpty(), "vulkan_detector_frame_handoff.cpp must be readable");

    QVERIFY2(shell.contains(QStringLiteral("bindPreviewFrame(previewResult.vulkanFrame")),
             "ImGui preview must try importing offscreen Vulkan frames before CPU upload");
    QVERIFY2(shell.contains(QStringLiteral("uploadPreviewImage(previewResult.image")),
             "ImGui preview must retain CPU upload fallback for devices without external-frame import");
    QVERIFY2(shell.contains(QStringLiteral("previewCpuFallbackPreferred")),
             "ImGui preview must adaptively switch to CPU fallback after zero-copy import fails");
    QVERIFY2(!shell.contains(QStringLiteral("audio disabled in Qt-free ImGui shell")),
             "ImGui audio must not be hard-disabled as a shell policy");
    QVERIFY2(shell.contains(QStringLiteral("#include \"imgui_audio_runtime.h\"")) &&
                 !shell.contains(QStringLiteral("#include \"audio_engine.h\"")) &&
                 !shell.contains(QStringLiteral("QVector<")),
             "the ImGui shell must consume audio through the Qt-free facade");
    QVERIFY2(audioRuntime.contains(QStringLiteral("AudioEngine m_audioEngine")) &&
                 audioRuntime.contains(QStringLiteral("normalizedPlaybackAudioWarpMode")) &&
                 audioRuntime.contains(QStringLiteral("effectivePlaybackAudioWarpRate")) &&
                 audioRuntime.contains(QStringLiteral("setTimelineStateAtFrame")) &&
                 audioRuntime.contains(QStringLiteral("{\"fadeSamples\", clip.fadeSamples}")) &&
                 audioRuntime.contains(QStringLiteral("std::launch::async")) &&
                 audioRuntime.contains(QStringLiteral("kAudioWarmTimeoutMs")),
             "the facade must preserve the existing audio engine, clip-fade invalidation, and playback-warp policy");
    QVERIFY2(audioRuntime.contains(QStringLiteral("audioSourceIdentitySignature")) &&
                 audioRuntime.contains(QStringLiteral("last_write_time")) &&
                 audioRuntime.contains(QStringLiteral("replace_extension(\".wav\")")) &&
                 audioRuntime.contains(QStringLiteral("invalidateAudioSourceCaches")),
             "source availability polling must cover replacements and derived WAV sidecars");
    QVERIFY2(shell.contains(QStringLiteral("holdForAudioWarmup")) &&
                 shell.contains(QStringLiteral("preTickAudioStatus.buffering")),
             "transport must remain at the requested frame while audio warms asynchronously");
    QVERIFY2(audioRuntime.contains(
                 QStringLiteral("playbackAudioWarmupPermanentlyFailed")) &&
                 audioRuntime.contains(QStringLiteral("decode still pending")) &&
                 !audioRuntime.contains(
                     QStringLiteral("continuing playback without warmed audio")),
             "a timed-out but pending decode must remain gated until ready or terminal");
    QVERIFY2(shell.contains(QStringLiteral("probeStandaloneMedia")) &&
                 shell.contains(QStringLiteral("mediaInfo.hasAudio")) &&
                 shell.contains(QStringLiteral("probeUnknownAudioPresence")),
             "ImGui imports and legacy loads must persist probed stream metadata instead of assuming every video has audio");
    QVERIFY2(shell.contains(QStringLiteral("#include \"imgui_vulkan_frame_importer.h\"")) &&
                 !shell.contains(QStringLiteral("#include \"vulkan_detector_frame_handoff.h\"")),
             "the ImGui shell must consume Vulkan frames through the neutral importer facade");
    QVERIFY2(shell.contains(QStringLiteral("CopySelectedClipsCommand")) &&
                 shell.contains(QStringLiteral("CutSelectedClipsCommand")) &&
                 shell.contains(QStringLiteral("PasteClipsCommand")) &&
                 shell.contains(QStringLiteral("DuplicateSelectedClipsCommand")) &&
                 shell.contains(QStringLiteral("TimelineClipContext")),
             "the ImGui shell must expose the neutral clipboard commands through shortcuts, menus, and timeline context actions");
    QVERIFY2(shell.contains(QStringLiteral("kProjectMediaDragPayload")) &&
                 shell.contains(QStringLiteral("kFilesystemMediaDragPayload")) &&
                 shell.contains(QStringLiteral("BeginDragDropSource")) &&
                 shell.contains(QStringLiteral("SetDragDropPayload")) &&
                 shell.contains(QStringLiteral("BeginDragDropTarget")) &&
                 shell.contains(QStringLiteral("AcceptDragDropPayload")) &&
                 shell.contains(QStringLiteral("InsertClipFromMediaCommand")) &&
                 shell.contains(QStringLiteral("addClipCommandForPath")) &&
                 shell.contains(QStringLiteral("timelineTrackDropTarget")) &&
                 shell.contains(QStringLiteral("firstNonConflictingTrackIndex")) &&
                 shell.contains(QStringLiteral("AddTrackCommand")) &&
                 shell.contains(QStringLiteral("ReorderTrackCommand")),
             "project and filesystem media must reuse neutral insertion commands through a real timeline drag/drop target with gap creation and conflict routing");
    QVERIFY2(shell.contains(QStringLiteral("isImageSequenceDirectory")) &&
                 shell.contains(QStringLiteral("isImportableMediaPath")) &&
                 shell.contains(QStringLiteral("isDir && !isSequence")) &&
                 shell.contains(QStringLiteral("[sequence]")) &&
                 shell.contains(QStringLiteral("resolvedMediaDurationFrames")) &&
                 qtMedia.contains(QStringLiteral("probeImageSequenceDirectory")) &&
                 timelineRenderer.contains(QStringLiteral("m_sequenceFramePaths")) &&
                 timelineRenderer.contains(QStringLiteral("m_sequenceFrameSource")) &&
                 sequenceCore.contains(QStringLiteral("numberedFiles * 2")),
             "Qt and ImGui media paths must share neutral image-sequence detection while ordinary directories still navigate and standalone rendering decodes ordered sequence frames");
    QVERIFY2(shell.contains(QStringLiteral("RemoveMediaCommand")) &&
                 shell.contains(QStringLiteral("ProjectMediaContext")) &&
                 shell.contains(QStringLiteral("Remove from Project")),
             "project media removal must use the neutral guarded command from an explicit context action");
    QVERIFY2(shell.contains(QStringLiteral("SplitSelectedClipsCommand")) &&
                 shell.contains(QStringLiteral("TimelineToolMode::Razor")) &&
                 shell.contains(QStringLiteral("snapTimelineMoveStart")) &&
                 shell.contains(QStringLiteral("timelineSnapIndicatorFrame")),
             "the ImGui timeline must expose shared razor semantics and visible boundary snapping");
    QVERIFY2(shell.contains(QStringLiteral("RemoveClipKeyframeCommand")) &&
                 shell.contains(QStringLiteral("EditorKeyframeChannel::Grading")) &&
                 shell.contains(QStringLiteral("EditorKeyframeChannel::Opacity")) &&
                 shell.contains(QStringLiteral("EditorKeyframeChannel::Transform")) &&
                 shell.contains(QStringLiteral("Set Hold")) &&
                 shell.contains(QStringLiteral("Set Linear")),
             "grading, opacity, and transform tables must expose undoable keyframe removal and interpolation editing");
    QVERIFY2(shell.contains(QStringLiteral("InspectorKeyframeDraft")) &&
                 shell.contains(QStringLiteral("drawKeyframeDraftEditor")) &&
                 shell.contains(QStringLiteral("commitKeyframeDraft")) &&
                 shell.contains(QStringLiteral("draft.originalFrame != keyframe->frame")) &&
                 shell.contains(QStringLiteral("UpsertCommand{clipId, *keyframe}")) &&
                 shell.contains(QStringLiteral("Grade Opacity")) &&
                 shell.contains(QStringLiteral("Lift RGB")) &&
                 shell.contains(QStringLiteral("Gamma RGB")) &&
                 shell.contains(QStringLiteral("Gain RGB")) &&
                 shell.contains(QStringLiteral("&draft->shadowsR")) &&
                 shell.contains(QStringLiteral("&draft->midtonesG")) &&
                 shell.contains(QStringLiteral("&draft->highlightsB")) &&
                 shell.contains(QStringLiteral("ImGuiDataType_Double")) &&
                 shell.contains(QStringLiteral(
                     "evaluateEditorClipGradingAtLocalFrame")) &&
                 shell.contains(QStringLiteral("Key Opacity")) &&
                 shell.contains(QStringLiteral("Transform Title")) &&
                 shell.contains(QStringLiteral("Load/Edit")) &&
                 shell.contains(QStringLiteral("New At Playhead")),
             "grade, opacity, and transform must share a full neutral keyframe draft editor with atomic scoped frame replacement, including Qt-range Lift/Gamma/Gain RGB editing");
    QVERIFY2(shell.contains(QStringLiteral("editor_grading_core.h")) &&
                 shell.contains(QStringLiteral("Three-point lock")) &&
                 shell.contains(QStringLiteral("Curve smoothing")) &&
                 shell.contains(QStringLiteral("CurvePointTable")) &&
                 shell.contains(QStringLiteral("##X")) &&
                 shell.contains(QStringLiteral("##Y")) &&
                 shell.contains(QStringLiteral("Add point")) &&
                 shell.contains(QStringLiteral("Remove")) &&
                 shell.contains(QStringLiteral("Reset channel")) &&
                 shell.contains(QStringLiteral("Fixed X")) &&
                 shell.contains(QStringLiteral("kCurveXMinimum = 0.0")) &&
                 shell.contains(QStringLiteral("kCurveXMaximum = 1.0")) &&
                 shell.contains(QStringLiteral("kCurveYMinimum = -1.0")) &&
                 shell.contains(QStringLiteral("kCurveYMaximum = 2.0")) &&
                 shell.contains(QStringLiteral("ImGui::BeginDisabled(editsDisabled)")) &&
                 shell.contains(QStringLiteral("sanitizeEditorGradingCurve")) &&
                 shell.contains(QStringLiteral(
                     "synchronizeEditorThreePointGradingCurves")) &&
                 shell.contains(QStringLiteral("Normalize curves")) &&
                 shell.contains(QStringLiteral(
                     "normalizeEditorGradingCurves(*draft)")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsR, draft->curveThreePointLock")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsG, draft->curveThreePointLock")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsB, draft->curveThreePointLock")) &&
                 shell.contains(QStringLiteral(
                     "\"Luma\", &draft->curvePointsLuma, false")),
             "the Grade draft must expose bounded, sanitized RGB/Luma point tables, synchronize locked RGB curves from tones, normalize through the shared helper, and keep Luma independently editable");
    QVERIFY2(!shell.contains(QStringLiteral("Curve lock: %s | smoothing: %s")),
             "editable curve controls must replace the old read-only curve status text");
    QVERIFY2(shell.contains(QStringLiteral("Scale to Fill Preview")) &&
                 shell.contains(QStringLiteral("scaleClipToFillPreview")) &&
                 shell.contains(QStringLiteral("jcut::SetClipTransformCommand")) &&
                 shell.contains(QStringLiteral("mediaInfo.frameSize")) &&
                 qtEditorPane.contains(QStringLiteral("jcut::scaleToFillFactor")) &&
                 qtMedia.contains(QStringLiteral("QImageReader(filePath).size()")) &&
                 scaleToFill.contains(QStringLiteral("scaleToFillFactor")) &&
                 timelineRenderer.contains(QStringLiteral("result.frameSize")),
             "Qt and ImGui scale-to-fill actions must share neutral aspect math and the undoable transform command while standalone probes expose source dimensions");
    QVERIFY2(shell.contains(QStringLiteral("RenderSyncMarkerDraft")) &&
                 shell.contains(QStringLiteral("renderSyncMarkerForClipAtFrame")) &&
                 shell.contains(QStringLiteral("requestRenderSyncMarkerCount")) &&
                 shell.contains(QStringLiteral("Duplicate Frames For Clip...")) &&
                 shell.contains(QStringLiteral("Skip Frames For Clip...")) &&
                 shell.contains(QStringLiteral("Clear At Playhead")) &&
                 shell.contains(QStringLiteral("Render Sync Count")) &&
                 shell.contains(QStringLiteral("kEditorRenderSyncMinCount")) &&
                 shell.contains(QStringLiteral("kEditorRenderSyncMaxCount")) &&
                 shell.contains(QStringLiteral("AddRenderSyncMarkerCommand")) &&
                 shell.contains(QStringLiteral("RemoveRenderSyncMarkerCommand")) &&
                 shell.contains(QStringLiteral("editorRenderSyncOwnerClipId")) &&
                 shell.contains(QStringLiteral("hoveredRenderSyncMarker")) &&
                 shell.contains(QStringLiteral("TimelineSyncMarkerContext")) &&
                 shell.contains(QStringLiteral("clipPersistentId")) &&
                 shell.contains(QStringLiteral("documentGeneration")),
             "the timeline must reuse neutral render-sync commands with Qt count bounds, canonical ownership, visible marker hit-testing, scoped removal, and reload-safe modal identity");
    QVERIFY2(shell.contains(QStringLiteral("Reset Grading")) &&
                 shell.contains(QStringLiteral("ResetClipGradingCommand")),
             "the timeline context menu must route grading reset through one undoable neutral command");
    QVERIFY2(shell.contains(QStringLiteral("jcut::EditorTitleKeyframe titleDraft")) &&
                 shell.contains(QStringLiteral("hydrateTitleDraft")) &&
                 shell.contains(QStringLiteral("&shellState->titleDraft.text")) &&
                 !shell.contains(QStringLiteral("titleDraftText")) &&
                 shell.contains(QStringLiteral("Title Opacity")) &&
                 shell.contains(QStringLiteral("Font Family")) &&
                 shell.contains(QStringLiteral("editHexRgbColor(\"Title Color\"")) &&
                 shell.contains(QStringLiteral("Linear Interpolation")) &&
                 shell.contains(QStringLiteral("UpsertTitleKeyframeCommand")) &&
                 shell.contains(QStringLiteral("RemoveTitleKeyframeCommand")) &&
                 shell.contains(QStringLiteral("title-frame-")) &&
                 shell.contains(QStringLiteral("SmallButton(\"Load\")")),
             "the title inspector must edit every neutral title field and support row load/seek/removal");
    QVERIFY2(shell.contains(QStringLiteral("parseHexRgbColor")) &&
                 shell.contains(QStringLiteral("formatHexRgbColor")) &&
                 shell.contains(QStringLiteral("editHexRgbColor")) &&
                 shell.contains(QStringLiteral("Manual Placement")) &&
                 shell.contains(QStringLiteral("Center X")) &&
                 shell.contains(QStringLiteral("Center Y")) &&
                 shell.contains(QStringLiteral("Font Family")) &&
                 shell.contains(QStringLiteral("Font Size")) &&
                 shell.contains(QStringLiteral("ImGui::Checkbox(\"Bold\", &overlay.bold)")) &&
                 shell.contains(QStringLiteral("ImGui::Checkbox(\"Italic\", &overlay.italic)")) &&
                 shell.contains(QStringLiteral("Show Shadow")) &&
                 shell.contains(QStringLiteral("Text Color")) &&
                 shell.contains(QStringLiteral("Background Color")) &&
                 shell.contains(QStringLiteral("Highlight Color")) &&
                 shell.contains(QStringLiteral("Highlight Text Color")) &&
                 shell.contains(QStringLiteral("SetClipTranscriptOverlayCommand")) &&
                 !shell.contains(QStringLiteral("#include <QColor>")),
             "the transcript inspector must expose every neutral overlay style and placement field through Qt-free color helpers and the shared runtime command");
    QVERIFY2(shell.contains(QStringLiteral("drawFrameSeekCell")) &&
                 shell.contains(QStringLiteral("currentClip->startFrame) + keyframe.frame")) &&
                 shell.contains(QStringLiteral("jcut::SeekToFrameCommand")) &&
                 shell.contains(QStringLiteral("SmallButton(\"Seek\")")) &&
                 shell.contains(QStringLiteral("RemoveRenderSyncMarkerCommand")) &&
                 shell.contains(QStringLiteral("replaceRenderSyncMarker")) &&
                 shell.contains(QStringLiteral("##operation")),
             "keyframe and sync tables must reuse neutral seeking and expose atomic marker frame/count/action editing plus scoped removal");
    QVERIFY2(shell.contains(QStringLiteral("kTrackVisualModeLabels")) &&
                 shell.contains(QStringLiteral("Force Opaque")) &&
                 shell.contains(QStringLiteral("SetTrackPropertiesCommand")) &&
                 shell.contains(QStringLiteral("##trackLabel")) &&
                 shell.contains(QStringLiteral(
                     "ImGui::InputText(\"##trackLabel\", &trackLabel)")) &&
                 shell.contains(QStringLiteral("##trackHeight")) &&
                 shell.contains(QStringLiteral("kEditorTrackMinHeight")) &&
                 shell.contains(QStringLiteral("kEditorTrackMaxHeight")) &&
                 shell.contains(QStringLiteral("##audioEnabled")) &&
                 shell.contains(QStringLiteral("##audioGain")) &&
                 shell.contains(QStringLiteral("##audioMuted")) &&
                 shell.contains(QStringLiteral("##audioSolo")) &&
                 shell.contains(QStringLiteral("SetTrackStateCommand trackState")) &&
                 shell.contains(QStringLiteral("trackState.visualMode = visualMode")) &&
                 shell.contains(QStringLiteral(
                     "trackState.gradingPreviewEnabled = gradingPreviewEnabled")) &&
                 shell.contains(QStringLiteral("trackState.audioMuted = audioMuted")) &&
                 shell.contains(QStringLiteral("trackState.audioSolo = audioSolo")),
             "the Tracks inspector must preserve peer state while exposing label/height, all three visual modes, and audio enable/gain/mute/solo controls");
    QVERIFY2(shell.contains(QStringLiteral("trackCrossfadeSeconds = 0.5f")) &&
                 shell.contains(QStringLiteral("trackCrossfadeMoveClips = false")) &&
                 shell.contains(QStringLiteral("Crossfade (seconds)")) &&
                 shell.contains(QStringLiteral("Move clips to overlap")) &&
                 shell.contains(QStringLiteral("Crossfade Consecutive Clips")) &&
                 shell.contains(QStringLiteral("crossfadeClipCount < 2")) &&
                 shell.contains(QStringLiteral("CrossfadeTrackCommand")) &&
                 shell.contains(QStringLiteral(
                     "jcut::isGeneratedEditorChildTrack(*crossfadeTrack)")),
             "the Tracks inspector must dispatch the neutral track crossfade command with Qt-matching duration and overlap controls while excluding derived lanes");
    QVERIFY2(shell.contains(QStringLiteral("kGeneratedTrackLabelPrefix")) &&
                 shell.contains(QStringLiteral("kGeneratedTrackLaneColor")) &&
                 shell.contains(QStringLiteral("kGeneratedTrackClipColor")) &&
                 shell.contains(QStringLiteral("kGeneratedTrackSelectedClipColor")) &&
                 shell.contains(QStringLiteral(
                     "jcut::isGeneratedEditorChildTrack(track)")),
             "generated child rows and clips must have an indented, subdued timeline treatment");
    QVERIFY2(shell.contains(QStringLiteral(
                 "jcut::isGeneratedEditorChildTrack(\n"
                 "            snapshot.tracks[static_cast<std::size_t>(row)])")) &&
                 shell.contains(QStringLiteral(
                     "inserting a normal lane before the child")) &&
                 shell.contains(QStringLiteral(
                     "requestedInsertionIndex});")) &&
                 shell.contains(QStringLiteral(
                     "candidate.selected &&")) &&
                 shell.contains(QStringLiteral("targetTrackIndex = -1")) &&
                 shell.contains(QStringLiteral(
                     "!jcut::isGeneratedEditorChildTrack(snapshot.tracks[")),
             "media drops must atomically insert a normal row at a generated-row boundary, conflict routing must reject child lanes, and clip moves must not target them");
    QVERIFY2(shell.contains(QStringLiteral("generatedTrackIdentity")) &&
                 shell.contains(QStringLiteral("track.parentClipId")) &&
                 shell.contains(QStringLiteral("track.childClipId")) &&
                 shell.contains(QStringLiteral(
                     "ImGui::TextUnformatted(track.label.c_str())")) &&
                 shell.contains(QStringLiteral(
                     "adjacentOrdinaryTrackIndex")) &&
                 shell.contains(QStringLiteral(
                     "ImGui::BeginDisabled(generatedChildTrack);")),
             "the Tracks inspector must expose read-only child identity, disable derived-row ordering/audio controls, and keep selection, height, and visual state outside those disabled scopes");
    const auto tracksTableOffset =
        shell.indexOf(QStringLiteral("void drawTracksTable"));
    const auto trackSelectionOffset = shell.indexOf(
        QStringLiteral("ImGui::Selectable(trackNumber.c_str(), track.selected)"),
        tracksTableOffset);
    const auto generatedLabelOffset = shell.indexOf(
        QStringLiteral("if (generatedChildTrack) {"), tracksTableOffset);
    const auto trackHeightOffset = shell.indexOf(
        QStringLiteral("ImGui::DragInt(\n            \"##trackHeight\""),
        generatedLabelOffset);
    const auto reorderDisabledOffset = shell.indexOf(
        QStringLiteral(
            "ImGui::BeginDisabled(\n            generatedChildTrack || previousOrdinaryTrack < 0)"),
        trackHeightOffset);
    const auto visualModeOffset = shell.indexOf(
        QStringLiteral("ImGui::Combo(\"##visualMode\""),
        reorderDisabledOffset);
    const auto audioDisabledOffset = shell.indexOf(
        QStringLiteral("ImGui::BeginDisabled(generatedChildTrack);"),
        visualModeOffset);
    QVERIFY2(tracksTableOffset >= 0 &&
                 trackSelectionOffset > tracksTableOffset &&
                 generatedLabelOffset > trackSelectionOffset &&
                 trackHeightOffset > generatedLabelOffset &&
                 reorderDisabledOffset > trackHeightOffset &&
                 visualModeOffset > reorderDisabledOffset &&
                 audioDisabledOffset > visualModeOffset,
             "child-row selection and height must remain enabled, visual mode must remain editable, and only reorder/audio controls may enter disabled scopes");
    QVERIFY2(shell.contains(QStringLiteral(
                 "ImGui::Checkbox(\"##gradingPreviewEnabled\"")) &&
                 shell.indexOf(QStringLiteral(
                     "ImGui::Checkbox(\"##gradingPreviewEnabled\""),
                     visualModeOffset) < audioDisabledOffset,
             "generated child rows must retain an independently editable grading-preview state outside the disabled audio scope");
    QVERIFY2(shell.contains(QStringLiteral(
                 "int adjacentOrdinaryTrackIndex")) &&
                 shell.contains(QStringLiteral(
                     "track.id, previousOrdinaryTrack")) &&
                 shell.contains(QStringLiteral(
                     "track.id, nextOrdinaryTrack")),
             "ordinary track reorder controls must skip derived child rows in either direction");
    QVERIFY2(shell.contains(QStringLiteral(
                 "clip.selected && !clip.locked && !maskMatteClip")) &&
                 shell.contains(QStringLiteral(
                     "clip.locked || maskMatteClip")) &&
                 shell.contains(QStringLiteral(
                     "canonicalEditorClipRole(clip.clipRole) == \"mask_matte\"")) &&
                 shell.contains(QStringLiteral(
                     "hoveredClipId != 0 && !hoveredClipIsMaskMatte")) &&
                 shell.contains(QStringLiteral(
                     "TimelineToolMode::Razor &&\n                !hoveredClipIsMaskMatte")) &&
                 shell.contains(QStringLiteral(
                     "const int maximumTrackHeight = generatedChildTrack")),
             "derived matte clips must not advertise direct drag/razor affordances and child height editing must use the runtime's 56px bound");
    QVERIFY2(shell.contains(
                 QStringLiteral("{\"mp4\", \"mov\", \"mkv\", \"webm\"}")),
             "the ImGui output format selector must expose the shared WebM backend");
    QVERIFY2(!previewHeader.contains(QStringLiteral("#include \"render_internal.h\"")) &&
                 previewHeader.contains(QStringLiteral("core/offscreen_vulkan_frame.h")),
             "the standalone preview contract must not expose the Qt render-internal header");
    QVERIFY2(!frameImporterHeader.contains(QStringLiteral("#include <Q")) &&
                 !frameImporterHeader.contains(QStringLiteral("vulkan_detector_frame_handoff.h")) &&
                 !frameImportCoreHeader.contains(QStringLiteral("#include <Q")) &&
                 !frameImportCore.contains(QStringLiteral("#include <Q")) &&
                 frameImporter.contains(QStringLiteral("VulkanExternalFrameImportCore importer")) &&
                 frameImportCore.contains(QStringLiteral("vkGetMemoryFdKHR")) &&
                 frameImportCore.contains(QStringLiteral("VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT")) &&
                 frameImportCore.contains(QStringLiteral("readySemaphoreFd")) &&
                 frameImportCore.contains(QStringLiteral("allocationResult != VK_SUCCESS")) &&
                 frameImportCore.contains(QStringLiteral("imageMemory != VK_NULL_HANDLE")) &&
                 frameImportCore.contains(QStringLiteral("importedImageMemory != VK_NULL_HANDLE")) &&
                 frameImportCore.contains(QStringLiteral("bool forceRecreate = false")) &&
                 frameImportCore.contains(QStringLiteral("errorMessage, true")) &&
                 frameImportCore.contains(QStringLiteral("image-memory binding is immutable")) &&
                 frameImportCore.contains(QStringLiteral("close(fd)")) &&
                 legacyFrameHandoff.contains(QStringLiteral("m_externalFrameImporter.importFrame")) &&
                 legacyFrameHandoff.contains(QStringLiteral("m_externalFrameImporter.recordFrameCopy")),
             "the ImGui frame importer must use the shared Qt-free opaque-FD core without changing caller-owned ready-semaphore behavior");

    QVERIFY2(preview.contains(QStringLiteral("renderPreviewFrameCore(")) &&
                 preview.contains(QStringLiteral("false,\n                    false")) &&
                 preview.contains(QStringLiteral("false,\n                    true")),
             "standalone preview must request zero-copy Vulkan first, then CPU readback fallback");
}

void TestImGuiStandaloneRender::testStandaloneImportProbeReportsAudioPresence()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString stillPath = tempDir.filePath(QStringLiteral("frame.bmp"));
    QVERIFY(writeSolidBmp(stillPath, 48, 96, 144, 7, 3));
    const jcut::standalone_render::StandaloneMediaInfo stillInfo =
        jcut::standalone_render::probeStandaloneMedia(stillPath.toStdString());
    QVERIFY(stillInfo.probed);
    QVERIFY(stillInfo.hasVideo);
    QCOMPARE(stillInfo.frameSize.width, 7);
    QCOMPARE(stillInfo.frameSize.height, 3);

    const QString audioPath = tempDir.filePath(QStringLiteral("voice.wav"));
    QVERIFY(writeSilentPcmWav(audioPath));
    const jcut::standalone_render::StandaloneMediaInfo audioInfo =
        jcut::standalone_render::probeStandaloneMedia(audioPath.toStdString());
    QVERIFY(audioInfo.probed);
    QVERIFY(audioInfo.hasAudio);
    QVERIFY(!audioInfo.hasVideo);
    QCOMPARE(QString::fromStdString(audioInfo.mediaKind), QStringLiteral("audio"));
    QVERIFY(audioInfo.audioStreamIndex >= 0);

    const jcut::standalone_render::StandaloneMediaInfo missingInfo =
        jcut::standalone_render::probeStandaloneMedia(
            tempDir.filePath(QStringLiteral("missing.mov")).toStdString());
    QVERIFY(!missingInfo.probed);
    QVERIFY(!missingInfo.hasAudio);
}

void TestImGuiStandaloneRender::testImageSequenceDirectoryProbeAndRender()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sequencePath = tempDir.filePath(QStringLiteral("frames"));
    QVERIFY(QDir().mkpath(sequencePath));

    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/shot10.BMP"), 20, 20, 240, 6, 4));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/shot2.bmp"), 20, 240, 20, 6, 4));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/shot1.bmp"), 240, 20, 20, 6, 4));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/slate.bmp"), 220, 220, 20, 6, 4));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/alternate1.jpg"), 20, 20, 20));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/alternate2.jpg"), 20, 20, 20));

    const jcut::ImageSequenceDirectoryInfo sequence =
        jcut::probeImageSequenceDirectory(
            std::filesystem::path(sequencePath.toStdString()));
    QVERIFY(sequence.detected());
    QCOMPARE(QString::fromStdString(sequence.extension), QStringLiteral(".bmp"));
    QCOMPARE(sequence.frameCount(), std::int64_t{4});
    QCOMPARE(QString::fromStdString(sequence.framePaths[0].filename().string()),
             QStringLiteral("shot1.bmp"));
    QCOMPARE(QString::fromStdString(sequence.framePaths[1].filename().string()),
             QStringLiteral("shot2.bmp"));
    QCOMPARE(QString::fromStdString(sequence.framePaths[2].filename().string()),
             QStringLiteral("shot10.BMP"));
    QCOMPARE(QString::fromStdString(sequence.framePaths[3].filename().string()),
             QStringLiteral("slate.bmp"));

    const QString ordinaryPath = tempDir.filePath(QStringLiteral("ordinary"));
    QVERIFY(QDir().mkpath(ordinaryPath));
    QVERIFY(writeSolidBmp(ordinaryPath + QStringLiteral("numbered1.bmp"), 20, 20, 20));
    QVERIFY(writeSolidBmp(ordinaryPath + QStringLiteral("poster.bmp"), 20, 20, 20));
    QVERIFY(!jcut::isImageSequenceDirectory(
        std::filesystem::path(ordinaryPath.toStdString())));

    const jcut::standalone_render::StandaloneMediaInfo mediaInfo =
        jcut::standalone_render::probeStandaloneMedia(sequencePath.toStdString());
    QVERIFY(mediaInfo.probed);
    QVERIFY(mediaInfo.hasVideo);
    QVERIFY(!mediaInfo.hasAudio);
    QCOMPARE(mediaInfo.videoFps, jcut::kImageSequenceFramesPerSecond);
    QCOMPARE(mediaInfo.durationFrames, std::int64_t{4});
    QCOMPARE(mediaInfo.frameSize.width, 6);
    QCOMPARE(mediaInfo.frameSize.height, 4);
    QCOMPARE(QString::fromStdString(mediaInfo.mediaKind), QStringLiteral("video"));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.label = "frames";
    clip.startFrame = 0;
    clip.durationFrames = 4;
    clip.sourcePath = sequencePath.toStdString();
    clip.mediaKind = "video";
    document.clips.push_back(std::move(clip));

    const jcut::standalone_render::TimelineRenderResult firstFrame =
        jcut::standalone_render::renderTimelineFrame({document, {32, 32}, 0.0, {}});
    const jcut::standalone_render::TimelineRenderResult secondFrame =
        jcut::standalone_render::renderTimelineFrame({document, {32, 32}, 1.0, {}});
    QVERIFY2(firstFrame.success, firstFrame.message.c_str());
    QVERIFY2(secondFrame.success, secondFrame.message.c_str());
    QVERIFY(!firstFrame.image.empty());
    QVERIFY(!secondFrame.image.empty());
    const auto centerOffset = [](const jcut::core::ImageBuffer& image) {
        return static_cast<std::size_t>(image.size.height / 2 * image.strideBytes +
                                        image.size.width / 2 * 4);
    };
    const std::size_t firstOffset = centerOffset(firstFrame.image);
    const std::size_t secondOffset = centerOffset(secondFrame.image);
    QVERIFY(firstFrame.image.bytes[firstOffset] >
            firstFrame.image.bytes[firstOffset + 1]);
    QVERIFY(secondFrame.image.bytes[secondOffset + 1] >
            secondFrame.image.bytes[secondOffset]);
}

void TestImGuiStandaloneRender::testLegacyUnknownAudioPresenceIsProbedOnLoad()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("silent.ppm"));
    {
        std::ofstream output(imagePath.toStdString(), std::ios::binary);
        QVERIFY(output.is_open());
        output << "P6\n1 1\n255\n";
        const unsigned char pixel[] = {32, 64, 96};
        output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        QVERIFY(output.good());
    }
    const QString audioPath = tempDir.filePath(QStringLiteral("voice.wav"));
    QVERIFY(writeSilentPcmWav(audioPath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Media"});
    document.mediaItems.push_back({"silent.ppm", "Silent", "video", false, true});
    document.mediaItems.push_back({"voice.wav", "Voice", "audio", false, false});

    jcut::EditorClip silentClip;
    silentClip.id = 1;
    silentClip.trackId = 1;
    silentClip.sourcePath = "silent.ppm";
    silentClip.mediaKind = "video";
    silentClip.audioPresenceKnown = false;
    silentClip.hasAudio = true;
    document.clips.push_back(silentClip);

    jcut::EditorClip voiceClip;
    voiceClip.id = 2;
    voiceClip.trackId = 1;
    voiceClip.sourcePath = "voice.wav";
    voiceClip.mediaKind = "audio";
    voiceClip.audioEnabled = false;
    voiceClip.audioPresenceKnown = false;
    document.clips.push_back(voiceClip);

    QCOMPARE(jcut::standalone_render::probeUnknownAudioPresence(
                 &document, tempDir.path().toStdString()),
             std::size_t{2});
    QVERIFY(document.clips.front().audioPresenceKnown);
    QVERIFY(!document.clips.front().hasAudio);
    QVERIFY(document.clips.back().audioPresenceKnown);
    QVERIFY(document.clips.back().hasAudio);
    QVERIFY(!document.clips.back().audioEnabled);
    QVERIFY(document.mediaItems.front().audioPresenceKnown);
    QVERIFY(!document.mediaItems.front().hasAudio);
    QVERIFY(document.mediaItems.back().audioPresenceKnown);
    QVERIFY(document.mediaItems.back().hasAudio);
    QCOMPARE(jcut::standalone_render::probeUnknownAudioPresence(
                 &document, tempDir.path().toStdString()),
             std::size_t{0});
}

void TestImGuiStandaloneRender::testAudioFacadeRefreshesIdleTimelineStatus()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Audio"});
    jcut::EditorRuntime editorRuntime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    QVERIFY(editorRuntime.execute(jcut::EditorCommand{jcut::ImportMediaCommand{
        "voice.wav", "Voice", "audio"}}).applied);
    QVERIFY(editorRuntime.execute(jcut::EditorCommand{
        jcut::InsertClipFromMediaCommand{"voice.wav", 1, 0, 30}}).applied);
    document = editorRuntime.snapshot();
    QVERIFY(document.clips.front().hasAudio);

    jcut::ImGuiAudioRuntime runtime;
    runtime.synchronize(document, tempDir.path().toStdString());

    const jcut::ImGuiAudioStatus missing = runtime.status();
    QVERIFY(missing.timelineConfigured);
    QVERIFY(!missing.playbackActive);
    QVERIFY(!missing.hasPlayableAudio);
    QVERIFY(missing.scheduledSourcePaths.empty());
    QCOMPARE(QString::fromStdString(missing.message),
             QStringLiteral("no playable audio on timeline"));

    const QString absoluteAudioPath =
        tempDir.filePath(QStringLiteral("voice.wav"));
    QVERIFY(writeSilentPcmWav(absoluteAudioPath));
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());

    const jcut::ImGuiAudioStatus ready = runtime.status();
    QVERIFY(ready.timelineConfigured);
    QVERIFY(!ready.playbackActive);
    QVERIFY(ready.hasPlayableAudio);
    QVERIFY(!ready.initialized);
    QVERIFY(!ready.outputUnavailable);
    QCOMPARE(ready.scheduledSourcePaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(ready.scheduledSourcePaths.front()),
             absoluteAudioPath);
    QCOMPARE(QString::fromStdString(ready.message),
             QStringLiteral("audio timeline configured"));

    QVERIFY(QFile::remove(absoluteAudioPath));
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());
    const jcut::ImGuiAudioStatus removed = runtime.status();
    QVERIFY(!removed.hasPlayableAudio);
    QVERIFY(removed.scheduledSourcePaths.empty());

    jcut::EditorDocumentCore emptyDocument;
    emptyDocument.projectName = "Empty project";
    runtime.synchronize(emptyDocument, tempDir.path().toStdString());

    const jcut::ImGuiAudioStatus empty = runtime.status();
    QVERIFY(empty.timelineConfigured);
    QVERIFY(!empty.playbackActive);
    QVERIFY(!empty.hasPlayableAudio);
    QCOMPARE(QString::fromStdString(empty.message),
             QStringLiteral("no playable audio on timeline"));

    runtime.shutdown();
    runtime.shutdown();
}

void TestImGuiStandaloneRender::testAudioFacadeTracksDerivedSidecarAvailability()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString videoPath = tempDir.filePath(QStringLiteral("clip.mov"));
    QFile videoFile(videoPath);
    QVERIFY(videoFile.open(QIODevice::WriteOnly));
    QCOMPARE(videoFile.write("placeholder", 11), qint64{11});
    videoFile.close();

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Audio"});
    jcut::EditorRuntime editorRuntime =
        jcut::EditorRuntime::fromDocument(std::move(document));
    QVERIFY(editorRuntime.execute(jcut::EditorCommand{jcut::ImportMediaCommand{
        "clip.mov", "Clip", "video"}}).applied);
    QVERIFY(editorRuntime.execute(jcut::EditorCommand{
        jcut::InsertClipFromMediaCommand{"clip.mov", 1, 0, 30}}).applied);
    document = editorRuntime.snapshot();

    jcut::ImGuiAudioRuntime runtime;
    runtime.synchronize(document, tempDir.path().toStdString());
    jcut::ImGuiAudioStatus status = runtime.status();
    QCOMPARE(status.scheduledSourcePaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(status.scheduledSourcePaths.front()),
             videoPath);

    const QString sidecarPath = tempDir.filePath(QStringLiteral("clip.wav"));
    QVERIFY(writeSilentPcmWav(sidecarPath));
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());
    status = runtime.status();
    QCOMPARE(status.scheduledSourcePaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(status.scheduledSourcePaths.front()),
             sidecarPath);

    QVERIFY(QFile::remove(sidecarPath));
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());
    status = runtime.status();
    QCOMPARE(status.scheduledSourcePaths.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(status.scheduledSourcePaths.front()),
             videoPath);
}

void TestImGuiStandaloneRender::testAudioFacadeRefreshesReplacedSourceTopology()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.filePath(QStringLiteral("replaceable.media"));
    const auto writeImage = [&]() {
        std::ofstream output(sourcePath.toStdString(),
                             std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output << "P6\n2 1\n255\n";
        const unsigned char pixels[] = {255, 0, 0, 0, 255, 0};
        output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
        return output.good();
    };
    QVERIFY(writeImage());

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Media"});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.persistentId = "replaceable";
    clip.sourcePath = "replaceable.media";
    clip.mediaKind = "video";
    clip.durationFrames = 30;
    clip.audioEnabled = true;
    clip.audioPresenceKnown = true;
    clip.hasAudio = false;
    document.clips.push_back(clip);

    jcut::ImGuiAudioRuntime runtime;
    runtime.synchronize(document, tempDir.path().toStdString());
    QVERIFY(!runtime.status().hasPlayableAudio);

    QVERIFY(writeSilentPcmWav(sourcePath));
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());
    jcut::ImGuiAudioStatus status = runtime.status();
    QVERIFY(status.hasPlayableAudio);
    QCOMPARE(status.scheduledSourcePaths.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(status.scheduledSourcePaths.front()),
             sourcePath);

    QVERIFY(writeImage());
    QTest::qWait(275);
    runtime.synchronize(document, tempDir.path().toStdString());
    status = runtime.status();
    QVERIFY(!status.hasPlayableAudio);
    QVERIFY(status.scheduledSourcePaths.empty());
}

QTEST_MAIN(TestImGuiStandaloneRender)
#include "test_imgui_standalone_render.moc"
