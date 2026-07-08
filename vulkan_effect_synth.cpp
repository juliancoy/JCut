#include "vulkan_effect_synth.h"

#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace render_detail {
namespace {

struct SynthNode {
    VulkanEffectPipelinePlan::DrawPass pass;
    float cameraDepth = 0.0f;
};

QPointF projectedPoint(const QVector3D& worldPoint,
                       const QMatrix4x4& viewProjection,
                       const QRectF& outputRect)
{
    const QVector4D clip = viewProjection * QVector4D(worldPoint, 1.0f);
    const float invW = std::abs(clip.w()) > 0.00001f ? 1.0f / clip.w() : 1.0f;
    const float ndcX = clip.x() * invW;
    const float ndcY = clip.y() * invW;
    return QPointF(outputRect.left() + (static_cast<qreal>(ndcX) + 1.0) * 0.5 * outputRect.width(),
                   outputRect.top() + (1.0 - (static_cast<qreal>(ndcY) + 1.0) * 0.5) * outputRect.height());
}

} // namespace

QVector<VulkanEffectPipelinePlan::DrawPass> vulkanSynth3DDrawPasses(
    const VulkanSynth3DParams& params)
{
    QVector<VulkanEffectPipelinePlan::DrawPass> draws;
    if (params.outputRect.isEmpty()) {
        return draws;
    }

    constexpr qreal kTwoPi = 6.28318530717958647692;
    const int count = qBound(1, params.copyCount, 96);
    const qreal aspect = std::max<qreal>(0.001, params.sourceAspect);
    const qreal scale = qBound<qreal>(0.1, params.scale, 8.0);
    const qreal speed = qBound<qreal>(-8.0, params.speed, 8.0);
    const qreal phase = params.timelineFrame * speed * 0.018;

    QMatrix4x4 view;
    view.lookAt(QVector3D(0.0f, 0.0f, 5.4f),
                QVector3D(0.0f, 0.0f, 0.0f),
                QVector3D(0.0f, 1.0f, 0.0f));

    QMatrix4x4 projection;
    const qreal outputAspect = params.outputRect.height() > 0.0
        ? params.outputRect.width() / params.outputRect.height()
        : 1.0;
    projection.perspective(44.0f,
                           static_cast<float>(std::max<qreal>(0.001, outputAspect)),
                           0.1f,
                           32.0f);
    const QMatrix4x4 viewProjection = projection * view;

    QVector<SynthNode> nodes;
    nodes.reserve(count);
    for (int i = 0; i < count; ++i) {
        const qreal t = count <= 1
            ? 0.0
            : static_cast<qreal>(i) / static_cast<qreal>(count - 1);
        const qreal angle = phase + (kTwoPi * 2.0 * t);
        const qreal lane = -0.88 + 1.76 * ((static_cast<qreal>(i) + 0.5) / static_cast<qreal>(count));
        const qreal radius = 1.28 * qBound<qreal>(0.35, scale, 3.5);
        const qreal x = std::sin(angle) * radius;
        const qreal y = lane + std::sin(angle * 1.7 + phase * 2.3) * 0.34;
        const qreal z = std::cos(angle) * 1.12;

        const QVector3D worldPoint(static_cast<float>(x),
                                   static_cast<float>(y),
                                   static_cast<float>(z));
        const QPointF center = projectedPoint(worldPoint, viewProjection, params.outputRect);
        const qreal cameraDepth = 5.4 - z;
        const qreal depthScale = qBound<qreal>(0.46, 5.4 / std::max<qreal>(0.001, cameraDepth), 1.58);
        const qreal pulse = 0.82 + 0.18 * std::sin(phase * 3.0 + static_cast<qreal>(i) * 0.9);
        const qreal baseTileH = std::min(params.outputRect.width(), params.outputRect.height()) * 0.13;
        const qreal tileH = std::max<qreal>(4.0, baseTileH * scale * depthScale * pulse);
        const qreal tileW = std::max<qreal>(4.0, tileH * aspect);

        SynthNode node;
        node.cameraDepth = static_cast<float>(cameraDepth);
        node.pass.outputRect = QRectF(center.x() - tileW * 0.5,
                                      center.y() - tileH * 0.5,
                                      tileW,
                                      tileH);
        node.pass.rotationDegrees =
            (params.alternateHandedness ? z : -z) * 32.0 +
            std::sin(angle + phase) * 9.0;
        node.pass.opacityMultiplier =
            static_cast<float>(qBound<qreal>(0.22, 0.56 + depthScale * 0.32 + z * 0.18, 1.0));
        node.pass.shaderMode = kVulkanEffectModeSynth3D;
        node.pass.depthSortKey = cameraDepth;
        nodes.push_back(node);
    }

    std::sort(nodes.begin(), nodes.end(), [](const SynthNode& a, const SynthNode& b) {
        return a.cameraDepth > b.cameraDepth;
    });

    draws.reserve(nodes.size());
    for (const SynthNode& node : nodes) {
        draws.push_back(node.pass);
    }
    return draws;
}

} // namespace render_detail
