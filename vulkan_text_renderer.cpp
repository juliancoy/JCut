#include "vulkan_text_renderer.h"

#include "preview_view_transform.h"
#include "vulkan_clear_helpers.h"

#include <QCryptographicHash>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QVulkanFunctions>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

namespace {

using jcut::vulkan::clearBoxOutline;
using jcut::vulkan::clearRect;
using jcut::vulkan::clearRectFromQRect;

constexpr int kAtlasSize = 2048;
constexpr int kGlyphPadding = 2;
constexpr uint32_t kTextPushSize = 96;

struct FreeTypeLibrary {
    FT_Library library = nullptr;
    FreeTypeLibrary() { FT_Init_FreeType(&library); }
    ~FreeTypeLibrary()
    {
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

FreeTypeLibrary& ftLibraryHolder()
{
    static FreeTypeLibrary holder;
    return holder;
}

struct ResolvedFontFace {
    QString path;
    int faceIndex = 0;
};

QString normalizedFontFamily(const QString& family)
{
    const QString trimmed = family.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("DejaVu Sans") : trimmed;
}

ResolvedFontFace resolveFontFaceUncached(const QString& family, bool bold)
{
    FcPattern* pattern = FcPatternCreate();
    if (!pattern) {
        return {};
    }
    const QByteArray familyUtf8 = normalizedFontFamily(family).toUtf8();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(familyUtf8.constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) {
        return {};
    }
    FcChar8* file = nullptr;
    ResolvedFontFace resolved;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
        resolved.path = QString::fromUtf8(reinterpret_cast<const char*>(file));
    }
    int faceIndex = 0;
    if (FcPatternGetInteger(match, FC_INDEX, 0, &faceIndex) == FcResultMatch) {
        resolved.faceIndex = qMax(0, faceIndex);
    }
    FcPatternDestroy(match);
    return resolved;
}

ResolvedFontFace cachedResolvedFontFace(const QString& family, bool bold)
{
    static QMutex mutex;
    static QHash<QString, ResolvedFontFace> cache;
    const QString key = normalizedFontFamily(family) + QLatin1Char('|') +
        (bold ? QLatin1String("bold") : QLatin1String("regular"));
    {
        QMutexLocker locker(&mutex);
        const auto it = cache.constFind(key);
        if (it != cache.constEnd()) {
            return it.value();
        }
    }

    const ResolvedFontFace resolved = resolveFontFaceUncached(family, bold);
    {
        QMutexLocker locker(&mutex);
        cache.insert(key, resolved);
    }
    return resolved;
}

struct FaceGuard {
    FT_Face face = nullptr;
    ~FaceGuard()
    {
        if (face) {
            FT_Done_Face(face);
        }
    }
};

bool loadFace(const QString& family, bool bold, int pixelSize, FaceGuard* guard)
{
    if (!guard || !ftLibraryHolder().library || pixelSize <= 0) {
        return false;
    }
    const ResolvedFontFace resolved = cachedResolvedFontFace(family, bold);
    if (resolved.path.isEmpty()) {
        return false;
    }
    const QByteArray pathUtf8 = resolved.path.toUtf8();
    if (FT_New_Face(ftLibraryHolder().library, pathUtf8.constData(), resolved.faceIndex, &guard->face) != 0 ||
        !guard->face) {
        return false;
    }
    if (FT_Select_Charmap(guard->face, FT_ENCODING_UNICODE) != 0 ||
        FT_Set_Pixel_Sizes(guard->face, 0, pixelSize) != 0) {
        return false;
    }
    return true;
}

QString rectKey(const QRectF& rect)
{
    return QString::number(rect.x(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.y(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.width(), 'f', 3) + QLatin1Char(',') +
        QString::number(rect.height(), 'f', 3);
}

QString speakerLabelLayoutKey(const QSize& outputSize, const render_detail::SpeakerLabelOverlaySpec& spec)
{
    const QString material =
        QStringLiteral("speaker-label-layout-v1|") +
        QString::number(outputSize.width()) + QLatin1Char('x') + QString::number(outputSize.height()) + QLatin1Char('|') +
        spec.name + QLatin1Char('|') +
        spec.organization + QLatin1Char('|') +
        QString::number(spec.showName ? 1 : 0) + QLatin1Char('|') +
        QString::number(spec.showOrganization ? 1 : 0) + QLatin1Char('|') +
        QString::number(spec.nameTextScale, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.organizationTextScale, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.nameVerticalPosition, 'f', 4) + QLatin1Char('|') +
        QString::number(spec.organizationVerticalPosition, 'f', 4) + QLatin1Char('|') +
        spec.fontFamily + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.nameColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.organizationColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.backgroundColor.rgba())) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(spec.borderColor.rgba()));
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString transcriptOverlayLayoutKey(const QSize& outputSize,
                                   const TimelineClip& clip,
                                   const TranscriptOverlayLayout& layout,
                                   const QRectF& outputRect,
                                   const QString& speakerTitle)
{
    QString layoutMaterial;
    for (const TranscriptOverlayLine& line : layout.lines) {
        layoutMaterial += line.words.join(QLatin1Char(' '));
        layoutMaterial += QLatin1Char('#');
        layoutMaterial += QString::number(line.activeWord);
        layoutMaterial += QLatin1Char('|');
    }
    const auto& overlay = clip.transcriptOverlay;
    const QString material =
        QStringLiteral("transcript-layout-v1|") +
        clip.id + QLatin1Char('|') +
        QString::number(outputSize.width()) + QLatin1Char('x') + QString::number(outputSize.height()) + QLatin1Char('|') +
        rectKey(outputRect) + QLatin1Char('|') +
        speakerTitle + QLatin1Char('|') +
        overlay.fontFamily + QLatin1Char('|') +
        QString::number(overlay.fontPointSize) + QLatin1Char('|') +
        QString::number(overlay.bold ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.showBackground ? 1 : 0) + QLatin1Char('|') +
        QString::number(overlay.showShadow ? 1 : 0) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(overlay.textColor.rgba())) + QLatin1Char('|') +
        layoutMaterial;
    return QString::fromLatin1(QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString glyphKey(uint codepoint, bool bold, int pixelSize)
{
    return QString::number(codepoint) + QLatin1Char('|') +
        (bold ? QLatin1String("b") : QLatin1String("r")) + QLatin1Char('|') +
        QString::number(pixelSize);
}

qreal measureText(FT_Face face, const QString& text)
{
    if (!face || text.isEmpty()) {
        return 0.0;
    }
    qreal width = 0.0;
    FT_UInt previous = 0;
    for (uint codepoint : text.toUcs4()) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_HAS_KERNING(face) && previous && glyphIndex) {
            FT_Vector delta{};
            FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
            width += static_cast<qreal>(delta.x) / 64.0;
        }
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) == 0) {
            width += static_cast<qreal>(face->glyph->advance.x) / 64.0;
        }
        previous = glyphIndex;
    }
    return width;
}

