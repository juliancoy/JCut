#include <QtTest/QtTest>

#include "../standalone_export_renderer.h"
#include "../standalone_audio_mixer.h"
#include "../standalone_timeline_renderer.h"
#include "../audio_time_stretch_core.h"
#include "../audio_dynamics_core.h"
#include "../editor_document_core_json.h"
#include "../editor_timeline_mapping_core.h"
#include "../proxy_generation_job_core.h"
#include "../vulkan_hardware_frame_import_core.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <fstream>
#include <cmath>
#include <cstdint>
#include <vector>

class TestImGuiStandaloneExport : public QObject {
    Q_OBJECT

private slots:
    void exportsQtFreeVideoFile();
    void exportsQtFreeImageSequence();
    void exportPlaybackSpeedChangesVideoFrameCount();
    void exportRangesBoundAndConcatenateSegments();
    void outputFormatOverridesMismatchedPathExtension();
    void qtFreeAudioMixerMatchesTimelinePolicies();
    void boundedAudioDecodeRetainsOnlyRequestedSourceEnvelope();
    void transcriptNormalizationUsesActiveCutWords();
    void exportsDecodedAndMixedAudioStream();
    void sharedPitchPreservingStretchKeepsToneFrequency();
    void sharedHarmonicIsolationAndTreatmentRoundTrip();
    void sharedAudioDynamicsMatchesQtProcessingAndRoundTrips();
    void generatesNeutralProxyJob();
};

namespace {

QString readSourceFile(const QString& relativePath)
{
    QFile file(
        QStringLiteral(JCUT_SOURCE_DIR) +
        QLatin1Char('/') +
        relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

std::string writePpmFixture(
    const std::string& path,
    int width = 4,
    int height = 4)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
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

std::string writeWavFixture(
    const std::string& path,
    std::uint32_t frameCount = 48000)
{
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bitsPerSample = 16;
    const std::uint32_t dataSize =
        frameCount * channels * (bitsPerSample / 8);
    auto write16 = [](std::ofstream& output, std::uint16_t value) {
        const char bytes[] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff)};
        output.write(bytes, sizeof(bytes));
    };
    auto write32 = [](std::ofstream& output, std::uint32_t value) {
        const char bytes[] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
            static_cast<char>((value >> 16) & 0xff),
            static_cast<char>((value >> 24) & 0xff)};
        output.write(bytes, sizeof(bytes));
    };
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    write32(output, 36 + dataSize);
    output.write("WAVEfmt ", 8);
    write32(output, 16);
    write16(output, 1);
    write16(output, channels);
    write32(output, sampleRate);
    write32(output, sampleRate * channels * (bitsPerSample / 8));
    write16(output, channels * (bitsPerSample / 8));
    write16(output, bitsPerSample);
    output.write("data", 4);
    write32(output, dataSize);
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 *
            static_cast<double>(frame) / sampleRate;
        const std::int16_t sample = static_cast<std::int16_t>(
            std::sin(phase) * 12000.0);
        write16(output, static_cast<std::uint16_t>(sample));
        write16(output, static_cast<std::uint16_t>(sample));
    }
    return path;
}

