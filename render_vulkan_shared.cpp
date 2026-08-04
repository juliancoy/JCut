#include "render_vulkan_shared.h"

#include "background_fill_effect.h"
#include "editor_shared_effects.h"
#include "editor_shared_keyframes.h"
#include "editor_shared_transcript.h"
#include "preview_view_transform.h"
#include "transform_skip_aware_timing.h"
#include "vulkan_effect_synth.h"

#include <QMatrix4x4>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace render_detail {
namespace {

float shaderModeForSinglePassPreset(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::Kaleidoscope: return kVulkanEffectModeKaleidoscope;
    case ClipEffectPreset::HexagonalPrism: return kVulkanEffectModeHexagonalPrism;
    case ClipEffectPreset::Droste: return kVulkanEffectModeDroste;
    case ClipEffectPreset::RecursiveZoomTile: return kVulkanEffectModeRecursiveZoomTile;
    case ClipEffectPreset::RecursiveZoomTunnel: return kVulkanEffectModeRecursiveZoomTunnel;
    case ClipEffectPreset::RecursiveZoomMirrorBox: return kVulkanEffectModeRecursiveZoomMirrorBox;
    case ClipEffectPreset::RecursiveZoomSpiral: return kVulkanEffectModeRecursiveZoomSpiral;
    case ClipEffectPreset::RecursiveZoomKaleidoscope: return kVulkanEffectModeRecursiveZoomKaleidoscope;
    case ClipEffectPreset::RecursiveZoomRadialRepeat: return kVulkanEffectModeRecursiveZoomRadialRepeat;
    case ClipEffectPreset::RecursiveZoomPixelMosaic: return kVulkanEffectModeRecursiveZoomPixelMosaic;
    case ClipEffectPreset::PolarTunnel: return kVulkanEffectModePolarTunnel;
    case ClipEffectPreset::TinyPlanet: return kVulkanEffectModeTinyPlanet;
    case ClipEffectPreset::InfiniteMirror: return kVulkanEffectModeInfiniteMirror;
    case ClipEffectPreset::QuadMirror: return kVulkanEffectModeQuadMirror;
    case ClipEffectPreset::SlitScan: return kVulkanEffectModeSlitScan;
    case ClipEffectPreset::DisplacementMap: return kVulkanEffectModeDisplacementMap;
    case ClipEffectPreset::TwirlVortex: return kVulkanEffectModeTwirlVortex;
    case ClipEffectPreset::RippleShockwave: return kVulkanEffectModeRippleShockwave;
    case ClipEffectPreset::PixelSorting: return kVulkanEffectModePixelSorting;
    case ClipEffectPreset::DatamoshGlitch: return kVulkanEffectModeDatamoshGlitch;
    case ClipEffectPreset::RgbSplit: return kVulkanEffectModeRgbSplit;
    case ClipEffectPreset::HalftoneMosaic: return kVulkanEffectModeHalftoneMosaic;
    case ClipEffectPreset::GlassRefraction: return kVulkanEffectModeGlassRefraction;
    case ClipEffectPreset::SobelEdges: return kVulkanEffectModeSobelEdges;
    case ClipEffectPreset::NeonGlow: return kVulkanEffectModeNeonGlow;
    case ClipEffectPreset::SpeakerMaskDilation: return kVulkanEffectModeSpeakerMaskDilation;
    case ClipEffectPreset::SpeakerMaskDilationPulse: return kVulkanEffectModeSpeakerMaskDilationPulse;
    case ClipEffectPreset::SpeakerMaskDilationRings: return kVulkanEffectModeSpeakerMaskDilationRings;
    default: return -1.0f;
    }
}

bool isSpeakerMaskDilationPreset(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::SpeakerMaskDilation ||
           preset == ClipEffectPreset::SpeakerMaskDilationPulse ||
           preset == ClipEffectPreset::SpeakerMaskDilationRings;
}

bool isRecursiveZoomPreset(ClipEffectPreset preset)
{
    return preset == ClipEffectPreset::RecursiveZoomTile ||
           preset == ClipEffectPreset::RecursiveZoomTunnel ||
           preset == ClipEffectPreset::RecursiveZoomMirrorBox ||
           preset == ClipEffectPreset::RecursiveZoomSpiral ||
           preset == ClipEffectPreset::RecursiveZoomKaleidoscope ||
           preset == ClipEffectPreset::RecursiveZoomRadialRepeat ||
           preset == ClipEffectPreset::RecursiveZoomPixelMosaic;
}

float sourceMosaicShaderMode(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::SourceMosaicGrid:
        return kVulkanEffectModeSourceMosaicGrid;
    case ClipEffectPreset::SourceMosaicStagger:
        return kVulkanEffectModeSourceMosaicStagger;
    case ClipEffectPreset::SourceMosaicHex:
        return kVulkanEffectModeSourceMosaicHex;
    case ClipEffectPreset::SourceMosaicRadial:
        return kVulkanEffectModeSourceMosaicRadial;
    case ClipEffectPreset::SourceMosaicFlow:
        return kVulkanEffectModeSourceMosaicFlow;
    default:
        return -1.0f;
    }
}

QString effectClockContinuityKey(const TimelineClip& clip)
{
    const QString linkedSourceId = clip.linkedSourceClipId.trimmed();
    if (!linkedSourceId.isEmpty()) {
        return QStringLiteral("linked:") + linkedSourceId;
    }
    const QString path = clip.filePath.trimmed();
    if (!path.isEmpty()) {
        return QStringLiteral("file:") + path;
    }
    const QString proxyPath = clip.proxyPath.trimmed();
    if (!proxyPath.isEmpty()) {
        return QStringLiteral("proxy:") + proxyPath;
    }
    return QStringLiteral("clip:") + clip.id;
}

bool clipSharesEffectClock(const TimelineClip& a, const TimelineClip& b)
{
    return a.trackIndex == b.trackIndex &&
           a.effectPreset == b.effectPreset &&
           effectClockContinuityKey(a) == effectClockContinuityKey(b);
}

} // namespace

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