QStringList wrapText(FT_Face face, const QString& text, qreal maxWidth)
{
    const QString normalized = text.simplified();
    if (normalized.isEmpty()) {
        return {};
    }
    const QStringList words = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    QString current;
    for (const QString& word : words) {
        const QString candidate = current.isEmpty() ? word : current + QLatin1Char(' ') + word;
        if (!current.isEmpty() && measureText(face, candidate) > maxWidth) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }
    }
    if (!current.isEmpty()) {
        lines.push_back(current);
    }
    return lines;
}

void blendAtlasPixel(render_detail::OverlayImage* image, int x, int y, unsigned char alpha)
{
    if (!image || x < 0 || y < 0 || x >= image->width || y >= image->height) {
        return;
    }
    const int index = ((y * image->width) + x) * 4;
    image->rgbaPremultiplied[index + 0] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 1] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 2] = static_cast<char>(255);
    image->rgbaPremultiplied[index + 3] = static_cast<char>(
        std::max<int>(static_cast<unsigned char>(image->rgbaPremultiplied[index + 3]), alpha));
}

VkClearValue clearValueForColor(const QColor& color)
{
    VkClearValue value{};
    value.color.float32[0] = static_cast<float>(color.redF() * color.alphaF());
    value.color.float32[1] = static_cast<float>(color.greenF() * color.alphaF());
    value.color.float32[2] = static_cast<float>(color.blueF() * color.alphaF());
    value.color.float32[3] = static_cast<float>(color.alphaF());
    return value;
}

void mvpForScreenRect(const QRectF& rect, const QSize& swapSize, float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, swapSize.width()));
    const float fullH = static_cast<float>(std::max(1, swapSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, rect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, rect.height())) * 0.5f;
    const float cx = static_cast<float>(rect.center().x());
    const float cy = static_cast<float>(rect.center().y());
    const float m[16] = {
        (2.0f * halfW) / fullW, 0.f, 0.f, 0.f,
        0.f, (2.0f * halfH) / fullH, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        (2.0f * cx / fullW) - 1.0f,
        (2.0f * cy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
}

} // namespace

VulkanTextPipeline::~VulkanTextPipeline()
{
    destroy();
}

bool VulkanTextPipeline::initialize(VkDevice device,
                                    QVulkanDeviceFunctions* funcs,
                                    VkRenderPass renderPass,
                                    VkDescriptorSetLayout descriptorSetLayout,
                                    QString* errorMessage)
{
    destroy();
    if (!device || !funcs || renderPass == VK_NULL_HANDLE || descriptorSetLayout == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Vulkan text pipeline init received invalid handles.");
        }
        return false;
    }
    m_device = device;
    m_funcs = funcs;

    const QString shaderDir = QStringLiteral(JCUT_VULKAN_SHADER_DIR);
    m_vertShader = createShaderModule(shaderDir + QStringLiteral("/text.vert.spv"), errorMessage);
    m_fragShader = createShaderModule(shaderDir + QStringLiteral("/text.frag.spv"), errorMessage);
    if (m_vertShader == VK_NULL_HANDLE || m_fragShader == VK_NULL_HANDLE) {
        destroy();
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = kTextPushSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (m_funcs->vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text pipeline layout.");
        }
        destroy();
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_vertShader;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_fragShader;
    shaderStages[1].pName = "main";

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
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (m_funcs->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text graphics pipeline.");
        }
        destroy();
        return false;
    }

    m_ready = true;
    return true;
}

