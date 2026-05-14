#pragma once

#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <vector>

#include <vulkan/vulkan.h>

#include "preview_surface.h"

class QVulkanDeviceFunctions;

namespace jcut {

class VulkanAudioTab final {
public:
    VulkanAudioTab() = default;
    ~VulkanAudioTab();

    VulkanAudioTab(const VulkanAudioTab&) = delete;
    VulkanAudioTab& operator=(const VulkanAudioTab&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QVulkanDeviceFunctions* funcs,
                    VkRenderPass renderPass,
                    QString* errorMessage = nullptr);
    void destroy();

    bool isReady() const { return m_ready; }
    int spectrogramHistoryColumns() const { return m_spectrogramHistoryColumns; }
    void resetSpectrogramHistory();
    quint64 spectrogramSignature() const { return m_spectrogramSignature; }
    void setSpectrogramSignature(quint64 signature) { m_spectrogramSignature = signature; }
    bool uploadWaveform(const QVector<qreal>& minValues,
                        const QVector<qreal>& maxValues,
                        int binCount);
    bool uploadSpectrumSignal(const std::vector<float>& signal, int validSamples);
    bool uploadSpectrumConfig(const std::vector<float>& freqs,
                              const std::vector<float>& norms,
                              const std::vector<int>& windowLengths,
                              int fftLength);
    bool uploadSpectrumTileSignals(const std::vector<float>& signalData,
                                   const std::vector<uint32_t>& validSamples,
                                   int columnCount);
    bool uploadSpeakerTint(const std::vector<float>& rgba, int binCount);
    void processWaveform(VkCommandBuffer commandBuffer,
                         int totalBins,
                         const PreviewSurface::AudioDynamicsSettings& settings);
    void processSpectrum(VkCommandBuffer commandBuffer,
                         int totalBins,
                         const PreviewSurface::LoiaconoSpectrumSettings& settings);
    void processSpectrumTile(VkCommandBuffer commandBuffer,
                             int totalBins,
                             int columnCount,
                             int historyColumnStart,
                             const PreviewSurface::LoiaconoSpectrumSettings& settings);
    void normalizeSpectrogramHistory(VkCommandBuffer commandBuffer,
                                     int totalBins,
                                     int historyColumns,
                                     const PreviewSurface::LoiaconoSpectrumSettings& settings);
    void draw(VkCommandBuffer commandBuffer,
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
              bool waveformReady) const;

private:
    VkShaderModule createShaderModule(const QString& path, QString* errorMessage);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool ensureWaveformBuffer();
    void barrierHostToCompute(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize size) const;
    void barrierComputeToFragment(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize size) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    bool m_ready = false;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_waveformComputePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumLoiaconoPipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumGoertzelPipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumFftPipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumNormalizePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumHistoryPipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumLoiaconoTilePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumGoertzelTilePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumFftTilePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrumTileNormalizePipeline = VK_NULL_HANDLE;
    VkPipeline m_spectrogramHistoryNormalizePipeline = VK_NULL_HANDLE;
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
    VkShaderModule m_waveformComputeShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumLoiaconoShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumGoertzelShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumFftShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumNormalizeShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumHistoryShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumLoiaconoTileShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumGoertzelTileShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumFftTileShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrumTileNormalizeShader = VK_NULL_HANDLE;
    VkShaderModule m_spectrogramHistoryNormalizeShader = VK_NULL_HANDLE;
    VkBuffer m_rawWaveformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_rawWaveformMemory = VK_NULL_HANDLE;
    void* m_rawWaveformMapped = nullptr;
    VkBuffer m_processedWaveformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_processedWaveformMemory = VK_NULL_HANDLE;
    VkBuffer m_spectrumSignalBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumSignalMemory = VK_NULL_HANDLE;
    void* m_spectrumSignalMapped = nullptr;
    VkBuffer m_spectrumFreqBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumFreqMemory = VK_NULL_HANDLE;
    void* m_spectrumFreqMapped = nullptr;
    VkBuffer m_spectrumNormBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumNormMemory = VK_NULL_HANDLE;
    void* m_spectrumNormMapped = nullptr;
    VkBuffer m_spectrumWindowBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumWindowMemory = VK_NULL_HANDLE;
    void* m_spectrumWindowMapped = nullptr;
    VkBuffer m_spectrumParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumParamsMemory = VK_NULL_HANDLE;
    void* m_spectrumParamsMapped = nullptr;
    VkBuffer m_spectrumMagnitudeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumMagnitudeMemory = VK_NULL_HANDLE;
    VkBuffer m_spectrumPeakBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumPeakMemory = VK_NULL_HANDLE;
    void* m_spectrumPeakMapped = nullptr;
    VkBuffer m_speakerTintBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_speakerTintMemory = VK_NULL_HANDLE;
    void* m_speakerTintMapped = nullptr;
    VkBuffer m_spectrogramHistoryBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrogramHistoryMemory = VK_NULL_HANDLE;
    VkBuffer m_spectrumTileSignalBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumTileSignalMemory = VK_NULL_HANDLE;
    void* m_spectrumTileSignalMapped = nullptr;
    VkBuffer m_spectrumTileMagnitudeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumTileMagnitudeMemory = VK_NULL_HANDLE;
    VkBuffer m_spectrumTilePeakBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumTilePeakMemory = VK_NULL_HANDLE;
    void* m_spectrumTilePeakMapped = nullptr;
    VkBuffer m_spectrumTileValidBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_spectrumTileValidMemory = VK_NULL_HANDLE;
    void* m_spectrumTileValidMapped = nullptr;
    VkDeviceSize m_waveformBufferSize = 0;
    float m_lastRawPeak = 1.0f;
    int m_lastSpectrumValidSamples = 0;
    int m_lastSpectrumFftLength = 2;
    int m_spectrogramHistoryColumns = 1024;
    int m_spectrogramHeadColumn = -1;
    int m_spectrogramFilledColumns = 0;
    int m_lastSpectrumTileColumns = 0;
    quint64 m_spectrogramSignature = 0;
};

} // namespace jcut
