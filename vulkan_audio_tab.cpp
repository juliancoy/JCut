#include "vulkan_audio_tab.h"
#include "vulkan_shader_paths.h"

#include <QFile>
#include <QVulkanFunctions>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr int kMaxWaveformBins = 8192;
constexpr int kMaxSpectrumBins = 2400;
constexpr int kSpectrogramHistoryColumns = 8192;
constexpr int kSpectrogramTileColumns = 64;
constexpr int kSpectrumSignalLength = 1 << 15;
constexpr VkDeviceSize kWaveformBufferBytes = kMaxWaveformBins * sizeof(float) * 2;
constexpr VkDeviceSize kSpeakerTintBytes = kMaxWaveformBins * sizeof(float) * 4;
constexpr VkDeviceSize kSpectrogramHistoryBytes =
    static_cast<VkDeviceSize>(kSpectrogramHistoryColumns) *
    static_cast<VkDeviceSize>(kMaxSpectrumBins) *
    sizeof(float);
constexpr VkDeviceSize kSpectrumSignalBytes = kSpectrumSignalLength * sizeof(float);
constexpr VkDeviceSize kSpectrumTileSignalBytes =
    static_cast<VkDeviceSize>(kSpectrogramTileColumns) *
    static_cast<VkDeviceSize>(kSpectrumSignalLength) *
    sizeof(float);
constexpr VkDeviceSize kSpectrumFloatConfigBytes = kMaxSpectrumBins * sizeof(float);
constexpr VkDeviceSize kSpectrumTileMagnitudeBytes =
    static_cast<VkDeviceSize>(kSpectrogramTileColumns) *
    static_cast<VkDeviceSize>(kMaxSpectrumBins) *
    sizeof(float);
constexpr VkDeviceSize kSpectrumIntConfigBytes = kMaxSpectrumBins * sizeof(int);
constexpr VkDeviceSize kSpectrumParamsBytes = 16 * sizeof(uint32_t);
constexpr VkDeviceSize kSpectrumPeakBytes = sizeof(uint32_t);
constexpr VkDeviceSize kSpectrumTilePeakBytes = kSpectrogramTileColumns * sizeof(uint32_t);
constexpr VkDeviceSize kSpectrumTileValidBytes = kSpectrogramTileColumns * sizeof(uint32_t);

struct Push {
    float panel[4] = {0.f, 0.f, 1.f, 1.f};
    float graph[4] = {0.f, 0.f, 1.f, 1.f};
    float params[4] = {0.f, 0.f, 0.f, 1.f};
    float flags[4] = {1.f, 0.f, 1.f, 1.f};
    float playhead[4] = {0.f, 0.f, 0.f, 0.f};
};

static_assert(sizeof(Push) == 80);

struct WaveformProcessPush {
    float gainNormalizeLimiterPeak[4] = {1.f, 1.f, 1.f, 1.f};
    float compressor[4] = {0.f, 1.f, 0.f, 0.f};
    float flags[4] = {0.f, 0.f, 0.f, 0.f};
    float bins[4] = {0.f, 0.f, 0.f, 0.f};
};

static_assert(sizeof(WaveformProcessPush) == 64);

struct SpectrumAlgorithmPush {
    float leakiness = 1.0f;
    float padding[15] = {};
};

static_assert(sizeof(SpectrumAlgorithmPush) == 64);

struct SpectrumNormalizePush {
    float params[4] = {0.f, 1.f, 0.6f, 0.05f};
    float padding[12] = {};
};

static_assert(sizeof(SpectrumNormalizePush) == 64);

struct SpectrumHistoryPush {
    float params[4] = {0.f, 0.f, 0.f, 0.f};
    float padding[12] = {};
};

static_assert(sizeof(SpectrumHistoryPush) == 64);

struct SpectrumTileNormalizePush {
    float params[8] = {0.f, 0.f, 0.f, 0.f, 1.f, 0.6f, 0.05f, 0.f};
    float padding[8] = {};
};

static_assert(sizeof(SpectrumTileNormalizePush) == 64);

struct SpectrogramHistoryNormalizePush {
    float meta[4] = {0.f, 0.f, 0.f, 0.f};
    float display[4] = {1.f, 0.6f, 0.05f, 0.f};
    float padding[8] = {};
};

static_assert(sizeof(SpectrogramHistoryNormalizePush) == 64);

float dbToLinear(qreal db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}
}

namespace jcut {

VulkanAudioTab::~VulkanAudioTab()
{
    destroy();
}

bool VulkanAudioTab::initialize(VkPhysicalDevice physicalDevice,
                                VkDevice device,
                                QVulkanDeviceFunctions* funcs,
                                VkRenderPass renderPass,
                                QString* errorMessage)
{
    destroy();
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || !funcs || renderPass == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Vulkan audio tab init received invalid handles.");
        }
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;
    m_funcs = funcs;