void VulkanTextPipeline::destroy()
{
    if (!m_device || !m_funcs) {
        m_device = VK_NULL_HANDLE;
        m_funcs = nullptr;
        m_ready = false;
        return;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        m_funcs->vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        m_funcs->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vertShader != VK_NULL_HANDLE) {
        m_funcs->vkDestroyShaderModule(m_device, m_vertShader, nullptr);
        m_vertShader = VK_NULL_HANDLE;
    }
    if (m_fragShader != VK_NULL_HANDLE) {
        m_funcs->vkDestroyShaderModule(m_device, m_fragShader, nullptr);
        m_fragShader = VK_NULL_HANDLE;
    }
    m_ready = false;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

VkShaderModule VulkanTextPipeline::createShaderModule(const QString& path, QString* errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    const QByteArray code = file.readAll();
    if (code.isEmpty() || (code.size() % 4) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<size_t>(code.size());
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    if (m_funcs->vkCreateShaderModule(m_device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create Vulkan text shader module: %1").arg(path);
        }
        return VK_NULL_HANDLE;
    }
    return module;
}

void VulkanTextPipeline::bindAndDraw(VkCommandBuffer commandBuffer,
                                     const VkViewport& viewport,
                                     const VkRect2D& scissor,
                                     VkDescriptorSet descriptorSet,
                                     const Push& push) const
{
    if (!m_ready || commandBuffer == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE) {
        return;
    }
    m_funcs->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    m_funcs->vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    m_funcs->vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    m_funcs->vkCmdBindDescriptorSets(commandBuffer,
                                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     m_pipelineLayout,
                                     0,
                                     1,
                                     &descriptorSet,
                                     0,
                                     nullptr);
    m_funcs->vkCmdPushConstants(commandBuffer,
                                m_pipelineLayout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0,
                                sizeof(Push),
                                &push);
    m_funcs->vkCmdDraw(commandBuffer, 4, 1, 0, 0);
}

VulkanTextRenderer::~VulkanTextRenderer()
{
    destroy();
}

bool VulkanTextRenderer::initialize(VkPhysicalDevice physicalDevice,
                                    VkDevice device,
                                    QVulkanDeviceFunctions* funcs,
                                    VkRenderPass renderPass,
                                    QString* errorMessage)
{
    destroy();
    if (!physicalDevice || !device || !funcs || renderPass == VK_NULL_HANDLE) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid Vulkan text renderer initialization handles.");
        }
        return false;
    }
    m_physicalDevice = physicalDevice;
    m_device = device;
    m_funcs = funcs;
    m_atlasResources = std::make_unique<VulkanResources>();
    if (!m_atlasResources->initialize(physicalDevice, device, funcs)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to initialize Vulkan text atlas resources.");
        }
        destroy();
        return false;
    }
    m_pipeline = std::make_unique<VulkanTextPipeline>();
    if (!m_pipeline->initialize(device, funcs, renderPass, m_atlasResources->descriptorSetLayout(), errorMessage)) {
        destroy();
        return false;
    }
    return true;
}

void VulkanTextRenderer::destroy()
{
    m_pipeline.reset();
    m_atlasResources.reset();
    m_uploadedAtlasKey.clear();
    m_speakerLayoutCache = SpeakerLayoutCache{};
    m_transcriptLayoutCache = TranscriptLayoutCache{};
    m_physicalDevice = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_funcs = nullptr;
}

bool VulkanTextRenderer::isReady() const
{
    return m_funcs && m_atlasResources && m_atlasResources->isReady() && m_pipeline && m_pipeline->isReady();
}

VulkanTextLayoutDebug VulkanTextRenderer::buildSpeakerLabelLayoutForTesting(
    const QSize& outputSize,
    const render_detail::SpeakerLabelOverlaySpec& spec) const
{
    Atlas atlas;
    QVector<LaidOutGlyph> glyphs;
    QVector<QRectF> cards;
    VulkanTextLayoutDebug debug;
    debug.valid = buildAtlasAndLayout(outputSize, spec, &atlas, &glyphs, &cards);
    debug.atlasKey = atlas.key;
    debug.atlasSize = QSize(atlas.image.width, atlas.image.height);
    debug.glyphAtlasEntryCount = atlas.glyphs.size();
    debug.glyphDrawCount = glyphs.size();
    debug.cardCount = cards.size();
    debug.cards = cards;
    debug.glyphRects.reserve(glyphs.size());
    debug.glyphColors.reserve(glyphs.size());
    for (const LaidOutGlyph& glyph : glyphs) {
        debug.glyphRects.push_back(glyph.rect);
        debug.glyphColors.push_back(glyph.color);
    }
    return debug;
}

VulkanTextLayoutDebug VulkanTextRenderer::buildTranscriptOverlayLayoutForTesting(
    const QSize& outputSize,
    const TimelineClip& clip,
    const TranscriptOverlayLayout& layout,
    const QRectF& outputRect,
    const QString& speakerTitle) const
{
    Atlas atlas;
    QVector<LaidOutGlyph> glyphs;
    QVector<QRectF> backgrounds;
    QVector<QRectF> highlights;
    VulkanTextLayoutDebug debug;
    debug.valid = buildTranscriptAtlasAndLayout(
        outputSize, clip, layout, outputRect, speakerTitle, &atlas, &glyphs, &backgrounds, &highlights);
    debug.atlasKey = atlas.key;
    debug.atlasSize = QSize(atlas.image.width, atlas.image.height);
    debug.glyphAtlasEntryCount = atlas.glyphs.size();
    debug.glyphDrawCount = glyphs.size();
    debug.backgroundCount = backgrounds.size();
    debug.highlightCount = highlights.size();
    debug.backgrounds = backgrounds;
    debug.highlights = highlights;
    debug.glyphRects.reserve(glyphs.size());
    debug.glyphColors.reserve(glyphs.size());
    for (const LaidOutGlyph& glyph : glyphs) {
        debug.glyphRects.push_back(glyph.rect);
        debug.glyphColors.push_back(glyph.color);
    }
    return debug;
}

