#include <QtTest/QtTest>

#include "../editor_grading_core.h"
#include "../editor_runtime.h"
#include "../face_avatar_crop_core.h"
#include "../image_sequence_directory.h"
#include "../imgui_audio_runtime.h"
#include "../prompt_mask_job_core.h"
#include "../standalone_preview_renderer.h"
#include "../standalone_timeline_renderer.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <fstream>
#include <array>

class TestImGuiStandaloneRender : public QObject {
    Q_OBJECT

private slots:
    void testRenderPreviewFrameDecodesImageClip();
    void testLegacyClipWithoutMediaKindIsVisual();
    void testStandalonePreviewCompositesOpacityAndTrackVisualModes();
    void testStandalonePreviewAppliesMaskMatteAndFailsClosed();
    void testStandalonePreviewRendersSourceTilingPatterns();
    void testStandalonePreviewRendersDifferenceMatteAndTemporalEcho();
    void testStandalonePreviewRendersGeneratedRepeatEffects();
    void testStandalonePreviewRendersSinglePassPixelEffects();
    void testStandalonePreviewRendersAnimatedTitleClip();
    void testStandalonePreviewRendersAdvancedTitleTreatments();
    void testStandalonePreviewRendersTranscriptOverlayFromActiveCut();
    void testStandalonePreviewAppliesDynamicSpeakerFraming();
    void testPreviewKeepsZeroCopyWithCpuFallbackContract();
    void testHardwareDirectEligibilitySupportsPresentationTransforms();
    void testFaceAvatarCropMatchesQtPolicy();
    void testStandaloneImportProbeReportsAudioPresence();
    void testStandaloneDecodePolicyBenchmark();
    void testImageSequenceDirectoryProbeAndRender();
    void testLegacyUnknownAudioPresenceIsProbedOnLoad();
    void testAudioFacadeRefreshesIdleTimelineStatus();
    void testAudioFacadeTracksDerivedSidecarAvailability();
    void testAudioFacadeRefreshesReplacedSourceTopology();
    void testPromptMaskJobPlanAndController();
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

bool writeJcutBoxV1(const QString& path, const QByteArray& jsonPayload)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    if (stream.writeRawData("JCUTBOX1", 8) != 8) return false;
    stream << quint32{1}
           << static_cast<quint32>(jsonPayload.size());
    return stream.writeRawData(
               jsonPayload.constData(), jsonPayload.size()) ==
            jsonPayload.size() &&
        stream.status() == QDataStream::Ok;
}

bool writeSplitMaskPgm(const QString& path, int width = 4, int height = 4)
{
    std::ofstream output(path.toStdString(), std::ios::binary);
    if (!output || width <= 1 || height <= 0) return false;
    output << "P5\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            output.put(static_cast<char>(x < width / 2 ? 255 : 0));
        }
    }
    return output.good();
}

