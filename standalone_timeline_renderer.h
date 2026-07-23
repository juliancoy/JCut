#pragma once

#include "core/image_buffer.h"
#include "decoder_policy_core.h"
#include "editor_document_core.h"
#include "frame_payload_core.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace jcut::standalone_render {

struct TimelineRenderRequest {
    EditorDocumentCore document;
    core::SizeI outputSize;
    double timelineFrame = 0.0;
    std::string rootDirectory;
    DecoderPolicySettingsCore decoderPolicy;
    bool preferHardwareFrame = false;
    bool allowCpuFallback = true;
    bool transparentBackground = false;
};

struct StandaloneDecodeBenchmarkResult {
    bool success = false;
    std::string message;
    std::string codecName;
    DecodePreferenceCore requestedPreference =
        DecodePreferenceCore::Auto;
    DecodePreferenceCore effectivePreference =
        DecodePreferenceCore::Software;
    int softwareThreadCount = 1;
    bool hardwareAccelerated = false;
    std::string hardwareDeviceLabel;
    std::string hardwareFallbackReason;
    int framesDecoded = 0;
    int failedFrames = 0;
    std::int64_t elapsedMs = 0;
    double framesPerSecond = 0.0;
};

struct StandaloneHardwareFrameResult {
    bool success = false;
    std::string message;
    std::shared_ptr<const core::FramePayloadCore> frame;
    DecodePreferenceCore effectivePreference =
        DecodePreferenceCore::Software;
    bool hardwareAccelerated = false;
    std::string hardwareDeviceLabel;
    std::string hardwareFallbackReason;
};

struct StandaloneDecodedFrameResult {
    bool success = false;
    std::string message;
    core::ImageBuffer image;
};

struct TimelineRenderResult {
    bool success = false;
    std::string message;
    core::ImageBuffer image;
    std::shared_ptr<const core::FramePayloadCore> hardwareFrame;
    bool hardwareDirectEligible = false;
    std::string hardwareDirectFallbackReason;
    bool hardwarePresentationTransformValid = false;
    EditorTransformKeyframe hardwarePresentationTransform;
    double hardwarePresentationOpacity = 1.0;
    EditorGradingKeyframe hardwarePresentationGrade;
    core::ImageBuffer hardwareOverlayImage;
    int hardwareOverlayX = 0;
    int hardwareOverlayY = 0;
    std::string sourcePath;
    DecodePreferenceCore requestedDecodePreference =
        DecodePreferenceCore::Auto;
    DecodePreferenceCore effectiveDecodePreference =
        DecodePreferenceCore::Software;
    bool hardwareAccelerated = false;
    std::string hardwareDeviceLabel;
    std::string hardwareFallbackReason;
};

struct StandaloneMediaInfo {
    bool probed = false;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreamIndex = -1;
    double videoFps = 0.0;
    std::int64_t sourceDurationFrames = 0;
    std::int64_t durationFrames = 0;
    core::SizeI frameSize;
    std::string mediaKind = "unknown";
    std::string message;
};

// Qt-free stream metadata probe used by the ImGui import and document-load
// paths. It shares the standalone renderer's FFmpeg boundary so
// command/document code can remain framework neutral.
StandaloneMediaInfo probeStandaloneMedia(const std::string& path);
StandaloneDecodeBenchmarkResult benchmarkStandaloneMediaDecode(
    const std::string& path,
    const DecoderPolicySettingsCore& policy,
    int maxFrames = 120);
StandaloneHardwareFrameResult retainStandaloneHardwareFrame(
    const std::string& path,
    int frameIndex,
    const DecoderPolicySettingsCore& policy);
class StandaloneMediaFrameDecoder {
public:
    StandaloneMediaFrameDecoder(
        std::string path,
        DecoderPolicySettingsCore policy);
    ~StandaloneMediaFrameDecoder();
    StandaloneMediaFrameDecoder(const StandaloneMediaFrameDecoder&) = delete;
    StandaloneMediaFrameDecoder& operator=(
        const StandaloneMediaFrameDecoder&) = delete;

    StandaloneDecodedFrameResult decodeFrame(
        int frameIndex,
        core::SizeI outputSize);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Qt-free raw-media decode used by analysis workflows that must inspect the
// source before timeline grading, transforms, masks, or compositing.
StandaloneDecodedFrameResult decodeStandaloneMediaFrame(
    const std::string& path,
    int frameIndex,
    core::SizeI outputSize,
    const DecoderPolicySettingsCore& policy);

// Resolves legacy/unknown stream-presence metadata once when a document is
// loaded. Missing or unprobeable sources remain unknown and can be retried on
// a later load. Returns the number of clips whose metadata was resolved.
std::size_t probeUnknownAudioPresence(EditorDocumentCore* document,
                                      const std::string& rootDirectory);

class TimelineRenderer {
public:
    TimelineRenderer();
    ~TimelineRenderer();

    TimelineRenderer(const TimelineRenderer&) = delete;
    TimelineRenderer& operator=(const TimelineRenderer&) = delete;

    TimelineRenderResult renderFrame(const TimelineRenderRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

TimelineRenderResult renderTimelineFrame(const TimelineRenderRequest& request);

} // namespace jcut::standalone_render