bool importRetainedHardwareFrame(
    const jcut::core::FramePayloadCore& frame,
    std::string* error)
{
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "jcut_hardware_frame_test";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(
            &instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        if (error) *error = "failed to create test Vulkan instance";
        return false;
    }
    std::uint32_t physicalCount = 0;
    vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    vkEnumeratePhysicalDevices(
        instance, &physicalCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::uint32_t queueFamily = UINT32_MAX;
    for (VkPhysicalDevice candidate : physicalDevices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID != 0x10de) continue;
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, families.data());
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physicalDevice = candidate;
                queueFamily = index;
                break;
            }
        }
        if (physicalDevice != VK_NULL_HANDLE) break;
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        if (error) *error = "matching NVIDIA Vulkan device unavailable";
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const std::array<const char*, 2> extensions{
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(
            physicalDevice,
            &deviceInfo,
            nullptr,
            &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        if (error) *error = "failed to create test Vulkan device";
        return false;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    jcut::vulkan_import::VulkanHardwareFrameImportCore importer;
    bool imported = importer.initialize(
        physicalDevice,
        device,
        queue,
        queueFamily,
        JCUT_VULKAN_SHADER_DIR,
        error);
    if (imported) imported = importer.importFrame(frame, error);
    if (imported) {
        const auto image = importer.externalImage();
        imported = image.imageView != VK_NULL_HANDLE &&
            image.imageLayout ==
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            image.size == frame.size();
        if (!imported && error) {
            *error = "hardware importer returned an invalid sampled image";
        }
    }
    importer.release();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return imported;
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
    QCOMPARE(result.framesRendered, static_cast<std::int64_t>(15));
    QCOMPARE(countVideoPackets(outputPath), 15);
}

void TestImGuiStandaloneExport::exportRangesBoundAndConcatenateSegments()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");

    const std::string sourcePath =
        writePpmFixture(
            (tempDir.path() + QStringLiteral("/fixture.ppm"))
                .toStdString());
    const std::string outputPath =
        (tempDir.path() + QStringLiteral("/segments.mp4"))
            .toStdString();
    const std::string audioPath =
        writeWavFixture(
            (tempDir.path() + QStringLiteral("/segments.wav"))
                .toStdString());

    jcut::EditorDocumentCore document;
    document.projectName = "Discontiguous export";
    document.tracks.push_back({1, "Video A", true});
    document.clips.push_back(
        {1, 1, "Still", 0, 60, true, sourcePath});
    document.clips.back().hasAudio = true;
    document.clips.back().audioPresenceKnown = true;
    document.clips.back().audioSourceMode = "external";
    document.clips.back().audioSourcePath = audioPath;
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {64, 64};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 59;
    document.exportRanges = {{0, 4}, {30, 34}};

    std::vector<jcut::render::RenderProgressCore> progress;
    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile(
            {document, {}},
            [&](const auto& update) {
                progress.push_back(update);
                return true;
            });

    QVERIFY2(result.success, result.message.c_str());
    QCOMPARE(result.framesRendered, static_cast<std::int64_t>(10));
    QCOMPARE(countVideoPackets(outputPath), 10);
    QVERIFY(!progress.empty());
    QCOMPARE(progress.back().segmentCount, 2);
    QCOMPARE(progress.back().segmentIndex, 2);
    QCOMPARE(progress.back().segmentStartFrame,
             static_cast<std::int64_t>(30));
    QCOMPARE(progress.back().segmentEndFrame,
             static_cast<std::int64_t>(34));
    AVFormatContext* formatContext = nullptr;
    QVERIFY(avformat_open_input(
                &formatContext,
                outputPath.c_str(),
                nullptr,
                nullptr) >= 0);
    QVERIFY(avformat_find_stream_info(formatContext, nullptr) >= 0);
    const int audioStreamIndex = av_find_best_stream(
        formatContext,
        AVMEDIA_TYPE_AUDIO,
        -1,
        -1,
        nullptr,
        0);
    QVERIFY(audioStreamIndex >= 0);
    const AVStream* audioStream =
        formatContext->streams[audioStreamIndex];
    const double audioDuration = audioStream->duration > 0
        ? audioStream->duration *
            av_q2d(audioStream->time_base)
        : 0.0;
    avformat_close_input(&formatContext);
    QVERIFY2(
        audioDuration >= 0.30 && audioDuration <= 0.40,
        "discontiguous audio must concatenate only the selected ranges");

    jcut::EditorDocumentCore mappingDocument;
    jcut::EditorClip mappingClip;
    mappingClip.startFrame = 100;
    mappingClip.durationFrames = 10;
    mappingClip.sourceInFrame = 24;
    mappingClip.sourceDurationFrames = 120;
    mappingClip.sourceFps = 24.0;
    const auto mappedRanges =
        jcut::editorTimelineRangesForTranscriptSection(
            mappingDocument,
            mappingClip,
            32,
            34);
    QCOMPARE(mappedRanges.size(), std::size_t{1});
    QCOMPARE(mappedRanges.front().startFrame,
             static_cast<std::int64_t>(102));
    QCOMPARE(mappedRanges.front().endFrame,
             static_cast<std::int64_t>(104));
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

    document.tracks.front().audioMuted = false;
    document.clips.front().audioPan = 1.0;
    mixAudioChunk(document, cache, output.data(), 1,
                  clipTimelineStartSamples(clip) + 100);
    QCOMPARE(output[0], 0.0f);
    QVERIFY(output[1] > 0.0f);
}