bool writePatternPpm(const QString& path)
{
    std::ofstream output(path.toStdString(), std::ios::binary);
    if (!output) return false;
    output << "P6\n4 2\n255\n";
    const unsigned char pixels[] = {
        255, 0, 0,  0, 255, 0,  0, 0, 255,  255, 255, 0,
        0, 255, 255,  255, 0, 255,  255, 255, 255,  32, 32, 32,
    };
    output.write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
    return output.good();
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

void TestImGuiStandaloneRender::testStandalonePreviewRendersSourceTilingPatterns()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString imagePath = tempDir.filePath(QStringLiteral("tile.bmp"));
    QVERIFY(writeSolidBmp(imagePath, 240, 24, 12, 4, 4));

    jcut::EditorDocumentCore document;
    document.projectName = "Source tiling";
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.label = "tile";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourcePath = imagePath.toStdString();
    clip.mediaKind = "image";
    clip.effectPreset = "source_tile";
    clip.effectRows = 4;
    clip.effectScale = 1.0;
    clip.tilingSpacing = 1.0;
    clip.tilingPattern = "grid";
    document.clips.push_back(clip);

    constexpr jcut::core::SizeI outputSize{32, 24};
    const auto grid = jcut::standalone_render::renderPreviewFrame({
        document, outputSize, 0});
    QVERIFY2(grid.success, grid.message.c_str());

    document.clips.front().tilingPattern = "encircle";
    const auto encircle = jcut::standalone_render::renderPreviewFrame({
        document, outputSize, 0});
    QVERIFY2(encircle.success, encircle.message.c_str());
    QVERIFY(grid.image.bytes != encircle.image.bytes);

    int foregroundPixels = 0;
    int backgroundPixels = 0;
    for (int y = 0; y < encircle.image.size.height; ++y) {
        for (int x = 0; x < encircle.image.size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(
                y * encircle.image.strideBytes + x * 4);
            if (encircle.image.bytes[offset] > 200 &&
                encircle.image.bytes[offset + 1] < 40) {
                ++foregroundPixels;
            } else if (encircle.image.bytes[offset] == 12 &&
                       encircle.image.bytes[offset + 1] == 14) {
                ++backgroundPixels;
            }
        }
    }
    QVERIFY(foregroundPixels > 0);
    QVERIFY(backgroundPixels > 0);
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersDifferenceMatteAndTemporalEcho()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sequencePath = tempDir.filePath(QStringLiteral("sequence"));
    QVERIFY(QDir().mkpath(sequencePath));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/frame000.bmp"), 255, 0, 0));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/frame001.bmp"), 0, 0, 255));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/frame002.bmp"), 0, 255, 0));
    QVERIFY(writeSolidBmp(sequencePath + QStringLiteral("/frame003.bmp"), 255, 255, 0));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.label = "sequence";
    clip.startFrame = 0;
    clip.durationFrames = 4;
    clip.sourceDurationFrames = 4;
    clip.sourcePath = sequencePath.toStdString();
    clip.mediaKind = "video";
    clip.effectPreset = "difference_matte";
    clip.differenceReferenceFrames = 1;
    clip.differenceThreshold = 0.1;
    clip.differenceSoftness = 0.01;
    document.clips.push_back(clip);

    const auto difference = jcut::standalone_render::renderTimelineFrame({
        document, {4, 4}, 1.0, {}});
    QVERIFY2(difference.success, difference.message.c_str());
    const std::size_t center = static_cast<std::size_t>(
        2 * difference.image.strideBytes + 2 * 4);
    QVERIFY(difference.image.bytes[center] > 245);
    QVERIFY(difference.image.bytes[center + 1] > 245);
    QVERIFY(difference.image.bytes[center + 2] > 245);

    document.clips.front().effectPreset = "temporal_echo";
    document.clips.front().temporalEchoCount = 1;
    document.clips.front().temporalEchoSpacingFrames = 1;
    document.clips.front().temporalEchoDecay = 0.5;
    const auto echo = jcut::standalone_render::renderTimelineFrame({
        document, {4, 4}, 1.0, {}});
    QVERIFY2(echo.success, echo.message.c_str());
    const std::size_t echoCenter = static_cast<std::size_t>(
        2 * echo.image.strideBytes + 2 * 4);
    QVERIFY(echo.image.bytes[echoCenter] >= 120);
    QVERIFY(echo.image.bytes[echoCenter] <= 135);
    QVERIFY(echo.image.bytes[echoCenter + 2] >= 120);
    QVERIFY(echo.image.bytes[echoCenter + 2] <= 135);
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersGeneratedRepeatEffects()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString imagePath = tempDir.filePath(QStringLiteral("source.ppm"));
    QVERIFY(writePatternPpm(imagePath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "generated-effect";
    clip.trackId = 1;
    clip.label = "source";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourcePath = imagePath.toStdString();
    clip.mediaKind = "image";
    clip.effectRows = 6;
    clip.effectScale = 0.8;
    clip.effectSpeed = 1.0;
    clip.effectAlternateDirection = true;
    document.clips.push_back(clip);

    const auto base = jcut::standalone_render::renderTimelineFrame({
        document, {64, 48}, 0.0, {}});
    QVERIFY2(base.success, base.message.c_str());
    const std::array<const char*, 7> presets = {
        "news_logo_ticker",
        "alternating_motion_background",
        "directional_trim_ticker",
        "person_orbit",
        "freeze_pattern",
        "step_repeat",
        "vulkan_3d_synth",
    };
    for (const char* preset : presets) {
        document.clips.front().effectPreset = preset;
        const auto first = jcut::standalone_render::renderTimelineFrame({
            document, {64, 48}, 0.0, {}});
        const auto animated = jcut::standalone_render::renderTimelineFrame({
            document, {64, 48}, 12.0, {}});
        QVERIFY2(first.success, first.message.c_str());
        QVERIFY2(animated.success, animated.message.c_str());
        QVERIFY2(first.image.bytes != base.image.bytes, preset);
        QVERIFY2(first.image.bytes != animated.image.bytes, preset);
    }

    document.clips.front().effectPreset = "person_orbit";
    document.clips.front().effectSkipAwareTiming = true;
    document.renderSyncMarkers.push_back({"generated-effect", 3, true, 4});
    const auto skipAware = jcut::standalone_render::renderTimelineFrame({
        document, {64, 48}, 12.0, {}});
    document.clips.front().effectSkipAwareTiming = false;
    const auto rawClock = jcut::standalone_render::renderTimelineFrame({
        document, {64, 48}, 12.0, {}});
    QVERIFY(skipAware.image.bytes != rawClock.image.bytes);
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersSinglePassPixelEffects()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString imagePath = tempDir.filePath(QStringLiteral("pattern.ppm"));
    QVERIFY(writePatternPpm(imagePath));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.label = "pattern";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourcePath = imagePath.toStdString();
    clip.mediaKind = "image";
    clip.effectRows = 2;
    clip.effectScale = 1.4;
    clip.effectSpeed = 1.0;
    document.clips.push_back(clip);

    const auto base = jcut::standalone_render::renderTimelineFrame({
        document, {96, 64}, 0.0, {}});
    QVERIFY2(base.success, base.message.c_str());
    const std::array<const char*, 20> presets = {
        "mirror_ring", "kaleidoscope", "quad_mirror", "infinite_mirror",
        "tessellation", "hexagonal_prism", "droste", "polar_tunnel",
        "tiny_planet", "twirl_vortex", "ripple_shockwave", "displacement_map",
        "glass_refraction", "slit_scan", "pixel_sorting", "datamosh_glitch",
        "rgb_split", "halftone_mosaic", "sobel_edges", "neon_glow",
    };
    for (const char* preset : presets) {
        document.clips.front().effectPreset = preset;
        const auto rendered = jcut::standalone_render::renderTimelineFrame({
            document, {96, 64}, 7.0, {}});
        QVERIFY2(rendered.success, rendered.message.c_str());
        QVERIFY2(rendered.image.bytes != base.image.bytes, preset);
    }

    document.clips.front().effectPreset = "progressive_edge_stretch";
    document.clips.front().effectRows = 12;
    document.clips.front().effectScale = 1.2;
    const auto progressive = jcut::standalone_render::renderTimelineFrame({
        document, {96, 64}, 0.0, {}});
    QVERIFY2(progressive.success, progressive.message.c_str());
    QVERIFY(progressive.image.bytes != base.image.bytes);
    const std::size_t topCenter = static_cast<std::size_t>(48 * 4);
    QVERIFY(progressive.image.bytes[topCenter] != 12 ||
            progressive.image.bytes[topCenter + 1] != 14);

    document.clips.front().effectPreset = "neon_glow";
    const auto neonFirst = jcut::standalone_render::renderTimelineFrame({
        document, {96, 64}, 0.0, {}});
    const auto neonAnimated = jcut::standalone_render::renderTimelineFrame({
        document, {96, 64}, 12.0, {}});
    QVERIFY(neonFirst.image.bytes != neonAnimated.image.bytes);
}

void TestImGuiStandaloneRender::testStandalonePreviewCompositesOpacityAndTrackVisualModes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString redPath = tempDir.filePath(QStringLiteral("red.bmp"));
    const QString bluePath = tempDir.filePath(QStringLiteral("blue.bmp"));
    QVERIFY(writeSolidBmp(redPath, 255, 0, 0, 4, 4));
    QVERIFY(writeSolidBmp(bluePath, 0, 0, 255, 4, 4));

    jcut::EditorDocumentCore document;
    jcut::EditorTrack bottomTrack;
    bottomTrack.id = 1;
    bottomTrack.label = "Bottom";
    jcut::EditorTrack topTrack;
    topTrack.id = 2;
    topTrack.label = "Top";
    document.tracks = {bottomTrack, topTrack};
    jcut::EditorClip bottom;
    bottom.id = 1;
    bottom.trackId = 1;
    bottom.startFrame = 0;
    bottom.durationFrames = 30;
    bottom.sourcePath = redPath.toStdString();
    bottom.mediaKind = "image";
    jcut::EditorClip top = bottom;
    top.id = 2;
    top.trackId = 2;
    top.sourcePath = bluePath.toStdString();
    top.opacity = 0.5;
    document.clips = {bottom, top};

    auto render = [&]() {
        return jcut::standalone_render::renderTimelineFrame({
            document, {4, 4}, 0.0, {}});
    };
    auto center = [](const jcut::core::ImageBuffer& image, int channel) {
        return image.bytes[static_cast<std::size_t>(
            2 * image.strideBytes + 2 * 4 + channel)];
    };

    jcut::standalone_render::TimelineRenderResult result = render();
    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(center(result.image, 0) >= 126 && center(result.image, 0) <= 129);
    QCOMPARE(center(result.image, 1), static_cast<std::uint8_t>(0));
    QVERIFY(center(result.image, 2) >= 127 && center(result.image, 2) <= 129);

    document.tracks[1].visualMode = 2;
    result = render();
    QCOMPARE(center(result.image, 0), static_cast<std::uint8_t>(255));
    QCOMPARE(center(result.image, 2), static_cast<std::uint8_t>(0));

    document.clips[0].brightness = -1.0;
    result = render();
    QCOMPARE(center(result.image, 0), static_cast<std::uint8_t>(0));
    QCOMPARE(center(result.image, 1), static_cast<std::uint8_t>(0));
    QCOMPARE(center(result.image, 2), static_cast<std::uint8_t>(0));
    document.clips[0].brightness = 0.0;

    jcut::EditorTransformKeyframe transformStart;
    transformStart.frame = 0;
    jcut::EditorTransformKeyframe transformEnd;
    transformEnd.frame = 10;
    transformEnd.translationX = 2.0;
    document.clips[0].transformKeyframes = {transformStart, transformEnd};
    result = jcut::standalone_render::renderTimelineFrame({
        document, {4, 4}, 5.0, {}});
    QVERIFY2(result.success, result.message.c_str());
    const std::size_t leftCenter = static_cast<std::size_t>(
        2 * result.image.strideBytes);
    QCOMPARE(result.image.bytes[leftCenter], static_cast<std::uint8_t>(12));
    QCOMPARE(center(result.image, 0), static_cast<std::uint8_t>(255));
    document.clips[0].transformKeyframes.clear();

    document.tracks[1].visualMode = 1;
    result = render();
    QCOMPARE(center(result.image, 0), static_cast<std::uint8_t>(0));
    QCOMPARE(center(result.image, 2), static_cast<std::uint8_t>(255));

    jcut::EditorDocumentCore upperDocument = document;
    upperDocument.clips = {top};
    upperDocument.tracks[1].visualMode = 0;
    jcut::standalone_render::TimelineRenderRequest
        transparentRequest;
    transparentRequest.document =
        std::move(upperDocument);
    transparentRequest.outputSize = {4, 4};
    transparentRequest.transparentBackground = true;
    result =
        jcut::standalone_render::renderTimelineFrame(
            transparentRequest);
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(
        center(result.image, 0),
        static_cast<std::uint8_t>(0));
    QVERIFY(
        center(result.image, 2) >= 254 &&
        center(result.image, 2) <= 255);
    QVERIFY(
        center(result.image, 3) >= 127 &&
        center(result.image, 3) <= 129);
}