bool VulkanTextRenderer::buildAtlasAndLayout(const QSize& outputSize,
                                             const render_detail::SpeakerLabelOverlaySpec& spec,
                                             Atlas* atlas,
                                             QVector<LaidOutGlyph>* glyphs,
                                             QVector<QRectF>* cards) const
{
    if (!atlas || !glyphs || !cards || !outputSize.isValid()) {
        return false;
    }
    const QString name = spec.name.trimmed();
    const QString organization = spec.organization.trimmed();
    if ((spec.showName && name.isEmpty()) && (spec.showOrganization && organization.isEmpty())) {
        return false;
    }

    const qreal base = qMax<qreal>(1.0, qMin(outputSize.width(), outputSize.height()));
    const int namePixelSize = qBound(8, static_cast<int>(std::round(base * 0.042 * qBound<qreal>(0.25, spec.nameTextScale, 3.0))), 160);
    const int orgPixelSize = qBound(8, static_cast<int>(std::round(base * 0.030 * qBound<qreal>(0.25, spec.organizationTextScale, 3.0))), 140);

    FaceGuard nameFace;
    FaceGuard orgFace;
    if (!loadFace(spec.fontFamily, true, namePixelSize, &nameFace) ||
        !loadFace(spec.fontFamily, false, orgPixelSize, &orgFace)) {
        return false;
    }

    struct Line {
        QString text;
        bool name = false;
    };
    QVector<Line> nameLines;
    QVector<Line> orgLines;
    const qreal maxTextWidth = qMax<qreal>(180.0, outputSize.width() * 0.72);
    if (spec.showName && !name.isEmpty()) {
        for (const QString& line : wrapText(nameFace.face, name, maxTextWidth)) {
            nameLines.push_back(Line{line, true});
        }
    }
    if (spec.showOrganization && !organization.isEmpty()) {
        for (const QString& line : wrapText(orgFace.face, organization, maxTextWidth)) {
            orgLines.push_back(Line{line, false});
        }
    }
    if (nameLines.isEmpty() && orgLines.isEmpty()) {
        return false;
    }

    atlas->image.width = kAtlasSize;
    atlas->image.height = kAtlasSize;
    atlas->image.rgbaPremultiplied = QByteArray(kAtlasSize * kAtlasSize * 4, char(0));
    int penX = kGlyphPadding;
    int penY = kGlyphPadding;
    int rowHeight = 0;

    auto addGlyph = [&](FT_Face face, uint codepoint, bool bold, int pixelSize) -> bool {
        const QString key = glyphKey(codepoint, bold, pixelSize);
        if (atlas->glyphs.contains(key)) {
            return true;
        }
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            return false;
        }
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        if (penX + width + kGlyphPadding >= kAtlasSize) {
            penX = kGlyphPadding;
            penY += rowHeight + kGlyphPadding;
            rowHeight = 0;
        }
        if (penY + height + kGlyphPadding >= kAtlasSize) {
            return false;
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const unsigned char alpha = bitmap.buffer[row * bitmap.pitch + col];
                blendAtlasPixel(&atlas->image, penX + col, penY + row, alpha);
            }
        }
        Glyph glyph;
        glyph.uv = QRectF(static_cast<qreal>(penX) / kAtlasSize,
                          static_cast<qreal>(penY) / kAtlasSize,
                          static_cast<qreal>(width) / kAtlasSize,
                          static_cast<qreal>(height) / kAtlasSize);
        glyph.size = QSize(width, height);
        glyph.bearing = QPointF(slot->bitmap_left, slot->bitmap_top);
        glyph.advance = static_cast<qreal>(slot->advance.x) / 64.0;
        atlas->glyphs.insert(key, glyph);
        penX += width + kGlyphPadding;
        rowHeight = std::max(rowHeight, height);
        return true;
    };

    auto addLineGlyphs = [&](const QVector<Line>& lines) -> bool {
        for (const Line& line : lines) {
            for (uint codepoint : line.text.toUcs4()) {
                FT_Face face = line.name ? nameFace.face : orgFace.face;
                if (!addGlyph(face, codepoint, line.name, line.name ? namePixelSize : orgPixelSize)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!addLineGlyphs(nameLines) || !addLineGlyphs(orgLines)) {
        return false;
    }

    auto lineHeight = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.height) / 64.0;
    };
    auto ascender = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.ascender) / 64.0;
    };

    const qreal paddingX = qMax<qreal>(18.0, base * 0.028);
    const qreal paddingY = qMax<qreal>(10.0, base * 0.018);
    const qreal lineGap = qMax<qreal>(4.0, base * 0.008);
    const auto layoutBlock = [&](const QVector<Line>& lines, qreal verticalPosition) {
        if (lines.isEmpty()) {
            return;
        }
        qreal contentWidth = 0.0;
        qreal contentHeight = 0.0;
        for (int i = 0; i < lines.size(); ++i) {
            FT_Face face = lines.at(i).name ? nameFace.face : orgFace.face;
            contentWidth = qMax(contentWidth, measureText(face, lines.at(i).text));
            contentHeight += lineHeight(face);
            if (i + 1 < lines.size()) {
                contentHeight += lineGap;
            }
        }
        const qreal cardWidth = qMin<qreal>(outputSize.width() - 32.0,
                                            qMax<qreal>(220.0, contentWidth + paddingX * 2.0));
        const qreal cardHeight = contentHeight + paddingY * 2.0;
        const qreal centerY = qBound<qreal>(cardHeight * 0.5,
                                            outputSize.height() * qBound<qreal>(0.0, verticalPosition, 1.0),
                                            outputSize.height() - cardHeight * 0.5);
        const QRectF cardRect((outputSize.width() - cardWidth) * 0.5,
                              centerY - cardHeight * 0.5,
                              cardWidth,
                              cardHeight);
        cards->push_back(cardRect);
        qreal y = cardRect.top() + paddingY;
        for (const Line& line : lines) {
            FT_Face face = line.name ? nameFace.face : orgFace.face;
            const int pixelSize = line.name ? namePixelSize : orgPixelSize;
            const qreal width = measureText(face, line.text);
            qreal cursor = cardRect.left() + qMax<qreal>(paddingX, (cardRect.width() - width) * 0.5);
            const qreal baseline = y + ascender(face);
            FT_UInt previous = 0;
            for (uint codepoint : line.text.toUcs4()) {
                const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
                if (FT_HAS_KERNING(face) && previous && glyphIndex) {
                    FT_Vector delta{};
                    FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
                    cursor += static_cast<qreal>(delta.x) / 64.0;
                }
                const QString key = glyphKey(codepoint, line.name, pixelSize);
                const Glyph glyph = atlas->glyphs.value(key);
                const QRectF glyphRect(cursor + glyph.bearing.x(),
                                       baseline - glyph.bearing.y(),
                                       glyph.size.width(),
                                       glyph.size.height());
                if (!glyphRect.isEmpty()) {
                    glyphs->push_back(LaidOutGlyph{glyphRect.translated(2.0, 2.0), glyph.uv, QColor(0, 0, 0, 190)});
                    glyphs->push_back(LaidOutGlyph{glyphRect, glyph.uv, line.name ? spec.nameColor : spec.organizationColor});
                }
                cursor += glyph.advance;
                previous = glyphIndex;
            }
            y += lineHeight(face) + lineGap;
        }
    };

    layoutBlock(nameLines, spec.nameVerticalPosition);
    layoutBlock(orgLines, spec.organizationVerticalPosition);

    const QString keyMaterial =
        QStringLiteral("speaker-label-vktext|") + spec.fontFamily + QLatin1Char('|') +
        QString::number(namePixelSize) + QLatin1Char('|') +
        QString::number(orgPixelSize) + QLatin1Char('|') +
        name + QLatin1Char('|') + organization + QLatin1Char('|') +
        QString::number(spec.nameTextScale, 'f', 3) + QLatin1Char('|') +
        QString::number(spec.organizationTextScale, 'f', 3);
    atlas->key = QString::fromLatin1(QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    return !glyphs->isEmpty();
}

