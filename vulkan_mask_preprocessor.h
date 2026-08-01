#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <functional>
#include <vector>

struct VulkanMaskPreprocessOptions {
    QSize outputSize;
    QString sourceIdentity;
    QByteArray correctionStorage;
    bool invert = false;
    int erodeRadius = 0;
    int dilateRadius = 0;
    int blurRadius = 0;
};

QString vulkanMaskTextureCacheKey(
    const VulkanMaskPreprocessOptions& options,
    const QSize& outputSize);

class VulkanMaskPreprocessor final {
public:
    static constexpr std::size_t kDescriptorSetCount = 128;

    struct Images {
        VkSampler sampler = VK_NULL_HANDLE;
        QSize inputSize;
        QSize outputSize;
        VkImageView inputView = VK_NULL_HANDLE;
        VkImage outputImage = VK_NULL_HANDLE;
        VkImageView outputView = VK_NULL_HANDLE;
        VkImageLayout* outputLayout = nullptr;
        VkImage workImage = VK_NULL_HANDLE;
        VkImageView workView = VK_NULL_HANDLE;
        VkImageLayout* workLayout = nullptr;
    };

    struct StagedCorrectionStorage {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize bytes = 0;
    };

    using StageCorrectionStorage = std::function<bool(
        const QByteArray& storage,
        VkDeviceSize alignment,
        StagedCorrectionStorage* staged)>;

    VulkanMaskPreprocessor() = default;
    ~VulkanMaskPreprocessor();

    VulkanMaskPreprocessor(const VulkanMaskPreprocessor&) = delete;
    VulkanMaskPreprocessor& operator=(const VulkanMaskPreprocessor&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QString* errorMessage = nullptr);
    void destroy();

    bool record(VkCommandBuffer commandBuffer,
                const Images& images,
                const VulkanMaskPreprocessOptions& options,
                const StageCorrectionStorage& stageCorrectionStorage);

    bool isReady() const;
    VkDeviceSize correctionStorageAlignment() const {
        return m_correctionStorageAlignment;
    }

private:
    enum class PipelineKind {
        Prepare,
        Morph,
        Blur,
    };

    bool recordPass(VkCommandBuffer commandBuffer,
                    PipelineKind kind,
                    const void* pushData,
                    std::uint32_t pushDataSize,
                    const Images& images,
                    VkImageView inputView,
                    VkImage outputImage,
                    VkImageView outputView,
                    VkImageLayout* outputLayout,
                    const StagedCorrectionStorage& correctionStorage);
    VkShaderModule createShaderModule(const QString& path) const;
    void transitionImage(VkCommandBuffer commandBuffer,
                         VkImage image,
                         VkImageLayout oldLayout,
                         VkImageLayout newLayout) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDeviceSize m_correctionStorageAlignment = 16;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
    std::size_t m_descriptorSetIndex = 0;
    VkPipelineLayout m_preparePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_morphBlurPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_prepareModule = VK_NULL_HANDLE;
    VkShaderModule m_morphModule = VK_NULL_HANDLE;
    VkShaderModule m_blurModule = VK_NULL_HANDLE;
    VkPipeline m_preparePipeline = VK_NULL_HANDLE;
    VkPipeline m_morphPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline = VK_NULL_HANDLE;
};
