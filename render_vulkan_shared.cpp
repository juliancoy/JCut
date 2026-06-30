#include "render_vulkan_shared.h"

#include "editor_shared_effects.h"

#include <QMatrix4x4>
#include <QVector>

#include <algorithm>
#include <cmath>

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

QVector<QRectF> vulkanPresetEffectRects(const TimelineClip& clip,
                                        const QRectF& outputRect,
                                        const QSize& textureSize,
                                        qreal timelineFrame)
{
    QVector<QRectF> rects;
    if (clip.effectPreset == ClipEffectPreset::None || outputRect.isEmpty()) {
        return rects;
    }

    const int count = qBound(1, clip.effectRows, 96);
    const qreal aspect = textureSize.height() > 0
        ? static_cast<qreal>(std::max(1, textureSize.width())) /
              static_cast<qreal>(textureSize.height())
        : 1.0;
    const qreal scale = qBound<qreal>(0.1, clip.effectScale, 8.0);
    const qreal speed = qBound<qreal>(-8.0, clip.effectSpeed, 8.0);

    if (clip.effectPreset == ClipEffectPreset::NewsLogoTicker ||
        clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground) {
        const qreal rowH = outputRect.height() / static_cast<qreal>(count);
        const qreal baseCoverage =
            clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground ? 1.08 : 0.78;
        const qreal baseSpacing =
            clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground ? 1.02 : 1.35;
        const qreal tileH = std::max<qreal>(2.0, rowH * baseCoverage * scale);
        const qreal tileW = std::max<qreal>(2.0, tileH * aspect);
        const qreal spacing = tileW * baseSpacing;
        for (int row = 0; row < count; ++row) {
            const qreal direction = (clip.effectAlternateDirection && (row % 2)) ? -1.0 : 1.0;
            qreal phase = std::fmod((timelineFrame * speed * direction * rowH * 0.08) +
                                        (row * spacing * 0.37),
                                    spacing);
            if (clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground && phase < 0.0) {
                phase += spacing;
            }
            const qreal y = outputRect.top() + (row + 0.5) * rowH - tileH * 0.5;
            for (qreal x = outputRect.left() - spacing + phase;
                 x < outputRect.right() + spacing;
                 x += spacing) {
                rects.push_back(QRectF(x, y, tileW, tileH));
            }
        }
    } else if (clip.effectPreset == ClipEffectPreset::PersonOrbit) {
        constexpr qreal kTwoPi = 6.28318530717958647692;
        const qreal tileSide =
            std::max<qreal>(4.0, std::min(outputRect.width(), outputRect.height()) * 0.072 * scale);
        const qreal tileW = tileSide * aspect;
        const qreal tileH = tileSide;
        const QPointF center = outputRect.center();
        const qreal rx = outputRect.width() * 0.28;
        const qreal ry = outputRect.height() * 0.24;
        const qreal phase = timelineFrame * speed * 0.025;
        for (int i = 0; i < count; ++i) {
            const qreal angle = phase + (kTwoPi * static_cast<qreal>(i) / static_cast<qreal>(count));
            const QPointF p(center.x() + std::cos(angle) * rx,
                            center.y() + std::sin(angle) * ry);
            rects.push_back(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
        }
    }

    return rects;
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

void vulkanMvpForExportVideoLayer(const QRectF& fittedRect,
                                  const QPointF& translation,
                                  const QSize& outputSize,
                                  qreal rotationDegrees,
                                  const QPointF& scale,
                                  float outMvp[16])
{
    const float fullW = static_cast<float>(std::max(1, outputSize.width()));
    const float fullH = static_cast<float>(std::max(1, outputSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, fittedRect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, fittedRect.height())) * 0.5f;
    constexpr double kPi = 3.141592653589793238462643383279502884;
    const float radians = static_cast<float>(rotationDegrees * kPi / 180.0);
    const float cosTheta = std::cos(radians);
    const float sinTheta = std::sin(radians);
    const float scaleX = static_cast<float>(scale.x());
    const float scaleY = static_cast<float>(scale.y());
    const float m11 = cosTheta * scaleX;
    const float m12 = sinTheta * scaleX;
    const float m21 = -sinTheta * scaleY;
    const float m22 = cosTheta * scaleY;
    const float dx = static_cast<float>(fittedRect.center().x() + translation.x());
    const float dy = static_cast<float>(fittedRect.center().y() + translation.y());
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
    state.shadows[3] = gradingUsesCurveLut(grade) ? kVulkanEffectModeCurve : kVulkanEffectModeNormal;
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
    return vulkanBackgroundFillEffectState(BackgroundFillEffect::BlurCover,
                                           VulkanDrawEffectState{},
                                           opacity);
}

VulkanDrawEffectState vulkanBackgroundFillEffectState(BackgroundFillEffect effect,
                                                      const VulkanDrawEffectState& baseEffects,
                                                      float opacity,
                                                      float brightness,
                                                      float saturation,
                                                      int edgePixels,
                                                      bool progressiveEdge,
                                                      float edgePower,
                                                      const QRectF& sourceRectNorm)
{
    VulkanDrawEffectState state;
    state.opacity = std::clamp(opacity, 0.0f, 1.0f);
    state.brightness = std::clamp(baseEffects.brightness + brightness, -1.0f, 1.0f);
    state.contrast = std::clamp(baseEffects.contrast, 0.0f, 4.0f);
    state.saturation = std::clamp(baseEffects.saturation * saturation, 0.0f, 3.0f);
    if (effect == BackgroundFillEffect::EdgeStretch ||
        effect == BackgroundFillEffect::Mirror) {
        state.shadows[0] = static_cast<float>(sourceRectNorm.left());
        state.shadows[1] = static_cast<float>(sourceRectNorm.top());
        state.shadows[2] = static_cast<float>(sourceRectNorm.right());
        state.shadows[3] = static_cast<float>(sourceRectNorm.bottom());
        state.midtones[0] = static_cast<float>(std::clamp(edgePixels, 1, 512));
        state.midtones[1] = progressiveEdge ? 1.0f : 0.0f;
        state.midtones[2] = std::clamp(edgePower, 0.25f, 8.0f);
        state.highlights[3] = effect == BackgroundFillEffect::Mirror
            ? kVulkanEffectModeBackgroundMirror
            : kVulkanEffectModeBackgroundEdgeStretch;
        return state;
    }
    state.highlights[3] = kVulkanEffectModeBackgroundBlur;
    state.midtones[3] = -34.0f;
    return state;
}

} // namespace render_detail