bool VulkanTextRenderer::buildTranscriptAtlasAndLayout(const QSize& outputSize,
                                                       const TimelineClip& clip,
                                                       const TranscriptOverlayLayout& layout,
                                                       const QRectF& outputRect,
                                                       const QString& speakerTitle,
                                                       Atlas* atlas,
                                                       QVector<LaidOutGlyph>* glyphs,
                                                       QVector<QRectF>* backgrounds,
                                                       QVector<QRectF>* highlights) const
{
    if (!atlas || !glyphs || !backgrounds || !highlights ||
        !outputSize.isValid() || layout.lines.isEmpty() || outputRect.isEmpty()) {
        return false;
    }
    const int bodyPixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize)));
    const int titlePixelSize = qMax(1, static_cast<int>(std::round(clip.transcriptOverlay.fontPointSize * 0.62)));
    FaceGuard bodyFace;
    if (!loadFace(clip.transcriptOverlay.fontFamily,
                  clip.transcriptOverlay.bold,
                  bodyPixelSize,
                  &bodyFace)) {
        return false;
    }
    FaceGuard titleFace;
    const bool hasTitle = !speakerTitle.trimmed().isEmpty() &&
        loadFace(clip.transcriptOverlay.fontFamily, true, titlePixelSize, &titleFace);

    atlas->image.width = kAtlasSize;
    atlas->image.height = kAtlasSize;
    atlas->image.rgbaPremultiplied = QByteArray(kAtlasSize * kAtlasSize * 4, char(0));
    int penX = kGlyphPadding;
    int penY = kGlyphPadding;
    int rowHeight = 0;

    auto addGlyph = [&](FT_Face face, uint codepoint, bool bold, int pixelSize) -> bool {
        const QString key = glyphKey(codepoint, bold, pixelSize);
        if (atlas->glyphs.contains(key)) {
            return true;
        }
        const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            return false;
        }
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        const int width = static_cast<int>(bitmap.width);
        const int height = static_cast<int>(bitmap.rows);
        if (penX + width + kGlyphPadding >= kAtlasSize) {
            penX = kGlyphPadding;
            penY += rowHeight + kGlyphPadding;
            rowHeight = 0;
        }
        if (penY + height + kGlyphPadding >= kAtlasSize) {
            return false;
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const unsigned char alpha = bitmap.buffer[row * bitmap.pitch + col];
                blendAtlasPixel(&atlas->image, penX + col, penY + row, alpha);
            }
        }
        Glyph glyph;
        glyph.uv = QRectF(static_cast<qreal>(penX) / kAtlasSize,
                          static_cast<qreal>(penY) / kAtlasSize,
                          static_cast<qreal>(width) / kAtlasSize,
                          static_cast<qreal>(height) / kAtlasSize);
        glyph.size = QSize(width, height);
        glyph.bearing = QPointF(slot->bitmap_left, slot->bitmap_top);
        glyph.advance = static_cast<qreal>(slot->advance.x) / 64.0;
        atlas->glyphs.insert(key, glyph);
        penX += width + kGlyphPadding;
        rowHeight = std::max(rowHeight, height);
        return true;
    };

    if (hasTitle) {
        for (uint codepoint : speakerTitle.toUcs4()) {
            if (!addGlyph(titleFace.face, codepoint, true, titlePixelSize)) {
                return false;
            }
        }
    }
    for (const TranscriptOverlayLine& line : layout.lines) {
        for (const QString& word : line.words) {
            for (uint codepoint : word.toUcs4()) {
                if (!addGlyph(bodyFace.face, codepoint, clip.transcriptOverlay.bold, bodyPixelSize)) {
                    return false;
                }
            }
        }
    }

    auto lineHeight = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.height) / 64.0;
    };
    auto ascender = [](FT_Face face) {
        return static_cast<qreal>(face->size->metrics.ascender) / 64.0;
    };
    const QRectF textBounds = outputRect.adjusted(18.0, 14.0, -18.0, -14.0);
    if (textBounds.isEmpty()) {
        return false;
    }
    if (clip.transcriptOverlay.showBackground) {
        backgrounds->push_back(outputRect);
    }

    QVector<qreal> lineWidths;
    lineWidths.reserve(layout.lines.size());
    qreal contentWidth = 0.0;
    for (const TranscriptOverlayLine& line : layout.lines) {
        qreal width = 0.0;
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            width += measureText(bodyFace.face, line.words.at(wordIndex));
            if (wordIndex + 1 < line.words.size()) {
                width += measureText(bodyFace.face, QStringLiteral(" "));
            }
        }
        lineWidths.push_back(width);
        contentWidth = qMax(contentWidth, width);
    }
    const qreal titleWidth = hasTitle ? measureText(titleFace.face, speakerTitle) : 0.0;
    contentWidth = qMax(contentWidth, titleWidth);
    const qreal titleGap = hasTitle ? clip.transcriptOverlay.fontPointSize * 0.30 : 0.0;
    const qreal contentHeight =
        (hasTitle ? lineHeight(titleFace.face) + titleGap : 0.0) +
        (lineHeight(bodyFace.face) * layout.lines.size());
    const qreal docScale = qMin(contentWidth > textBounds.width() ? textBounds.width() / contentWidth : 1.0,
                                contentHeight > textBounds.height() ? textBounds.height() / contentHeight : 1.0);
    qreal cursorY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - (contentHeight * docScale)) / 2.0);
    const qreal shadowOffset = 5.0 * docScale;

    auto emitRun = [&](FT_Face face,
                       const QString& text,
                       qreal x,
                       qreal baseline,
                       bool bold,
                       int pixelSize,
                       const QColor& color) {
        qreal cursor = x;
        FT_UInt previous = 0;
        for (uint codepoint : text.toUcs4()) {
            const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
            if (FT_HAS_KERNING(face) && previous && glyphIndex) {
                FT_Vector delta{};
                FT_Get_Kerning(face, previous, glyphIndex, FT_KERNING_DEFAULT, &delta);
                cursor += (static_cast<qreal>(delta.x) / 64.0) * docScale;
            }
            const Glyph glyph = atlas->glyphs.value(glyphKey(codepoint, bold, pixelSize));
            const QRectF glyphRect(cursor + (glyph.bearing.x() * docScale),
                                   baseline - (glyph.bearing.y() * docScale),
                                   glyph.size.width() * docScale,
                                   glyph.size.height() * docScale);
            if (!glyphRect.isEmpty()) {
                glyphs->push_back(LaidOutGlyph{glyphRect, glyph.uv, color});
            }
            cursor += glyph.advance * docScale;
            previous = glyphIndex;
        }
    };

    const QColor textColor = clip.transcriptOverlay.textColor.isValid()
        ? clip.transcriptOverlay.textColor
        : QColor(Qt::white);
    const QColor shadowColor(0, 0, 0, 200);
    const QColor highlightFillColor(QStringLiteral("#fff2a8"));
    const QColor highlightTextColor(QStringLiteral("#181818"));

    if (hasTitle) {
        const qreal x = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - (titleWidth * docScale)) / 2.0);
        const qreal baseline = cursorY + ascender(titleFace.face) * docScale;
        if (clip.transcriptOverlay.showShadow) {
            emitRun(titleFace.face, speakerTitle, x + shadowOffset, baseline + shadowOffset, true, titlePixelSize, shadowColor);
        }
        emitRun(titleFace.face, speakerTitle, x, baseline, true, titlePixelSize, textColor);
        cursorY += (lineHeight(titleFace.face) + titleGap) * docScale;
    }

    const qreal spaceWidth = measureText(bodyFace.face, QStringLiteral(" ")) * docScale;
    const qreal bodyLineHeight = lineHeight(bodyFace.face) * docScale;
    const qreal bodyAscender = ascender(bodyFace.face) * docScale;
    const qreal bodyTextHeight =
        (static_cast<qreal>(bodyFace.face->size->metrics.height) / 64.0) * docScale;
    for (int lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const TranscriptOverlayLine& line = layout.lines.at(lineIndex);
        qreal cursorX = textBounds.left() + qMax<qreal>(0.0, (textBounds.width() - (lineWidths.at(lineIndex) * docScale)) / 2.0);
        const qreal baseline = cursorY + bodyAscender;
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            const QString& word = line.words.at(wordIndex);
            const qreal wordWidth = measureText(bodyFace.face, word) * docScale;
            const bool active = wordIndex == line.activeWord;
            if (active) {
                const qreal padX = 0.18 * clip.transcriptOverlay.fontPointSize * docScale;
                const qreal padY = 0.02 * clip.transcriptOverlay.fontPointSize * docScale;
                highlights->push_back(QRectF(cursorX - padX,
                                             baseline - bodyAscender - padY,
                                             wordWidth + padX * 2.0,
                                             bodyTextHeight + padY * 2.0));
            }
            const QColor glyphColor = active ? highlightTextColor : textColor;
            if (clip.transcriptOverlay.showShadow && !active) {
                emitRun(bodyFace.face,
                        word,
                        cursorX + shadowOffset,
                        baseline + shadowOffset,
                        clip.transcriptOverlay.bold,
                        bodyPixelSize,
                        shadowColor);
            }
            emitRun(bodyFace.face,
                    word,
                    cursorX,
                    baseline,
                    clip.transcriptOverlay.bold,
                    bodyPixelSize,
                    glyphColor);
            cursorX += wordWidth;
            if (wordIndex + 1 < line.words.size()) {
                cursorX += spaceWidth;
            }
        }
        cursorY += bodyLineHeight;
    }

    QString layoutKey;
    for (const TranscriptOverlayLine& line : layout.lines) {
        layoutKey += line.words.join(QLatin1Char(' '));
        layoutKey += QLatin1Char('#');
        layoutKey += QString::number(line.activeWord);
        layoutKey += QLatin1Char('|');
    }
    const QString keyMaterial =
        QStringLiteral("transcript-vktext|") + clip.id + QLatin1Char('|') +
        clip.transcriptOverlay.fontFamily + QLatin1Char('|') +
        QString::number(bodyPixelSize) + QLatin1Char('|') +
        QString::number(titlePixelSize) + QLatin1Char('|') +
        QString::number(static_cast<quint32>(textColor.rgba())) + QLatin1Char('|') +
        speakerTitle + QLatin1Char('|') +
        layoutKey;
    atlas->key = QString::fromLatin1(QCryptographicHash::hash(keyMaterial.toUtf8(), QCryptographicHash::Sha1).toHex());
    return !glyphs->isEmpty();
}