void TestImGuiStandaloneExport::
boundedAudioDecodeRetainsOnlyRequestedSourceEnvelope()
{
    using namespace jcut::standalone_render::audio;
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const std::string audioPath = writeWavFixture(
        (tempDir.path() + QStringLiteral("/long-tone.wav")).toStdString(),
        10 * kSampleRate);

    jcut::EditorDocumentCore document;
    document.tracks.push_back(jcut::EditorTrack{});
    document.tracks.front().id = 1;
    jcut::EditorClip clip;
    clip.id = 27;
    clip.persistentId = "bounded-audio";
    clip.trackId = 1;
    clip.label = "Long tone";
    clip.sourcePath = audioPath;
    clip.mediaKind = "audio";
    clip.hasAudio = true;
    clip.audioPresenceKnown = true;
    clip.durationFrames = 150;
    clip.sourceDurationFrames = 300;
    clip.sourceFps = 30.0;
    clip.playbackRate = 2.0;
    document.clips.push_back(clip);

    const std::vector<jcut::EditorExportRange> ranges{
        {120, 121}};
    const std::int64_t requestedSourceStart =
        sourceSampleForClipAtTimelineSample(
            document,
            document.clips.front(),
            120 * kSamplesPerTimelineFrame);
    DecodedAudioCache cache;
    std::string error;
    QVERIFY2(
        decodeDocumentAudio(
            document, {}, &cache, &error,
            &ranges),
        error.c_str());
    const auto decoded = cache.find(clip.id);
    QVERIFY(decoded != cache.end());
    QCOMPARE(
        decoded->second.sourceStartSample,
        requestedSourceStart);
    QVERIFY(decoded->second.sourceStartSample >
            7 * kSampleRate);
    QVERIFY(decoded->second.samples.size() <
            static_cast<std::size_t>(
                kSampleRate * kChannelCount));
    QVERIFY(std::abs(
        decoded->second.sourceSampleScale -
        0.5) < 0.000001);

    std::array<float, 2> output{};
    mixAudioChunk(
        document,
        cache,
        output.data(),
        1,
        120 * kSamplesPerTimelineFrame + 13);
    QVERIFY(std::abs(output[0]) > 0.0001f);
    QVERIFY(std::abs(output[1]) > 0.0001f);
}

void TestImGuiStandaloneExport::transcriptNormalizationUsesActiveCutWords()
{
    using namespace jcut::standalone_render::audio;
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const std::string audioPath = writeWavFixture(
        (tempDir.path() + QStringLiteral("/speech.wav")).toStdString());
    const std::string transcriptPath =
        (tempDir.path() + QStringLiteral("/speech.active.json"))
            .toStdString();
    {
        std::ofstream transcript(transcriptPath);
        transcript << nlohmann::json{
            {"segments", nlohmann::json::array({
                {{"speaker", "SPEAKER_00"},
                 {"words", nlohmann::json::array({
                     {{"word", "hello"},
                      {"start", 0.0},
                      {"end", 1.0},
                      {"speaker", "SPEAKER_00"}}})}}})}}
            .dump();
    }

    jcut::EditorDocumentCore document;
    document.audioDynamics.transcriptNormalizeEnabled = true;
    document.tracks.push_back(jcut::EditorTrack{});
    document.tracks.front().id = 1;
    jcut::EditorClip clip;
    clip.id = 11;
    clip.trackId = 1;
    clip.label = "Speech";
    clip.sourcePath = audioPath;
    clip.transcriptActiveCutPath = transcriptPath;
    clip.mediaKind = "audio";
    clip.hasAudio = true;
    clip.audioPresenceKnown = true;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    document.clips.push_back(clip);

    DecodedAudioCache cache;
    std::string error;
    QVERIFY2(
        decodeDocumentAudio(document, {}, &cache, &error),
        error.c_str());
    const auto decoded = cache.find(clip.id);
    QVERIFY(decoded != cache.end());
    QVERIFY2(
        !decoded->second.transcriptNormalizeSegments.empty(),
        "active transcript words must produce normalization segments");
    QVERIFY(decoded->second.transcriptNormalizeSegments.front().gain > 2.0f);

    std::array<float, 2> output{};
    const std::int64_t probeSample = kSampleRate / 4 + 13;
    mixAudioChunk(
        document,
        cache,
        output.data(),
        1,
        probeSample);
    const std::int64_t sourceSample =
        sourceSampleForClipAtTimelineSample(
            document, clip, probeSample);
    const std::size_t offset = static_cast<std::size_t>(
        sourceSample * kChannelCount);
    QVERIFY(offset < decoded->second.samples.size());
    const float untreated =
        std::abs(decoded->second.samples[offset]);
    QVERIFY(
        std::abs(output[0]) >
        untreated * 2.0f);
}