void TestImGuiStandaloneRender::testStandalonePreviewAppliesMaskMatteAndFailsClosed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sourcePath = tempDir.filePath(QStringLiteral("red.bmp"));
    QVERIFY(writeSolidBmp(sourcePath, 255, 0, 0, 4, 4));
    QFile transcript(tempDir.filePath(QStringLiteral("red.json")));
    QVERIFY(transcript.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray transcriptJson = R"json({
  "speaker_profiles": {"S1": {
    "primary_color": "#0000ff",
    "secondary_color": "#0000ff",
    "accent_color": "#0000ff"
  }},
  "segments": [{"words": [
    {"word": "speaker", "start": 0.0, "end": 1.0, "speaker": "S1"}
  ]}]
})json";
    QCOMPARE(transcript.write(transcriptJson),
             qint64{transcriptJson.size()});
    transcript.close();
    const QString maskDir = tempDir.filePath(QStringLiteral("manual_mask"));
    QVERIFY(QDir().mkpath(maskDir));
    QVERIFY(writeSolidBmp(
        QDir(maskDir).filePath(QStringLiteral("frame_000001.png")),
        255, 255, 255, 4, 4));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Mask Matte", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "matte-1";
    clip.trackId = 1;
    clip.startFrame = 0;
    clip.durationFrames = 10;
    clip.sourcePath = sourcePath.toStdString();
    clip.mediaKind = "image";
    clip.clipRole = "mask_matte";
    clip.maskEnabled = true;
    clip.maskFramesDir = maskDir.toStdString();
    jcut::EditorCorrectionPolygon correction;
    correction.pointsNormalized = {
        {0.5, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.5, 1.0}};
    clip.correctionPolygons.push_back(correction);
    document.clips.push_back(clip);

    auto render = [&]() {
        return jcut::standalone_render::renderTimelineFrame({
            document, {4, 4}, 0.0, {}});
    };
    auto pixel = [](const jcut::core::ImageBuffer& image, int x, int channel) {
        return image.bytes[static_cast<std::size_t>(
            2 * image.strideBytes + x * 4 + channel)];
    };
    auto result = render();
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(pixel(result.image, 0, 0), static_cast<std::uint8_t>(255));
    QCOMPARE(pixel(result.image, 3, 0), static_cast<std::uint8_t>(12));

    document.clips[0].correctionPolygons.clear();
    QVERIFY(writeSplitMaskPgm(
        QDir(maskDir).filePath(QStringLiteral("frame_000001.png"))));
    document.clips[0].clipRole = "media";
    document.clips[0].maskGradeEnabled = true;
    document.clips[0].maskGradeBrightness = -1.0;
    result = render();
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(pixel(result.image, 0, 0), static_cast<std::uint8_t>(0));
    QCOMPARE(pixel(result.image, 3, 0), static_cast<std::uint8_t>(255));

    document.clips[0].clipRole = "mask_matte";
    document.clips[0].maskGradeEnabled = false;
    document.clips[0].maskDropShadowEnabled = true;
    document.clips[0].maskDropShadowRadius = 0.0;
    document.clips[0].maskDropShadowOffsetX = 2.0;
    document.clips[0].maskDropShadowOffsetY = 0.0;
    document.clips[0].maskDropShadowOpacity = 1.0;
    result = render();
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(pixel(result.image, 0, 0), static_cast<std::uint8_t>(255));
    QCOMPARE(pixel(result.image, 2, 0), static_cast<std::uint8_t>(0));

    document.clips[0].maskDropShadowEnabled = false;
    document.clips[0].maskRepeatEnabled = true;
    document.clips[0].effectRows = 3;
    document.clips[0].maskRepeatDeltaX = 1.0;
    document.clips[0].maskRepeatDeltaY = 0.0;
    result = render();
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(pixel(result.image, 2, 0), static_cast<std::uint8_t>(255));

    document.clips[0].maskRepeatEnabled = false;
    document.clips[0].clipRole = "media";
    document.clips[0].effectRows = 2;
    document.clips[0].effectScale = 1.0;
    document.clips[0].tilingSpacing = 0.1;
    for (const char* preset : {
             "speaker_mask_dilation",
             "speaker_mask_dilation_pulse",
             "speaker_mask_dilation_rings"}) {
        document.clips[0].effectPreset = preset;
        result = render();
        QVERIFY2(result.success, result.message.c_str());
        QCOMPARE(pixel(result.image, 2, 1), static_cast<std::uint8_t>(0));
        QVERIFY2(pixel(result.image, 2, 2) > 0, preset);
    }

    document.clips[0].effectPreset = "none";
    document.clips[0].clipRole = "mask_matte";
    document.clips[0].maskFramesDir = tempDir.filePath(
        QStringLiteral("missing_mask")).toStdString();
    result = render();
    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(pixel(result.image, 0, 0), static_cast<std::uint8_t>(12));
    QCOMPARE(pixel(result.image, 3, 0), static_cast<std::uint8_t>(12));
}

void TestImGuiStandaloneRender::testPromptMaskJobPlanAndController()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sourcePath =
        tempDir.filePath(QStringLiteral("My Video!.mp4"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("video"), qint64{5});
    source.close();

    const QString scriptPath =
        tempDir.filePath(QStringLiteral("fake_sam3.sh"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray scriptBytes = R"sh(#!/bin/sh
set -eu
output=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "--binary-mask-dir" ]; then
        output="$2"
        shift 2
    else
        shift
    fi
done
mkdir -p "$output"
printf 'P5\n1 1\n255\n\377' > "$output/frame_000001.png"
printf '%s\n%s\n%s\n' "$SAM3_MODEL_CACHE" "$SAM3_RUNTIME_CACHE" "$SAM3_JOB_DIR" > "$SAM3_JOB_DIR/environment.txt"
)sh";
    QCOMPARE(script.write(scriptBytes), qint64{scriptBytes.size()});
    script.close();
    QVERIFY(QFile::setPermissions(
        scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup));

    jcut::masks::PromptMaskJobRequest request;
    request.scriptPath = scriptPath.toStdString();
    request.mediaPath = sourcePath.toStdString();
    request.prompt = "a person";
    request.modelCachePath =
        tempDir.filePath(QStringLiteral("model-cache")).toStdString();
    request.runtimeCachePath =
        tempDir.filePath(QStringLiteral("runtime-cache")).toStdString();
    request.scaleWidth = 960;
    request.prescaleWidth = 1280;
    request.compileModel = true;
    request.exportCentersJson = false;

    const auto plan = jcut::masks::buildPromptMaskJobPlan(request);
    QVERIFY(QString::fromStdString(plan.jobRoot).endsWith(
        QStringLiteral(".jcut_jobs/sam3_a_person_My_Video_")));
    QVERIFY(QString::fromStdString(plan.binaryMasksPath).endsWith(
        QStringLiteral("My Video!_sam3_a_person_binary_masks")));
    QCOMPARE(
        plan.manifest.value("operation", std::string{}),
        std::string("sam3"));
    QCOMPARE(
        plan.manifest["parameters"].value("prompt", std::string{}),
        std::string("a person"));
    QVERIFY(std::find(
                plan.command.begin(),
                plan.command.end(),
                "--binary-mask-dir") != plan.command.end());
    QVERIFY(std::find(
                plan.command.begin(),
                plan.command.end(),
                "--compile-model") != plan.command.end());

    jcut::masks::PromptMaskJobController controller;
    std::string error;
    QVERIFY2(controller.start(request, &error), error.c_str());
    controller.wait();
    const auto completed = controller.snapshot();
    QCOMPARE(
        completed.state,
        jcut::masks::PromptMaskJobSnapshot::State::Completed);
    QCOMPARE(completed.exitCode, 0);
    QVERIFY(!completed.selectedMaskId.empty());
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(completed.selectedMaskPath) +
        QStringLiteral("/frame_000001.png")));
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(completed.manifestPath)));
    const QString environmentPath =
        QString::fromStdString(completed.jobRoot) +
        QStringLiteral("/environment.txt");
    QFile environment(environmentPath);
    QVERIFY(environment.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString environmentText =
        QString::fromUtf8(environment.readAll());
    QVERIFY(environmentText.contains(
        QString::fromStdString(request.modelCachePath)));
    QVERIFY(environmentText.contains(
        QString::fromStdString(request.runtimeCachePath)));
    QVERIFY(environmentText.contains(
        QString::fromStdString(completed.jobRoot)));
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersAnimatedTitleClip()
{
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Titles", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "title-1";
    clip.trackId = 1;
    clip.label = "Title";
    clip.startFrame = 0;
    clip.durationFrames = 20;
    clip.mediaKind = "title";
    clip.videoEnabled = true;
    jcut::EditorTitleKeyframe first;
    first.frame = 0;
    first.text = "JCut";
    first.fontSize = 36.0;
    first.color = "#ff0000";
    jcut::EditorTitleKeyframe last = first;
    last.frame = 10;
    last.translationX = 40.0;
    last.opacity = 0.5;
    clip.titleKeyframes = {first, last};
    document.clips.push_back(clip);

    const auto centroidAndCount = [](const jcut::core::ImageBuffer& image) {
        double xSum = 0.0;
        int count = 0;
        for (int y = 0; y < image.size.height; ++y) {
            for (int x = 0; x < image.size.width; ++x) {
                const std::size_t offset = static_cast<std::size_t>(
                    y * image.strideBytes + x * 4);
                if (image.bytes[offset] > 80 &&
                    image.bytes[offset] > image.bytes[offset + 1] * 2) {
                    xSum += x;
                    ++count;
                }
            }
        }
        return std::pair<double, int>{count > 0 ? xSum / count : 0.0, count};
    };

    const auto start = jcut::standalone_render::renderTimelineFrame({
        document, {320, 180}, 0.0, {}});
    QVERIFY2(start.success, start.message.c_str());
    const auto [startX, startCount] = centroidAndCount(start.image);
    QVERIFY(startCount > 20);

    const auto midpointTitle = jcut::evaluateEditorClipTitleAtLocalFrame(
        document.clips.front(), 5);
    QCOMPARE(midpointTitle.text, std::string("JCut"));
    QCOMPARE(midpointTitle.translationX, 20.0);
    QCOMPARE(midpointTitle.opacity, 0.75);
    const auto midpoint = jcut::standalone_render::renderTimelineFrame({
        document, {320, 180}, 5.0, {}});
    QVERIFY2(midpoint.success, midpoint.message.c_str());
    const auto [midpointX, midpointCount] = centroidAndCount(midpoint.image);
    QVERIFY(midpointCount > 20);
    QVERIFY(midpointX > startX + 15.0);
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersAdvancedTitleTreatments()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPatternPath = tempDir.filePath(QStringLiteral("text-pattern.bmp"));
    const QString framePatternPath = tempDir.filePath(QStringLiteral("frame-pattern.bmp"));
    const QString logoPath = tempDir.filePath(QStringLiteral("logo.bmp"));
    QVERIFY(writeSolidBmp(textPatternPath, 0, 255, 0, 12, 12));
    QVERIFY(writeSolidBmp(framePatternPath, 0, 0, 255, 12, 12));
    QVERIFY(writeSolidBmp(logoPath, 255, 255, 0, 24, 24));
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Titles", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "advanced-title";
    clip.trackId = 1;
    clip.label = "Advanced Title";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.mediaKind = "title";
    clip.videoEnabled = true;
    jcut::EditorTitleKeyframe title;
    title.text = "Advanced";
    title.fontSize = 42.0;
    title.color = "#00ff00";
    title.autoFitToOutput = true;
    title.logoPath = logoPath.toStdString();
    title.textMaterialStyle = "image_pattern";
    title.textPatternImagePath = textPatternPath.toStdString();
    title.textPatternScale = 1.4;
    title.dropShadowEnabled = true;
    title.dropShadowColor = "#000000";
    title.dropShadowOpacity = 0.9;
    title.dropShadowOffsetX = 5.0;
    title.dropShadowOffsetY = 5.0;
    title.windowEnabled = true;
    title.windowColor = "#ff0000";
    title.windowOpacity = 1.0;
    title.windowPadding = 12.0;
    title.windowWidth = 240.0;
    title.windowFrameEnabled = true;
    title.windowFrameColor = "#0000ff";
    title.windowFrameOpacity = 1.0;
    title.windowFrameWidth = 4.0;
    title.windowFrameGap = 2.0;
    title.windowFrameMaterialStyle = "image_pattern";
    title.windowFramePatternImagePath = framePatternPath.toStdString();
    title.windowFramePatternScale = 1.0;
    title.vulkan3DEnabled = true;
    title.vulkan3DExtrudeEnabled = true;
    title.textExtrudeMode = "stacked_copies";
    title.vulkan3DExtrudeDepth = 0.12;
    title.vulkan3DBevelScale = 0.8;
    title.vulkan3DYawDegrees = 55.0;
    title.vulkan3DPitchDegrees = -10.0;
    title.vulkan3DRollDegrees = 7.0;
    title.vulkan3DDepth = 0.5;
    title.vulkan3DScale = 1.1;
    clip.titleKeyframes.push_back(title);
    document.clips.push_back(clip);

    constexpr jcut::core::SizeI outputSize{320, 180};
    const auto advanced = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 0.0, tempDir.path().toStdString()});
    QVERIFY2(advanced.success, advanced.message.c_str());
    int redPixels = 0;
    int bluePixels = 0;
    int greenPixels = 0;
    int yellowPixels = 0;
    for (int y = 0; y < advanced.image.size.height; ++y) {
        for (int x = 0; x < advanced.image.size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(
                y * advanced.image.strideBytes + x * 4);
            const auto red = advanced.image.bytes[offset];
            const auto green = advanced.image.bytes[offset + 1];
            const auto blue = advanced.image.bytes[offset + 2];
            if (red > 180 && green < 60 && blue < 60) ++redPixels;
            if (blue > 120 && red < 100 && green < 100) ++bluePixels;
            if (green > 140 && red < 120 && blue < 150) ++greenPixels;
            if (red > 180 && green > 180 && blue < 80) ++yellowPixels;
        }
    }
    QVERIFY2(redPixels > 500, "title window was not rendered");
    QVERIFY2(bluePixels > 100, "material title window frame was not rendered");
    QVERIFY2(greenPixels > 20, "material title glyphs were not rendered");
    QVERIFY2(yellowPixels > 100, "title logo asset was not rendered");

    auto& basic = document.clips.front().titleKeyframes.front();
    basic.vulkan3DEnabled = false;
    const auto axisAligned = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 0.0, tempDir.path().toStdString()});
    QVERIFY2(axisAligned.success, axisAligned.message.c_str());
    int axisAlignedRedPixels = 0;
    for (int y = 0; y < axisAligned.image.size.height; ++y) {
        for (int x = 0; x < axisAligned.image.size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(
                y * axisAligned.image.strideBytes + x * 4);
            if (axisAligned.image.bytes[offset] > 180 &&
                axisAligned.image.bytes[offset + 1] < 60 &&
                axisAligned.image.bytes[offset + 2] < 60) {
                ++axisAlignedRedPixels;
            }
        }
    }
    QVERIFY2(
        axisAlignedRedPixels > redPixels + 500,
        "the title window/frame must participate in the 3D yaw transform");

    basic.textMaterialStyle = "solid";
    basic.dropShadowEnabled = false;
    basic.windowEnabled = false;
    basic.windowFrameEnabled = false;
    basic.vulkan3DExtrudeEnabled = false;
    basic.textExtrudeMode = "none";
    const auto plain = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 0.0, tempDir.path().toStdString()});
    QVERIFY2(plain.success, plain.message.c_str());
    QVERIFY(advanced.image.bytes != plain.image.bytes);
}