bool VulkanTextRenderer::ensureAtlasUploaded(VkCommandBuffer commandBuffer, const Atlas& atlas)
{
    if (!m_atlasResources || atlas.key.isEmpty()) {
        return false;
    }
    if (m_uploadedAtlasKey == atlas.key) {
        return true;
    }
    if (!m_atlasResources->uploadImageTexture(commandBuffer, atlas.image)) {
        return false;
    }
    m_uploadedAtlasKey = atlas.key;
    return true;
}

const VulkanTextRenderer::SpeakerLayoutCache* VulkanTextRenderer::speakerLabelLayout(
    const QSize& outputSize,
    const render_detail::SpeakerLabelOverlaySpec& spec) const
{
    const QString layoutKey = speakerLabelLayoutKey(outputSize, spec);
    if (m_speakerLayoutCache.valid && m_speakerLayoutCache.layoutKey == layoutKey) {
        return &m_speakerLayoutCache;
    }

    SpeakerLayoutCache rebuilt;
    rebuilt.layoutKey = layoutKey;
    rebuilt.valid = buildAtlasAndLayout(outputSize,
                                        spec,
                                        &rebuilt.atlas,
                                        &rebuilt.glyphs,
                                        &rebuilt.cards);
    if (!rebuilt.valid) {
        m_speakerLayoutCache = SpeakerLayoutCache{};
        return nullptr;
    }
    m_speakerLayoutCache = rebuilt;
    return &m_speakerLayoutCache;
}