void VulkanRenderLayerPacket::setGrading(
    const TimelineClip::GradingKeyframe& value)
{
    grading = value;
    gradePayload = vulkanGradePayloadForGrade(value);
}

void VulkanRenderLayerPacket::setMaskGrade(
    const TimelineClip::GradingKeyframe& value)
{
    maskGrade = value;
    maskGradePayload = vulkanGradePayloadForGrade(value);
}

void VulkanRenderLayerPacket::setCorrectionPolygons(
    const QVector<TimelineClip::CorrectionPolygon>& value)
{
    correctionPolygons = value;
    correctionPolygonCount = value.size();
    maskCorrectionStorage = vulkanMaskCorrectionStorageData(value);
}

QString vulkanSourceFrameCacheKey(
    const QString& mediaOwnerClipId,
    const editor::FrameHandle& frame)
{
    const QString owner = mediaOwnerClipId.trimmed();
    const QString sourcePath = frame.sourcePath();
    if (owner.isEmpty() || frame.isNull() || sourcePath.isEmpty() ||
        frame.frameNumber() < 0 || !frame.size().isValid()) {
        return {};
    }
    return QStringLiteral(
               "owner=%1\x1fsource=%2\x1fframe=%3\x1fpts=%4"
               "\x1fsize=%5x%6\x1fpixel=%7:%8")
        .arg(owner,
             sourcePath,
             QString::number(frame.frameNumber()),
             QString::number(frame.sourcePresentationTimestamp()),
             QString::number(frame.size().width()),
             QString::number(frame.size().height()),
             QString::number(frame.hardwarePixelFormat()),
             QString::number(frame.hardwareSwPixelFormat()));
}

QByteArray vulkanMaskCorrectionStorageData(
    const QVector<TimelineClip::CorrectionPolygon>& polygons)
{
    QVector<const TimelineClip::CorrectionPolygon*> active;
    qsizetype pointCount = 0;
    for (const TimelineClip::CorrectionPolygon& polygon : polygons) {
        if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
            continue;
        }
        active.push_back(&polygon);
        pointCount += polygon.pointsNormalized.size();
    }

    // std430 vec4 entries:
    //   [0]                  polygon count
    //   [1..polygonCount]    first-point index, point count
    //   [1+polygonCount..]   normalized xy points
    const qsizetype entryCount = 1 + active.size() + pointCount;
    QByteArray storage(
        static_cast<qsizetype>(entryCount * 4 * sizeof(float)), Qt::Uninitialized);
    auto* entries = reinterpret_cast<float*>(storage.data());
    std::fill(entries, entries + entryCount * 4, 0.0f);
    entries[0] = static_cast<float>(active.size());
    qsizetype firstPoint = 0;
    for (qsizetype polygonIndex = 0;
         polygonIndex < active.size();
         ++polygonIndex) {
        const TimelineClip::CorrectionPolygon& polygon =
            *active.at(polygonIndex);
        float* header = entries + ((1 + polygonIndex) * 4);
        header[0] = static_cast<float>(firstPoint);
        header[1] = static_cast<float>(polygon.pointsNormalized.size());
        for (const QPointF& point : polygon.pointsNormalized) {
            float* encodedPoint =
                entries + ((1 + active.size() + firstPoint) * 4);
            encodedPoint[0] = static_cast<float>(
                qBound<qreal>(0.0, point.x(), 1.0));
            encodedPoint[1] = static_cast<float>(
                qBound<qreal>(0.0, point.y(), 1.0));
            ++firstPoint;
        }
    }
    return storage;
}

QVector<QRectF> VulkanEffectPipelinePlan::generatedDrawRects() const
{
    QVector<QRectF> rects;
    rects.reserve(generatedDraws.size());
    for (const DrawPass& pass : generatedDraws) {
        rects.push_back(pass.outputRect);
    }
    return rects;
}

bool vulkanClipSupportsBackgroundFillSource(const TimelineClip& clip)
{
    if (clip.clipRole != ClipRole::Media) {
        return false;
    }
    if (clip.mediaType != ClipMediaType::Image &&
        clip.mediaType != ClipMediaType::Video) {
        return false;
    }
    return !playbackMediaPathForClip(clip).trimmed().isEmpty();
}