void TestImGuiStandaloneRender::testStandalonePreviewRendersTranscriptOverlayFromActiveCut()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString audioPath = tempDir.filePath(QStringLiteral("voice.wav"));
    const QString transcriptPath = tempDir.filePath(QStringLiteral("voice.json"));
    QVERIFY(writeSilentPcmWav(audioPath));

    QFile transcript(transcriptPath);
    QVERIFY(transcript.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray transcriptJson = R"json({
  "speaker_profiles": {"S1": {
    "name": "Alice",
    "organization": "CompAI",
    "location": {"x": 0.9, "y": 0.5},
    "framing": {"enabled": true, "keyframes": []}
  }},
  "segments": [{
    "speaker": "S1",
    "words": [
      {"word": "Hello", "start": 0.0, "end": 0.49, "speaker": "S1"},
      {"word": "world", "start": 0.5, "end": 1.0, "speaker": "S1"}
    ]
  }]
})json";
    QCOMPARE(transcript.write(transcriptJson), qint64{transcriptJson.size()});
    transcript.close();

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Dialogue", true});
    document.exportRequest.transcriptPrependMs = 0;
    document.exportRequest.transcriptPostpendMs = 0;
    document.exportRequest.transcriptOffsetMs = 0;
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "dialogue-1";
    clip.trackId = 1;
    clip.label = "Voice";
    clip.startFrame = 0;
    clip.durationFrames = 60;
    clip.sourcePath = audioPath.toStdString();
    clip.mediaKind = "audio";
    clip.sourceFps = 30.0;
    clip.hasAudio = true;
    clip.audioPresenceKnown = true;
    clip.transcriptActiveCutPath = transcriptPath.toStdString();
    clip.transcriptOverlay.enabled = true;
    clip.transcriptOverlay.useManualPlacement = true;
    clip.transcriptOverlay.showBackground = true;
    clip.transcriptOverlay.backgroundColor = "#000000";
    clip.transcriptOverlay.backgroundOpacity = 1.0;
    clip.transcriptOverlay.backgroundCornerRadius = 12.0;
    clip.transcriptOverlay.backgroundPadding = 16.0;
    clip.transcriptOverlay.backgroundFrameEnabled = true;
    clip.transcriptOverlay.backgroundFrameColor = "#ff0000";
    clip.transcriptOverlay.backgroundFrameOpacity = 1.0;
    clip.transcriptOverlay.backgroundFrameWidth = 3.0;
    clip.transcriptOverlay.backgroundFrameGap = 2.0;
    clip.transcriptOverlay.showShadow = true;
    clip.transcriptOverlay.shadowColor = "#0000ff";
    clip.transcriptOverlay.shadowOpacity = 0.8;
    clip.transcriptOverlay.shadowOffsetX = 3.0;
    clip.transcriptOverlay.shadowOffsetY = 3.0;
    clip.transcriptOverlay.textOutlineEnabled = true;
    clip.transcriptOverlay.textOutlineColor = "#ff0000";
    clip.transcriptOverlay.textOutlineOpacity = 1.0;
    clip.transcriptOverlay.textOutlineWidth = 1.0;
    clip.transcriptOverlay.textExtrudeMode = "stacked_copies";
    clip.transcriptOverlay.textExtrudeDepth = 0.10;
    clip.transcriptOverlay.textExtrudeBevelScale = 0.7;
    clip.transcriptOverlay.showSpeakerTitle = true;
    clip.transcriptOverlay.highlightCurrentWord = true;
    clip.transcriptOverlay.highlightColor = "#00ff00";
    clip.transcriptOverlay.highlightTextColor = "#000000";
    clip.transcriptOverlay.textColor = "#ffffff";
    clip.transcriptOverlay.fontFamily = "DejaVu Sans";
    clip.transcriptOverlay.fontPointSize = 28;
    clip.transcriptOverlay.boxWidth = 260.0;
    clip.transcriptOverlay.boxHeight = 100.0;
    clip.transcriptOverlay.maxLines = 2;
    clip.transcriptOverlay.maxCharsPerLine = 28;
    document.clips.push_back(clip);

    constexpr jcut::core::SizeI outputSize{320, 180};
    const auto firstWord = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(firstWord.success, firstWord.message.c_str());
    QCOMPARE(firstWord.image.size.width, outputSize.width);
    QCOMPARE(firstWord.image.size.height, outputSize.height);

    int greenPixels = 0;
    int whitePixels = 0;
    int redPixels = 0;
    for (int y = 0; y < firstWord.image.size.height; ++y) {
        for (int x = 0; x < firstWord.image.size.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(
                y * firstWord.image.strideBytes + x * 4);
            const auto red = firstWord.image.bytes[offset];
            const auto green = firstWord.image.bytes[offset + 1];
            const auto blue = firstWord.image.bytes[offset + 2];
            if (green > 180 && red < 60 && blue < 60) ++greenPixels;
            if (red > 180 && green > 180 && blue > 180) ++whitePixels;
            if (red > 180 && green < 60 && blue < 60) ++redPixels;
        }
    }
    QVERIFY2(greenPixels > 100, "active transcript word highlight was not rendered");
    QVERIFY2(whitePixels > 20, "inactive transcript word typography was not rendered");
    QVERIFY2(redPixels > 100, "transcript frame/outline styling was not rendered");

    document.clips.front().transcriptOverlay.showSpeakerTitle = false;
    const auto withoutSpeakerTitle = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(withoutSpeakerTitle.success, withoutSpeakerTitle.message.c_str());
    QVERIFY(firstWord.image.bytes != withoutSpeakerTitle.image.bytes);
    document.clips.front().transcriptOverlay.showSpeakerTitle = true;

    document.clips.front().transcriptOverlay.useManualPlacement = false;
    document.clips.front().transcriptOverlay.translationX = -1.0;
    const auto speakerPlaced = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(speakerPlaced.success, speakerPlaced.message.c_str());
    const std::size_t trackedFrame = static_cast<std::size_t>(
        42 * speakerPlaced.image.strideBytes + 200 * 4);
    QVERIFY(speakerPlaced.image.bytes[trackedFrame] > 180);
    QVERIFY(speakerPlaced.image.bytes[trackedFrame + 1] < 60);
    QVERIFY(speakerPlaced.image.bytes[trackedFrame + 2] < 60);
    document.clips.front().transcriptOverlay.useManualPlacement = true;
    document.clips.front().transcriptOverlay.translationX = 0.0;

    const auto secondWord = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 20.0, tempDir.path().toStdString()});
    QVERIFY2(secondWord.success, secondWord.message.c_str());
    QVERIFY(firstWord.image.bytes != secondWord.image.bytes);

    document.clips.front().transcriptOverlay.translationX = 0.5;
    const auto shifted = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(shifted.success, shifted.message.c_str());
    const std::size_t formerlyCovered = static_cast<std::size_t>(45 * shifted.image.strideBytes + 35 * 4);
    QCOMPARE(shifted.image.bytes[formerlyCovered], std::uint8_t{12});
    QCOMPARE(shifted.image.bytes[formerlyCovered + 1], std::uint8_t{14});
    QCOMPARE(shifted.image.bytes[formerlyCovered + 2], std::uint8_t{18});
}