const VulkanTextRenderer::TranscriptLayoutCache* VulkanTextRenderer::transcriptOverlayLayout(
    const QSize& outputSize,
    const TimelineClip& clip,
    const TranscriptOverlayLayout& layout,
    const QRectF& outputRect,
    const QString& speakerTitle) const
{
    const QString layoutKey = transcriptOverlayLayoutKey(outputSize, clip, layout, outputRect, speakerTitle);
    if (m_transcriptLayoutCache.valid && m_transcriptLayoutCache.layoutKey == layoutKey) {
        return &m_transcriptLayoutCache;
    }

    TranscriptLayoutCache rebuilt;
    rebuilt.layoutKey = layoutKey;
    rebuilt.valid = buildTranscriptAtlasAndLayout(outputSize,
                                                  clip,
                                                  layout,
                                                  outputRect,
                                                  speakerTitle,
                                                  &rebuilt.atlas,
                                                  &rebuilt.glyphs,
                                                  &rebuilt.backgrounds,
                                                  &rebuilt.highlights);
    if (!rebuilt.valid) {
        m_transcriptLayoutCache = TranscriptLayoutCache{};
        return nullptr;
    }
    m_transcriptLayoutCache = rebuilt;
    return &m_transcriptLayoutCache;
}

void VulkanTextRenderer::drawGlyph(VkCommandBuffer commandBuffer,
                                   const QSize& swapSize,
                                   const QRectF& rect,
                                   const QRectF& uv,
                                   const QColor& color)
{
    if (!isReady() || !color.isValid() || color.alpha() <= 0 || rect.isEmpty()) {
        return;
    }
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(std::max(1, swapSize.width()));
    viewport.height = static_cast<float>(std::max(1, swapSize.height()));
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(std::max(1, swapSize.width())),
                      static_cast<uint32_t>(std::max(1, swapSize.height()))};

    VulkanTextPipeline::Push push{};
    mvpForScreenRect(rect, swapSize, push.mvp);
    push.uvRect[0] = static_cast<float>(uv.x());
    push.uvRect[1] = static_cast<float>(uv.y());
    push.uvRect[2] = static_cast<float>(uv.width());
    push.uvRect[3] = static_cast<float>(uv.height());
    push.color[0] = static_cast<float>(color.redF());
    push.color[1] = static_cast<float>(color.greenF());
    push.color[2] = static_cast<float>(color.blueF());
    push.color[3] = static_cast<float>(color.alphaF());
    m_pipeline->bindAndDraw(commandBuffer, viewport, scissor, m_atlasResources->descriptorSet(), push);
}

