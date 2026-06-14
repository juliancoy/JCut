#include "render_vulkan_shared.h"

#include "editor_shared_effects.h"

#include <QMatrix4x4>
#include <QVector>

#include <algorithm>

namespace render_detail {

QByteArray vulkanCurveLutRgbaBytes(const TimelineClip::GradingKeyframe& grade)
{
    const QVector<quint8> lutR =
        gradingCurveLut8(grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutG =
        gradingCurveLut8(grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutB =
        gradingCurveLut8(grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> lutL =
        gradingCurveLut8(grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    if (lutR.size() != TimelineClip::kGradingCurveLutSize ||
        lutG.size() != TimelineClip::kGradingCurveLutSize ||
        lutB.size() != TimelineClip::kGradingCurveLutSize ||
        lutL.size() != TimelineClip::kGradingCurveLutSize) {
        return {};
    }

    QByteArray rgba;
    rgba.resize(TimelineClip::kGradingCurveLutSize * 4);
    for (int i = 0; i < TimelineClip::kGradingCurveLutSize; ++i) {
        rgba[i * 4 + 0] = static_cast<char>(lutR[i]);
        rgba[i * 4 + 1] = static_cast<char>(lutG[i]);
        rgba[i * 4 + 2] = static_cast<char>(lutB[i]);
        rgba[i * 4 + 3] = static_cast<char>(lutL[i]);
    }
    return rgba;
}

QByteArray vulkanIdentityCurveLutRgbaBytes()
{
    TimelineClip::GradingKeyframe grade;
    grade.curvePointsR = defaultGradingCurvePoints();
    grade.curvePointsG = defaultGradingCurvePoints();
    grade.curvePointsB = defaultGradingCurvePoints();
    grade.curvePointsLuma = defaultGradingCurvePoints();
    return vulkanCurveLutRgbaBytes(grade);
}

void vulkanMvpForOutputRect(const QRectF& rect,
                            const QSize& outputSize,
                            qreal rotationDegrees,
                            float outMvp[16])
{
    vulkanMvpForOutputRectMaybeFlippedY(rect, outputSize, rotationDegrees, false, outMvp);
}

void vulkanMvpForOutputRectMaybeFlippedY(const QRectF& rect,
                                         const QSize& outputSize,
                                         qreal rotationDegrees,
                                         bool flipY,
                                         float outMvp[16])
{
    QMatrix4x4 projection;
    projection.ortho(0.0f,
                     static_cast<float>(std::max(1, outputSize.width())),
                     static_cast<float>(std::max(1, outputSize.height())),
                     0.0f,
                     -1.0f,
                     1.0f);
    QMatrix4x4 model;
    model.translate(static_cast<float>(rect.center().x()), static_cast<float>(rect.center().y()), 0.0f);
    model.rotate(static_cast<float>(rotationDegrees), 0.0f, 0.0f, 1.0f);
    model.scale(static_cast<float>(rect.width()),
                static_cast<float>(rect.height()) * (flipY ? -1.0f : 1.0f),
                1.0f);
    QMatrix4x4 shaderQuadToOpenGlQuad;
    shaderQuadToOpenGlQuad.scale(0.5f, 0.5f, 1.0f);
    const QMatrix4x4 mvp = projection * model * shaderQuadToOpenGlQuad;
    const float* data = mvp.constData();
    std::copy(data, data + 16, outMvp);
}

void vulkanMvpForPreviewTransform(const QTransform& clipToSwapchain,
                                  const QRectF& localRect,
                                  const QSize& swapSize,
                                  float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, swapSize.width()));
    const float fullH = static_cast<float>(std::max(1, swapSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, localRect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, localRect.height())) * 0.5f;
    const float m11 = static_cast<float>(clipToSwapchain.m11());
    const float m12 = static_cast<float>(clipToSwapchain.m12());
    const float m21 = static_cast<float>(clipToSwapchain.m21());
    const float m22 = static_cast<float>(clipToSwapchain.m22());
    const float dx = static_cast<float>(clipToSwapchain.dx());
    const float dy = static_cast<float>(clipToSwapchain.dy());
    const float m[16] = {
        (2.0f * m11 * halfW) / fullW, (2.0f * m12 * halfW) / fullH, 0.f, 0.f,
        (2.0f * m21 * halfH) / fullW, (2.0f * m22 * halfH) / fullH, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        (2.0f * dx / fullW) - 1.0f,
        (2.0f * dy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
}

VulkanDrawEffectState vulkanDrawEffectStateForGrade(const TimelineClip::GradingKeyframe& grade)
{
    VulkanDrawEffectState state;
    state.brightness = static_cast<float>(grade.brightness);
    state.contrast = static_cast<float>(grade.contrast);
    state.saturation = static_cast<float>(grade.saturation);
    state.opacity = static_cast<float>(std::clamp(static_cast<double>(grade.opacity), 0.0, 1.0));
    state.shadows[0] = static_cast<float>(grade.shadowsR);
    state.shadows[1] = static_cast<float>(grade.shadowsG);
    state.shadows[2] = static_cast<float>(grade.shadowsB);
    state.shadows[3] = 1.0f;
    state.midtones[0] = static_cast<float>(grade.midtonesR);
    state.midtones[1] = static_cast<float>(grade.midtonesG);
    state.midtones[2] = static_cast<float>(grade.midtonesB);
    state.highlights[0] = static_cast<float>(grade.highlightsR);
    state.highlights[1] = static_cast<float>(grade.highlightsG);
    state.highlights[2] = static_cast<float>(grade.highlightsB);
    state.highlights[3] = 1.0f;
    return state;
}

VulkanDrawEffectState vulkanBlurredBackgroundEffectState(float opacity)
{
    VulkanDrawEffectState state;
    state.opacity = std::clamp(opacity, 0.0f, 1.0f);
    state.brightness = -0.12f;
    state.contrast = 1.0f;
    state.saturation = 0.75f;
    state.shadows[3] = 1.0f;
    state.midtones[3] = -14.0f;
    state.highlights[3] = 1.0f;
    return state;
}

} // namespace render_detail