void TestImGuiStandaloneRender::testStandalonePreviewAppliesDynamicSpeakerFraming()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString imagePath =
        tempDir.filePath(QStringLiteral("speaker.ppm"));
    const QString transcriptPath =
        tempDir.filePath(QStringLiteral("speaker.json"));
    QVERIFY(writePatternPpm(imagePath));

    QFile transcript(transcriptPath);
    QVERIFY(transcript.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray transcriptJson = R"json({
  "speaker_profiles": {"S1": {}},
  "speaker_flow": {"clips": {"speaker-video": {
    "resolved_current": {"section_track_map": [
      {"speaker_id": "S1", "start_frame": 0, "end_frame": 30,
       "rotation": 12.0, "tracks": [{"track_id": 7}]}
    ]}
  }}},
  "segments": [{"words": [
    {"word": "hello", "start": 0.0, "end": 1.0, "speaker": "S1"}
  ]}]
})json";
    QCOMPARE(transcript.write(transcriptJson),
             qint64{transcriptJson.size()});
    transcript.close();
    const QByteArray artifactJson = R"json({
  "continuity_facedetections_by_clip": {
    "speaker-video": {
      "raw_tracks_frame_domain": "source_absolute",
      "raw_tracks": [{
        "track_id": 7,
        "detections": [
          {"frame": 5, "x": 0.8, "y": 0.5, "box": 0.2, "score": 1.0}
        ]
      }]
    }
  }
})json";
    QVERIFY(writeJcutBoxV1(
        tempDir.filePath(QStringLiteral(
            "speaker_facedetections.bin")),
        artifactJson));

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Video", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "speaker-video";
    clip.trackId = 1;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourcePath = imagePath.toStdString();
    clip.mediaKind = "image";
    clip.sourceFps = 30.0;
    clip.transcriptActiveCutPath = transcriptPath.toStdString();
    clip.speakerFramingTargetKeyframes.push_back(
        {0, "Target", 0.5, 0.35, 0.0, 0.2, 0.2, true});
    document.clips.push_back(clip);

    constexpr jcut::core::SizeI outputSize{64, 64};
    const auto plain = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(plain.success, plain.message.c_str());

    document.clips.front().speakerFramingEnabled = true;
    const auto framed = jcut::standalone_render::renderTimelineFrame({
        document, outputSize, 5.0, tempDir.path().toStdString()});
    QVERIFY2(framed.success, framed.message.c_str());
    QVERIFY2(
        plain.image.bytes != framed.image.bytes,
        "standalone preview/export must apply assigned continuity-track "
        "face-box framing when no baked framing keyframes exist");
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
    QVERIFY2(
        timelineRenderer.contains(
            QStringLiteral("renderTranscriptOverlayLayer")) &&
            timelineRenderer.contains(
                QStringLiteral("renderTitleClipLayer")) &&
            timelineRenderer.contains(
                QStringLiteral("renderHardwareOverlayLayer")) &&
            previewHeader.contains(
                QStringLiteral("hardwareOverlayImage")) &&
            shell.contains(
                QStringLiteral("previewOverlayTexture")) &&
            shell.contains(
                QStringLiteral(
                    "previewResult.hardwareOverlayImage")),
        "hardware-direct preview must keep transcript rasterization "
        "separate and composite its overlay as a Vulkan texture");
    QVERIFY2(shell.contains(QStringLiteral("previewCpuFallbackPreferred")),
             "ImGui preview must adaptively switch to CPU fallback after zero-copy import fails");
    QVERIFY2(shell.contains(QStringLiteral("editorTrackMediaPresenceCore")),
             "ImGui track controls must consume the shared media-presence policy");
    QVERIFY2(shell.contains(QStringLiteral("!trackPresence.hasVisual")) &&
                 shell.contains(QStringLiteral("!trackPresence.hasAudio")),
             "ImGui visual and audio track controls must disable when their media is absent");
    QVERIFY2(shell.contains(QStringLiteral("hoveredTrackVisualToggleId")) &&
                 shell.contains(QStringLiteral("hoveredTrackAudioToggleId")) &&
                 shell.contains(QStringLiteral(
                     "(std::clamp(trackIt->visualMode, 0, 2) + 1) % 3")) &&
                 shell.contains(QStringLiteral("!trackIt->audioEnabled")),
             "timeline track headers must expose Qt-compatible visual-mode cycling and audio toggling");
    QVERIFY2(shell.contains(QStringLiteral("readTextFileTail")) &&
                 shell.contains(QStringLiteral("64U * 1024U")) &&
                 shell.contains(QStringLiteral("Refresh Artifact")) &&
                 shell.contains(QStringLiteral("##JobsArtifactText")),
             "Jobs must provide bounded, refreshable in-app manifest and log inspection");
    QVERIFY2(shell.contains(QStringLiteral("previewPipelineStages")) &&
                 shell.contains(QStringLiteral("PipelineGraph")) &&
                 shell.contains(QStringLiteral("PipelineStageFacts")) &&
                 shell.contains(QStringLiteral("Retry Zero Copy")),
             "Pipeline must expose a selectable live stage graph, stage facts, refresh, and zero-copy retry");
    QVERIFY2(shell.contains(QStringLiteral("transportSpeeds")) &&
                 shell.contains(QStringLiteral("0.1f, 0.25f")) &&
                 shell.contains(QStringLiteral("2.0f, 3.0f")) &&
                 shell.contains(QStringLiteral(
                     "SetPlaybackLoopEnabledCommand")) &&
                 shell.contains(QStringLiteral(
                     "SetPreviewViewModeCommand")) &&
                 shell.contains(QStringLiteral(
                     "SetTransportAudioCommand")) &&
                 shell.contains(QStringLiteral(
                     "SeekToFrameCommand{transportEndFrame}")),
             "transport must expose Qt's exact speed presets, durable loop/view/audio controls, and start/end navigation");
    QVERIFY2(!shell.contains(QStringLiteral("audio disabled in Qt-free ImGui shell")),
             "ImGui audio must not be hard-disabled as a shell policy");
    QVERIFY2(shell.contains(QStringLiteral("#include \"imgui_audio_runtime.h\"")) &&
                 !shell.contains(QStringLiteral("#include \"audio_engine.h\"")) &&
                 !shell.contains(QStringLiteral("QVector<")),
             "the ImGui shell must consume audio through the Qt-free facade");
    QVERIFY2(audioRuntime.contains(QStringLiteral("#include \"RtAudio.h\"")) &&
                 audioRuntime.contains(QStringLiteral("standalone_audio_mixer.h")) &&
                 audioRuntime.contains(QStringLiteral("mixAudioChunk")) &&
                 audioRuntime.contains(QStringLiteral(
                     "data->muted.load")) &&
                 audioRuntime.contains(QStringLiteral(
                     "output[index] *= volume")) &&
                 audioRuntime.contains(QStringLiteral(
                     "status.recentWaveform")) &&
                 shell.contains(QStringLiteral(
                     "audioStatus.recentWaveform")) &&
                 audioRuntime.contains(QStringLiteral("normalizedPlaybackSpeed")) &&
                 audioRuntime.contains(QStringLiteral("{\"fadeSamples\", clip.fadeSamples}")) &&
                 audioRuntime.contains(QStringLiteral("std::launch::async")) &&
                 audioRuntime.contains(QStringLiteral("buffering audio")) &&
                 !audioRuntime.contains(QStringLiteral("#include \"audio_engine.h\"")) &&
                 !audioRuntime.contains(QStringLiteral("QVector<")),
             "the facade must use the Qt-free RtAudio/standalone mixer with clip-fade invalidation, asynchronous decode, and playback-speed mapping");
    QVERIFY2(audioRuntime.contains(QStringLiteral("fileIdentity")) &&
                 audioRuntime.contains(QStringLiteral("last_write_time")) &&
                 audioRuntime.contains(QStringLiteral("replace_extension(\".wav\")")) &&
                 audioRuntime.contains(QStringLiteral("probeStandaloneMedia")),
             "source availability polling must cover replacements and derived WAV sidecars");
    QVERIFY2(shell.contains(QStringLiteral("holdForAudioWarmup")) &&
                 shell.contains(QStringLiteral("preTickAudioStatus.buffering")),
             "transport must remain at the requested frame while audio warms asynchronously");
    QVERIFY2(audioRuntime.contains(QStringLiteral("m_decodeFuture.valid()")) &&
                 audioRuntime.contains(QStringLiteral("!m_cacheReady")) &&
                 audioRuntime.contains(QStringLiteral("m_decodeFailed")) &&
                 audioRuntime.contains(QStringLiteral("startOutputLocked")) &&
                 !audioRuntime.contains(QStringLiteral(
                     "continuing playback without warmed audio")),
             "pending Qt-free decode must remain gated until its cache is ready");
    QVERIFY2(shell.contains(QStringLiteral("probeStandaloneMedia")) &&
                 shell.contains(QStringLiteral("mediaInfo.hasAudio")) &&
                 shell.contains(QStringLiteral("probeUnknownAudioPresence")),
             "ImGui imports and legacy loads must persist probed stream metadata instead of assuming every video has audio");
    QVERIFY2(shell.contains(QStringLiteral("#include \"imgui_vulkan_frame_importer.h\"")) &&
                 !shell.contains(QStringLiteral("#include \"vulkan_detector_frame_handoff.h\"")),
             "the ImGui shell must consume Vulkan frames through the neutral importer facade");
    QVERIFY2(
        shell.contains(QStringLiteral("#include \"prompt_mask_job_core.h\"")) &&
            shell.contains(QStringLiteral("Run Prompt Mask Job")) &&
            shell.contains(QStringLiteral("MaterializeMaskMatteCommand")) &&
            shell.contains(QStringLiteral("Cancel Prompt Mask")),
        "the Masks and Jobs tabs must launch, materialize, and cancel the shared prompt-mask workflow");
    QVERIFY2(
        shell.contains(QStringLiteral("#include \"transcription_job_core.h\"")) &&
            shell.contains(QStringLiteral("startTranscriptionJob")) &&
            shell.contains(QStringLiteral("Transcription stdin")) &&
            shell.contains(QStringLiteral("Cancel Transcription")) &&
            shell.contains(QStringLiteral("Transcript Manifest")),
        "Transcript and Jobs must launch, communicate with, cancel, and inspect the shared WhisperX workflow");
    QVERIFY2(
        shell.contains(QStringLiteral("#include \"birefnet_job_core.h\"")) &&
            shell.contains(QStringLiteral("Run BiRefNet Job")) &&
            shell.contains(QStringLiteral("BiRefNetJobControllerCore")) &&
            shell.contains(QStringLiteral("Cancel BiRefNet")) &&
            shell.contains(QStringLiteral("BiRefNet Progress")) &&
            shell.contains(QStringLiteral("BiRefNet Alpha")) &&
            shell.contains(QStringLiteral(
                "refreshBiRefNetLivePreviewTexture")) &&
            shell.contains(QStringLiteral(
                "uploadAuxiliaryImage")) &&
            shell.contains(QStringLiteral(
                "birefnetLivePreviewTextureId")) &&
            shell.contains(QStringLiteral(
                "ImGui::Image")),
        "Masks and Jobs must configure, launch, render live progress, materialize, cancel, and inspect the shared BiRefNet workflow");
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
                 shell.contains(QStringLiteral("CurveCanvas")) &&
                 shell.contains(QStringLiteral("canvasToPoint")) &&
                 shell.contains(QStringLiteral("nearestPoint")) &&
                 shell.contains(QStringLiteral("ImGuiMouseButton_Right")) &&
                 shell.contains(QStringLiteral("sampleEditorGradingCurveAt")) &&
                 shell.contains(QStringLiteral("sanitizeEditorGradingCurve")) &&
                 shell.contains(QStringLiteral(
                     "synchronizeEditorThreePointGradingCurves")) &&
                 shell.contains(QStringLiteral("Normalize curves")) &&
                 shell.contains(QStringLiteral(
                     "normalizeEditorGradingCurves(*draft)")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsR")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsG")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsB")) &&
                 shell.contains(QStringLiteral(
                     "&draft->curvePointsLuma")) &&
                 shell.contains(QStringLiteral("applyLockedTone")),
             "the Grade draft must expose bounded direct-manipulation canvases and numeric RGB/Luma point tables, feed locked curves back into tones, normalize through the shared helper, and keep Luma independently editable");
    QVERIFY2(shell.contains(QStringLiteral("startAutoOpposeJob")) &&
                 shell.contains(QStringLiteral("pollAutoOpposeJob")) &&
                 shell.contains(QStringLiteral(
                     "StandaloneMediaFrameDecoder")) &&
                 shell.contains(QStringLiteral(
                     "detectEditorOpposeGradeEvents")) &&
                 shell.contains(QStringLiteral("Auto Oppose Settings")) &&
                 !shell.contains(QStringLiteral(
                     "Auto Oppose (Qt workflow)")),
             "Grade Auto Oppose must run the shared decoded-frame analysis workflow rather than expose a disabled Qt-only placeholder");
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
    QVERIFY2(shell.contains(QStringLiteral("Copy Clip Name")) &&
                 shell.contains(QStringLiteral("Copy title")) &&
                 shell.contains(QStringLiteral("ImGui::SetClipboardText")) &&
                 shell.contains(QStringLiteral("requestedInspectorTab")) &&
                 shell.contains(QStringLiteral("Grading...")) &&
                 shell.contains(QStringLiteral("refreshClipMetadata")) &&
                 shell.contains(QStringLiteral(
                     "RefreshClipMetadataCommand")) &&
                 shell.contains(QStringLiteral(
                     "mediaInfo.sourceDurationFrames")) &&
                 shell.contains(QStringLiteral("Generated Clips")) &&
                 shell.contains(QStringLiteral("Run SAM 3...")) &&
                 shell.contains(QStringLiteral("Run BiRefNet...")) &&
                 shell.contains(QStringLiteral("\"Transcribe\"")) &&
                 shell.contains(QStringLiteral("Open Transcript Tools")) &&
                 shell.contains(QStringLiteral("Open Proxy Controls")) &&
                 shell.contains(QStringLiteral("FaceDetections")) &&
                 shell.contains(QStringLiteral("Properties")),
             "the timeline context menu must provide clipboard actions, real transcription/BiRefNet routing, and direct access to grading, sync, mask, speaker/face, proxy, job, and properties workflows");
    QVERIFY2(
        shell.contains(QStringLiteral("Split Selected At Playhead")) &&
            shell.contains(QStringLiteral("SplitSelectedClipsCommand")) &&
            shell.contains(QStringLiteral("selectedClipsCanSplitAtFrame")) &&
            shell.contains(QStringLiteral("WantTextInput")) &&
            shell.contains(QStringLiteral("IsAnyItemActive")) &&
            shell.contains(QStringLiteral("InspectorDeleteTargetKind")) &&
            shell.contains(QStringLiteral("markInspectorDeleteTargetForLastItem")) &&
            shell.contains(QStringLiteral("markSyncDeleteTargetForLastItem")) &&
            shell.contains(QStringLiteral("markTranscriptDeleteTargetForLastItem")) &&
            shell.contains(QStringLiteral("transcriptDeletePopupRequested")) &&
            shell.contains(QStringLiteral("focusedUiFrame + 1 ==")) &&
            shell.contains(QStringLiteral("rowBackspacePressed")) &&
            shell.contains(QStringLiteral("EditExportRangesCommand")) &&
            shell.contains(QStringLiteral("ExportRangeEdit::SplitAtPlayhead")) &&
            shell.contains(QStringLiteral("\"Create Title\"")) &&
            shell.contains(QStringLiteral("Lock Transform To Source")) &&
            shell.contains(QStringLiteral("SetClipSourceTransformLockedCommand")) &&
            shell.contains(QStringLiteral("Lock Selected")) &&
            shell.contains(QStringLiteral("Unlock Selected")) &&
            shell.contains(QStringLiteral("SetSelectedClipsLockedCommand")),
        "keyboard and clip-context actions must preserve multi-selection, "
        "suppress globals for editable widgets, and route focused keyframe "
        "or sync-row Delete/Backspace through generation-scoped neutral "
        "commands, retain explicit transcript confirmation, and expose the "
        "shared Qt-style export-range/title context actions");
    QVERIFY2(
        timelineRenderer.contains(QStringLiteral(
            "evaluateEditorClipRenderTransformAtTimelineFrame")) &&
            timelineRenderer.contains(QStringLiteral(
                "clip.sourceTransformLocked")),
        "standalone preview/export must use the neutral cycle-safe source "
        "transform evaluator and keep inherited transforms off the direct "
        "hardware bypass");
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
                 shell.contains(QStringLiteral("SmallButton(\"Load\")")) &&
                 shell.contains(QStringLiteral("previewTitleDragActive")) &&
                 shell.contains(QStringLiteral("evaluateEditorClipTitleAtLocalFrame")) &&
                 shell.contains(QStringLiteral("io.MouseDelta.x * outputWidth")) &&
                 shell.contains(QStringLiteral("beginRuntimeHistoryTransaction(shellState)")) &&
                 shell.contains(QStringLiteral("endRuntimeHistoryTransaction(shellState)")),
             "titles must expose every neutral field, row load/seek/removal, and direct output-space preview dragging as one undoable transaction");
    QVERIFY2(
        shell.contains(QStringLiteral("PreviewTransformDragMode::Rotate")) &&
            shell.contains(QStringLiteral("transformRotationHandleCenter")) &&
            shell.contains(QStringLiteral("rotationForPointerDrag")) &&
            shell.contains(QStringLiteral("io.KeyShift ? 15.0 : 0.0")) &&
            shell.contains(QStringLiteral("CommitPreviewTransformCommand")),
        "Program-monitor rotation must reuse wrap-safe neutral angle math, "
        "Shift snapping, and the coalesced preview-transform command");
    QVERIFY2(
        shell.contains(QStringLiteral(
            "Automatic Speaker Framing")) &&
            shell.contains(QStringLiteral(
                "SetClipSpeakerFramingCommand")) &&
            shell.contains(QStringLiteral(
                "UpsertSpeakerFramingEnabledKeyframeCommand")) &&
            shell.contains(QStringLiteral(
                "UpsertSpeakerFramingKeyframeCommand")) &&
            shell.contains(QStringLiteral(
                "UpsertSpeakerFramingTargetKeyframeCommand")) &&
            shell.contains(QStringLiteral(
                "SpeakerFramingEnabled")) &&
            shell.contains(QStringLiteral(
                "SpeakerFramingTarget")) &&
            shell.contains(QStringLiteral(
                "Center Smoothing Frames")) &&
            shell.contains(QStringLiteral(
                "Manual Stream ID")),
        "the Transform inspector must bind complete neutral speaker-framing "
        "settings and all three keyframe channels through undoable runtime "
        "commands");
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
                     "generatedChildTrack || !trackPresence.hasAudio")) &&
                 shell.contains(QStringLiteral(
                     "ImGui::BeginDisabled(audioControlsDisabled);")),
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
        QStringLiteral("ImGui::BeginDisabled(audioControlsDisabled);"),
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

    QVERIFY2(preview.contains(QStringLiteral("renderTimelineFrame(")) &&
                 preview.contains(QStringLiteral("request.allowCpuFallback")) &&
                 preview.contains(QStringLiteral(
                     "timelineResult.hardwareFrame")) &&
                 previewHeader.contains(QStringLiteral(
                     "std::shared_ptr<const core::FramePayloadCore> hardwareFrame")) &&
                 !preview.contains(QStringLiteral("renderPreviewFrameCore(")) &&
                 !preview.contains(QStringLiteral("render_runtime.h")),
             "the Qt-free preview facade must publish retained hardware frames through the neutral payload while keeping CPU fallback and the Qt render runtime out of the binary");
}