bool VulkanTextRenderer::drawSpeakerLabel(VkCommandBuffer commandBuffer,
                                          const QSize& swapSize,
                                          const QSize& outputSize,
                                          const QRectF& outputTargetRect,
                                          const render_detail::SpeakerLabelOverlaySpec& spec)
{
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !swapSize.isValid() || !outputSize.isValid()) {
        return false;
    }
    const SpeakerLayoutCache* layout = speakerLabelLayout(outputSize, spec);
    if (!layout || !ensureAtlasUploaded(commandBuffer, layout->atlas)) {
        return false;
    }

    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    auto mapRect = [&](const QRectF& rect) {
        return QRectF(outputTargetRect.left() + rect.left() * scaleX,
                      outputTargetRect.top() + rect.top() * scaleY,
                      rect.width() * scaleX,
                      rect.height() * scaleY);
    };

    for (const QRectF& card : layout->cards) {
        const QRectF mapped = mapRect(card);
        clearRect(m_funcs, commandBuffer, clearValueForColor(spec.backgroundColor), clearRectFromQRect(mapped, swapSize));
        clearBoxOutline(m_funcs,
                        commandBuffer,
                        clearValueForColor(spec.borderColor),
                        clearRectFromQRect(mapped.adjusted(0.5, 0.5, -0.5, -0.5), swapSize),
                        qMax(1, static_cast<int>(std::round(qMin(scaleX, scaleY)))));
    }
    for (const LaidOutGlyph& glyph : layout->glyphs) {
        drawGlyph(commandBuffer, swapSize, mapRect(glyph.rect), glyph.uv, glyph.color);
    }
    return true;
}

bool VulkanTextRenderer::prepareSpeakerLabelAtlas(VkCommandBuffer commandBuffer,
                                                  const QSize& outputSize,
                                                  const render_detail::SpeakerLabelOverlaySpec& spec)
{
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !outputSize.isValid()) {
        return false;
    }
    const SpeakerLayoutCache* layout = speakerLabelLayout(outputSize, spec);
    return layout && ensureAtlasUploaded(commandBuffer, layout->atlas);
}

bool VulkanTextRenderer::drawTranscriptOverlay(VkCommandBuffer commandBuffer,
                                               const QSize& swapSize,
                                               const QSize& outputSize,
                                               const QRectF& outputTargetRect,
                                               const TimelineClip& clip,
                                               const TranscriptOverlayLayout& layout,
                                               const QRectF& outputRect,
                                               const QString& speakerTitle)
{
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !swapSize.isValid() ||
        !outputSize.isValid() || layout.lines.isEmpty() || outputRect.isEmpty()) {
        return false;
    }
    const TranscriptLayoutCache* cachedLayout =
        transcriptOverlayLayout(outputSize, clip, layout, outputRect, speakerTitle);
    if (!cachedLayout || !ensureAtlasUploaded(commandBuffer, cachedLayout->atlas)) {
        return false;
    }

    const qreal scaleX = outputTargetRect.width() / qMax<qreal>(1.0, outputSize.width());
    const qreal scaleY = outputTargetRect.height() / qMax<qreal>(1.0, outputSize.height());
    auto mapRect = [&](const QRectF& rect) {
        return QRectF(outputTargetRect.left() + rect.left() * scaleX,
                      outputTargetRect.top() + rect.top() * scaleY,
                      rect.width() * scaleX,
                      rect.height() * scaleY);
    };

    for (const QRectF& background : cachedLayout->backgrounds) {
        clearRect(m_funcs,
                  commandBuffer,
                  clearValueForColor(QColor(0, 0, 0, 120)),
                  clearRectFromQRect(mapRect(background), swapSize));
    }
    for (const QRectF& highlight : cachedLayout->highlights) {
        clearRect(m_funcs,
                  commandBuffer,
                  clearValueForColor(QColor(QStringLiteral("#fff2a8"))),
                  clearRectFromQRect(mapRect(highlight), swapSize));
    }
    for (const LaidOutGlyph& glyph : cachedLayout->glyphs) {
        drawGlyph(commandBuffer, swapSize, mapRect(glyph.rect), glyph.uv, glyph.color);
    }
    return true;
}

bool VulkanTextRenderer::prepareTranscriptOverlayAtlas(VkCommandBuffer commandBuffer,
                                                       const QSize& outputSize,
                                                       const TimelineClip& clip,
                                                       const TranscriptOverlayLayout& layout,
                                                       const QRectF& outputRect,
                                                       const QString& speakerTitle)
{
    if (!isReady() || commandBuffer == VK_NULL_HANDLE || !outputSize.isValid() ||
        layout.lines.isEmpty() || outputRect.isEmpty()) {
        return false;
    }
    const TranscriptLayoutCache* cachedLayout =
        transcriptOverlayLayout(outputSize, clip, layout, outputRect, speakerTitle);
    return cachedLayout && ensureAtlasUploaded(commandBuffer, cachedLayout->atlas);
}
