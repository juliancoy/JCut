#pragma once

#include "core/image_buffer.h"
#include "core/offscreen_vulkan_frame.h"
#include "decoder_policy_core.h"
#include "editor_document_core.h"
#include "frame_payload_core.h"

#include <memory>
#include <string>

namespace jcut::standalone_render {

struct PreviewRenderRequest {
    EditorDocumentCore document;
    core::SizeI outputSize;
    int timelineFrame = 0;
    std::string rootDirectory;
    bool preferVulkanFrame = true;
    bool allowCpuFallback = true;
    DecoderPolicySettingsCore decoderPolicy;
};

struct PreviewRenderResult {
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
    render_detail::OffscreenVulkanFrame vulkanFrame;
    std::string sourcePath;
    DecodePreferenceCore requestedDecodePreference =
        DecodePreferenceCore::Auto;
    DecodePreferenceCore effectiveDecodePreference =
        DecodePreferenceCore::Software;
    bool hardwareAccelerated = false;
    std::string hardwareDeviceLabel;
    std::string hardwareFallbackReason;
};

PreviewRenderResult renderPreviewFrame(const PreviewRenderRequest& request);

} // namespace jcut::standalone_render