    m_spectrogramHistoryColumns = kSpectrogramHistoryColumns;
    std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (m_funcs->vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio descriptor set layout.");
        }
        destroy();
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(bindings.size());
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (m_funcs->vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio descriptor pool.");
        }
        destroy();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (m_funcs->vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate Vulkan audio descriptor set.");
        }
        destroy();
        return false;
    }

    if (!ensureWaveformBuffer()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to allocate Vulkan audio buffers.");
        }
        destroy();
        return false;
    }

    const QString shaderDir = jcutVulkanShaderDirectory();
    m_vertShader = createShaderModule(shaderDir + QStringLiteral("/audio_waveform.vert.spv"), errorMessage);
    m_fragShader = createShaderModule(shaderDir + QStringLiteral("/audio_waveform.frag.spv"), errorMessage);
    m_waveformComputeShader = createShaderModule(shaderDir + QStringLiteral("/audio_waveform_process.comp.spv"), errorMessage);
    m_spectrumLoiaconoShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_loiacono.comp.spv"), errorMessage);
    m_spectrumGoertzelShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_goertzel.comp.spv"), errorMessage);
    m_spectrumFftShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_fft.comp.spv"), errorMessage);
    m_spectrumNormalizeShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_normalize.comp.spv"), errorMessage);
    m_spectrumHistoryShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_history.comp.spv"), errorMessage);
    m_spectrumLoiaconoTileShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_loiacono_tile.comp.spv"), errorMessage);
    m_spectrumGoertzelTileShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_goertzel_tile.comp.spv"), errorMessage);
    m_spectrumFftTileShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_fft_tile.comp.spv"), errorMessage);
    m_spectrumTileNormalizeShader = createShaderModule(shaderDir + QStringLiteral("/audio_spectrum_tile_normalize.comp.spv"), errorMessage);
    m_spectrogramHistoryNormalizeShader =
        createShaderModule(shaderDir + QStringLiteral("/audio_spectrogram_history_normalize.comp.spv"), errorMessage);
    if (m_vertShader == VK_NULL_HANDLE ||
        m_fragShader == VK_NULL_HANDLE ||
        m_waveformComputeShader == VK_NULL_HANDLE ||
        m_spectrumLoiaconoShader == VK_NULL_HANDLE ||
        m_spectrumGoertzelShader == VK_NULL_HANDLE ||
        m_spectrumFftShader == VK_NULL_HANDLE ||
        m_spectrumNormalizeShader == VK_NULL_HANDLE ||
        m_spectrumHistoryShader == VK_NULL_HANDLE ||
        m_spectrumLoiaconoTileShader == VK_NULL_HANDLE ||
        m_spectrumGoertzelTileShader == VK_NULL_HANDLE ||
        m_spectrumFftTileShader == VK_NULL_HANDLE ||
        m_spectrumTileNormalizeShader == VK_NULL_HANDLE ||
        m_spectrogramHistoryNormalizeShader == VK_NULL_HANDLE) {
        destroy();
        return false;
    }

    VkPushConstantRange fragmentPushRange{};
    fragmentPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentPushRange.offset = 0;
    fragmentPushRange.size = sizeof(Push);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &fragmentPushRange;
    if (m_funcs->vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio graphics pipeline layout.");
        }
        destroy();
        return false;
    }

    VkPushConstantRange computePushRange{};
    computePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computePushRange.offset = 0;
    computePushRange.size = sizeof(WaveformProcessPush);
    VkPipelineLayoutCreateInfo computeLayoutInfo{};
    computeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayoutInfo.setLayoutCount = 1;
    computeLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    computeLayoutInfo.pushConstantRangeCount = 1;
    computeLayoutInfo.pPushConstantRanges = &computePushRange;
    if (m_funcs->vkCreatePipelineLayout(m_device, &computeLayoutInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio compute pipeline layout.");
        }
        destroy();
        return false;
    }

    auto createComputePipeline = [this](VkShaderModule shader, VkPipeline* pipeline) -> bool {
        VkComputePipelineCreateInfo computePipelineInfo{};
        computePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computePipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computePipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computePipelineInfo.stage.module = shader;
        computePipelineInfo.stage.pName = "main";
        computePipelineInfo.layout = m_computePipelineLayout;
        return m_funcs->vkCreateComputePipelines(
                   m_device, VK_NULL_HANDLE, 1, &computePipelineInfo, nullptr, pipeline) == VK_SUCCESS;
    };

    if (!createComputePipeline(m_waveformComputeShader, &m_waveformComputePipeline) ||
        !createComputePipeline(m_spectrumLoiaconoShader, &m_spectrumLoiaconoPipeline) ||
        !createComputePipeline(m_spectrumGoertzelShader, &m_spectrumGoertzelPipeline) ||
        !createComputePipeline(m_spectrumFftShader, &m_spectrumFftPipeline) ||
        !createComputePipeline(m_spectrumNormalizeShader, &m_spectrumNormalizePipeline) ||
        !createComputePipeline(m_spectrumHistoryShader, &m_spectrumHistoryPipeline) ||
        !createComputePipeline(m_spectrumLoiaconoTileShader, &m_spectrumLoiaconoTilePipeline) ||
        !createComputePipeline(m_spectrumGoertzelTileShader, &m_spectrumGoertzelTilePipeline) ||
        !createComputePipeline(m_spectrumFftTileShader, &m_spectrumFftTilePipeline) ||
        !createComputePipeline(m_spectrumTileNormalizeShader, &m_spectrumTileNormalizePipeline) ||
        !createComputePipeline(m_spectrogramHistoryNormalizeShader, &m_spectrogramHistoryNormalizePipeline)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio compute pipelines.");
        }
        destroy();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    if (m_funcs->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio graphics pipeline.");
        }
        destroy();
        return false;
    }

    m_ready = true;
    return true;
}

