#pragma once

#include "frame_payload_core.h"
#include "vulkan_external_frame_import_core.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace jcut::vulkan_import {

struct VulkanHardwareFrameImportStatus {
    bool supported = false;
    bool hardwareDirect = false;
    std::string path;
    std::string reason;
    int hardwarePixelFormat = -1;
    int softwarePixelFormat = -1;
};

struct HardwareFrameColorGradeCore {
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float shadowsR = 0.0f;
    float shadowsG = 0.0f;
    float shadowsB = 0.0f;
    float midtonesR = 0.0f;
    float midtonesG = 0.0f;
    float midtonesB = 0.0f;
    float highlightsR = 0.0f;
    float highlightsG = 0.0f;
    float highlightsB = 0.0f;
    // Four 8-bit LUT values packed as R | G<<8 | B<<16 | Luma<<24.
    // The direct shader indexes each channel independently, then applies the
    // luma mapping as a chroma-preserving scale exactly like the compositor.
    std::array<std::uint32_t, 256> curveLut{};
    bool curvesEnabled = false;
};

// Qt-free CUDA decoded-frame to Vulkan sampled-image handoff. The CUDA copy is
// device-to-device into Vulkan-exported memory; no AVFrame CPU transfer or
// host staging allocation is used.
class VulkanHardwareFrameImportCore final {
public:
    VulkanHardwareFrameImportCore();
    ~VulkanHardwareFrameImportCore();

    VulkanHardwareFrameImportCore(
        const VulkanHardwareFrameImportCore&) = delete;
    VulkanHardwareFrameImportCore& operator=(
        const VulkanHardwareFrameImportCore&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    VkQueue queue,
                    std::uint32_t queueFamilyIndex,
                    std::string shaderDirectory,
                    std::string* errorMessage = nullptr);
    void release();

    VulkanHardwareFrameImportStatus probe(
        const core::FramePayloadCore& payload) const;
    bool importFrame(const core::FramePayloadCore& payload,
                     std::string* errorMessage = nullptr);
    bool importFrame(
        const core::FramePayloadCore& payload,
        const HardwareFrameColorGradeCore& grade,
        std::string* errorMessage = nullptr);
    ExternalImage externalImage() const;
    const VulkanHardwareFrameImportStatus& lastStatus() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace jcut::vulkan_import
