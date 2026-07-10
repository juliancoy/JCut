#pragma once

#include "cpu_overlay_render_backend.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QVector>

#include <vulkan/vulkan.h>

#include <array>

class QVulkanDeviceFunctions;

struct VulkanMaskPreprocessOptions {
    QSize outputSize;
    bool invert = false;
    int erodeRadius = 0;
    int dilateRadius = 0;
    int blurRadius = 0;
};

class VulkanResources final {
public:
    static constexpr size_t kDescriptorSetCount = 3;
    static constexpr size_t kMaskComputeDescriptorSetCount = 128;

    VulkanResources() = default;
    ~VulkanResources();

    VulkanResources(const VulkanResources&) = delete;
    VulkanResources& operator=(const VulkanResources&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QVulkanDeviceFunctions* funcs);
    void destroy();

    bool ensureCheckerTextureUploaded(VkCommandBuffer commandBuffer);
    bool setSampledImage(VkImageView imageView, VkImageLayout imageLayout);
    bool uploadImageTexture(VkCommandBuffer commandBuffer, const render_detail::OverlayImage& image);
    bool uploadImageTexture(VkCommandBuffer commandBuffer, const QImage& image);
    bool uploadMaskTexture(VkCommandBuffer commandBuffer, const QImage& image);
    bool uploadMaskTexture(VkCommandBuffer commandBuffer,
                           const QImage& image,
                           const VulkanMaskPreprocessOptions& options);
    bool ensureAuxiliaryImagesReadable(VkCommandBuffer commandBuffer);
    bool uploadCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut);
    bool uploadMaskCurveLut(VkCommandBuffer commandBuffer, const QByteArray& rgbaLut);
    bool beginFrameUploads(size_t frameSlot, size_t frameSlotCount);
    bool updateFrameUniform(const QSize& outputSize);

    VkDescriptorSetLayout descriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet descriptorSet() const { return m_descriptorSets[m_descriptorSetIndex]; }
    size_t descriptorSetIndex() const { return m_descriptorSetIndex; }
    size_t descriptorSetCount() const { return m_descriptorSets.size(); }
    bool isReady() const { return m_initialized; }

private:
    bool createTextureResources();
    bool createMaskComputeResources();
    void destroyMaskComputeResources();
    bool createTextureImage(const QSize& size);
    void destroyTextureImage();
    void retireTextureImage();
    bool ensureRawMaskImage(const QSize& size);
    bool ensureMaskImages(const QSize& size);
    bool createMaskImage(const QSize& size,
                         VkImage* image,
                         VkDeviceMemory* memory,
                         VkImageView* view);
    void destroyMaskImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view);
    void retireMaskImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view);
    VkShaderModule createShaderModule(const QString& path) const;
    bool runMaskComputePass(VkCommandBuffer commandBuffer,
                            VkPipeline pipeline,
                            const void* pushData,
                            uint32_t pushDataSize,
                            VkImageView inputView,
                            VkImageView outputView,
                            VkImage outputImage,
                            VkImageLayout& outputLayout);
    bool preprocessMaskTexture(VkCommandBuffer commandBuffer,
                               const VulkanMaskPreprocessOptions& options);
    bool ensureTextureSize(const QSize& size);
    bool ensureStagingCapacity(VkDeviceSize bytes);
    bool reserveStagingUpload(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize* offsetOut);
    bool writeStagingUpload(const void* data, VkDeviceSize bytes, VkDeviceSize* offsetOut);
    bool uploadCurveLutImage(VkCommandBuffer commandBuffer,
                             const QByteArray& rgbaLut,
                             VkImage image,
                             VkImageLayout& layout,
                             QByteArray& cachedBytes);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    void transitionTextureImage(VkCommandBuffer cb,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout,
                         VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage,
                         VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    bool m_initialized = false;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kDescriptorSetCount> m_descriptorSets{};
    size_t m_descriptorSetIndex = 0;
    VkBuffer m_frameUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_frameUniformMemory = VK_NULL_HANDLE;
    void* m_frameUniformMapped = nullptr;

    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;
    VkImageLayout m_textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_textureUploaded = false;
    QSize m_textureSize;

    VkImage m_curveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_curveLutMemory = VK_NULL_HANDLE;
    VkImageView m_curveLutView = VK_NULL_HANDLE;
    VkImageLayout m_curveLutLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QByteArray m_curveLutBytes;
    VkImage m_maskCurveLutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_maskCurveLutMemory = VK_NULL_HANDLE;
    VkImageView m_maskCurveLutView = VK_NULL_HANDLE;
    VkImageLayout m_maskCurveLutLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QByteArray m_maskCurveLutBytes;

    VkImage m_maskImage = VK_NULL_HANDLE;
    VkDeviceMemory m_maskMemory = VK_NULL_HANDLE;
    VkImageView m_maskView = VK_NULL_HANDLE;
    VkImageLayout m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage m_maskRawImage = VK_NULL_HANDLE;
    VkDeviceMemory m_maskRawMemory = VK_NULL_HANDLE;
    VkImageView m_maskRawView = VK_NULL_HANDLE;
    VkImageLayout m_maskRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage m_maskWorkImage = VK_NULL_HANDLE;
    VkDeviceMemory m_maskWorkMemory = VK_NULL_HANDLE;
    VkImageView m_maskWorkView = VK_NULL_HANDLE;
    VkImageLayout m_maskWorkLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize m_maskSize;
    QSize m_maskRawSize;
    qint64 m_uploadedMaskCacheKey = 0;
    QSize m_uploadedMaskOutputSize;
    bool m_uploadedMaskInvert = false;
    int m_uploadedMaskErodeRadius = 0;
    int m_uploadedMaskDilateRadius = 0;
    int m_uploadedMaskBlurRadius = 0;

    VkDescriptorSetLayout m_maskComputeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_maskComputeDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaskComputeDescriptorSetCount> m_maskComputeDescriptorSets{};
    size_t m_maskComputeDescriptorSetIndex = 0;
    VkPipelineLayout m_maskPreparePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_maskMorphBlurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_maskPreparePipeline = VK_NULL_HANDLE;
    VkPipeline m_maskMorphPipeline = VK_NULL_HANDLE;
    VkPipeline m_maskBlurPipeline = VK_NULL_HANDLE;
    VkShaderModule m_maskPrepareModule = VK_NULL_HANDLE;
    VkShaderModule m_maskMorphModule = VK_NULL_HANDLE;
    VkShaderModule m_maskBlurModule = VK_NULL_HANDLE;

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    struct StagingUploadRing {
        VkDeviceSize capacity = 0;
        VkDeviceSize frameSlotBytes = 0;
        VkDeviceSize writeOffset = 0;
        size_t frameSlot = 0;
        size_t frameSlotCount = kDescriptorSetCount;

        void resetAllocation()
        {
            capacity = 0;
            frameSlotBytes = 0;
            writeOffset = 0;
        }

        void reset()
        {
            resetAllocation();
            frameSlot = 0;
            frameSlotCount = kDescriptorSetCount;
        }
    };
    StagingUploadRing m_stagingRing;
    struct RetiredStagingBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    QVector<RetiredStagingBuffer> m_retiredStagingBuffers;
    struct RetiredImageResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    QVector<RetiredImageResource> m_retiredImageResources;
};