void VulkanAudioTab::destroy()
{
    if (!m_device || !m_funcs) {
        m_physicalDevice = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_funcs = nullptr;
        m_ready = false;
        return;
    }

    auto unmapIfNeeded = [this](VkDeviceMemory memory, void** mapped) {
        if (*mapped) {
            m_funcs->vkUnmapMemory(m_device, memory);
            *mapped = nullptr;
        }
    };
    auto destroyBuffer = [this](VkBuffer* buffer, VkDeviceMemory* memory) {
        if (*buffer != VK_NULL_HANDLE) {
            m_funcs->vkDestroyBuffer(m_device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
        }
        if (*memory != VK_NULL_HANDLE) {
            m_funcs->vkFreeMemory(m_device, *memory, nullptr);
            *memory = VK_NULL_HANDLE;
        }
    };

    unmapIfNeeded(m_rawWaveformMemory, &m_rawWaveformMapped);
    unmapIfNeeded(m_spectrumSignalMemory, &m_spectrumSignalMapped);
    unmapIfNeeded(m_spectrumFreqMemory, &m_spectrumFreqMapped);
    unmapIfNeeded(m_spectrumNormMemory, &m_spectrumNormMapped);
    unmapIfNeeded(m_spectrumWindowMemory, &m_spectrumWindowMapped);
    unmapIfNeeded(m_spectrumParamsMemory, &m_spectrumParamsMapped);
    unmapIfNeeded(m_spectrumPeakMemory, &m_spectrumPeakMapped);
    unmapIfNeeded(m_speakerTintMemory, &m_speakerTintMapped);
    unmapIfNeeded(m_spectrumTileSignalMemory, &m_spectrumTileSignalMapped);
    unmapIfNeeded(m_spectrumTilePeakMemory, &m_spectrumTilePeakMapped);
    unmapIfNeeded(m_spectrumTileValidMemory, &m_spectrumTileValidMapped);

    destroyBuffer(&m_rawWaveformBuffer, &m_rawWaveformMemory);
    destroyBuffer(&m_processedWaveformBuffer, &m_processedWaveformMemory);
    destroyBuffer(&m_spectrumSignalBuffer, &m_spectrumSignalMemory);
    destroyBuffer(&m_spectrumFreqBuffer, &m_spectrumFreqMemory);
    destroyBuffer(&m_spectrumNormBuffer, &m_spectrumNormMemory);
    destroyBuffer(&m_spectrumWindowBuffer, &m_spectrumWindowMemory);
    destroyBuffer(&m_spectrumParamsBuffer, &m_spectrumParamsMemory);
    destroyBuffer(&m_spectrumMagnitudeBuffer, &m_spectrumMagnitudeMemory);
    destroyBuffer(&m_spectrumPeakBuffer, &m_spectrumPeakMemory);
    destroyBuffer(&m_speakerTintBuffer, &m_speakerTintMemory);
    destroyBuffer(&m_spectrogramHistoryBuffer, &m_spectrogramHistoryMemory);
    destroyBuffer(&m_spectrumTileSignalBuffer, &m_spectrumTileSignalMemory);
    destroyBuffer(&m_spectrumTileMagnitudeBuffer, &m_spectrumTileMagnitudeMemory);
    destroyBuffer(&m_spectrumTilePeakBuffer, &m_spectrumTilePeakMemory);
    destroyBuffer(&m_spectrumTileValidBuffer, &m_spectrumTileValidMemory);

    auto destroyPipeline = [this](VkPipeline* pipeline) {
        if (*pipeline != VK_NULL_HANDLE) {
            m_funcs->vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    };
    destroyPipeline(&m_spectrumNormalizePipeline);
    destroyPipeline(&m_spectrumFftPipeline);
    destroyPipeline(&m_spectrumGoertzelPipeline);
    destroyPipeline(&m_spectrumLoiaconoPipeline);
    destroyPipeline(&m_spectrumHistoryPipeline);
    destroyPipeline(&m_spectrumTileNormalizePipeline);
    destroyPipeline(&m_spectrogramHistoryNormalizePipeline);
    destroyPipeline(&m_spectrumFftTilePipeline);
    destroyPipeline(&m_spectrumGoertzelTilePipeline);
    destroyPipeline(&m_spectrumLoiaconoTilePipeline);
    destroyPipeline(&m_waveformComputePipeline);
    destroyPipeline(&m_pipeline);

    if (m_computePipelineLayout != VK_NULL_HANDLE) {
        m_funcs->vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
        m_computePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        m_funcs->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    auto destroyShader = [this](VkShaderModule* shader) {
        if (*shader != VK_NULL_HANDLE) {
            m_funcs->vkDestroyShaderModule(m_device, *shader, nullptr);
            *shader = VK_NULL_HANDLE;
        }
    };
    destroyShader(&m_vertShader);
    destroyShader(&m_fragShader);
    destroyShader(&m_waveformComputeShader);
    destroyShader(&m_spectrumLoiaconoShader);
    destroyShader(&m_spectrumGoertzelShader);
    destroyShader(&m_spectrumFftShader);
    destroyShader(&m_spectrumNormalizeShader);
    destroyShader(&m_spectrumHistoryShader);
    destroyShader(&m_spectrumLoiaconoTileShader);
    destroyShader(&m_spectrumGoertzelTileShader);
    destroyShader(&m_spectrumFftTileShader);
    destroyShader(&m_spectrumTileNormalizeShader);
    destroyShader(&m_spectrogramHistoryNormalizeShader);

    if (m_descriptorPool != VK_NULL_HANDLE) {
        m_funcs->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        m_funcs->vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_descriptorSet = VK_NULL_HANDLE;
    m_waveformBufferSize = 0;
    m_lastRawPeak = 1.0f;
    m_lastSpectrumValidSamples = 0;
    m_lastSpectrumFftLength = 2;
    m_spectrogramHeadColumn = -1;
    m_spectrogramFilledColumns = 0;
    m_lastSpectrumTileColumns = 0;
    m_spectrogramSignature = 0;
    m_ready = false;
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

bool VulkanAudioTab::uploadWaveform(const QVector<qreal>& minValues,
                                    const QVector<qreal>& maxValues,
                                    int binCount)
{
    if (!m_ready || !m_rawWaveformMapped || minValues.isEmpty() || maxValues.isEmpty()) {
        return false;
    }
    const int safeBins = std::clamp(binCount, 1, kMaxWaveformBins);
    const int sourceLimit = std::max(1, static_cast<int>(std::min(minValues.size(), maxValues.size())));
    float* dst = static_cast<float*>(m_rawWaveformMapped);
    float peak = 0.0f;
    for (int i = 0; i < safeBins; ++i) {
        const int source = std::min(i, sourceLimit - 1);
        const float minV = static_cast<float>(std::clamp<double>(minValues[source], -1.0, 1.0));
        const float maxV = static_cast<float>(std::clamp<double>(maxValues[source], -1.0, 1.0));
        dst[i * 2] = minV;
        dst[i * 2 + 1] = maxV;
        peak = std::max(peak, std::max(std::abs(minV), std::abs(maxV)));
    }
    m_lastRawPeak = std::max(peak, 0.000001f);
    return true;
}

bool VulkanAudioTab::uploadSpectrumSignal(const std::vector<float>& signal, int validSamples)
{
    if (!m_ready || !m_spectrumSignalMapped) {
        return false;
    }
    std::memset(m_spectrumSignalMapped, 0, static_cast<size_t>(kSpectrumSignalBytes));
    const int copyCount = std::min(kSpectrumSignalLength, static_cast<int>(signal.size()));
    if (copyCount > 0) {
        std::memcpy(m_spectrumSignalMapped, signal.data(), static_cast<size_t>(copyCount) * sizeof(float));
    }
    m_lastSpectrumValidSamples = std::clamp(validSamples, 1, kSpectrumSignalLength);
    return true;
}

bool VulkanAudioTab::uploadSpectrumConfig(const std::vector<float>& freqs,
                                          const std::vector<float>& norms,
                                          const std::vector<int>& windowLengths,
                                          int fftLength)
{
    if (!m_ready ||
        !m_spectrumFreqMapped ||
        !m_spectrumNormMapped ||
        !m_spectrumWindowMapped) {
        return false;
    }
    const int safeBins = std::min({kMaxSpectrumBins,
                                   static_cast<int>(freqs.size()),
                                   static_cast<int>(norms.size()),
                                   static_cast<int>(windowLengths.size())});
    if (safeBins <= 0) {
        return false;
    }
    std::memset(m_spectrumFreqMapped, 0, static_cast<size_t>(kSpectrumFloatConfigBytes));
    std::memset(m_spectrumNormMapped, 0, static_cast<size_t>(kSpectrumFloatConfigBytes));
    std::memset(m_spectrumWindowMapped, 0, static_cast<size_t>(kSpectrumIntConfigBytes));
    std::memcpy(m_spectrumFreqMapped, freqs.data(), static_cast<size_t>(safeBins) * sizeof(float));
    std::memcpy(m_spectrumNormMapped, norms.data(), static_cast<size_t>(safeBins) * sizeof(float));
    std::memcpy(m_spectrumWindowMapped, windowLengths.data(), static_cast<size_t>(safeBins) * sizeof(int));
    m_lastSpectrumFftLength = std::max(2, fftLength);
    return true;
}

bool VulkanAudioTab::uploadSpectrumTileSignals(const std::vector<float>& signalData,
                                               const std::vector<uint32_t>& validSamples,
                                               int columnCount)
{
    if (!m_ready ||
        !m_spectrumTileSignalMapped ||
        !m_spectrumTilePeakMapped ||
        !m_spectrumTileValidMapped) {
        return false;
    }
    const int safeColumns = std::clamp(columnCount, 1, kSpectrogramTileColumns);
    const size_t signalFloats = static_cast<size_t>(safeColumns) * static_cast<size_t>(kSpectrumSignalLength);
    const size_t peakCount = static_cast<size_t>(safeColumns);
    if (signalData.size() < signalFloats || validSamples.size() < peakCount) {
        return false;
    }
    std::memset(m_spectrumTileSignalMapped, 0, static_cast<size_t>(kSpectrumTileSignalBytes));
    std::memcpy(m_spectrumTileSignalMapped, signalData.data(), signalFloats * sizeof(float));
    std::memset(m_spectrumTilePeakMapped, 0, static_cast<size_t>(kSpectrumTilePeakBytes));
    std::memset(m_spectrumTileValidMapped, 0, static_cast<size_t>(kSpectrumTileValidBytes));
    std::memcpy(m_spectrumTileValidMapped, validSamples.data(), peakCount * sizeof(uint32_t));
    m_lastSpectrumTileColumns = safeColumns;
    return true;
}

bool VulkanAudioTab::uploadSpeakerTint(const std::vector<float>& rgba, int binCount)
{
    if (!m_ready || !m_speakerTintMapped) {
        return false;
    }
    std::memset(m_speakerTintMapped, 0, static_cast<size_t>(kSpeakerTintBytes));
    const int safeBins = std::clamp(binCount, 0, kMaxWaveformBins);
    const int sourceFloatCount = std::min(static_cast<int>(rgba.size()), safeBins * 4);
    if (sourceFloatCount > 0) {
        std::memcpy(m_speakerTintMapped, rgba.data(), static_cast<size_t>(sourceFloatCount) * sizeof(float));
    }
    return true;
}

void VulkanAudioTab::resetSpectrogramHistory()
{
    if (m_rawWaveformMapped) {
        std::memset(m_rawWaveformMapped, 0, sizeof(uint32_t));
    }
    m_spectrogramHeadColumn = -1;
    m_spectrogramFilledColumns = 0;
    m_lastSpectrumTileColumns = 0;
    m_spectrogramSignature = 0;
}

void VulkanAudioTab::processWaveform(VkCommandBuffer commandBuffer,
                                     int totalBins,
                                     const PreviewSurface::AudioDynamicsSettings& settings)
{
    if (!m_ready ||
        commandBuffer == VK_NULL_HANDLE ||
        m_waveformComputePipeline == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE) {
        return;
    }

    const int safeBins = std::clamp(totalBins, 1, kMaxWaveformBins);
    WaveformProcessPush push{};
    push.gainNormalizeLimiterPeak[0] = settings.amplifyEnabled ? dbToLinear(settings.amplifyDb) : 1.0f;
    push.gainNormalizeLimiterPeak[1] = settings.normalizeEnabled
        ? (dbToLinear(settings.normalizeTargetDb) / m_lastRawPeak)
        : 1.0f;
    push.gainNormalizeLimiterPeak[2] = settings.limiterEnabled ? dbToLinear(settings.limiterThresholdDb) : 1.0f;
    push.gainNormalizeLimiterPeak[3] = settings.peakReductionEnabled ? dbToLinear(settings.peakThresholdDb) : 1.0f;
    push.compressor[0] = settings.compressorEnabled ? dbToLinear(settings.compressorThresholdDb) : 0.0f;
    push.compressor[1] = static_cast<float>(std::max<qreal>(1.0, settings.compressorRatio));
    push.flags[0] = settings.amplifyEnabled ? 1.0f : 0.0f;
    push.flags[1] = settings.normalizeEnabled ? 1.0f : 0.0f;
    push.flags[2] = settings.limiterEnabled ? 1.0f : 0.0f;
    push.flags[3] = settings.peakReductionEnabled ? 1.0f : 0.0f;
    push.bins[0] = static_cast<float>(safeBins);
    push.bins[1] = settings.compressorEnabled ? 1.0f : 0.0f;
    push.bins[2] = settings.softClipEnabled ? 1.0f : 0.0f;

    barrierHostToCompute(commandBuffer, m_rawWaveformBuffer, kWaveformBufferBytes);
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_waveformComputePipeline);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     m_computePipelineLayout,
                                     0,
                                     1,
                                     &m_descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(WaveformProcessPush),
                                &push);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>((safeBins + 127) / 128), 1, 1);
    barrierComputeToFragment(commandBuffer, m_processedWaveformBuffer, kWaveformBufferBytes);
}

void VulkanAudioTab::processSpectrum(VkCommandBuffer commandBuffer,
                                     int totalBins,
                                     const PreviewSurface::LoiaconoSpectrumSettings& settings)
{
    if (!m_ready ||
        commandBuffer == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE ||
        !m_spectrumParamsMapped ||
        !m_spectrumPeakMapped) {
        return;
    }

    const int safeBins = std::clamp(totalBins, 1, std::min(kMaxWaveformBins, kMaxSpectrumBins));
    *static_cast<uint32_t*>(m_spectrumPeakMapped) = 0u;
    std::array<uint32_t, 16> params{};
    params[0] = 0u;
    params[1] = static_cast<uint32_t>(m_lastSpectrumValidSamples);
    params[2] = static_cast<uint32_t>(std::max(2, m_lastSpectrumFftLength));
    params[3] = static_cast<uint32_t>(std::max(0, settings.temporalWeightingMode));
    params[4] = static_cast<uint32_t>(std::max(0, settings.normalizationMode));
    params[5] = static_cast<uint32_t>(safeBins);
    std::memcpy(m_spectrumParamsMapped, params.data(), sizeof(params));

    barrierHostToCompute(commandBuffer, m_spectrumSignalBuffer, kSpectrumSignalBytes);
    barrierHostToCompute(commandBuffer, m_spectrumFreqBuffer, kSpectrumFloatConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumNormBuffer, kSpectrumFloatConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumWindowBuffer, kSpectrumIntConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumParamsBuffer, kSpectrumParamsBytes);
    barrierHostToCompute(commandBuffer, m_spectrumPeakBuffer, kSpectrumPeakBytes);

    VkPipeline algorithmPipeline = m_spectrumLoiaconoPipeline;
    if (settings.algorithmMode == 1) {
        algorithmPipeline = m_spectrumFftPipeline;
    } else if (settings.algorithmMode == 2) {
        algorithmPipeline = m_spectrumGoertzelPipeline;
    }

    SpectrumAlgorithmPush algorithmPush{};
    algorithmPush.leakiness = static_cast<float>(std::clamp(settings.leakiness, 0.99, 1.0));
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, algorithmPipeline);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     m_computePipelineLayout,
                                     0,
                                     1,
                                     &m_descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrumAlgorithmPush),
                                &algorithmPush);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>(safeBins), 1, 1);

    VkBufferMemoryBarrier computeToCompute[2]{};
    computeToCompute[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToCompute[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToCompute[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    computeToCompute[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].buffer = m_spectrumMagnitudeBuffer;
    computeToCompute[0].offset = 0;
    computeToCompute[0].size = kSpectrumFloatConfigBytes;
    computeToCompute[1] = computeToCompute[0];
    computeToCompute[1].buffer = m_spectrumPeakBuffer;
    computeToCompute[1].size = kSpectrumPeakBytes;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  2,
                                  computeToCompute,
                                  0,
                                  nullptr);

    SpectrumNormalizePush normalizePush{};
    normalizePush.params[0] = static_cast<float>(safeBins);
    normalizePush.params[1] = static_cast<float>(std::max(0.1, settings.gain));
    normalizePush.params[2] = static_cast<float>(std::clamp(settings.gamma, 0.1, 2.0));
    normalizePush.params[3] = static_cast<float>(std::clamp(settings.floor, 0.0, 0.5));
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectrumNormalizePipeline);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrumNormalizePush),
                                &normalizePush);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>((safeBins + 127) / 128), 1, 1);

    VkBufferMemoryBarrier normalizeToHistory{};
    normalizeToHistory.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    normalizeToHistory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    normalizeToHistory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    normalizeToHistory.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    normalizeToHistory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    normalizeToHistory.buffer = m_processedWaveformBuffer;
    normalizeToHistory.offset = 0;
    normalizeToHistory.size = kWaveformBufferBytes;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &normalizeToHistory,
                                  0,
                                  nullptr);

    m_spectrogramHeadColumn = (m_spectrogramHeadColumn + 1) % std::max(1, m_spectrogramHistoryColumns);
    m_spectrogramFilledColumns = std::min(m_spectrogramFilledColumns + 1, m_spectrogramHistoryColumns);
    SpectrumHistoryPush historyPush{};
    historyPush.params[0] = static_cast<float>(safeBins);
    historyPush.params[1] = static_cast<float>(m_spectrogramHistoryColumns);
    historyPush.params[2] = static_cast<float>(m_spectrogramHeadColumn);
    historyPush.params[3] = 0.0f;
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectrumHistoryPipeline);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrumHistoryPush),
                                &historyPush);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>((safeBins + 127) / 128), 1, 1);
    barrierComputeToFragment(commandBuffer, m_spectrogramHistoryBuffer, kSpectrogramHistoryBytes);
}

