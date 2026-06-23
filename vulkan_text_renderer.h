#pragma once

#include "cpu_overlay_render_backend.h"
#include "vulkan_resources.h"

#include <QHash>
#include <QColor>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

#include <vulkan/vulkan.h>

class QVulkanDeviceFunctions;

struct VulkanTextLayoutDebug {
    bool valid = false;
    QString atlasKey;
    QString layoutKey;
    QSize atlasSize;
    int glyphAtlasEntryCount = 0;
    int glyphDrawCount = 0;
    int cardCount = 0;
    int backgroundCount = 0;
    int highlightCount = 0;
    QVector<QRectF> glyphRects;
    QVector<QColor> glyphColors;
    QVector<QRectF> cards;
    QVector<QRectF> backgrounds;
    QVector<QRectF> highlights;
};

class VulkanTextPipeline final {
public:
    struct Push {
        float mvp[16]{};
        float uvRect[4]{};
        float color[4]{};
    };

    ~VulkanTextPipeline();

    bool initialize(VkDevice device,
                    QVulkanDeviceFunctions* funcs,
                    VkRenderPass renderPass,
                    VkDescriptorSetLayout descriptorSetLayout,
                    QString* errorMessage);
    void destroy();
    bool isReady() const { return m_ready; }

    void bindAndDraw(VkCommandBuffer commandBuffer,
                     const VkViewport& viewport,
                     const VkRect2D& scissor,
                     VkDescriptorSet descriptorSet,
                     const Push& push) const;

private:
    VkShaderModule createShaderModule(const QString& path, QString* errorMessage);

    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
    bool m_ready = false;
};

class VulkanTextRenderer final {
public:
    VulkanTextRenderer() = default;
    ~VulkanTextRenderer();

    VulkanTextRenderer(const VulkanTextRenderer&) = delete;
    VulkanTextRenderer& operator=(const VulkanTextRenderer&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice,
                    VkDevice device,
                    QVulkanDeviceFunctions* funcs,
                    VkRenderPass renderPass,
                    QString* errorMessage);
    void destroy();
    bool isReady() const;

    bool drawSpeakerLabel(VkCommandBuffer commandBuffer,
                          const QSize& swapSize,
                          const QSize& outputSize,
                          const QRectF& outputTargetRect,
                          const render_detail::SpeakerLabelOverlaySpec& spec);
    bool prepareSpeakerLabelAtlas(VkCommandBuffer commandBuffer,
                                  const QSize& outputSize,
                                  const render_detail::SpeakerLabelOverlaySpec& spec);
    bool drawTranscriptOverlay(VkCommandBuffer commandBuffer,
                               const QSize& swapSize,
                               const QSize& outputSize,
                               const QRectF& outputTargetRect,
                               const TimelineClip& clip,
                               const TranscriptOverlayLayout& layout,
                               const QRectF& outputRect,
                               const QString& speakerTitle);
    bool prepareTranscriptOverlayAtlas(VkCommandBuffer commandBuffer,
                                       const QSize& outputSize,
                                       const TimelineClip& clip,
                                       const TranscriptOverlayLayout& layout,
                                       const QRectF& outputRect,
                                       const QString& speakerTitle);
    VulkanTextLayoutDebug buildSpeakerLabelLayoutForTesting(
        const QSize& outputSize,
        const render_detail::SpeakerLabelOverlaySpec& spec) const;
    VulkanTextLayoutDebug buildTranscriptOverlayLayoutForTesting(
        const QSize& outputSize,
        const TimelineClip& clip,
        const TranscriptOverlayLayout& layout,
        const QRectF& outputRect,
        const QString& speakerTitle) const;

private:
    struct Glyph {
        QRectF uv;
        QSize size;
        QPointF bearing;
        qreal advance = 0.0;
    };
    struct LaidOutGlyph {
        QRectF rect;
        QRectF uv;
        QColor color;
    };
    struct TranscriptBackground {
        QRectF rect;
        QColor color;
        qreal radius = 0.0;
    };
    struct TranscriptHighlight {
        QRectF rect;
        QColor color;
    };
    struct Atlas {
        QString key;
        render_detail::OverlayImage image;
        QHash<QString, Glyph> glyphs;
        QRectF solidUv;
    };
    struct SpeakerLayoutCache {
        bool valid = false;
        QString layoutKey;
        Atlas atlas;
        QVector<LaidOutGlyph> glyphs;
        QVector<QRectF> cards;
    };
    struct TranscriptLayoutCache {
        bool valid = false;
        QString layoutKey;
        QString atlasKey;
        Atlas atlas;
        QVector<LaidOutGlyph> glyphs;
        QVector<TranscriptBackground> backgrounds;
        QVector<TranscriptHighlight> highlights;
    };

    bool buildAtlasAndLayout(const QSize& outputSize,
                             const render_detail::SpeakerLabelOverlaySpec& spec,
                             Atlas* atlas,
                             QVector<LaidOutGlyph>* glyphs,
                             QVector<QRectF>* cards) const;
    bool buildTranscriptAtlasAndLayout(const QSize& outputSize,
                                       const TimelineClip& clip,
                                       const TranscriptOverlayLayout& layout,
                                       const QRectF& outputRect,
                                       const QString& speakerTitle,
                                       Atlas* atlas,
                                       QVector<LaidOutGlyph>* glyphs,
                                       QVector<TranscriptBackground>* backgrounds,
                                       QVector<TranscriptHighlight>* highlights) const;
    bool buildTranscriptAtlas(const TimelineClip& clip,
                              const TranscriptOverlayLayout& layout,
                              const QString& speakerTitle,
                              Atlas* atlas) const;
    bool buildTranscriptLayout(const QSize& outputSize,
                               const TimelineClip& clip,
                               const TranscriptOverlayLayout& layout,
                               const QRectF& outputRect,
                               const QString& speakerTitle,
                               const Atlas& atlas,
                               QVector<LaidOutGlyph>* glyphs,
                               QVector<TranscriptBackground>* backgrounds,
                               QVector<TranscriptHighlight>* highlights) const;
    const SpeakerLayoutCache* speakerLabelLayout(const QSize& outputSize,
                                                 const render_detail::SpeakerLabelOverlaySpec& spec) const;
    const TranscriptLayoutCache* transcriptOverlayLayout(const QSize& outputSize,
                                                        const TimelineClip& clip,
                                                        const TranscriptOverlayLayout& layout,
                                                        const QRectF& outputRect,
                                                        const QString& speakerTitle) const;
    bool ensureAtlasUploaded(VkCommandBuffer commandBuffer, const Atlas& atlas);
    void drawGlyph(VkCommandBuffer commandBuffer,
                   const QSize& swapSize,
                   const QRectF& rect,
                   const QRectF& uv,
                   const QColor& color);
    void drawSolidRoundedRect(VkCommandBuffer commandBuffer,
                              const QSize& swapSize,
                              const QRectF& rect,
                              qreal radius,
                              const QRectF& uv,
                              const QColor& color);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* m_funcs = nullptr;
    std::unique_ptr<VulkanResources> m_atlasResources;
    std::unique_ptr<VulkanTextPipeline> m_pipeline;
    QString m_uploadedAtlasKey;
    mutable SpeakerLayoutCache m_speakerLayoutCache;
    mutable TranscriptLayoutCache m_transcriptLayoutCache;
};
