#include "../editor_document_render_bridge.h"
#include "../render_internal.h"
#include "../render_runtime.h"
#include "../standalone_audio_mixer.h"
#include "../standalone_timeline_renderer.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

namespace {

std::string writeGradientPpm(
    const std::string& path,
    int width,
    int height)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height
           << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char pixel[] = {
                static_cast<unsigned char>(
                    24 + x * 180 /
                        std::max(1, width - 1)),
                static_cast<unsigned char>(
                    32 + y * 160 /
                        std::max(1, height - 1)),
                static_cast<unsigned char>(
                    48 + (x + y) * 120 /
                        std::max(1, width + height - 2))};
            output.write(
                reinterpret_cast<const char*>(pixel),
                sizeof(pixel));
        }
    }
    return path;
}

std::string writeSolidPpm(
    const std::string& path,
    int width,
    int height,
    unsigned char red,
    unsigned char green,
    unsigned char blue)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height
           << "\n255\n";
    const unsigned char pixel[] = {red, green, blue};
    for (int index = 0; index < width * height; ++index) {
        output.write(
            reinterpret_cast<const char*>(pixel),
            sizeof(pixel));
    }
    return path;
}

std::string writeSplitMaskPgm(
    const std::string& path,
    int width,
    int height)
{
    std::ofstream output(path, std::ios::binary);
    output << "P5\n" << width << ' ' << height
           << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char value =
                x < width / 2 ? 255 : 0;
            output.write(
                reinterpret_cast<const char*>(&value),
                1);
        }
    }
    return path;
}

std::string writeMovingVideo(
    const QString& path)
{
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        return {};
    }
    QProcess process;
    process.start(
        ffmpeg,
        {QStringLiteral("-y"),
         QStringLiteral("-loglevel"),
         QStringLiteral("error"),
         QStringLiteral("-f"),
         QStringLiteral("lavfi"),
         QStringLiteral("-i"),
         QStringLiteral(
             "testsrc2=size=96x64:rate=30:duration=1"),
         QStringLiteral("-an"),
         QStringLiteral("-c:v"),
         QStringLiteral("mpeg4"),
         QStringLiteral("-q:v"),
         QStringLiteral("4"),
         path});
    if (!process.waitForFinished(15000) ||
        process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return {};
    }
    return path.toStdString();
}

jcut::render::PreviewFrameResultCore renderQtPreview(
    const jcut::EditorDocumentCore& document,
    int width,
    int height,
    std::int64_t timelineFrame = 0)
{
    jcut::render::RenderRequestCore request;
    request.outputPath = "test://imgui-qt-parity";
    request.outputFormat = "preview";
    request.outputSize = {width, height};
    request.outputFps = 30.0;
    request.exportStartFrame = timelineFrame;
    request.exportEndFrame = timelineFrame;
    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(
            document, false);
    jcut::render::PreviewFrameResultCore result;
    for (int attempt = 0; attempt < 40; ++attempt) {
        result = jcut::render::renderPreviewFrameCore(
            request, timeline, timelineFrame, true, true);
        if (result.success) {
            break;
        }
        QTest::qWait(25);
    }
    return result;
}

struct ImageDifference {
    double meanAbsolute = 0.0;
    double within16Fraction = 0.0;
};

int countYellowPixels(
    const jcut::core::ImageBuffer& image)
{
    int result = 0;
    for (int y = 0; y < image.size.height; ++y) {
        for (int x = 0; x < image.size.width; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(
                    y * image.strideBytes + x * 4);
            const int red = image.bytes[offset];
            const int green = image.bytes[offset + 1];
            const int blue = image.bytes[offset + 2];
            if (red > 200 && green > 180 &&
                blue < 210) {
                ++result;
            }
        }
    }
    return result;
}

