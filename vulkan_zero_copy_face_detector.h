#pragma once

#include <QSize>
#include <QString>
#include <QVector>

#include <vulkan/vulkan.h>

namespace jcut::vulkan_detector {

struct VulkanDeviceContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
};

struct VulkanExternalImage {
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    QSize size;
    bool sourceIsSrgb = false;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceWidth = 1.0f;
    float sourceHeight = 1.0f;
};

struct VulkanTensorBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize byteSize = 0;
};

struct FaceDetectorTensorSpec {
    int width = 300;
    int height = 300;
    int channels = 3;

    VkDeviceSize byteSize() const;
};

struct VulkanFaceDetection {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float confidence = 0.0f;
};

struct ScrfdTensorLayout {
    int inputWidth = 0;
    int inputHeight = 0;
    int resizedWidth = 0;
    int resizedHeight = 0;
    int padLeft = 0;
    int padTop = 0;
    float scale = 1.0f;

    VkDeviceSize byteSize() const;
};

class VulkanZeroCopyFaceDetector {
public:
    VulkanZeroCopyFaceDetector();
    ~VulkanZeroCopyFaceDetector();

    VulkanZeroCopyFaceDetector(const VulkanZeroCopyFaceDetector&) = delete;
    VulkanZeroCopyFaceDetector& operator=(const VulkanZeroCopyFaceDetector&) = delete;

    bool initialize(const VulkanDeviceContext& context, QString* errorMessage = nullptr);
    void release();

    bool isInitialized() const;
    FaceDetectorTensorSpec tensorSpec() const;

    // GPU-only preprocessing path: VkImageView -> NCHW BGR float tensor buffer.
    // This is the zero-copy boundary needed before a Vulkan-native inference stage.
    bool preprocessToTensor(const VulkanExternalImage& source,
                            const VulkanTensorBuffer& outputTensor,
                            QString* errorMessage = nullptr);

    // GPU-only SCRFD preprocessing: VkImageView -> padded NCHW RGB tensor,
    // normalized as (rgb * 255 - 127.5) / 128.0.
    bool preprocessScrfdToTensor(const VulkanExternalImage& source,
                                 const VulkanTensorBuffer& outputTensor,
                                 int targetSize,
                                 ScrfdTensorLayout* layout,
                                 QString* errorMessage = nullptr);

    bool inferFromTensor(const VulkanTensorBuffer& inputTensor,
                         const VulkanTensorBuffer& outputDetections,
                         int maxDetections,
                         float threshold,
                         QString* errorMessage = nullptr);

    QString backendId() const;

private:
    bool createDescriptorResources(QString* errorMessage);
    bool createPipeline(QString* errorMessage);
    bool createScrfdPipeline(QString* errorMessage);
    bool createInferenceDescriptorResources(QString* errorMessage);
    bool createInferencePipeline(QString* errorMessage);
    bool createCommandResources(QString* errorMessage);
    bool createSampler(QString* errorMessage);
    VkShaderModule createShaderModule(const QByteArray& spirv, const QString& name, QString* errorMessage) const;
    bool submitPreprocess(VkDescriptorSet descriptorSet,
                          const VulkanExternalImage& source,
                          QString* errorMessage);
    bool submitScrfdPreprocess(VkDescriptorSet descriptorSet,
                               const VulkanExternalImage& source,
                               const ScrfdTensorLayout& layout,
                               QString* errorMessage);
    bool submitInference(VkDescriptorSet descriptorSet,
                         const VulkanTensorBuffer& outputDetections,
                         int maxDetections,
                         float threshold,
                         QString* errorMessage);

    VulkanDeviceContext m_context;
    FaceDetectorTensorSpec m_tensorSpec;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_scrfdPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_scrfdPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_inferenceDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_inferenceDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_inferencePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_inferencePipeline = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    bool m_initialized = false;
};

} // namespace jcut::vulkan_detector