VulkanEffectPipelinePlan vulkanEffectPipelinePlan(const TimelineClip& clip,
                                                  const QRectF& outputRect,
                                                  const QSize& textureSize,
                                                  qreal timelineFrame,
                                                  qreal effectFrame,
                                                  const PlaybackTimingContext& timing,
                                                  const QRectF& tilingMaskBounds)
{
    VulkanEffectPipelinePlan plan;
    const qreal temporalFrame = effectFrame >= 0.0 ? effectFrame : timelineFrame;
    const bool maskedRepeatEnabled =
        clip.maskRepeatEnabled && clip.maskEnabled && !clip.maskFramesDir.trimmed().isEmpty();
    if ((clip.effectPreset == ClipEffectPreset::None && !maskedRepeatEnabled) || outputRect.isEmpty()) {
        return plan;
    }

    QVector<VulkanEffectPipelinePlan::DrawPass>& draws = plan.generatedDraws;
    const TimelineClip::GradingKeyframe grade =
        evaluateClipGradingAtFrame(clip, qRound64(timelineFrame));
    const float generatedShaderMode = gradingUsesCurveLut(grade)
                                          ? kVulkanEffectModeCurve
                                          : kVulkanEffectModeNormal;
    auto addDraw = [&draws, generatedShaderMode](const QRectF& rect) {
        VulkanEffectPipelinePlan::DrawPass pass;
        pass.outputRect = rect;
        pass.shaderMode = generatedShaderMode;
        draws.push_back(pass);
    };
    const int count = qBound(1, clip.effectRows, 96);
    const qreal aspect = textureSize.height() > 0
        ? static_cast<qreal>(std::max(1, textureSize.width())) /
              static_cast<qreal>(textureSize.height())
        : 1.0;
    const qreal scale = qBound<qreal>(0.1, clip.effectScale, 8.0);
    const qreal speed = qBound<qreal>(-8.0, clip.effectSpeed, 8.0);

    if (clip.effectPreset == ClipEffectPreset::NewsLogoTicker ||
        clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground ||
        clip.effectPreset == ClipEffectPreset::DirectionalTrimTicker) {
        const qreal rowH = outputRect.height() / static_cast<qreal>(count);
        qreal baseCoverage = 0.78;
        qreal baseSpacing = 1.35;
        qreal phaseScale = 0.08;
        if (clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground) {
            baseCoverage = 1.08;
            baseSpacing = 1.02;
        } else if (clip.effectPreset == ClipEffectPreset::DirectionalTrimTicker) {
            baseCoverage = 0.92;
            baseSpacing = 0.74;
            phaseScale = 0.18;
        }
        const qreal tileH = std::max<qreal>(2.0, rowH * baseCoverage * scale);
        const qreal trimPulse = clip.effectPreset == ClipEffectPreset::DirectionalTrimTicker
            ? 0.58 + 0.42 * std::abs(std::sin(temporalFrame * std::max<qreal>(0.1, std::abs(speed)) * 0.12))
            : 1.0;
        const qreal tileW = std::max<qreal>(2.0, tileH * aspect * trimPulse);
        const qreal spacing = tileW * baseSpacing;
        for (int row = 0; row < count; ++row) {
            const qreal direction = (clip.effectAlternateDirection && (row % 2)) ? -1.0 : 1.0;
            qreal phase = std::fmod((temporalFrame * speed * direction * rowH * phaseScale) +
                                        (row * spacing * 0.37),
                                    spacing);
            if ((clip.effectPreset == ClipEffectPreset::AlternatingMotionBackground ||
                 clip.effectPreset == ClipEffectPreset::DirectionalTrimTicker) &&
                phase < 0.0) {
                phase += spacing;
            }
            const qreal y = outputRect.top() + (row + 0.5) * rowH - tileH * 0.5;
            for (qreal x = outputRect.left() - spacing + phase;
                 x < outputRect.right() + spacing;
                 x += spacing) {
                addDraw(QRectF(x, y, tileW, tileH));
            }
        }
    } else if (clip.effectPreset == ClipEffectPreset::SourceTile) {
        constexpr qreal kTwoPi = 6.28318530717958647692;
        const QRectF tilingRect =
            (clip.tilingUseMaskBounds && !tilingMaskBounds.isEmpty())
                ? tilingMaskBounds.normalized().intersected(outputRect)
                : outputRect;
        if (tilingRect.isEmpty()) {
            return plan;
        }
        const bool constrainToTilingRect =
            clip.tilingUseMaskBounds && !tilingMaskBounds.isEmpty();
        auto addTilingDraw = [&](const QRectF& rect) {
            if (!constrainToTilingRect) {
                addDraw(rect);
                return;
            }
            const QRectF clipped = rect.intersected(tilingRect);
            if (!clipped.isEmpty()) {
                addDraw(clipped);
            }
        };
        const qreal spacing = qBound<qreal>(0.1, clip.tilingSpacing, 8.0);
        const qreal minDimension = std::max<qreal>(
            1.0, std::min(tilingRect.width(), tilingRect.height()));
        const qreal baseTileW = tilingRect.width() / static_cast<qreal>(qBound(1, count, 96));
        const qreal tileW = std::max<qreal>(2.0, baseTileW * scale);
        const qreal tileH = std::max<qreal>(2.0, tileW / std::max<qreal>(0.001, aspect));
        const qreal stepX = std::max<qreal>(1.0, tileW * spacing);
        const qreal stepY = std::max<qreal>(1.0, tileH * spacing);
        const qreal phaseX = std::fmod(temporalFrame * speed * tileW * 0.015, stepX);
        const qreal phaseY = std::fmod(temporalFrame * speed * tileH * 0.006, stepY);
        const qreal normalizedPhaseX = phaseX < 0.0 ? phaseX + stepX : phaseX;
        const qreal normalizedPhaseY = phaseY < 0.0 ? phaseY + stepY : phaseY;

        if (clip.tilingPattern == ClipTilingPattern::Encircle) {
            const QPointF center = tilingRect.center();
            const qreal radius = minDimension * 0.34 * spacing;
            const qreal phase = temporalFrame * speed * 0.018;
            for (int i = 0; i < count; ++i) {
                const qreal angle = phase + (kTwoPi * static_cast<qreal>(i) / static_cast<qreal>(count));
                const QPointF p(center.x() + std::cos(angle) * radius,
                                center.y() + std::sin(angle) * radius);
                addTilingDraw(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
            }
        } else if (clip.tilingPattern == ClipTilingPattern::SpiralXY ||
                   clip.tilingPattern == ClipTilingPattern::SpiralXZ ||
                   clip.tilingPattern == ClipTilingPattern::SpiralYZ) {
            const QPointF center = tilingRect.center();
            const qreal maxRadius = minDimension * 0.46 * spacing;
            const qreal phase = temporalFrame * speed * 0.014;
            for (int i = 0; i < count; ++i) {
                const qreal t = count <= 1 ? 0.0 : static_cast<qreal>(i) / static_cast<qreal>(count - 1);
                const qreal angle = phase + (kTwoPi * 1.61803398875 * static_cast<qreal>(i));
                const qreal radius = maxRadius * t;
                const qreal u = std::cos(angle) * radius;
                const qreal v = std::sin(angle) * radius;
                qreal x = center.x() + u;
                qreal y = center.y() + v;
                qreal sizeMultiplier = 1.0;
                if (clip.tilingPattern == ClipTilingPattern::SpiralXZ) {
                    y = center.y() + ((t - 0.5) * tilingRect.height() * 0.68);
                    sizeMultiplier = qBound<qreal>(0.45, 0.76 + (v / std::max<qreal>(1.0, maxRadius)) * 0.34, 1.18);
                } else if (clip.tilingPattern == ClipTilingPattern::SpiralYZ) {
                    x = center.x() + ((t - 0.5) * tilingRect.width() * 0.68);
                    y = center.y() + u;
                    sizeMultiplier = qBound<qreal>(0.45, 0.76 + (v / std::max<qreal>(1.0, maxRadius)) * 0.34, 1.18);
                }
                const qreal drawW = tileW * sizeMultiplier;
                const qreal drawH = tileH * sizeMultiplier;
                addTilingDraw(QRectF(x - drawW * 0.5, y - drawH * 0.5, drawW, drawH));
            }
        } else if (clip.tilingPattern == ClipTilingPattern::Diamond) {
            const QPointF center = tilingRect.center();
            const int rings = qBound(1, static_cast<int>(std::ceil(std::sqrt(static_cast<qreal>(count)))), 10);
            int emitted = 0;
            addTilingDraw(QRectF(center.x() - tileW * 0.5, center.y() - tileH * 0.5, tileW, tileH));
            ++emitted;
            for (int ring = 1; ring <= rings && emitted < count; ++ring) {
                const qreal dx = stepX * static_cast<qreal>(ring);
                const qreal dy = stepY * static_cast<qreal>(ring);
                const QVector<QPointF> points{
                    QPointF(center.x(), center.y() - dy),
                    QPointF(center.x() + dx, center.y()),
                    QPointF(center.x(), center.y() + dy),
                    QPointF(center.x() - dx, center.y())};
                for (const QPointF& p : points) {
                    addTilingDraw(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
                    if (++emitted >= count) {
                        break;
                    }
                }
            }
        } else {
            const int columns = qBound(1, count, 96);
            const qreal startY = clip.tilingWrap ? tilingRect.top() - tileH + normalizedPhaseY : tilingRect.top();
            const qreal endY = clip.tilingWrap ? tilingRect.bottom() + tileH : tilingRect.bottom() - tileH + 1.0;
            int row = 0;
            for (qreal y = startY; y < endY; y += stepY, ++row) {
                const qreal rowOffset =
                    (clip.effectAlternateDirection && (row % 2)) ? stepX * 0.5 : 0.0;
                const qreal startX = clip.tilingWrap
                    ? tilingRect.left() - tileW + normalizedPhaseX - rowOffset
                    : tilingRect.left() - rowOffset;
                const qreal endX = clip.tilingWrap
                    ? tilingRect.right() + tileW
                    : tilingRect.left() + (columns * stepX);
                for (qreal x = startX; x < endX; x += stepX) {
                    addTilingDraw(QRectF(x, y, tileW, tileH));
                }
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
        const qreal phase = temporalFrame * speed * 0.025;
        for (int i = 0; i < count; ++i) {
            const qreal angle = phase + (kTwoPi * static_cast<qreal>(i) / static_cast<qreal>(count));
            const QPointF p(center.x() + std::cos(angle) * rx,
                            center.y() + std::sin(angle) * ry);
            addDraw(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
        }
    } else if (clip.effectPreset == ClipEffectPreset::FreezePattern) {
        const int columns = qBound(1, static_cast<int>(std::ceil(std::sqrt(static_cast<qreal>(count)))), 12);
        const int rows = qBound(1, static_cast<int>(std::ceil(static_cast<qreal>(count) / columns)), 12);
        const qreal cellW = outputRect.width() / static_cast<qreal>(columns);
        const qreal cellH = outputRect.height() / static_cast<qreal>(rows);
        const qreal tileH = std::max<qreal>(2.0, cellH * 0.86 * scale);
        const qreal tileW = std::max<qreal>(2.0, std::min(cellW * 0.92, tileH * aspect));
        const int activeStep = static_cast<int>(std::floor(temporalFrame * std::max<qreal>(0.1, std::abs(speed)) / 8.0));
        for (int i = 0; i < count; ++i) {
            const int column = i % columns;
            const int row = i / columns;
            if (row >= rows) {
                break;
            }
            const qreal holdJitter = static_cast<qreal>((activeStep + i * 3) % 5 - 2) * std::min(cellW, cellH) * 0.025;
            const qreal x = outputRect.left() + (column + 0.5) * cellW - tileW * 0.5 + holdJitter;
            const qreal y = outputRect.top() + (row + 0.5) * cellH - tileH * 0.5 - holdJitter;
            addDraw(QRectF(x, y, tileW, tileH));
        }
    } else if (clip.effectPreset == ClipEffectPreset::StepRepeat) {
        const qreal tileH = std::max<qreal>(
            4.0,
            std::min(outputRect.width(), outputRect.height()) *
                qBound<qreal>(0.03, 0.12 * scale, 0.75));
        const qreal tileW = std::max<qreal>(4.0, tileH * aspect);
        const qreal stepX = outputRect.width() / static_cast<qreal>(count + 1);
        const qreal stepY = outputRect.height() * 0.18;
        const int snappedStep = static_cast<int>(std::floor(temporalFrame * std::max<qreal>(0.1, std::abs(speed)) / 6.0));
        const qreal direction = speed < 0.0 ? -1.0 : 1.0;
        for (int i = 0; i < count; ++i) {
            const int sequenced = (snappedStep + i) % count;
            const qreal x = direction > 0.0
                ? outputRect.left() + (sequenced + 1) * stepX - tileW * 0.5
                : outputRect.right() - (sequenced + 1) * stepX - tileW * 0.5;
            const qreal y = outputRect.center().y() - tileH * 0.5 +
                            std::sin(static_cast<qreal>(i) * 1.57079632679) * stepY;
            addDraw(QRectF(x, y, tileW, tileH));
        }
    } else if (clip.effectPreset == ClipEffectPreset::StepRepeatFill) {
        const int columns = qBound(1, count, 96);
        const qreal tileW = std::max<qreal>(
            2.0, outputRect.width() / static_cast<qreal>(columns));
        const qreal tileH = std::max<qreal>(
            2.0, tileW / std::max<qreal>(0.001, aspect));
        const int rows = qBound(
            1,
            static_cast<int>(std::ceil(outputRect.height() / tileH)) + 1,
            96);
        const qreal guideScale = qBound<qreal>(0.5, clip.tilingSpacing, 8.0);
        const qreal lumaMatch = qBound<qreal>(0.0, clip.effectSpeed / 8.0, 1.0);
        const qreal hueMatch = qBound<qreal>(0.0, clip.effectScale / 8.0, 1.0);
        const qreal phase = std::fmod(temporalFrame * 0.012, 1.0);
        for (int row = 0; row < rows; ++row) {
            const qreal rowOffset =
                (clip.effectAlternateDirection && (row % 2)) ? tileW * 0.5 : 0.0;
            for (int column = -1; column <= columns; ++column) {
                VulkanEffectPipelinePlan::DrawPass pass;
                pass.outputRect = QRectF(outputRect.left() + column * tileW - rowOffset + phase * tileW,
                                         outputRect.top() + row * tileH,
                                         tileW,
                                         tileH);
                pass.shaderMode = kVulkanEffectModeStepRepeatFill;
                pass.effectParams[0] = static_cast<float>(guideScale);
                pass.effectParams[1] = static_cast<float>(lumaMatch);
                pass.effectParams[2] = static_cast<float>(hueMatch);
                pass.effectParams[3] = 0.0f;
                draws.push_back(pass);
            }
        }
    } else if (const float mosaicMode = sourceMosaicShaderMode(clip.effectPreset);
               mosaicMode >= 0.0f) {
        VulkanEffectPipelinePlan::DrawPass pass;
        pass.outputRect = outputRect;
        pass.shaderMode = mosaicMode;
        pass.effectParams[0] = static_cast<float>(qBound<qreal>(0.5, clip.tilingSpacing, 8.0));
        pass.effectParams[1] = static_cast<float>(qBound<qreal>(0.0, clip.effectSpeed / 8.0, 1.0));
        pass.effectParams[2] = static_cast<float>(qBound<qreal>(0.0, clip.effectScale / 8.0, 1.0));
        pass.effectParams[3] = static_cast<float>(qBound(1, clip.effectRows, 96));
        draws.push_back(pass);
    } else if (clip.effectPreset == ClipEffectPreset::DirectionalFrameEcho) {
        constexpr qreal kQuarterPi = 0.78539816339744830962;
        const qreal spread = qBound<qreal>(0.1, clip.tilingSpacing, 8.0);
        const qreal hueAmount = qBound<qreal>(0.1, clip.effectScale, 8.0);
        const qreal angle = speed * kQuarterPi;
        const QPointF direction(std::cos(angle), std::sin(angle));
        const qreal spreadPixels =
            std::max<qreal>(1.0, std::min(outputRect.width(), outputRect.height()) * 0.035 * spread);
        const qreal centerIndex = (static_cast<qreal>(count) - 1.0) * 0.5;
        const qreal maxDistance = std::max<qreal>(1.0, centerIndex);
        for (int i = 0; i < count; ++i) {
            const qreal offsetIndex = static_cast<qreal>(i) - centerIndex;
            VulkanEffectPipelinePlan::DrawPass pass;
            pass.outputRect = outputRect.translated(direction.x() * spreadPixels * offsetIndex,
                                                    direction.y() * spreadPixels * offsetIndex);
            pass.shaderMode = kVulkanEffectModeDirectionalFrameEcho;
            pass.effectParams[0] =
                static_cast<float>((offsetIndex * hueAmount) / 8.0);
            pass.effectParams[1] = static_cast<float>(count);
            pass.effectParams[2] = static_cast<float>(offsetIndex);
            pass.effectParams[3] = static_cast<float>(spread);
            pass.opacityMultiplier =
                static_cast<float>(qBound<qreal>(
                    0.18, 1.0 - 0.34 * std::abs(offsetIndex) / maxDistance, 1.0));
            pass.depthSortKey = -std::abs(offsetIndex);
            draws.push_back(pass);
        }
    } else if (clip.effectPreset == ClipEffectPreset::MirrorRing ||
               clip.effectPreset == ClipEffectPreset::Tessellation) {
        VulkanEffectPipelinePlan::DrawPass pass;
        pass.outputRect = outputRect;
        pass.shaderMode = clip.effectPreset == ClipEffectPreset::MirrorRing
            ? kVulkanEffectModeMirrorRing
            : kVulkanEffectModeTessellation;
        pass.effectParams[0] =
            static_cast<float>(qBound<qreal>(0.1, clip.effectScale, 8.0));
        pass.effectParams[1] =
            static_cast<float>(qBound(1, clip.effectRows, 96));
        pass.effectParams[2] = static_cast<float>(
            temporalFrame * qBound<qreal>(-8.0, clip.effectSpeed, 8.0));
        pass.effectParams[3] =
            static_cast<float>(qBound<qreal>(0.1, clip.tilingSpacing, 8.0));
        draws.push_back(pass);
    } else if (const float shaderMode = shaderModeForSinglePassPreset(clip.effectPreset);
               shaderMode >= 0.0f) {
        VulkanEffectPipelinePlan::DrawPass pass;
        pass.outputRect = outputRect;
        pass.shaderMode = shaderMode;
        pass.effectParams[0] = static_cast<float>(qBound<qreal>(0.1, clip.effectScale, 8.0));
        pass.effectParams[1] = static_cast<float>(qBound(1, clip.effectRows, 96));
        pass.effectParams[2] = static_cast<float>(temporalFrame * qBound<qreal>(-8.0, clip.effectSpeed, 8.0));
        pass.effectParams[3] = static_cast<float>(qBound<qreal>(0.1, clip.tilingSpacing, 8.0));
        if (isSpeakerMaskDilationPreset(clip.effectPreset)) {
            const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
            const std::shared_ptr<const TranscriptRuntimeDocument> document =
                loadTranscriptRuntimeDocument(transcriptPath);
            const int64_t sourceFrame = qMax<int64_t>(0, clip.sourceInFrame + qRound64(temporalFrame));
            const SpeakerProfile profile = document
                ? transcriptSpeakerProfileForSourceFrame(
                      transcriptPath, document->sections, sourceFrame)
                : SpeakerProfile{};
            const QColor colors[3] = {
                profile.primaryColor.isValid() ? profile.primaryColor : QColor(Qt::red),
                profile.secondaryColor.isValid() ? profile.secondaryColor : QColor(Qt::green),
                profile.accentColor.isValid() ? profile.accentColor : QColor(Qt::yellow)};
            for (int color = 0; color < 3; ++color) {
                pass.palette[color * 3] = colors[color].redF();
                pass.palette[color * 3 + 1] = colors[color].greenF();
                pass.palette[color * 3 + 2] = colors[color].blueF();
            }
        }
        draws.push_back(pass);
    } else if (clip.effectPreset == ClipEffectPreset::Vulkan3DSynth) {
        draws += vulkanSynth3DDrawPasses(VulkanSynth3DParams{
            .outputRect = outputRect,
            .sourceAspect = aspect,
            .copyCount = count,
            .scale = scale,
            .speed = speed,
            .timelineFrame = temporalFrame,
            .alternateHandedness = clip.effectAlternateDirection,
        });
    }

    if (maskedRepeatEnabled) {
        const TimelineClip::TransformKeyframe transform =
            evaluateClipTransformAtPosition(clip, static_cast<qreal>(clip.startFrame) + temporalFrame, timing);
        const qreal repeatX = qBound<qreal>(-100000.0, transform.maskRepeatDeltaX, 100000.0);
        const qreal repeatY = qBound<qreal>(-100000.0, transform.maskRepeatDeltaY, 100000.0);
        const bool hasRepeatStep = !qFuzzyIsNull(repeatX) || !qFuzzyIsNull(repeatY);
        const int repeatCount = hasRepeatStep ? count : 1;
        const qreal centerIndex = (static_cast<qreal>(repeatCount) - 1.0) * 0.5;
        for (int i = 0; i < repeatCount; ++i) {
            const qreal offsetIndex = static_cast<qreal>(i) - centerIndex;
            VulkanEffectPipelinePlan::DrawPass pass;
            pass.outputRect = outputRect.translated(repeatX * offsetIndex, repeatY * offsetIndex);
            pass.shaderMode = kVulkanEffectModeMaskGrade;
            draws.push_back(pass);
        }
    }

    if (!draws.isEmpty()) {
        plan.mode = VulkanEffectPipelinePlan::Mode::GeneratedDraws;
    }
    return plan;
}

QRectF normalizedMaskContentBounds(const jcut::core::ImageBuffer& mask,
                                   qreal outsidePixelsPercent,
                                   bool invert)
{
    if (mask.empty() || mask.format != jcut::core::PixelFormat::Gray8) {
        return {};
    }
    const int width = mask.size.width;
    const int height = mask.size.height;
    if (width <= 0 || height <= 0 || mask.strideBytes < width) {
        return {};
    }
    const qreal allowedOutsideFraction =
        qBound<qreal>(0.0, outsidePixelsPercent, 100.0) / 100.0;
    auto foregroundAt = [&](int x, int y) {
        const std::uint8_t value =
            mask.bytes[static_cast<std::size_t>(y * mask.strideBytes + x)];
        return invert ? value < 128 : value >= 128;
    };
    struct Component {
        QRect bounds;
        int area = 0;
    };
    std::vector<Component> components;
    int totalForegroundPixels = 0;
    std::vector<std::uint8_t> visited(
        static_cast<std::size_t>(width * height), 0);
    const QPoint offsets[4] = {
        QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)};
    std::queue<QPoint> queue;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            if (visited[static_cast<std::size_t>(index)] || !foregroundAt(x, y)) {
                continue;
            }
            visited[static_cast<std::size_t>(index)] = 1;
            QRect component(x, y, 1, 1);
            int area = 0;
            queue.push(QPoint(x, y));
            while (!queue.empty()) {
                const QPoint p = queue.front();
                queue.pop();
                ++area;
                component = component.united(QRect(p, QSize(1, 1)));
                for (const QPoint& offset : offsets) {
                    const int nx = p.x() + offset.x();
                    const int ny = p.y() + offset.y();
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const int nIndex = ny * width + nx;
                    if (visited[static_cast<std::size_t>(nIndex)] ||
                        !foregroundAt(nx, ny)) {
                        continue;
                    }
                    visited[static_cast<std::size_t>(nIndex)] = 1;
                    queue.push(QPoint(nx, ny));
                }
            }
            totalForegroundPixels += area;
            components.push_back(Component{component, area});
        }
    }
    if (components.empty() || totalForegroundPixels <= 0) {
        return {};
    }
    std::sort(components.begin(), components.end(),
              [](const Component& left, const Component& right) {
                  if (left.area != right.area) {
                      return left.area > right.area;
                  }
                  return left.bounds.topLeft().manhattanLength() <
                         right.bounds.topLeft().manhattanLength();
              });
    const qreal allowedOutsidePixels =
        static_cast<qreal>(totalForegroundPixels) * allowedOutsideFraction;
    QRect accepted;
    bool hasAccepted = false;
    int keptPixels = 0;
    for (const Component& component : components) {
        accepted = hasAccepted ? accepted.united(component.bounds)
                               : component.bounds;
        hasAccepted = true;
        keptPixels += component.area;
        const int outsidePixels = totalForegroundPixels - keptPixels;
        if (static_cast<qreal>(outsidePixels) <= allowedOutsidePixels) {
            break;
        }
    }
    if (!hasAccepted || accepted.isEmpty()) {
        return {};
    }
    return QRectF(
        static_cast<qreal>(accepted.left()) / static_cast<qreal>(width),
        static_cast<qreal>(accepted.top()) / static_cast<qreal>(height),
        static_cast<qreal>(accepted.width()) / static_cast<qreal>(width),
        static_cast<qreal>(accepted.height()) / static_cast<qreal>(height));
}

bool effectPresetUsesGeneratedMaskDomain(ClipEffectPreset preset)
{
    return isRecursiveZoomPreset(preset) ||
           preset == ClipEffectPreset::StepRepeatFill ||
           preset == ClipEffectPreset::SourceMosaicGrid ||
           preset == ClipEffectPreset::SourceMosaicStagger ||
           preset == ClipEffectPreset::SourceMosaicHex ||
           preset == ClipEffectPreset::SourceMosaicRadial ||
           preset == ClipEffectPreset::SourceMosaicFlow;
}

QRectF mappedMaskContentBounds(const jcut::core::ImageBuffer& mask,
                               const QTransform& clipToScreen,
                               const QRectF& localRect,
                               const QRectF& outputRect,
                               qreal outsidePixelsPercent,
                               bool invert)
{
    const QRectF normalized =
        normalizedMaskContentBounds(mask, outsidePixelsPercent, invert);
    if (normalized.isEmpty() || localRect.isEmpty() || outputRect.isEmpty()) {
        return {};
    }
    return clipToScreen
        .mapRect(PreviewViewTransform::localRectForNormalizedRect(
            normalized, localRect))
        .intersected(outputRect);
}

void applyGeneratedEffectMaskDomain(VulkanEffectPipelinePlan& plan,
                                    const QRectF& domainRect,
                                    const QRectF& outputRect,
                                    bool applyMaskMatte,
                                    const QRectF& maskDomain)
{
    if (!plan.usesGeneratedDraws() || domainRect.isEmpty() ||
        outputRect.isEmpty() || outputRect.width() <= 0.0 ||
        outputRect.height() <= 0.0) {
        return;
    }
    const QRectF boundedDomain = applyMaskMatte
        ? outputRect
        : domainRect.normalized().intersected(outputRect);
    if (boundedDomain.isEmpty()) {
        return;
    }
    const float x = static_cast<float>(
        qBound<qreal>(0.0,
                      (boundedDomain.left() - outputRect.left()) /
                          outputRect.width(),
                      1.0));
    const float y = static_cast<float>(
        qBound<qreal>(0.0,
                      (boundedDomain.top() - outputRect.top()) /
                          outputRect.height(),
                      1.0));
    const float width = static_cast<float>(
        qBound<qreal>(0.0001,
                      boundedDomain.width() / outputRect.width(),
                      1.0));
    const float height = static_cast<float>(
        qBound<qreal>(0.0001,
                      boundedDomain.height() / outputRect.height(),
                      1.0));
    const QRectF boundedMaskDomain = applyMaskMatte
        ? QRectF(0.0, 0.0, 1.0, 1.0)
        : maskDomain.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    const float maskX = static_cast<float>(
        qBound<qreal>(0.0, boundedMaskDomain.left(), 1.0));
    const float maskY = static_cast<float>(
        qBound<qreal>(0.0, boundedMaskDomain.top(), 1.0));
    const float maskWidth = static_cast<float>(
        qBound<qreal>(0.0001, boundedMaskDomain.width(), 1.0));
    const float maskHeight = static_cast<float>(
        qBound<qreal>(0.0001, boundedMaskDomain.height(), 1.0));
    for (VulkanEffectPipelinePlan::DrawPass& pass : plan.generatedDraws) {
        pass.effectDomain[0] = x;
        pass.effectDomain[1] = y;
        pass.effectDomain[2] = width;
        pass.effectDomain[3] = applyMaskMatte ? -height : height;
        pass.effectMaskDomain[0] = maskX;
        pass.effectMaskDomain[1] = maskY;
        pass.effectMaskDomain[2] = maskWidth;
        pass.effectMaskDomain[3] = maskHeight;
    }
}

QVector<QRectF> vulkanPresetEffectRects(const TimelineClip& clip,
                                        const QRectF& outputRect,
                                        const QSize& textureSize,
                                        qreal timelineFrame)
{
    return vulkanEffectPipelinePlan(clip, outputRect, textureSize, timelineFrame).generatedDrawRects();
}

qreal clipEffectPlaybackFramePosition(const TimelineClip& clip,
                                      const QVector<TimelineClip>& timelineClips,
                                      qreal timelineFramePosition,
                                      const QVector<TimelineTrack>& tracks)
{
    return clipEffectPlaybackFramePosition(
        clip, timelineClips, timelineFramePosition, activePlaybackTimingContext(), tracks);
}

qreal clipEffectPlaybackFramePosition(const TimelineClip& clip,
                                      const QVector<TimelineClip>& timelineClips,
                                      qreal timelineFramePosition,
                                      const PlaybackTimingContext& timing,
                                      const QVector<TimelineTrack>& tracks)
{
    const qreal localFrame =
        clip.effectSkipAwareTiming
            ? clipPlaybackFramePositionForTimelineFrame(clip, timelineFramePosition, timing)
            : qBound<qreal>(
                  0.0,
                  timelineFramePosition - static_cast<qreal>(clip.startFrame),
                  static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1)));
    if (timelineClips.isEmpty() || clip.id.trimmed().isEmpty()) {
        return localFrame;
    }

    QVector<const TimelineClip*> matchingClips;
    matchingClips.reserve(timelineClips.size());
    for (const TimelineClip& candidate : timelineClips) {
        const TimelineClip effectiveCandidate =
            tracks.isEmpty() ? candidate : clipWithTrackEffectSettings(candidate, tracks);
        if (effectiveCandidate.id.trimmed().isEmpty() ||
            !clipSharesEffectClock(clip, effectiveCandidate)) {
            continue;
        }
        matchingClips.push_back(&candidate);
    }
    std::sort(matchingClips.begin(),
              matchingClips.end(),
              [](const TimelineClip* a, const TimelineClip* b) {
                  if (a->startFrame == b->startFrame) {
                      return a->id < b->id;
                  }
                  return a->startFrame < b->startFrame;
              });

    qreal elapsed = 0.0;
    for (const TimelineClip* candidate : matchingClips) {
        if (candidate->id == clip.id) {
            return elapsed + localFrame;
        }
        if (candidate->startFrame < clip.startFrame) {
            elapsed += candidate->effectSkipAwareTiming
                           ? clipPlaybackDurationFrames(*candidate, timing)
                           : static_cast<qreal>(qMax<int64_t>(0, candidate->durationFrames));
        }
    }
    return localFrame;
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
    const float fullW = static_cast<float>(std::max(1, outputSize.width()));
    const float fullH = static_cast<float>(std::max(1, outputSize.height()));
    const float halfW = static_cast<float>(std::max<qreal>(1.0, rect.width())) * 0.5f;
    const float halfH = static_cast<float>(std::max<qreal>(1.0, rect.height())) * 0.5f;
    constexpr double kPi = 3.141592653589793238462643383279502884;
    const float radians = static_cast<float>(rotationDegrees * kPi / 180.0);
    const float cosTheta = std::cos(radians);
    const float sinTheta = std::sin(radians);
    const float scaleY = flipY ? -1.0f : 1.0f;
    const float m21 = -sinTheta * scaleY;
    const float m22 = cosTheta * scaleY;
    const float dx = static_cast<float>(rect.center().x());
    const float dy = static_cast<float>(rect.center().y());
    const float m[16] = {
        (2.0f * cosTheta * halfW) / fullW, (2.0f * sinTheta * halfW) / fullH, 0.f, 0.f,
        (2.0f * m21 * halfH) / fullW, (2.0f * m22 * halfH) / fullH, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        (2.0f * dx / fullW) - 1.0f,
        (2.0f * dy / fullH) - 1.0f,
        0.f,
        1.f
    };
    std::copy(std::begin(m), std::end(m), outMvp);
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

VulkanGradePayload vulkanGradePayloadForGrade(
    const TimelineClip::GradingKeyframe& grade)
{
    VulkanGradePayload payload;
    payload.effects = vulkanDrawEffectStateForGrade(grade);
    payload.curveLutApplied = gradingUsesCurveLut(grade);
    payload.curveLutRgba = vulkanCurveLutRgbaBytes(grade);
    return payload;
}

VulkanDrawEffectState vulkanBlurredBackgroundEffectState(float opacity)
{
    return vulkanBackgroundFillEffectState(BackgroundFillEffect::BlurCover,
                                           opacity);
}

VulkanDrawEffectState vulkanBackgroundFillEffectState(BackgroundFillEffect effect,
                                                      float opacity,
                                                      float brightness,
                                                      float saturation,
                                                      int edgePixels,
                                                      float edgePower,
                                                      const QRectF& validTextureRectNorm,
                                                      const VulkanBackgroundFillMapping& mapping)
{
    VulkanDrawEffectState state;
    state.opacity = std::clamp(opacity, 0.0f, 1.0f);
    state.brightness = std::clamp(brightness, -1.0f, 1.0f);
    state.contrast = 1.0f;
    state.saturation = std::clamp(saturation, 0.0f, 3.0f);
    if (effect == BackgroundFillEffect::EdgeStretch ||
        effect == BackgroundFillEffect::ProgressiveEdgeStretch ||
        effect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
        effect == BackgroundFillEffect::Tile ||
        effect == BackgroundFillEffect::Mirror) {
        const bool progressive = effect == BackgroundFillEffect::ProgressiveEdgeStretch;
        const bool bidirectional =
            effect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
        state.shadows[0] = mapping.centerXNorm;
        state.shadows[1] = mapping.centerYNorm;
        state.shadows[2] = mapping.outputHeightOverSourceWidth;
        state.shadows[3] = mapping.signedOutputHeightOverSourceHeight;
        state.midtones[0] = static_cast<float>(std::clamp(edgePixels, 1, 512));
        state.midtones[1] = mapping.rotationRadians;
        state.midtones[2] = std::clamp(edgePower, 0.25f, 8.0f);
        const QRectF validRect = validTextureRectNorm.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        state.highlights[0] = static_cast<float>(validRect.left());
        state.highlights[1] = static_cast<float>(validRect.top());
        state.highlights[2] = static_cast<float>(validRect.right());
        state.midtones[3] = static_cast<float>(validRect.bottom());
        state.highlights[3] = effect == BackgroundFillEffect::Tile
            ? kVulkanEffectModeBackgroundTile
            : effect == BackgroundFillEffect::Mirror
            ? kVulkanEffectModeBackgroundMirror
            : (bidirectional
                ? kVulkanEffectModeBackgroundProgressiveBidirectionalEdgeStretch
                : progressive
                ? kVulkanEffectModeBackgroundProgressiveEdgeStretch
                : kVulkanEffectModeBackgroundEdgeStretch);
        return state;
    }
    state.highlights[3] = kVulkanEffectModeBackgroundBlur;
    state.midtones[3] = -34.0f;
    return state;
}

VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QSize& outputSize)
{
    return vulkanBackgroundFillMapping(
        sourceToOutput, localRect, QRectF(QPointF(), QSizeF(outputSize)));
}