void VulkanAudioTab::processSpectrumTile(VkCommandBuffer commandBuffer,
                                         int totalBins,
                                         int columnCount,
                                         int historyColumnStart,
                                         const PreviewSurface::LoiaconoSpectrumSettings& settings)
{
    if (!m_ready ||
        commandBuffer == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE ||
        !m_spectrumParamsMapped) {
        return;
    }

    const int safeBins = std::clamp(totalBins, 1, std::min(kMaxWaveformBins, kMaxSpectrumBins));
    const int safeColumns = std::clamp(columnCount, 1, kSpectrogramTileColumns);
    std::array<uint32_t, 16> params{};
    params[0] = 0u;
    params[2] = static_cast<uint32_t>(std::max(2, m_lastSpectrumFftLength));
    params[3] = static_cast<uint32_t>(std::max(0, settings.temporalWeightingMode));
    params[4] = static_cast<uint32_t>(std::max(0, settings.normalizationMode));
    params[5] = static_cast<uint32_t>(safeBins);
    std::memcpy(m_spectrumParamsMapped, params.data(), sizeof(params));

    barrierHostToCompute(commandBuffer, m_spectrumTileSignalBuffer, kSpectrumTileSignalBytes);
    barrierHostToCompute(commandBuffer, m_spectrumTilePeakBuffer, kSpectrumTilePeakBytes);
    barrierHostToCompute(commandBuffer, m_spectrumTileValidBuffer, kSpectrumTileValidBytes);
    barrierHostToCompute(commandBuffer, m_rawWaveformBuffer, sizeof(uint32_t));
    barrierHostToCompute(commandBuffer, m_spectrumFreqBuffer, kSpectrumFloatConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumNormBuffer, kSpectrumFloatConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumWindowBuffer, kSpectrumIntConfigBytes);
    barrierHostToCompute(commandBuffer, m_spectrumParamsBuffer, kSpectrumParamsBytes);

    VkPipeline algorithmPipeline = m_spectrumLoiaconoTilePipeline;
    if (settings.algorithmMode == 1) {
        algorithmPipeline = m_spectrumFftTilePipeline;
    } else if (settings.algorithmMode == 2) {
        algorithmPipeline = m_spectrumGoertzelTilePipeline;
    }

    SpectrumAlgorithmPush algorithmPush{};
    algorithmPush.leakiness = static_cast<float>(std::clamp(settings.leakiness, 0.99, 1.0));
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, algorithmPipeline);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     m_computePipelineLayout,
                                     0,
                                     1,
                                     &m_descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrumAlgorithmPush),
                                &algorithmPush);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>(safeBins), static_cast<uint32_t>(safeColumns), 1);

    VkBufferMemoryBarrier computeToCompute[2]{};
    computeToCompute[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToCompute[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToCompute[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    computeToCompute[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].buffer = m_spectrumTileMagnitudeBuffer;
    computeToCompute[0].offset = 0;
    computeToCompute[0].size = kSpectrumTileMagnitudeBytes;
    computeToCompute[1] = computeToCompute[0];
    computeToCompute[1].buffer = m_spectrumTilePeakBuffer;
    computeToCompute[1].size = kSpectrumTilePeakBytes;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  2,
                                  computeToCompute,
                                  0,
                                  nullptr);

    SpectrumTileNormalizePush normalizePush{};
    normalizePush.params[0] = static_cast<float>(safeBins);
    normalizePush.params[1] = static_cast<float>(safeColumns);
    normalizePush.params[2] = static_cast<float>(historyColumnStart);
    normalizePush.params[3] = static_cast<float>(m_spectrogramHistoryColumns);
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectrumTileNormalizePipeline);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrumTileNormalizePush),
                                &normalizePush);
    m_funcs->vkCmdDispatch(commandBuffer, static_cast<uint32_t>((safeBins + 127) / 128), static_cast<uint32_t>(safeColumns), 1);

    m_spectrogramHeadColumn = (historyColumnStart + safeColumns - 1) % std::max(1, m_spectrogramHistoryColumns);
    m_spectrogramFilledColumns = std::max(
        m_spectrogramFilledColumns,
        std::min(historyColumnStart + safeColumns, m_spectrogramHistoryColumns));
    barrierComputeToFragment(commandBuffer, m_spectrogramHistoryBuffer, kSpectrogramHistoryBytes);
}

