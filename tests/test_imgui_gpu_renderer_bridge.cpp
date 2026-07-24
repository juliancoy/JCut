#include "../imgui_gpu_renderer_bridge.h"
#include "../standalone_timeline_renderer.h"

#include <QtTest/QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
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
                static_cast<unsigned char>(96)};
            output.write(
                reinterpret_cast<const char*>(pixel),
                sizeof(pixel));
        }
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

double meanAbsoluteRgbDifference(
    const jcut::core::ImageBuffer& left,
    const jcut::core::ImageBuffer& right)
{
    if (left.empty() || right.empty() ||
        left.size.width != right.size.width ||
        left.size.height != right.size.height) {
        return 255.0;
    }
    std::uint64_t sum = 0;
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(left.size.width) *
        left.size.height;
    for (int y = 0; y < left.size.height; ++y) {
        for (int x = 0; x < left.size.width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const std::size_t leftOffset =
                    static_cast<std::size_t>(
                        y * left.strideBytes + x * 4 +
                        channel);
                const std::size_t rightOffset =
                    static_cast<std::size_t>(
                        y * right.strideBytes + x * 4 +
                        channel);
                sum += static_cast<std::uint64_t>(
                    std::abs(
                        static_cast<int>(
                            left.bytes[leftOffset]) -
                        static_cast<int>(
                            right.bytes[rightOffset])));
            }
        }
    }
    return static_cast<double>(sum) /
        static_cast<double>(pixelCount * 3);
}

jcut::EditorDocumentCore makeDocument(
    const std::string& sourcePath,
    const std::string& outputPath)
{
    jcut::EditorDocumentCore document;
    document.projectName = "Shared GPU bridge";
    document.tracks.push_back({1, "Media", true});
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "shared-gpu-source";
    clip.trackId = 1;
    clip.label = "Source";
    clip.durationFrames = 1;
    clip.sourceDurationFrames = 1;
    clip.sourceFps = 30.0;
    clip.sourcePath = sourcePath;
    clip.mediaKind = "image";
    clip.videoEnabled = true;
    clip.brightness = 0.06;
    clip.contrast = 1.08;
    document.clips.push_back(clip);
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {320, 180};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 0;
    return document;
}

} // namespace

class TestImGuiGpuRendererBridge : public QObject {
    Q_OBJECT

private slots:
    void rendersBorrowedVulkanPreview();
    void rendersScopesFromSameVulkanFrame();
    void rendersMismatchedMaskAcrossLayerBatches();
    void exportsThroughQtFreeHardwarePipeline();
};

void TestImGuiGpuRendererBridge::
rendersBorrowedVulkanPreview()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("source.ppm"))
            .toStdString(),
        320,
        180);
    const auto document = makeDocument(
        sourcePath,
        directory.filePath(QStringLiteral("unused.mp4"))
            .toStdString());

    jcut::imgui_gpu::RendererBridge bridge;
    std::string error;
    QVERIFY2(
        bridge.initialize(
            QCoreApplication::applicationFilePath()
                .toStdString(),
            &error),
        error.c_str());
    render_detail::OffscreenVulkanFrame frame;
    QVERIFY2(
        bridge.renderPreview(
            document,
            directory.path().toStdString(),
            {320, 180},
            0,
            false,
            &frame,
            nullptr,
            &error),
        error.c_str());
    QVERIFY(frame.valid);
    QCOMPARE(frame.size.width, 320);
    QCOMPARE(frame.size.height, 180);
}

void TestImGuiGpuRendererBridge::
rendersScopesFromSameVulkanFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("source.ppm"))
            .toStdString(),
        320,
        180);
    const auto document = makeDocument(
        sourcePath,
        directory.filePath(QStringLiteral("unused.mp4"))
            .toStdString());

    jcut::imgui_gpu::RendererBridge bridge;
    std::string error;
    QVERIFY2(
        bridge.initialize(
            QCoreApplication::applicationFilePath()
                .toStdString(),
            &error),
        error.c_str());
    render_detail::OffscreenVulkanFrame frame;
    jcut::core::ImageBuffer scopeImage;
    QVERIFY2(
        bridge.renderPreview(
            document,
            directory.path().toStdString(),
            {320, 180},
            0,
            true,
            &frame,
            &scopeImage,
            &error),
        error.c_str());
    QVERIFY(frame.valid);
    QVERIFY(!scopeImage.empty());
    QCOMPARE(scopeImage.size.width, 320);
    QCOMPARE(scopeImage.size.height, 180);
}