double averageChannel(
    const jcut::core::ImageBuffer& image,
    int channel)
{
    std::uint64_t sum = 0;
    const std::uint64_t count =
        static_cast<std::uint64_t>(image.size.width) *
        static_cast<std::uint64_t>(image.size.height);
    for (int y = 0; y < image.size.height; ++y) {
        for (int x = 0; x < image.size.width; ++x) {
            sum += image.bytes[
                static_cast<std::size_t>(
                    y * image.strideBytes + x * 4 +
                    channel)];
        }
    }
    return count > 0
        ? static_cast<double>(sum) /
            static_cast<double>(count)
        : 0.0;
}

ImageDifference compareRgb(
    const jcut::core::ImageBuffer& left,
    const jcut::core::ImageBuffer& right)
{
    if (left.empty() || right.empty() ||
        left.size.width != right.size.width ||
        left.size.height != right.size.height) {
        return {255.0, 0.0};
    }
    std::uint64_t absoluteSum = 0;
    std::uint64_t within = 0;
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(left.size.width) *
        static_cast<std::uint64_t>(left.size.height);
    for (int y = 0; y < left.size.height; ++y) {
        for (int x = 0; x < left.size.width; ++x) {
            int maximumDifference = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const int leftValue = left.bytes[
                    static_cast<std::size_t>(
                        y * left.strideBytes + x * 4 +
                        channel)];
                const int rightValue = right.bytes[
                    static_cast<std::size_t>(
                        y * right.strideBytes + x * 4 +
                        channel)];
                const int difference =
                    std::abs(leftValue - rightValue);
                absoluteSum +=
                    static_cast<std::uint64_t>(difference);
                maximumDifference =
                    std::max(maximumDifference, difference);
            }
            if (maximumDifference <= 16) {
                ++within;
            }
        }
    }
    return {
        static_cast<double>(absoluteSum) /
            static_cast<double>(pixelCount * 3),
        static_cast<double>(within) /
            static_cast<double>(pixelCount)};
}

} // namespace

