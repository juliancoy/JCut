#include <QtTest/QtTest>

#include "../standalone_export_renderer.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QFileInfo>
#include <QTemporaryDir>

#include <fstream>

class TestImGuiStandaloneExport : public QObject {
    Q_OBJECT

private slots:
    void exportsQtFreeVideoFile();
    void exportsQtFreeImageSequence();
    void exportPlaybackSpeedChangesVideoFrameCount();
};

namespace {

std::string writePpmFixture(const std::string& path)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n4 4\n255\n";
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const unsigned char red = static_cast<unsigned char>(x * 48 + 32);
            const unsigned char green = static_cast<unsigned char>(y * 48 + 32);
            const unsigned char blue = 96;
            output.write(reinterpret_cast<const char*>(&red), 1);
            output.write(reinterpret_cast<const char*>(&green), 1);
            output.write(reinterpret_cast<const char*>(&blue), 1);
        }
    }
    return path;
}

} // namespace

int countVideoPackets(const std::string& path)
{
    AVFormatContext* formatContext = nullptr;
    const int openResult = avformat_open_input(&formatContext, path.c_str(), nullptr, nullptr);
    if (openResult < 0 || avformat_find_stream_info(formatContext, nullptr) < 0) {
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
        return -1;
    }

    const int videoStreamIndex =
        av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIndex < 0) {
        avformat_close_input(&formatContext);
        return -1;
    }

    int packetCount = 0;
    AVPacket* packet = av_packet_alloc();
    while (packet && av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            ++packetCount;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&formatContext);
    return packetCount;
}

void TestImGuiStandaloneExport::exportsQtFreeVideoFile()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");

    const std::string sourcePath =
        writePpmFixture((tempDir.path() + QStringLiteral("/fixture.ppm")).toStdString());
    const std::string outputPath = (tempDir.path() + QStringLiteral("/export.mp4")).toStdString();

    jcut::EditorDocumentCore document;
    document.projectName = "Standalone export";
    document.tracks.push_back({1, "Video A", true});
    document.clips.push_back({1, 1, "Still", 0, 30, true, sourcePath});
    document.transport.currentFrame = 0;
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {320, 240};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 29;

    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile({document, {}});

    QVERIFY2(result.success, result.message.c_str());
    QVERIFY2(result.framesRendered > 0, "export should render at least one frame");

    AVFormatContext* formatContext = nullptr;
    const int openResult = avformat_open_input(&formatContext, outputPath.c_str(), nullptr, nullptr);
    QVERIFY2(openResult >= 0, "exported container must be readable");
    QVERIFY2(avformat_find_stream_info(formatContext, nullptr) >= 0,
             "exported container must expose stream info");
    const int videoStreamIndex =
        av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    QVERIFY2(videoStreamIndex >= 0, "exported file must contain a video stream");
    avformat_close_input(&formatContext);
}

void TestImGuiStandaloneExport::exportsQtFreeImageSequence()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");

    const std::string sourcePath =
        writePpmFixture((tempDir.path() + QStringLiteral("/fixture.ppm")).toStdString());
    const std::string outputPath = (tempDir.path() + QStringLiteral("/sequence.mp4")).toStdString();

    jcut::EditorDocumentCore document;
    document.projectName = "Standalone image sequence export";
    document.tracks.push_back({1, "Video A", true});
    document.clips.push_back({1, 1, "Still", 0, 10, true, sourcePath});
    document.transport.currentFrame = 0;
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {320, 240};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 9;
    document.exportRequest.createVideoFromImageSequence = true;
    document.exportRequest.imageSequenceFormat = "png";
    document.exportRequest.outputMode =
        jcut::render::RenderOutputMode::EncodedFileAndImageSequence;

    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile({document, {}});

    QVERIFY2(result.success, result.message.c_str());
    const QString sequenceDir =
        QFileInfo(QString::fromStdString(outputPath)).dir().filePath(
            QFileInfo(QString::fromStdString(outputPath)).completeBaseName());
    const QString framePath = sequenceDir + QStringLiteral("/frame_00000000.png");
    QVERIFY2(QFileInfo::exists(framePath), "image-sequence export must write frame files");
}

void TestImGuiStandaloneExport::exportPlaybackSpeedChangesVideoFrameCount()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");

    const std::string sourcePath =
        writePpmFixture((tempDir.path() + QStringLiteral("/fixture.ppm")).toStdString());
    const std::string outputPath = (tempDir.path() + QStringLiteral("/export_2x.mp4")).toStdString();

    jcut::EditorDocumentCore document;
    document.projectName = "Standalone speed export";
    document.tracks.push_back({1, "Video A", true});
    document.clips.push_back({1, 1, "Still", 0, 30, true, sourcePath});
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {320, 240};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.playbackSpeed = 2.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 29;

    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile({document, {}});

    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(result.framesRendered, static_cast<std::int64_t>(16));
    QCOMPARE(countVideoPackets(outputPath), 16);
}

QTEST_MAIN(TestImGuiStandaloneExport)

#include "test_imgui_standalone_export.moc"
