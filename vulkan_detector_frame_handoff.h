#pragma once

#include "core/geometry.h"
#include "frame_handle.h"
#include "vulkan_zero_copy_face_detector.h"

#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>

#include <string>

namespace render_detail {
struct OffscreenVulkanFrame;
}

namespace jcut::vulkan_detector {

enum class FrameHandoffMode {
    Invalid = 0,
    CpuUpload = 1,
    HardwareDirect = 2,
    ExternalMemoryImport = 3,
};

struct HardwareInteropProbeResult {
    bool supported = false;
    QString reason;
    QString path; // "cuda", "vaapi", or ""
};

struct FrameHandoffResourceStats {
    quint64 descriptorAllocations = 0;
    quint64 descriptorFrees = 0;
    quint64 imageMemoryAllocations = 0;
    quint64 imageMemoryFrees = 0;
    quint64 stagingBufferAllocations = 0;
    quint64 stagingBufferFrees = 0;
    quint64 importedMemoryAllocations = 0;
    quint64 importedMemoryFrees = 0;
    quint64 computePipelineCreations = 0;
    quint64 cudaStreamSynchronizeCalls = 0;
    double cudaStreamSynchronizeMs = 0.0;
    quint64 cudaExternalSemaphoreSignals = 0;
    double cudaExternalSemaphoreSignalMs = 0.0;
};

class VulkanDetectorFrameHandoff final {
public:
    VulkanDetectorFrameHandoff();
    ~VulkanDetectorFrameHandoff();

    VulkanDetectorFrameHandoff(const VulkanDetectorFrameHandoff&) = delete;
    VulkanDetectorFrameHandoff& operator=(const VulkanDetectorFrameHandoff&) = delete;

    bool initialize(const VulkanDeviceContext& context, std::string* errorMessage = nullptr);
    void release();

    bool isInitialized() const { return m_initialized; }
    FrameHandoffMode lastMode() const { return m_lastMode; }
    VkImage image() const { return m_image; }
    VkImageLayout imageLayout() const { return m_imageLayout; }
    VkFormat imageFormat() const { return m_imageFormat; }
    bool usedCpuUpload() const { return m_lastMode == FrameHandoffMode::CpuUpload; }
    const HardwareInteropProbeResult& lastProbe() const { return m_lastProbe; }

    HardwareInteropProbeResult probeHardwareInterop(const editor::FrameHandle& frame) const;

    bool uploadFrame(const editor::FrameHandle& frame,
                     bool allowCpuUploadFallback,
                     double* uploadMs = nullptr,
                     std::string* errorMessage = nullptr);
    bool recordHardwareFrameUpload(VkCommandBuffer commandBuffer,
                                   const editor::FrameHandle& frame,
                                   double* uploadMs = nullptr,
                                   std::string* errorMessage = nullptr);
    bool finishPendingUpload(double* uploadMs = nullptr,
                             std::string* errorMessage = nullptr);
    bool importOffscreenFrame(const render_detail::OffscreenVulkanFrame& frame,
                              std::string* errorMessage = nullptr);
    bool recordImportedFrameCopy(VkCommandBuffer commandBuffer,
                                 const render_detail::OffscreenVulkanFrame& frame,
                                 std::string* errorMessage = nullptr);
    QString lastHardwareDirectAttemptReason() const { return m_lastHardwareDirectAttemptReason; }
    FrameHandoffResourceStats resourceStats() const { return m_resourceStats; }
    void resetResourceStats();