void TestImGuiStandaloneRender::testHardwareDirectEligibilitySupportsPresentationTransforms()
{
    jcut::EditorGradingKeyframe packedGrade;
    packedGrade.curveSmoothingEnabled = false;
    packedGrade.curvePointsR = {{0.0, 0.1}, {1.0, 0.1}};
    packedGrade.curvePointsG = {{0.0, 0.2}, {1.0, 0.2}};
    packedGrade.curvePointsB = {{0.0, 0.3}, {1.0, 0.3}};
    packedGrade.curvePointsLuma = {{0.0, 0.4}, {1.0, 0.4}};
    const auto packedLut =
        jcut::editorPackedGradingCurveLut(packedGrade);
    QCOMPARE(packedLut[128] & 0xffu, 26u);
    QCOMPARE((packedLut[128] >> 8u) & 0xffu, 51u);
    QCOMPARE((packedLut[128] >> 16u) & 0xffu, 77u);
    QCOMPARE((packedLut[128] >> 24u) & 0xffu, 102u);

    jcut::EditorDocumentCore document;
    document.exportRequest.outputSize = {64, 64};
    document.tracks.push_back(jcut::EditorTrack{1, "Video"});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.trackId = 1;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourcePath = "/definitely/missing/video.mp4";
    clip.mediaKind = "video";
    document.clips.push_back(clip);

    const auto renderWithoutFallback =
        [&](const jcut::EditorDocumentCore& candidate) {
            jcut::standalone_render::TimelineRenderRequest request;
            request.document = candidate;
            request.outputSize = {64, 64};
            request.preferHardwareFrame = true;
            request.allowCpuFallback = false;
            return jcut::standalone_render::renderTimelineFrame(request);
        };

    const auto identity = renderWithoutFallback(document);
    QVERIFY(identity.hardwareDirectEligible);
    QVERIFY(!identity.success);
    QVERIFY(!identity.hardwareDirectFallbackReason.empty());

    document.clips.front().baseScaleX = 1.1;
    document.clips.front().baseTranslationX = 4.0;
    document.clips.front().baseRotation = 12.0;
    document.clips.front().opacity = 0.75;
    document.clips.front().brightness = 0.15;
    document.clips.front().contrast = 1.2;
    document.clips.front().saturation = 0.8;
    const auto transformed = renderWithoutFallback(document);
    QVERIFY(transformed.hardwareDirectEligible);
    QVERIFY(!transformed.success);
    QVERIFY(transformed.hardwareDirectFallbackReason.find("transform") ==
            std::string::npos);

    jcut::EditorGradingKeyframe animatedGrade;
    animatedGrade.frame = 0;
    animatedGrade.brightness = 0.1;
    animatedGrade.contrast = 1.15;
    animatedGrade.saturation = 0.9;
    animatedGrade.shadowsR = 0.12;
    animatedGrade.midtonesG = -0.08;
    animatedGrade.highlightsB = 0.2;
    document.clips.front().gradingKeyframes = {animatedGrade};
    const auto keyedTonal = renderWithoutFallback(document);
    QVERIFY(keyedTonal.hardwareDirectEligible);
    QVERIFY(keyedTonal.hardwareDirectFallbackReason.find("grading") ==
            std::string::npos);

    document.clips.front().gradingKeyframes.front().curvePointsR =
        {{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}};
    const auto curved = renderWithoutFallback(document);
    QVERIFY(curved.hardwareDirectEligible);
    QVERIFY(curved.hardwareDirectFallbackReason.find("curves") ==
            std::string::npos);

    document.clips.front().gradingKeyframes.clear();
    document.clips.front().baseScaleX = 1.0;
    document.clips.front().baseTranslationX = 0.0;
    document.clips.front().baseRotation = 0.0;
    document.clips.front().opacity = 1.0;
    document.clips.front().brightness = 0.0;
    document.clips.front().contrast = 1.0;
    document.clips.front().saturation = 1.0;
    document.tracks.front().visualMode = 1;
    const auto forceOpaque =
        renderWithoutFallback(document);
    QVERIFY(forceOpaque.hardwareDirectEligible);
    QVERIFY(forceOpaque.hardwareDirectFallbackReason.find(
                "visual mode") == std::string::npos);
    document.tracks.front().visualMode = 0;
    document.clips.front().effectPreset = "temporal_echo";
    const auto effected = renderWithoutFallback(document);
    QVERIFY(!effected.hardwareDirectEligible);
    QVERIFY(effected.hardwareDirectFallbackReason.find("effects") !=
            std::string::npos);

    document.clips.front().effectPreset = "none";
    document.clips.front().transcriptOverlay.enabled = true;
    const auto overlay = renderWithoutFallback(document);
    QVERIFY(overlay.hardwareDirectEligible);
    QVERIFY(overlay.hardwareDirectFallbackReason.find("overlay") ==
            std::string::npos);

    document.clips.front().transcriptOverlay.enabled = false;
    document.clips.front().speakerFramingEnabled = true;
    document.clips.front().speakerFramingKeyframes.push_back(
        {0, "Framing", 0.0, 0.0, 0.0, 1.0, 1.0, true});
    const auto speakerFramed = renderWithoutFallback(document);
    QVERIFY(speakerFramed.hardwareDirectEligible);
    QVERIFY(speakerFramed.hardwareDirectFallbackReason.find("speaker") ==
            std::string::npos);

    document.clips.front().speakerFramingEnabled = false;
    document.clips.front().speakerFramingKeyframes.clear();
    document.tracks.push_back(
        jcut::EditorTrack{2, "Titles"});
    jcut::EditorClip title;
    title.id = 2;
    title.trackId = 2;
    title.startFrame = 0;
    title.durationFrames = 30;
    title.mediaKind = "title";
    title.titleKeyframes.push_back(
        jcut::EditorTitleKeyframe{
            0, "Hardware title"});
    document.clips.push_back(title);
    const auto titled =
        renderWithoutFallback(document);
    QVERIFY(titled.hardwareDirectEligible);
    QVERIFY(titled.hardwareDirectFallbackReason.find("title") ==
            std::string::npos);
    std::swap(
        document.tracks[0],
        document.tracks[1]);
    const auto titleBelowVideo =
        renderWithoutFallback(document);
    QVERIFY(!titleBelowVideo.hardwareDirectEligible);
    QVERIFY(titleBelowVideo.hardwareDirectFallbackReason.find(
                "ordered") != std::string::npos);
    std::swap(
        document.tracks[0],
        document.tracks[1]);
    document.clips.pop_back();
    document.tracks.pop_back();

    jcut::EditorClip second = document.clips.front();
    second.id = 3;
    second.trackId = 2;
    second.effectPreset = "temporal_echo";
    document.tracks.push_back(
        jcut::EditorTrack{2, "Upper video"});
    document.clips.push_back(second);
    const auto upperDecodedLayer =
        renderWithoutFallback(document);
    QVERIFY(upperDecodedLayer.hardwareDirectEligible);
    QVERIFY(upperDecodedLayer.hardwareDirectFallbackReason.find(
                "multiple") == std::string::npos);
    document.clips.pop_back();
    document.tracks.pop_back();

    second.trackId = 1;
    document.clips.push_back(second);
    const auto layered = renderWithoutFallback(document);
    QVERIFY(!layered.hardwareDirectEligible);
    QVERIFY(layered.hardwareDirectFallbackReason.find("multiple") !=
            std::string::npos);
}