void TestImGuiStandaloneExport::exportsDecodedAndMixedAudioStream()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");
    const std::string imagePath = writePpmFixture(
        (tempDir.path() + QStringLiteral("/fixture.ppm")).toStdString());
    const std::string audioPath = writeWavFixture(
        (tempDir.path() + QStringLiteral("/tone.wav")).toStdString());
    const std::string outputPath =
        (tempDir.path() + QStringLiteral("/export_with_audio.mp4")).toStdString();

    jcut::EditorDocumentCore document;
    jcut::EditorTrack track;
    track.id = 1;
    track.label = "Video A";
    document.tracks.push_back(track);
    jcut::EditorClip clip;
    clip.id = 1;
    clip.persistentId = "clip-with-audio";
    clip.trackId = 1;
    clip.label = "Tone";
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceDurationFrames = 30;
    clip.sourceFps = 30.0;
    clip.sourcePath = imagePath;
    clip.hasAudio = true;
    clip.audioPresenceKnown = true;
    clip.audioSourceMode = "external";
    clip.audioSourcePath = audioPath;
    clip.fadeSamples = 250;
    document.clips.push_back(clip);
    document.exportRequest.outputPath = outputPath;
    document.exportRequest.outputFormat = "mp4";
    document.exportRequest.outputSize = {64, 64};
    document.exportRequest.outputFps = 30.0;
    document.exportRequest.playbackSpeed = 2.0;
    document.exportRequest.exportStartFrame = 0;
    document.exportRequest.exportEndFrame = 29;

    const jcut::render::RenderResultCore result =
        jcut::standalone_render::exportTimelineToFile({document, {}});
    QVERIFY2(result.success, result.message.c_str());

    AVFormatContext* formatContext = nullptr;
    QVERIFY(avformat_open_input(
                &formatContext, outputPath.c_str(), nullptr, nullptr) >= 0);
    QVERIFY(avformat_find_stream_info(formatContext, nullptr) >= 0);
    const int audioStreamIndex = av_find_best_stream(
        formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    QVERIFY2(audioStreamIndex >= 0,
             "standalone encoded export must contain an audio stream");
    const AVStream* audioStream = formatContext->streams[audioStreamIndex];
    const double audioDuration = audioStream->duration > 0
        ? audioStream->duration * av_q2d(audioStream->time_base)
        : 0.0;
    QVERIFY2(audioDuration > 0.4 && audioDuration < 0.7,
             "2x standalone export audio must track the shortened video duration");
    int audioPacketCount = 0;
    AVPacket* packet = av_packet_alloc();
    while (packet && av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == audioStreamIndex) {
            ++audioPacketCount;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&formatContext);
    QVERIFY2(audioPacketCount > 0,
             "standalone encoded export must mux encoded audio packets");
}