VulkanBackgroundFillMapping vulkanBackgroundFillMapping(
    const QTransform& sourceToOutput,
    const QRectF& localRect,
    const QRectF& outputRect)
{
    VulkanBackgroundFillMapping mapping;
    const qreal outputWidth = qMax<qreal>(1.0, outputRect.width());
    const qreal outputHeight = qMax<qreal>(1.0, outputRect.height());
    const QPointF localCenter = localRect.center();
    const QPointF center = sourceToOutput.map(localCenter);
    const QPointF xEdge = sourceToOutput.map(
        localCenter + QPointF(localRect.width() * 0.5, 0.0));
    const QPointF yEdge = sourceToOutput.map(
        localCenter + QPointF(0.0, localRect.height() * 0.5));
    const QPointF xAxis = xEdge - center;
    const QPointF yAxis = yEdge - center;
    const qreal determinant = (xAxis.x() * yAxis.y()) - (xAxis.y() * yAxis.x());
    mapping.centerXNorm = static_cast<float>((center.x() - outputRect.left()) / outputWidth);
    mapping.centerYNorm = static_cast<float>((center.y() - outputRect.top()) / outputHeight);
    const qreal sourceWidth = qMax<qreal>(0.0001, 2.0 * std::hypot(xAxis.x(), xAxis.y()));
    const qreal sourceHeight = qMax<qreal>(0.0001, 2.0 * std::hypot(yAxis.x(), yAxis.y()));
    mapping.outputHeightOverSourceWidth = static_cast<float>(outputHeight / sourceWidth);
    mapping.signedOutputHeightOverSourceHeight = static_cast<float>(
        std::copysign(outputHeight / sourceHeight,
                      determinant == 0.0 ? 1.0 : determinant));
    mapping.rotationRadians = static_cast<float>(std::atan2(xAxis.y(), xAxis.x()));
    return mapping;
}

} // namespace render_detail
