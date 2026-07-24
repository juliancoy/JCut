#include "../imgui_gpu_renderer_bridge.h"

#include <QtTest/QtTest>

#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
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
    void exportsThroughQtVulkanPipeline();
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
exportsThroughQtVulkanPipeline()
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
    QVERIFY(result.framesRendered > 0);
    QVERIFY(progressCalls > 0);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(outputPath)));
}

QTEST_MAIN(TestImGuiGpuRendererBridge)
#include "test_imgui_gpu_renderer_bridge.moc"