    VulkanExternalImage externalImage() const;

private:
    bool tryHardwareDirect(const editor::FrameHandle& frame,
                           double* uploadMs,
                           QString* errorMessage);
    bool prepareCudaHardwareFrame(const editor::FrameHandle& frame,
                                  bool* isNv12,
                                  jcut::core::SizeI* size,
                                  int* yPitch,
                                  int* uvPitch,
                                  double* uploadMs,
                                  QString* errorMessage);
    bool recordCudaHardwareFrameCopy(VkCommandBuffer commandBuffer,
                                     const jcut::core::SizeI& size,
                                     QString* errorMessage);
    bool recordNv12Conversion(VkCommandBuffer commandBuffer,
                              int width,
                              int height,
                              int yPitch,
                              int uvPitch,
                              QString* errorMessage);
    bool createNv12ConversionPipeline(QString* errorMessage);
    bool ensureCudaExportBuffer(VkDeviceSize bytes,
                                VkBuffer& buffer,
                                VkDeviceMemory& memory,
                                VkDeviceSize& size,
                                QString* errorMessage);
    bool ensureNv12ConversionResources(QString* errorMessage);
    bool ensureCudaReadySemaphore(void* cudaContext, QString* errorMessage);
    bool signalCudaReadySemaphore(void* cudaStream, QString* errorMessage);
    bool submitCommandBufferWaitingOnCuda(VkPipelineStageFlags waitStage,
                                          QString* errorMessage);
    bool convertNv12BuffersToImage(int width,
                                   int height,
                                   int yPitch,
                                   int uvPitch,
                                   QString* errorMessage);
    bool ensureImageResources(const jcut::core::SizeI& size,
                              VkFormat format,
                              QString* errorMessage);
    bool ensureImportedImageResources(const jcut::core::SizeI& size,
                                      VkFormat format,
                                      QString* errorMessage);
    bool copyImportedFrameToLocal(VkImageLayout sourceLayout,
                                  QString* errorMessage);
    bool recordImportedFrameCopyToLocal(VkCommandBuffer commandBuffer,
                                        VkImageLayout sourceLayout,
                                        QString* errorMessage);
    bool ensureStagingBuffer(VkDeviceSize bytes, QString* errorMessage);
    void transitionImage(VkCommandBuffer commandBuffer,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout);
    void transitionImage(VkImageLayout oldLayout, VkImageLayout newLayout);
    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory, quint64* freeCounter = nullptr);
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
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    jcut::core::SizeI m_imageSize;
    bool m_imageImported = false;
    VkDevice m_importSourceDevice = VK_NULL_HANDLE;
    VkDeviceMemory m_importSourceMemory = VK_NULL_HANDLE;
    VkImage m_importedImage = VK_NULL_HANDLE;
    VkDeviceMemory m_importedImageMemory = VK_NULL_HANDLE;
    VkImageView m_importedImageView = VK_NULL_HANDLE;
    VkImageLayout m_importedImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat m_importedImageFormat = VK_FORMAT_UNDEFINED;
    jcut::core::SizeI m_importedImageSize;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;

    VkBuffer m_cudaExportBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cudaExportMemory = VK_NULL_HANDLE;
    VkDeviceSize m_cudaExportSize = 0;
    VkBuffer m_cudaExportUvBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_cudaExportUvMemory = VK_NULL_HANDLE;
    VkDeviceSize m_cudaExportUvSize = 0;
    void* m_cudaExternalMemory = nullptr;
    void* m_cudaExternalUvMemory = nullptr;
    void* m_cudaExternalReadySemaphore = nullptr;
    quint64 m_cudaExternalDevicePtr = 0;
    quint64 m_cudaExternalUvDevicePtr = 0;
    void* m_cudaImportContext = nullptr;
    void* m_cudaSemaphoreImportContext = nullptr;
    VkSemaphore m_cudaReadySemaphore = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_nv12DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_nv12DescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_nv12PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_nv12Pipeline = VK_NULL_HANDLE;
    VkDescriptorSet m_pendingNv12DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet m_reusableNv12DescriptorSet = VK_NULL_HANDLE;
    bool m_uploadPending = false;
    QElapsedTimer m_pendingUploadTimer;

    FrameHandoffMode m_lastMode = FrameHandoffMode::Invalid;
    HardwareInteropProbeResult m_lastProbe;
    QString m_lastHardwareDirectAttemptReason;
    FrameHandoffResourceStats m_resourceStats;
};

} // namespace jcut::vulkan_detector