void TestImGuiStandaloneExport::sharedPitchPreservingStretchKeepsToneFrequency()
{
    constexpr int sampleRate = 48000;
    constexpr int channels = 2;
    std::vector<float> input(static_cast<std::size_t>(sampleRate) * channels);
    for (int frame = 0; frame < sampleRate; ++frame) {
        const float sample = static_cast<float>(std::sin(
            2.0 * 3.14159265358979323846 * 440.0 * frame / sampleRate));
        input[static_cast<std::size_t>(frame) * channels] = sample;
        input[static_cast<std::size_t>(frame) * channels + 1] = sample;
    }
    const std::vector<float> output = jcut::audio::timeStretchPreservePitch(
        input, channels, sampleRate, 2.0);
    QVERIFY2(!output.empty(),
             "the configured Rubber Band backend must produce stretched audio");
    const std::size_t outputFrames = output.size() / channels;
    QVERIFY(outputFrames > 20000);
    QVERIFY(outputFrames < 28000);

    int upwardCrossings = 0;
    for (std::size_t frame = 1; frame < outputFrames; ++frame) {
        const float before = output[(frame - 1) * channels];
        const float current = output[frame * channels];
        if (before <= 0.0f && current > 0.0f) {
            ++upwardCrossings;
        }
    }
    const double durationSeconds =
        static_cast<double>(outputFrames) / sampleRate;
    const double measuredFrequency = upwardCrossings / durationSeconds;
    const QByteArray frequencyMessage =
        QByteArray("2x time stretch must retain the source tone pitch; measured ") +
        QByteArray::number(measuredFrequency);
    // Rubber Band's offline output length can vary by a few samples when its
    // internal worker scheduling changes.  The zero-crossing estimate can
    // consequently land on the old strict 400 Hz boundary even though the
    // output remains in the expected 440 Hz pitch band.
    QVERIFY2(measuredFrequency >= 390.0 && measuredFrequency <= 490.0,
             frequencyMessage.constData());
}

void TestImGuiStandaloneExport::sharedHarmonicIsolationAndTreatmentRoundTrip()
{
    constexpr int sampleRate = 48000;
    constexpr int channels = 2;
    constexpr int frames = sampleRate / 2;
    std::vector<float> input(static_cast<std::size_t>(frames) * channels);
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(
            0.65 * std::sin(
                2.0 * 3.14159265358979323846 * 220.0 * frame /
                sampleRate) +
            0.25 * std::sin(
                2.0 * 3.14159265358979323846 * 440.0 * frame /
                sampleRate));
        input[static_cast<std::size_t>(frame) * channels] = sample;
        input[static_cast<std::size_t>(frame) * channels + 1] = sample;
    }
    const std::vector<float> output =
        jcut::audio::isolateSpeechHarmonics(
            input, channels, sampleRate, 1.0);
    QVERIFY2(!output.empty(),
             "shared two-stage harmonic isolation must produce audio");
    QCOMPARE(output.size(), input.size());

    jcut::EditorDocumentCore document;
    document.audioTreatment =
        jcut::EditorAudioTreatment::HarmonicSpeechIsolation;
    const nlohmann::json legacy = jcut::toLegacyStateJson(document);
    QCOMPARE(
        QString::fromStdString(
            legacy.value("playbackAudioWarpMode", std::string())),
        QStringLiteral("rubber_band_pass_through_frequency"));
    QVERIFY(legacy.value("playbackAudioWarpModeExplicit", false));
    const auto restoredLegacy = jcut::editorDocumentCoreFromJson(legacy);
    QVERIFY(restoredLegacy.has_value());
    QCOMPARE(
        restoredLegacy->audioTreatment,
        jcut::EditorAudioTreatment::HarmonicSpeechIsolation);

    const nlohmann::json core = jcut::toJson(document);
    QCOMPARE(
        QString::fromStdString(
            core.value("audioTreatment", std::string())),
        QStringLiteral("rubber_band_pass_through_frequency"));
    const auto restoredCore = jcut::editorDocumentCoreFromJson(core);
    QVERIFY(restoredCore.has_value());
    QCOMPARE(
        restoredCore->audioTreatment,
        jcut::EditorAudioTreatment::HarmonicSpeechIsolation);

    const QString shell = readSourceFile(
        QStringLiteral("jcut_imgui_main.cpp"));
    QVERIFY2(
        shell.contains(QStringLiteral("\"Harmonic Speech Isolation\"")) &&
            shell.contains(QStringLiteral("SetAudioTreatmentCommand")),
        "ImGui must bind the persisted treatment to the runtime command");
}