class TestImGuiQtRenderParity : public QObject {
    Q_OBJECT

private slots:
    void identicalNeutralProjectMatchesQtVulkanComposition();
    void identicalNeutralProjectMatchesQtVulkanMask();
    void identicalNeutralProjectMatchesQtVulkanGeneratedEffect();
    void identicalNeutralProjectMatchesQtVulkanTemporalEffect();
    void identicalNeutralProjectMatchesQtVulkanTranscript();
    void identicalNeutralProjectMatchesQtVulkanSpeakerFraming();
    void identicalNeutralProjectMatchesQtAudioMix();
};

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanComposition()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int width = 96;
    constexpr int height = 64;
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("gradient.ppm"))
            .toStdString(),
        width,
        height);

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Media", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-media";
    clip.trackId = 1;
    clip.label = "Gradient";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.brightness = 0.08;
    clip.contrast = 1.12;
    clip.saturation = 0.86;
    clip.opacity = 0.82;
    document.clips.push_back(clip);
    document.tracks.push_back({2, "Titles", true});
    jcut::EditorClip titleClip;
    titleClip.id = 2;
    titleClip.persistentId = "parity-title";
    titleClip.trackId = 2;
    titleClip.label = "Parity";
    titleClip.startFrame = 0;
    titleClip.durationFrames = 30;
    titleClip.mediaKind = "title";
    titleClip.videoEnabled = true;
    jcut::EditorTitleKeyframe title;
    title.text = "JCut";
    title.fontSize = 22.0;
    title.color = "#f4f8fc";
    title.translationX = 9.0;
    title.translationY = -6.0;
    title.opacity = 0.88;
    title.windowEnabled = true;
    title.windowColor = "#182838";
    title.windowOpacity = 0.72;
    title.windowWidth = 58.0;
    title.windowPadding = 5.0;
    titleClip.titleKeyframes.push_back(title);
    document.clips.push_back(titleClip);

    const jcut::standalone_render::TimelineRenderResult
        imguiFrame =
            jcut::standalone_render::renderTimelineFrame(
                {document,
                 {width, height},
                 0.0,
                 directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());

    const jcut::render::PreviewFrameResultCore qtFrame =
        renderQtPreview(document, width, height);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    QVERIFY2(
        difference.meanAbsolute < 10.0,
        qPrintable(QStringLiteral(
            "mean RGB difference %1 exceeds parity budget")
            .arg(difference.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.within16Fraction > 0.80,
        qPrintable(QStringLiteral(
            "only %1% of pixels are within 16 RGB levels")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanMask()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int width = 96;
    constexpr int height = 64;
    const std::string sourcePath = writeSolidPpm(
        directory.filePath(QStringLiteral("red.ppm"))
            .toStdString(),
        width, height, 240, 32, 32);
    const QString maskDirectory =
        directory.filePath(QStringLiteral("manual_mask"));
    QVERIFY(QDir().mkpath(maskDirectory));
    writeSplitMaskPgm(
        QDir(maskDirectory)
            .filePath(QStringLiteral("frame_000001.png"))
            .toStdString(),
        width,
        height);

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Masked Media", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-mask";
    clip.trackId = 1;
    clip.label = "Masked red";
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.maskEnabled = true;
    clip.maskFramesDir = maskDirectory.toStdString();
    clip.maskOpacity = 0.65;
    clip.maskFeather = 0.0;
    document.clips.push_back(clip);

    const auto imguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {width, height},
             0.0,
             directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());
    const auto qtFrame =
        renderQtPreview(document, width, height);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    QVERIFY2(
        difference.meanAbsolute < 8.0,
        qPrintable(QStringLiteral(
            "masked mean RGB difference is %1")
            .arg(difference.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.within16Fraction > 0.93,
        qPrintable(QStringLiteral(
            "masked agreement is only %1%")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanGeneratedEffect()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int width = 96;
    constexpr int height = 64;
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("effect.ppm"))
            .toStdString(),
        width,
        height);
    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Effect", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-effect";
    clip.trackId = 1;
    clip.label = "Grid";
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.effectPreset = "grid_tiling";
    clip.effectRows = 3;
    clip.effectScale = 0.82;
    clip.effectSpeed = 0.0;
    clip.effectAlternateDirection = true;
    document.clips.push_back(clip);

    const auto imguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {width, height},
             0.0,
             directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());
    const auto qtFrame =
        renderQtPreview(document, width, height);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    QVERIFY2(
        difference.meanAbsolute < 12.0,
        qPrintable(QStringLiteral(
            "generated-effect mean RGB difference is %1")
            .arg(difference.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.within16Fraction > 0.82,
        qPrintable(QStringLiteral(
            "generated-effect agreement is only %1%")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanTemporalEffect()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int width = 96;
    constexpr int height = 64;
    constexpr std::int64_t timelineFrame = 20;
    const std::string sourcePath = writeMovingVideo(
        directory.filePath(QStringLiteral("temporal.mp4")));
    if (sourcePath.empty()) {
        QSKIP("ffmpeg CLI with MPEG-4 encoding is required");
    }

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Temporal", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-temporal";
    clip.trackId = 1;
    clip.label = "Temporal echo";
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "video";
    clip.videoEnabled = true;
    document.clips.push_back(clip);

    const auto plainImguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {width, height},
             static_cast<double>(timelineFrame),
             directory.path().toStdString()});
    QVERIFY2(
        plainImguiFrame.success,
        plainImguiFrame.message.c_str());
    const auto plainQtFrame = renderQtPreview(
        document, width, height, timelineFrame);
    if (!plainQtFrame.success &&
        plainQtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(plainQtFrame.message.c_str());
    }
    QVERIFY2(plainQtFrame.success, plainQtFrame.message.c_str());
    const ImageDifference plainDifference =
        compareRgb(plainImguiFrame.image, plainQtFrame.image);

    document.clips.front().effectPreset = "temporal_echo";
    document.clips.front().temporalEchoCount = 2;
    document.clips.front().temporalEchoSpacingFrames = 4;
    document.clips.front().temporalEchoDecay = 0.35;

    const auto imguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {width, height},
             static_cast<double>(timelineFrame),
             directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());
    const auto qtFrame = renderQtPreview(
        document, width, height, timelineFrame);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    const ImageDifference imguiEffectDelta =
        compareRgb(plainImguiFrame.image, imguiFrame.image);
    const ImageDifference qtEffectDelta =
        compareRgb(plainQtFrame.image, qtFrame.image);
    QVERIFY2(
        imguiEffectDelta.meanAbsolute > 1.0 &&
            qtEffectDelta.meanAbsolute > 1.0,
        qPrintable(QStringLiteral(
            "temporal echo must affect both renderers "
            "(ImGui delta %1, Qt delta %2)")
            .arg(imguiEffectDelta.meanAbsolute, 0, 'f', 3)
            .arg(qtEffectDelta.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.meanAbsolute <
            std::max(25.0, plainDifference.meanAbsolute + 5.0),
        qPrintable(QStringLiteral(
            "temporal-effect mean RGB difference is %1 "
            "from a plain-video decoder baseline of %2")
            .arg(difference.meanAbsolute, 0, 'f', 3)
            .arg(plainDifference.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.within16Fraction > 0.52,
        qPrintable(QStringLiteral(
            "temporal-effect agreement is only %1%")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanTranscript()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int width = 320;
    constexpr int height = 180;
    constexpr std::int64_t timelineFrame = 10;
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("speech.ppm"))
            .toStdString(),
        width,
        height);
    const std::string transcriptPath =
        directory.filePath(
            QStringLiteral("speech.active.json"))
            .toStdString();
    {
        std::ofstream transcript(transcriptPath);
        transcript
            << R"json({"speaker_profiles":{"S1":{"name":"Ada","organization":"JCut","primary_color":"#ffcc33","secondary_color":"#ffffff","accent_color":"#ffcc33"}},"segments":[{"speaker":"S1","words":[{"word":"shared","start":0.0,"end":0.7,"speaker":"S1"},{"word":"render","start":0.7,"end":1.4,"speaker":"S1"}]}]})json";
    }

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Speech", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-transcript";
    clip.trackId = 1;
    clip.label = "Speech";
    clip.durationFrames = 60;
    clip.sourceDurationFrames = 60;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.transcriptActiveCutPath = transcriptPath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.hasAudio = true;
    clip.audioPresenceKnown = true;
    clip.transcriptOverlay.enabled = true;
    clip.transcriptOverlay.highlightCurrentWord = true;
    clip.transcriptOverlay.highlightColor = "#ffcc00";
    clip.transcriptOverlay.highlightTextColor = "#101010";
    clip.transcriptOverlay.textColor = "#ffffff";
    clip.transcriptOverlay.showBackground = true;
    clip.transcriptOverlay.backgroundColor = "#101820";
    clip.transcriptOverlay.backgroundOpacity = 0.82;
    clip.transcriptOverlay.fontFamily = "DejaVu Sans";
    clip.transcriptOverlay.fontPointSize = 24;
    clip.transcriptOverlay.boxWidth = 260.0;
    clip.transcriptOverlay.boxHeight = 84.0;
    document.clips.push_back(clip);

    const auto imguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {width, height},
             static_cast<double>(timelineFrame),
             directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());
    const auto qtFrame = renderQtPreview(
        document, width, height, timelineFrame);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    QVERIFY2(
        countYellowPixels(imguiFrame.image) > 20,
        "standalone transcript must highlight the active word");
    QVERIFY2(
        countYellowPixels(qtFrame.image) > 20,
        "Qt transcript must highlight the active word");
    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    QVERIFY2(
        difference.meanAbsolute < 17.0,
        qPrintable(QStringLiteral(
            "transcript mean RGB difference is %1")
            .arg(difference.meanAbsolute, 0, 'f', 3)));
    QVERIFY2(
        difference.within16Fraction > 0.72,
        qPrintable(QStringLiteral(
            "transcript agreement is only %1%")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtVulkanSpeakerFraming()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int sourceWidth = 160;
    constexpr int sourceHeight = 90;
    constexpr int outputWidth = 160;
    constexpr int outputHeight = 90;
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("speaker.ppm"))
            .toStdString(),
        sourceWidth,
        sourceHeight);

    jcut::EditorDocumentCore document;
    document.tracks.push_back({1, "Speaker", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "parity-speaker";
    clip.trackId = 1;
    clip.label = "Speaker";
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.speakerFramingEnabled = true;
    clip.speakerFramingBakedTargetXNorm = 0.76;
    clip.speakerFramingBakedTargetYNorm = 0.48;
    clip.speakerFramingBakedTargetBoxNorm = 0.34;
    clip.speakerFramingKeyframes.push_back(
        {0, "Framing", -12.0, 5.0, 0.0, 1.22, 1.22, true});
    clip.speakerFramingTargetKeyframes.push_back(
        {0, "Target", 0.46, 0.37, 0.0, 0.28, 0.28, true});
    document.clips.push_back(clip);

    const auto imguiFrame =
        jcut::standalone_render::renderTimelineFrame(
            {document,
             {outputWidth, outputHeight},
             0.0,
             directory.path().toStdString()});
    QVERIFY2(imguiFrame.success, imguiFrame.message.c_str());
    const auto qtFrame = renderQtPreview(
        document, outputWidth, outputHeight);
    if (!qtFrame.success &&
        qtFrame.message.find("Vulkan") != std::string::npos) {
        QSKIP(qtFrame.message.c_str());
    }
    QVERIFY2(qtFrame.success, qtFrame.message.c_str());

    const ImageDifference difference =
        compareRgb(imguiFrame.image, qtFrame.image);
    QVERIFY2(
        difference.meanAbsolute < 9.0,
        qPrintable(QStringLiteral(
            "speaker-framing mean RGB difference is %1 "
            "(ImGui avg RGB %2,%3,%4; Qt %5,%6,%7)")
            .arg(difference.meanAbsolute, 0, 'f', 3)
            .arg(averageChannel(imguiFrame.image, 0), 0, 'f', 1)
            .arg(averageChannel(imguiFrame.image, 1), 0, 'f', 1)
            .arg(averageChannel(imguiFrame.image, 2), 0, 'f', 1)
            .arg(averageChannel(qtFrame.image, 0), 0, 'f', 1)
            .arg(averageChannel(qtFrame.image, 1), 0, 'f', 1)
            .arg(averageChannel(qtFrame.image, 2), 0, 'f', 1)));
    QVERIFY2(
        // Scaling moves most samples off their original integer coordinates;
        // keep the aggregate error strict while allowing the Vulkan and CPU
        // bilinear samplers to round individual gradient pixels differently.
        difference.within16Fraction > 0.60,
        qPrintable(QStringLiteral(
            "speaker-framing agreement is only %1%")
            .arg(
                difference.within16Fraction * 100.0,
                0,
                'f',
                2)));
}

void TestImGuiQtRenderParity::
identicalNeutralProjectMatchesQtAudioMix()
{
    constexpr int decodedFrames = 512;
    constexpr int outputFrames = 64;
    constexpr std::int64_t chunkStartSample = 300;
    jcut::EditorDocumentCore document;
    jcut::EditorTrack track;
    track.id = 1;
    track.label = "Dialogue";
    track.audioGain = 0.75;
    document.tracks.push_back(track);

    jcut::EditorClip first;
    first.id = 1;
    first.persistentId = "parity-audio-a";
    first.trackId = 1;
    first.durationFrames = 30;
    first.sourceDurationFrames = 30;
    first.sourceFps = 30.0;
    first.sourcePath = "parity-a.wav";
    first.mediaKind = "audio";
    first.audioEnabled = true;
    first.hasAudio = true;
    first.audioPresenceKnown = true;
    first.audioGain = 0.8;
    document.clips.push_back(first);

    jcut::EditorClip second = first;
    second.id = 2;
    second.persistentId = "parity-audio-b";
    second.sourcePath = "parity-b.wav";
    second.audioGain = 0.4;
    document.clips.push_back(second);

    std::vector<float> firstSamples(
        decodedFrames * 2);
    std::vector<float> secondSamples(
        decodedFrames * 2);
    for (int frame = 0; frame < decodedFrames; ++frame) {
        firstSamples[frame * 2] =
            static_cast<float>((frame % 31) - 15) / 40.0f;
        firstSamples[frame * 2 + 1] =
            static_cast<float>((frame % 23) - 11) / 36.0f;
        secondSamples[frame * 2] =
            static_cast<float>((frame % 17) - 8) / 32.0f;
        secondSamples[frame * 2 + 1] =
            static_cast<float>((frame % 13) - 6) / 28.0f;
    }

    jcut::standalone_render::audio::DecodedAudioCache
        standaloneCache;
    standaloneCache.emplace(
        first.id,
        jcut::standalone_render::audio::DecodedAudioClip{
            firstSamples, 0, 1.0, {}, true});
    standaloneCache.emplace(
        second.id,
        jcut::standalone_render::audio::DecodedAudioClip{
            secondSamples, 0, 1.0, {}, true});
    std::vector<float> standaloneOutput(outputFrames * 2);
    jcut::standalone_render::audio::mixAudioChunk(
        document,
        standaloneCache,
        standaloneOutput.data(),
        outputFrames,
        chunkStartSample);

    const jcut::render::TimelineRenderData timeline =
        jcut::render::buildTimelineRenderData(document, false);
    QHash<QString, render_detail::DecodedAudioClip> qtCache;
    render_detail::DecodedAudioClip qtFirst;
    qtFirst.samples = QVector<float>(
        firstSamples.begin(), firstSamples.end());
    qtFirst.valid = true;
    qtCache.insert(
        QFileInfo(QStringLiteral("parity-a.wav"))
            .absoluteFilePath(),
        qtFirst);
    render_detail::DecodedAudioClip qtSecond;
    qtSecond.samples = QVector<float>(
        secondSamples.begin(), secondSamples.end());
    qtSecond.valid = true;
    qtCache.insert(
        QFileInfo(QStringLiteral("parity-b.wav"))
            .absoluteFilePath(),
        qtSecond);
    std::vector<float> qtOutput(outputFrames * 2);
    const QVector<TimelineClip> qtClips(
        timeline.clips.begin(), timeline.clips.end());
    const QVector<TimelineTrack> qtTracks(
        timeline.tracks.begin(), timeline.tracks.end());
    const QVector<RenderSyncMarker> qtMarkers(
        timeline.renderSyncMarkers.begin(),
        timeline.renderSyncMarkers.end());
    render_detail::mixAudioChunk(
        qtClips,
        qtTracks,
        qtMarkers,
        qtCache,
        qtOutput.data(),
        outputFrames,
        chunkStartSample);

    QCOMPARE(standaloneOutput.size(), qtOutput.size());
    for (std::size_t index = 0;
         index < standaloneOutput.size();
         ++index) {
        QVERIFY2(
            std::abs(
                standaloneOutput[index] -
                qtOutput[index]) < 0.00001f,
            qPrintable(QStringLiteral(
                "mixed sample %1 differs: ImGui %2, Qt %3")
                .arg(index)
                .arg(standaloneOutput[index], 0, 'f', 7)
                .arg(qtOutput[index], 0, 'f', 7)));
    }
}

QTEST_MAIN(TestImGuiQtRenderParity)
#include "test_imgui_qt_render_parity.moc"