void TestImGuiGpuRendererBridge::
rendersMismatchedMaskAcrossLayerBatches()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int outputWidth = 96;
    constexpr int outputHeight = 160;
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("source.ppm"))
            .toStdString(),
        outputWidth,
        outputHeight);
    const QString maskDirectory =
        directory.filePath(QStringLiteral("mask"));
    QVERIFY(QDir().mkpath(maskDirectory));
    writeSplitMaskPgm(
        QDir(maskDirectory)
            .filePath(QStringLiteral("frame_000001.png"))
            .toStdString(),
        outputHeight,
        outputWidth);

    jcut::EditorDocumentCore document;
    document.projectName = "Mismatched mask batches";
    for (int index = 0; index < 13; ++index) {
        const int id = index + 1;
        document.tracks.push_back(
            {id, "Layer " + std::to_string(id), true});
        jcut::EditorClip clip;
        clip.id = id;
        clip.persistentId =
            "batch-layer-" + std::to_string(id);
        clip.trackId = id;
        clip.label = "Layer";
        clip.durationFrames = 1;
        clip.sourceDurationFrames = 1;
        clip.sourceFps = 30.0;
        clip.sourcePath = sourcePath;
        clip.mediaKind = "image";
        clip.videoEnabled = true;
        clip.opacity = 0.2;
        if (index == 0) {
            clip.maskEnabled = true;
            clip.maskFramesDir =
                maskDirectory.toStdString();
        }
        document.clips.push_back(std::move(clip));
    }
    document.exportRequest.outputSize = {
        outputWidth, outputHeight};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 0;

    jcut::imgui_gpu::RendererBridge bridge;
    std::string error;
    QVERIFY2(
        bridge.initialize(
            QCoreApplication::applicationFilePath()
                .toStdString(),
            &error),
        error.c_str());
    render_detail::OffscreenVulkanFrame frame;
    jcut::core::ImageBuffer image;
    QVERIFY2(
        bridge.renderPreview(
            document,
            directory.path().toStdString(),
            {outputWidth, outputHeight},
            0,
            true,
            &frame,
            &image,
            &error),
        error.c_str());
    QVERIFY(frame.valid);
    QVERIFY(!image.empty());
    QCOMPARE(frame.size.width, outputWidth);
    QCOMPARE(frame.size.height, outputHeight);
    const auto expected =
        jcut::standalone_render::renderTimelineFrame({
            document,
            {outputWidth, outputHeight},
            0.0,
            directory.path().toStdString()});
    QVERIFY2(expected.success, expected.message.c_str());
    const double difference =
        meanAbsoluteRgbDifference(image, expected.image);
    QVERIFY2(
        difference < 2.0,
        qPrintable(QStringLiteral(
            "neutral Vulkan composition differs by %1 RGB levels")
            .arg(difference, 0, 'f', 3)));
}

void TestImGuiGpuRendererBridge::
exportsThroughQtFreeHardwarePipeline()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::string sourcePath = writeGradientPpm(
        directory.filePath(QStringLiteral("source.ppm"))
            .toStdString(),
        320,
        180);
    const std::string outputPath =
        directory.filePath(QStringLiteral("gpu.mp4"))
            .toStdString();
    const auto document =
        makeDocument(sourcePath, outputPath);

    jcut::imgui_gpu::RendererBridge bridge;
    std::string error;
    QVERIFY2(
        bridge.initialize(
            QCoreApplication::applicationFilePath()
                .toStdString(),
            &error),
        error.c_str());
    int progressCalls = 0;
    const jcut::render::RenderResultCore result =
        bridge.exportTimeline(
            document,
            directory.path().toStdString(),
            [&progressCalls](
                const jcut::render::RenderProgressCore& progress) {
                ++progressCalls;
                return progress.framesCompleted <=
                    progress.totalFrames;
            });
    QVERIFY2(result.success, result.message.c_str());
    QVERIFY(result.usedGpu);
    QVERIFY(result.usedHardwareEncode);
    QVERIFY(
        result.encoderLabel.find("nvenc") !=
        std::string::npos);
    QVERIFY(result.framesRendered > 0);
    QVERIFY(progressCalls > 0);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(outputPath)));
}

QTEST_MAIN(TestImGuiGpuRendererBridge)
#include "test_imgui_gpu_renderer_bridge.moc"
