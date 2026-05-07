#pragma once

#include "frame_handle.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QSize>
#include <QString>
#include <QtGlobal>

namespace jcut::vulkan_detector {

enum class FrameHandoffMode {
    Invalid = 0,
    CpuUpload = 1,
    HardwareDirect = 2,
};

struct HardwareInteropProbeResult {
    bool supported = false;
    QString reason;
    QString path; // "cuda", "vaapi", or ""
};

class VulkanDetectorFrameHandoff final {
public:
    VulkanDetectorFrameHandoff();
    ~VulkanDetectorFrameHandoff();

    VulkanDetectorFrameHandoff(const VulkanDetectorFrameHandoff&) = delete;
    VulkanDetectorFrameHandoff& operator=(const VulkanDetectorFrameHandoff&) = delete;

    bool initialize(const VulkanDeviceContext& context, QString* errorMessage = nullptr);
    void release();

    bool isInitialized() const { return m_initialized; }
    FrameHandoffMode lastMode() const { return m_lastMode; }
    bool usedCpuUpload() const { return m_lastMode == FrameHandoffMode::CpuUpload; }
    const HardwareInteropProbeResult& lastProbe() const { return m_lastProbe; }

    HardwareInteropProbeResult probeHardwareInterop(const editor::FrameHandle& frame) const;

    bool uploadFrame(const editor::FrameHandle& frame,
                     bool allowCpuUploadFallback,
                     double* uploadMs = nullptr,
                     QString* errorMessage = nullptr);
    QString lastHardwareDirectAttemptReason() const { return m_lastHardwareDirectAttemptReason; }

    VulkanExternalImage externalImage() const;

private:
    bool tryHardwareDirect(const editor::FrameHandle& frame,
                           double* uploadMs,
                           QString* errorMessage);
    bool ensureImageResources(const QSize& size, QString* errorMessage);
    bool ensureStagingBuffer(VkDeviceSize bytes, QString* errorMessage);
    void transitionImage(VkImageLayout oldLayout, VkImageLayout newLayout);
    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;

    VulkanDeviceContext m_context;
    bool m_initialized = false;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkImageLayout m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize m_imageSize;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;

    VkBuffer m_cudaExportBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cudaExportMemory = VK_NULL_HANDLE;
    VkDeviceSize m_cudaExportSize = 0;
    void* m_cudaExternalMemory = nullptr;
    quint64 m_cudaExternalDevicePtr = 0;
    void* m_cudaImportContext = nullptr;

    FrameHandoffMode m_lastMode = FrameHandoffMode::Invalid;
    HardwareInteropProbeResult m_lastProbe;
    QString m_lastHardwareDirectAttemptReason;
};

} // namespace jcut::vulkan_detector