void VulkanAudioTab::normalizeSpectrogramHistory(
    VkCommandBuffer commandBuffer,
    int totalBins,
    int historyColumns,
    const PreviewSurface::LoiaconoSpectrumSettings& settings)
{
    if (!m_ready ||
        commandBuffer == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE ||
        m_spectrogramHistoryNormalizePipeline == VK_NULL_HANDLE) {
        return;
    }

    const int safeBins = std::clamp(totalBins, 1, std::min(kMaxWaveformBins, kMaxSpectrumBins));
    const int safeHistoryColumns = std::clamp(historyColumns, 1, m_spectrogramHistoryColumns);

    VkBufferMemoryBarrier computeToCompute[2]{};
    computeToCompute[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToCompute[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToCompute[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    computeToCompute[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCompute[0].buffer = m_spectrogramHistoryBuffer;
    computeToCompute[0].offset = 0;
    computeToCompute[0].size = kSpectrogramHistoryBytes;
    computeToCompute[1] = computeToCompute[0];
    computeToCompute[1].buffer = m_rawWaveformBuffer;
    computeToCompute[1].size = sizeof(uint32_t);
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  2,
                                  computeToCompute,
                                  0,
                                  nullptr);

    SpectrogramHistoryNormalizePush push{};
    push.meta[0] = static_cast<float>(safeBins);
    push.meta[1] = static_cast<float>(safeHistoryColumns);
    push.display[0] = static_cast<float>(std::max(0.1, settings.gain));
    push.display[1] = static_cast<float>(std::clamp(settings.gamma, 0.1, 2.0));
    push.display[2] = static_cast<float>(std::clamp(settings.floor, 0.0, 0.5));

    m_funcs->vkCmdBindPipeline(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectrogramHistoryNormalizePipeline);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_COMPUTE,
                                     m_computePipelineLayout,
                                     0,
                                     1,
                                     &m_descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_computePipelineLayout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(SpectrogramHistoryNormalizePush),
                                &push);
    m_funcs->vkCmdDispatch(commandBuffer,
                           static_cast<uint32_t>((safeBins + 127) / 128),
                           static_cast<uint32_t>(safeHistoryColumns),
                           1);

    barrierComputeToFragment(commandBuffer, m_spectrogramHistoryBuffer, kSpectrogramHistoryBytes);
}

void VulkanAudioTab::draw(VkCommandBuffer commandBuffer,
                          const QSize& swapchainSize,
                          const QRectF& panelRect,
                          const QRectF& graphRect,
                          int rowCount,
                          int binsPerRow,
                          int totalBins,
                          qreal zoom,
                          bool waveformVisible,
                          bool selectiveNormalizeVisible,
                          qreal selectiveThreshold,
                          bool playheadVisible,
                          qreal playheadNorm,
                          int playheadRowIndex,
                          bool spectrumMode,
                          bool waveformReady) const
{
    if (!m_ready || commandBuffer == VK_NULL_HANDLE || m_descriptorSet == VK_NULL_HANDLE) {
        return;
    }
    VkBufferMemoryBarrier speakerTintBarrier{};
    speakerTintBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    speakerTintBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    speakerTintBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    speakerTintBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    speakerTintBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    speakerTintBarrier.buffer = m_speakerTintBuffer;
    speakerTintBarrier.offset = 0;
    speakerTintBarrier.size = kSpeakerTintBytes;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_HOST_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &speakerTintBarrier,
                                  0,
                                  nullptr);
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(std::max(1, swapchainSize.width()));
    viewport.height = static_cast<float>(std::max(1, swapchainSize.height()));
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    const QRect scissorRect = panelRect.normalized().toAlignedRect().intersected(
        QRect(0, 0, std::max(1, swapchainSize.width()), std::max(1, swapchainSize.height())));
    VkRect2D scissor{};
    scissor.offset = {scissorRect.x(), scissorRect.y()};
    scissor.extent = {static_cast<uint32_t>(std::max(1, scissorRect.width())),
                      static_cast<uint32_t>(std::max(1, scissorRect.height()))};

    Push push{};
    push.panel[0] = static_cast<float>(panelRect.left());
    push.panel[1] = static_cast<float>(panelRect.top());
    push.panel[2] = static_cast<float>(panelRect.width());
    push.panel[3] = static_cast<float>(panelRect.height());
    push.graph[0] = static_cast<float>(graphRect.left());
    push.graph[1] = static_cast<float>(graphRect.top());
    push.graph[2] = static_cast<float>(graphRect.width());
    push.graph[3] = static_cast<float>(graphRect.height());
    push.params[0] = static_cast<float>(std::clamp(totalBins, 1, kMaxWaveformBins));
    push.params[1] = static_cast<float>(std::max(1, binsPerRow));
    push.params[2] = static_cast<float>(std::max(1, rowCount));
    push.params[3] = static_cast<float>(std::max<qreal>(1.0, zoom)) * (spectrumMode ? -1.0f : 1.0f);
    if (spectrumMode) {
        push.flags[0] = static_cast<float>(std::max(0, m_spectrogramHeadColumn));
        push.flags[1] = static_cast<float>(std::max(0, m_spectrogramFilledColumns));
        push.flags[2] = waveformReady ? 1.0f : 0.0f;
        push.flags[3] = 0.0f;
    } else {
        push.flags[0] = waveformVisible ? 1.0f : 0.0f;
        push.flags[1] = selectiveNormalizeVisible ? 1.0f : 0.0f;
        push.flags[2] = static_cast<float>(std::clamp<double>(selectiveThreshold, 0.0, 1.0));
        push.flags[3] = waveformReady ? 1.0f : 0.0f;
    }
    push.playhead[0] = static_cast<float>(std::clamp<qreal>(playheadNorm, 0.0, 1.0));
    push.playhead[1] = playheadVisible ? 1.0f : 0.0f;
    push.playhead[2] = 1.35f;
    push.playhead[3] = static_cast<float>(std::max(0, playheadRowIndex));

    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    m_funcs->vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    m_funcs->vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     m_pipelineLayout,
                                     0,
                                     1,
                                     &m_descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_pipelineLayout,
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                0,
                                sizeof(Push),
                                &push);
    m_funcs->vkCmdDraw(commandBuffer, 4, 1, 0, 0);
}