void TestImGuiStandaloneRender::testFaceAvatarCropMatchesQtPolicy()
{
    const auto centered = jcut::faceAvatarCropRectCore(
        100, 80, 0.5, 0.5, 0.5);
    QCOMPARE(centered.left, 30);
    QCOMPARE(centered.top, 20);
    QCOMPARE(centered.width, 40);
    QCOMPARE(centered.height, 40);
    const auto edge = jcut::faceAvatarCropRectCore(
        100, 80, 0.0, 0.0, 0.5);
    QCOMPARE(edge.left, 0);
    QCOMPARE(edge.top, 0);
    const auto explicitBox = jcut::faceAvatarCropRectCore(
        100, 80, 0.5, 0.5, -1.0,
        0.1, 0.2, 0.6, 0.7);
    QCOMPARE(explicitBox.left, 10);
    QCOMPARE(explicitBox.top, 16);
    QCOMPARE(explicitBox.width, 50);
    QCOMPARE(explicitBox.height, 40);

    jcut::core::ImageBuffer source;
    source.format = jcut::core::PixelFormat::Rgba8;
    source.size = {100, 80};
    source.strideBytes = 400;
    source.bytes.resize(
        static_cast<std::size_t>(
            source.strideBytes * source.size.height));
    for (int y = 0; y < source.size.height; ++y) {
        for (int x = 0; x < source.size.width; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(
                    y * source.strideBytes + x * 4);
            source.bytes[offset] =
                static_cast<std::uint8_t>(x);
            source.bytes[offset + 1] =
                static_cast<std::uint8_t>(y);
            source.bytes[offset + 2] = 0;
            source.bytes[offset + 3] = 255;
        }
    }
    const jcut::core::ImageBuffer avatar =
        jcut::cropFaceAvatarImageCore(
            source, 0.5, 0.5, 0.5, 20);
    QVERIFY(!avatar.empty());
    QCOMPARE(avatar.size.width, 20);
    QCOMPARE(avatar.size.height, 20);
    QCOMPARE(avatar.bytes[0], std::uint8_t(30));
    QCOMPARE(avatar.bytes[1], std::uint8_t(20));
    const std::size_t last =
        static_cast<std::size_t>(
            19 * avatar.strideBytes + 19 * 4);
    QCOMPARE(avatar.bytes[last], std::uint8_t(68));
    QCOMPARE(avatar.bytes[last + 1], std::uint8_t(58));

    const jcut::core::ImageBuffer strip =
        jcut::faceAvatarStripImageCore({avatar, avatar}, 2);
    QVERIFY(!strip.empty());
    QCOMPARE(strip.size.width, 42);
    QCOMPARE(strip.size.height, 20);
    QCOMPARE(strip.bytes[0], std::uint8_t(30));
    const std::size_t gapOffset =
        static_cast<std::size_t>(20 * 4);
    QCOMPARE(strip.bytes[gapOffset + 3], std::uint8_t(0));
    const std::size_t secondTileOffset =
        static_cast<std::size_t>(22 * 4);
    QCOMPARE(strip.bytes[secondTileOffset], std::uint8_t(30));
    QCOMPARE(strip.bytes[secondTileOffset + 1], std::uint8_t(20));
    const auto firstUv =
        jcut::faceAvatarStripUvCore(0, strip.size.width, 20, 2);
    const auto secondUv =
        jcut::faceAvatarStripUvCore(1, strip.size.width, 20, 2);
    QVERIFY(firstUv.valid());
    QVERIFY(secondUv.valid());
    QCOMPARE(firstUv.left, 0.0);
    QCOMPARE(firstUv.right, 20.0 / 42.0);
    QCOMPARE(secondUv.left, 22.0 / 42.0);
    QCOMPARE(secondUv.right, 1.0);
    QVERIFY(!jcut::faceAvatarStripUvCore(
        2, strip.size.width, 20, 2).valid());
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

void TestImGuiStandaloneRender::testStandaloneDecodePolicyBenchmark()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString stillPath =
        tempDir.filePath(QStringLiteral("benchmark.bmp"));
    QVERIFY(writeSolidBmp(stillPath, 80, 120, 160, 16, 8));

    jcut::DecoderPolicySettingsCore policy;
    policy.decodePreference =
        jcut::DecodePreferenceCore::HardwareZeroCopy;
    policy.h26xThreadingMode =
        jcut::H26xThreadingModeCore::FrameAndSliceThreads;
    const auto benchmark =
        jcut::standalone_render::benchmarkStandaloneMediaDecode(
            stillPath.toStdString(), policy, 5);
    QVERIFY2(benchmark.success, benchmark.message.c_str());
    QVERIFY(benchmark.framesDecoded > 0);
    QCOMPARE(
        benchmark.requestedPreference,
        jcut::DecodePreferenceCore::HardwareZeroCopy);
    QCOMPARE(
        benchmark.effectivePreference,
        jcut::DecodePreferenceCore::Software);
    QVERIFY(!benchmark.hardwareAccelerated);
    QVERIFY(benchmark.hardwareDeviceLabel.empty());
    QVERIFY(!benchmark.hardwareFallbackReason.empty());
    QCOMPARE(benchmark.softwareThreadCount, 1);
    QVERIFY(benchmark.framesPerSecond > 0.0);

    policy.deterministic = true;
    const auto deterministic =
        jcut::standalone_render::benchmarkStandaloneMediaDecode(
            stillPath.toStdString(), policy, 1);
    QVERIFY(deterministic.success);
    QCOMPARE(deterministic.softwareThreadCount, 1);
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
    QCOMPARE(mediaInfo.sourceDurationFrames, std::int64_t{4});
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

    document.clips.front().sourceInFrame = 1;
    document.clips.front().sourceDurationFrames = 4;
    const auto trimmedFrame = jcut::standalone_render::renderTimelineFrame(
        {document, {32, 32}, 0.0, {}});
    QVERIFY2(trimmedFrame.success, trimmedFrame.message.c_str());
    const std::size_t trimmedOffset = centerOffset(trimmedFrame.image);
    QVERIFY(trimmedFrame.image.bytes[trimmedOffset + 1] >
            trimmedFrame.image.bytes[trimmedOffset]);

    document.clips.front().sourceInFrame = 0;
    document.clips.front().playbackRate = 2.0;
    const auto spedFrame = jcut::standalone_render::renderTimelineFrame(
        {document, {32, 32}, 1.0, {}});
    QVERIFY2(spedFrame.success, spedFrame.message.c_str());
    const std::size_t spedOffset = centerOffset(spedFrame.image);
    QVERIFY(spedFrame.image.bytes[spedOffset + 2] >
            spedFrame.image.bytes[spedOffset]);
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
    QCOMPARE(runtime.status().requestedBufferFrames, 1024U);
    runtime.setBufferFrames(256);
    QCOMPARE(runtime.status().requestedBufferFrames, 256U);
    QCOMPARE(runtime.status().actualBufferFrames, 0U);
    runtime.setOutputDeviceName("JCut unavailable test device");
    QCOMPARE(
        QString::fromStdString(
            runtime.status().requestedOutputDeviceName),
        QStringLiteral("JCut unavailable test device"));
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
