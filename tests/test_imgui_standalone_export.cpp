#include <QtTest/QtTest>

#include "../standalone_export_renderer.h"
#include "../standalone_audio_mixer.h"

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
    void outputFormatOverridesMismatchedPathExtension();
    void qtFreeAudioMixerMatchesTimelinePolicies();
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

void TestImGuiStandaloneExport::outputFormatOverridesMismatchedPathExtension()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");

    const std::string sourcePath =
        writePpmFixture((tempDir.path() + QStringLiteral("/fixture.ppm")).toStdString());
    const std::string outputPath =
        (tempDir.path() + QStringLiteral("/matroska_with_mp4_extension.mp4")).toStdString();

    jcut::EditorDocumentCore document;
    document.projectName = "Standalone Matroska export";
    document.tracks.push_back({1, "Video A", true});
    document.clips.push_back({1, 1, "Still", 0, 2, true, sourcePath});
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mkv";
    document.exportRequest.outputSize = {64, 64};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 1;

    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile({document, {}});

    QVERIFY2(result.success, result.message.c_str());
    AVFormatContext* formatContext = nullptr;
    const int openResult = avformat_open_input(
        &formatContext, outputPath.c_str(), nullptr, nullptr);
    QVERIFY2(openResult >= 0, "Matroska content must be readable despite the .mp4 suffix");
    QVERIFY2(avformat_find_stream_info(formatContext, nullptr) >= 0,
             "Matroska content must expose stream info");
    QVERIFY(formatContext->iformat != nullptr);
    const QString detectedFormat = QString::fromUtf8(formatContext->iformat->name);
    QVERIFY2(detectedFormat.contains(QStringLiteral("matroska")),
             qPrintable(QStringLiteral("expected Matroska, detected %1")
                            .arg(detectedFormat)));
    avformat_close_input(&formatContext);

    document.exportRequest.outputFormat = "webm";
    document.exportRequest.outputPath =
        (tempDir.path() + QStringLiteral("/webm_with_mp4_extension.mp4")).toStdString();
    const bool hasPortableWebmEncoder =
        avcodec_find_encoder_by_name("libvpx-vp9") != nullptr ||
        avcodec_find_encoder_by_name("libvpx") != nullptr;
    const jcut::render::RenderResultCore webmResult =
        jcut::standalone_render::exportTimelineToFile({document, {}});
    if (!hasPortableWebmEncoder) {
        QCOMPARE(webmResult.success, false);
        QCOMPARE(QString::fromStdString(webmResult.message),
                 QStringLiteral("WebM export requires a libvpx VP8/VP9 encoder"));
    } else {
        QVERIFY2(webmResult.success, webmResult.message.c_str());
        formatContext = nullptr;
        QVERIFY(avformat_open_input(
                    &formatContext,
                    document.exportRequest.outputPath.c_str(),
                    nullptr,
                    nullptr) >= 0);
        QVERIFY(formatContext->iformat != nullptr);
        const QString webmFormat = QString::fromUtf8(formatContext->iformat->name);
        QVERIFY(webmFormat.contains(QStringLiteral("webm")) ||
                webmFormat.contains(QStringLiteral("matroska")));
        avformat_close_input(&formatContext);
    }
}

void TestImGuiStandaloneExport::qtFreeAudioMixerMatchesTimelinePolicies()
{
    using namespace jcut::standalone_render::audio;
    jcut::EditorDocumentCore document;
    jcut::EditorTrack track;
    track.id = 1;
    track.audioGain = 0.5;
    document.tracks.push_back(track);

    jcut::EditorClip clip;
    clip.id = 7;
    clip.persistentId = "audio-7";
    clip.trackId = 1;
    clip.startFrame = 1;
    clip.durationFrames = 4;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.mediaKind = "audio";
    clip.hasAudio = true;
    clip.audioGain = 0.5;
    clip.fadeSamples = 100;
    document.clips.push_back(clip);

    DecodedAudioClip decoded;
    decoded.samples.assign(10'000 * kChannelCount, 0.8f);
    decoded.valid = true;
    DecodedAudioCache cache{{clip.id, std::move(decoded)}};

    std::array<float, 8> output{};
    mixAudioChunk(document, cache, output.data(), 4,
                  clipTimelineStartSamples(clip));
    QCOMPARE(output[0], 0.0f);
    QVERIFY(std::abs(output[2] - 0.002f) < 0.00001f);
    QVERIFY(std::abs(output[4] - 0.004f) < 0.00001f);

    document.renderSyncMarkers.push_back(
        {clip.persistentId, clip.startFrame + 1, true, 2});
    const std::int64_t afterMarker =
        clipTimelineStartSamples(clip) + 2 * kSamplesPerTimelineFrame + 8;
    QCOMPARE(sourceSampleForClipAtTimelineSample(
                 document, clip, afterMarker),
             4 * kSamplesPerTimelineFrame + 8);

    document.tracks.front().audioMuted = true;
    output.fill(1.0f);
    mixAudioChunk(document, cache, output.data(), 4,
                  clipTimelineStartSamples(clip) + 100);
    for (float sample : output) {
        QCOMPARE(sample, 0.0f);
    }
}

QTEST_MAIN(TestImGuiStandaloneExport)

#include "test_imgui_standalone_export.moc"