VkShaderModule VulkanAudioTab::createShaderModule(const QString& path, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open Vulkan audio shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    const QByteArray code = file.readAll();
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<size_t>(code.size());
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.constData());
    VkShaderModule shader = VK_NULL_HANDLE;
    if (code.isEmpty() || m_funcs->vkCreateShaderModule(m_device, &createInfo, nullptr, &shader) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan audio shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    return shader;
}

uint32_t VulkanAudioTab::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanAudioTab::ensureWaveformBuffer()
{
    auto createStorageBuffer = [this](VkDeviceSize size,
                                      VkBuffer* buffer,
                                      VkDeviceMemory* memory,
                                      void** mapped) -> bool {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (m_funcs->vkCreateBuffer(m_device, &bufferInfo, nullptr, buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req{};
        m_funcs->vkGetBufferMemoryRequirements(m_device, *buffer, &req);
        const uint32_t memType = findMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memType == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memType;
        if (m_funcs->vkAllocateMemory(m_device, &alloc, nullptr, memory) != VK_SUCCESS) {
            return false;
        }
        if (m_funcs->vkBindBufferMemory(m_device, *buffer, *memory, 0) != VK_SUCCESS) {
            return false;
        }
        if (mapped &&
            m_funcs->vkMapMemory(m_device, *memory, 0, req.size, 0, mapped) != VK_SUCCESS) {
            return false;
        }
        if (mapped && *mapped) {
            std::memset(*mapped, 0, static_cast<size_t>(req.size));
        }
        m_waveformBufferSize = std::max(m_waveformBufferSize, req.size);
        return true;
    };

    if (!createStorageBuffer(kWaveformBufferBytes, &m_rawWaveformBuffer, &m_rawWaveformMemory, &m_rawWaveformMapped) ||
        !createStorageBuffer(kWaveformBufferBytes, &m_processedWaveformBuffer, &m_processedWaveformMemory, nullptr) ||
        !createStorageBuffer(kSpectrumSignalBytes, &m_spectrumSignalBuffer, &m_spectrumSignalMemory, &m_spectrumSignalMapped) ||
        !createStorageBuffer(kSpectrumFloatConfigBytes, &m_spectrumFreqBuffer, &m_spectrumFreqMemory, &m_spectrumFreqMapped) ||
        !createStorageBuffer(kSpectrumFloatConfigBytes, &m_spectrumNormBuffer, &m_spectrumNormMemory, &m_spectrumNormMapped) ||
        !createStorageBuffer(kSpectrumIntConfigBytes, &m_spectrumWindowBuffer, &m_spectrumWindowMemory, &m_spectrumWindowMapped) ||
        !createStorageBuffer(kSpectrumParamsBytes, &m_spectrumParamsBuffer, &m_spectrumParamsMemory, &m_spectrumParamsMapped) ||
        !createStorageBuffer(kSpectrumFloatConfigBytes, &m_spectrumMagnitudeBuffer, &m_spectrumMagnitudeMemory, nullptr) ||
        !createStorageBuffer(kSpectrumPeakBytes, &m_spectrumPeakBuffer, &m_spectrumPeakMemory, &m_spectrumPeakMapped) ||
        !createStorageBuffer(kSpeakerTintBytes, &m_speakerTintBuffer, &m_speakerTintMemory, &m_speakerTintMapped) ||
        !createStorageBuffer(kSpectrogramHistoryBytes, &m_spectrogramHistoryBuffer, &m_spectrogramHistoryMemory, nullptr) ||
        !createStorageBuffer(kSpectrumTileSignalBytes, &m_spectrumTileSignalBuffer, &m_spectrumTileSignalMemory, &m_spectrumTileSignalMapped) ||
        !createStorageBuffer(kSpectrumTileMagnitudeBytes, &m_spectrumTileMagnitudeBuffer, &m_spectrumTileMagnitudeMemory, nullptr) ||
        !createStorageBuffer(kSpectrumTilePeakBytes, &m_spectrumTilePeakBuffer, &m_spectrumTilePeakMemory, &m_spectrumTilePeakMapped) ||
        !createStorageBuffer(kSpectrumTileValidBytes, &m_spectrumTileValidBuffer, &m_spectrumTileValidMemory, &m_spectrumTileValidMapped)) {
        return false;
    }

    std::array<VkDescriptorBufferInfo, 15> descriptors{};
    descriptors[0] = {m_rawWaveformBuffer, 0, kWaveformBufferBytes};
    descriptors[1] = {m_processedWaveformBuffer, 0, kWaveformBufferBytes};
    descriptors[2] = {m_spectrumSignalBuffer, 0, kSpectrumSignalBytes};
    descriptors[3] = {m_spectrumFreqBuffer, 0, kSpectrumFloatConfigBytes};
    descriptors[4] = {m_spectrumNormBuffer, 0, kSpectrumFloatConfigBytes};
    descriptors[5] = {m_spectrumWindowBuffer, 0, kSpectrumIntConfigBytes};
    descriptors[6] = {m_spectrumParamsBuffer, 0, kSpectrumParamsBytes};
    descriptors[7] = {m_spectrumMagnitudeBuffer, 0, kSpectrumFloatConfigBytes};
    descriptors[8] = {m_spectrumPeakBuffer, 0, kSpectrumPeakBytes};
    descriptors[9] = {m_speakerTintBuffer, 0, kSpeakerTintBytes};
    descriptors[10] = {m_spectrogramHistoryBuffer, 0, kSpectrogramHistoryBytes};
    descriptors[11] = {m_spectrumTileSignalBuffer, 0, kSpectrumTileSignalBytes};
    descriptors[12] = {m_spectrumTileMagnitudeBuffer, 0, kSpectrumTileMagnitudeBytes};
    descriptors[13] = {m_spectrumTilePeakBuffer, 0, kSpectrumTilePeakBytes};
    descriptors[14] = {m_spectrumTileValidBuffer, 0, kSpectrumTileValidBytes};

    std::array<VkWriteDescriptorSet, 15> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &descriptors[i];
    }
    m_funcs->vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

void VulkanAudioTab::barrierHostToCompute(VkCommandBuffer commandBuffer,
                                          VkBuffer buffer,
                                          VkDeviceSize size) const
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_HOST_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &barrier,
                                  0,
                                  nullptr);
}

void VulkanAudioTab::barrierComputeToFragment(VkCommandBuffer commandBuffer,
                                              VkBuffer buffer,
                                              VkDeviceSize size) const
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    m_funcs->vkCmdPipelineBarrier(commandBuffer,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &barrier,
                                  0,
                                  nullptr);
}

} // namespace jcut