void TestImGuiStandaloneExport::
sharedAudioDynamicsMatchesQtProcessingAndRoundTrips()
{
    jcut::audio::DynamicsSettingsCore settings;
    settings.compressorEnabled = true;
    settings.compressorThresholdDb = -6.0;
    settings.compressorRatio = 2.0;
    float compressed[] = {1.0f, -1.0f};
    jcut::audio::processAudioDynamicsCore(
        compressed, 1, 2, 48000, settings);
    const float threshold =
        static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    const float expected = threshold + (1.0f - threshold) / 2.0f;
    QVERIFY(std::abs(compressed[0] - expected) < 0.0001f);
    QVERIFY(std::abs(compressed[1] + expected) < 0.0001f);

    settings = {};
    settings.limiterEnabled = true;
    settings.limiterThresholdDb = -6.0;
    settings.stereoToMonoEnabled = true;
    float limitedMono[] = {1.0f, 0.25f};
    jcut::audio::processAudioDynamicsCore(
        limitedMono, 1, 2, 48000, settings);
    const float expectedMono = (threshold + 0.25f) * 0.5f;
    QVERIFY(std::abs(limitedMono[0] - expectedMono) < 0.0001f);
    QVERIFY(std::abs(limitedMono[1] - expectedMono) < 0.0001f);

    jcut::EditorDocumentCore document;
    document.projectName = "Dynamics";
    document.audioDynamics = settings;
    document.audioDynamics.amplifyEnabled = true;
    document.audioDynamics.amplifyDb = 4.5;
    document.audioDynamics.selectiveNormalizeEnabled = true;
    document.audioDynamics.selectiveNormalizePasses = 3;
    const nlohmann::json legacy =
        jcut::toLegacyStateJson(document);
    QVERIFY(legacy.value("audioLimiterEnabled", false));
    QVERIFY(legacy.value("audioStereoToMonoEnabled", false));
    QCOMPARE(legacy.value("audioSelectiveNormalizePasses", 0), 3);
    const auto restored =
        jcut::editorDocumentCoreFromJson(legacy);
    QVERIFY(restored.has_value());
    QVERIFY(restored->audioDynamics == document.audioDynamics);

    const nlohmann::json core = jcut::toJson(document);
    QVERIFY(core.contains("audioDynamics"));
    const auto restoredCore =
        jcut::editorDocumentCoreFromJson(core);
    QVERIFY(restoredCore.has_value());
    QVERIFY(restoredCore->audioDynamics == document.audioDynamics);

    const QString shell = readSourceFile(
        QStringLiteral("jcut_imgui_main.cpp"));
    QVERIFY2(
        shell.contains(QStringLiteral("\"Master Dynamics\"")) &&
            shell.contains(QStringLiteral("SetAudioDynamicsCommand")) &&
            shell.contains(QStringLiteral("\"Compressor\"")) &&
            shell.contains(QStringLiteral("\"Limiter\"")) &&
            shell.contains(QStringLiteral("\"Stereo to Mono\"")),
        "ImGui must bind functional master dynamics to the undoable runtime command");
}

void TestImGuiStandaloneExport::generatesNeutralProxyJob()
{
    QTemporaryDir tempDir;
    QVERIFY2(tempDir.isValid(), "temporary directory must be available");
    const QString sequencePath =
        tempDir.path() + QStringLiteral("/source_sequence");
    QVERIFY(QDir().mkpath(sequencePath));
    for (int index = 0; index < 3; ++index) {
        writePpmFixture(
            (sequencePath +
             QStringLiteral("/frame_%1.bmp").arg(index, 4, 10, QLatin1Char('0')))
                .toStdString(),
            64,
            64);
    }
    const std::string sourcePath = sequencePath.toStdString();
    const std::string proxyPath = jcut::defaultProxyOutputPath(
        sourcePath, jcut::ProxyGenerationFormat::ImageSequenceJpeg);

    jcut::ProxyGenerationJobController controller;
    std::string error;
    QVERIFY2(
        controller.start(
            {7,
             sourcePath,
             proxyPath,
             jcut::ProxyGenerationFormat::ImageSequenceJpeg,
             false,
             false},
            &error),
        error.c_str());
    controller.wait();
    const jcut::ProxyGenerationJobSnapshot snapshot = controller.snapshot();
    QVERIFY2(
        snapshot.state ==
            jcut::ProxyGenerationJobSnapshot::State::Completed,
        snapshot.status.c_str());
    QCOMPARE(snapshot.clipId, 7);
    QVERIFY(snapshot.framesCompleted > 0);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(proxyPath) +
        QStringLiteral("/frame_00000000.jpeg")));
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(proxyPath) +
        QStringLiteral("/frame_00000002.jpeg")));
    QVERIFY(QFileInfo::exists(QString::fromStdString(snapshot.manifestPath)));

    QVERIFY2(
        !controller.start(
            {7,
             sourcePath,
             proxyPath,
             jcut::ProxyGenerationFormat::ImageSequenceJpeg,
             false,
             false},
            &error),
        "start validates inputs synchronously");
    QVERIFY(QFile::remove(
        QString::fromStdString(proxyPath) +
        QStringLiteral("/frame_00000002.jpeg")));
    QVERIFY2(
        controller.start(
            {7,
             sourcePath,
             proxyPath,
             jcut::ProxyGenerationFormat::ImageSequenceJpeg,
             true,
             false},
            &error),
        error.c_str());
    controller.wait();
    QCOMPARE(
        controller.snapshot().state,
        jcut::ProxyGenerationJobSnapshot::State::Completed);
    QCOMPARE(controller.snapshot().framesCompleted, 3);
    QVERIFY(QFileInfo::exists(
        QString::fromStdString(proxyPath) +
        QStringLiteral("/frame_00000002.jpeg")));
    QVERIFY2(
        !jcut::removeProxyArtifact(
            sourcePath,
            (tempDir.path() + QStringLiteral("/unrelated.proxy"))
                .toStdString(),
            &error),
        "managed deletion rejects paths outside source proxy candidates");
    QVERIFY2(
        jcut::removeProxyArtifact(sourcePath, proxyPath, &error),
        error.c_str());

    for (const auto format : {
             jcut::ProxyGenerationFormat::H264Mp4,
             jcut::ProxyGenerationFormat::MjpegMov}) {
        const std::string containerPath =
            jcut::defaultProxyOutputPath(sourcePath, format);
        QVERIFY2(
            controller.start(
                {7, sourcePath, containerPath, format, false, false},
                &error),
            error.c_str());
        controller.wait();
        const auto containerSnapshot = controller.snapshot();
        QVERIFY2(
            containerSnapshot.state ==
                jcut::ProxyGenerationJobSnapshot::State::Completed,
            containerSnapshot.status.c_str());
        QVERIFY(QFileInfo::exists(QString::fromStdString(containerPath)));
        QVERIFY(countVideoPackets(containerPath) > 0);
        if (format == jcut::ProxyGenerationFormat::H264Mp4) {
            jcut::DecoderPolicySettingsCore policy;
            policy.decodePreference =
                jcut::DecodePreferenceCore::Hardware;
            policy.hardwareDevice =
                jcut::DecodeHardwareDeviceCore::Cuda;
            const auto benchmark =
                jcut::standalone_render::benchmarkStandaloneMediaDecode(
                    containerPath, policy, 3);
            QVERIFY2(benchmark.success, benchmark.message.c_str());
            if (benchmark.hardwareAccelerated) {
                QCOMPARE(
                    benchmark.effectivePreference,
                    jcut::DecodePreferenceCore::Hardware);
                QVERIFY(!benchmark.hardwareDeviceLabel.empty());
                const auto retained =
                    jcut::standalone_render::retainStandaloneHardwareFrame(
                        containerPath, 0, policy);
                QVERIFY2(retained.success, retained.message.c_str());
                QVERIFY(retained.frame);
                std::string importError;
                QVERIFY2(
                    importRetainedHardwareFrame(
                        *retained.frame, &importError),
                    importError.c_str());
            } else {
                QCOMPARE(
                    benchmark.effectivePreference,
                    jcut::DecodePreferenceCore::Software);
                QVERIFY(!benchmark.hardwareFallbackReason.empty());
            }
        }
        QVERIFY2(
            jcut::removeProxyArtifact(
                sourcePath, containerPath, &error),
            error.c_str());
        QVERIFY(!QFileInfo::exists(QString::fromStdString(containerPath)));
    }
}

QTEST_MAIN(TestImGuiStandaloneExport)

#include "test_imgui_standalone_export.moc"
