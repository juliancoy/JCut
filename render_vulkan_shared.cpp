#include "render_vulkan_shared.h"

#include "background_fill_effect.h"
#include "editor_shared_effects.h"
#include "editor_shared_keyframes.h"
#include "editor_shared_transcript.h"
#include "transform_skip_aware_timing.h"
#include "vulkan_effect_synth.h"

#include <QMatrix4x4>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace render_detail {
namespace {

float shaderModeForSinglePassPreset(ClipEffectPreset preset)
{
    switch (preset) {
    case ClipEffectPreset::Kaleidoscope: return kVulkanEffectModeKaleidoscope;
    case ClipEffectPreset::HexagonalPrism: return kVulkanEffectModeHexagonalPrism;
    case ClipEffectPreset::Droste: return kVulkanEffectModeDroste;
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

QVector<QRectF> VulkanEffectPipelinePlan::generatedDrawRects() const
{
    QVector<QRectF> rects;
    rects.reserve(generatedDraws.size());
    for (const DrawPass& pass : generatedDraws) {
        rects.push_back(pass.outputRect);
    }
    return rects;
}

bool vulkanClipSupportsProgressiveEdgeStretchSource(const TimelineClip& clip)
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

VulkanProgressiveEdgeStretchLayerPolicy vulkanProgressiveEdgeStretchLayerPolicy(
    const TimelineClip& clip,
    const QVector<TimelineTrack>& tracks)
{
    VulkanProgressiveEdgeStretchLayerPolicy policy;
    const TimelineClip effectiveClip =
        tracks.isEmpty() ? clip : clipWithTrackEffectSettings(clip, tracks);
    policy.presetActive =
        effectiveClip.effectPreset == ClipEffectPreset::ProgressiveEdgeStretch;
    policy.sourceEligible = vulkanClipSupportsProgressiveEdgeStretchSource(clip);
    policy.drawBackground = policy.presetActive && policy.sourceEligible;
    return policy;
}

VulkanEffectPipelinePlan vulkanEffectPipelinePlan(const TimelineClip& clip,
                                                  const QRectF& outputRect,
                                                  const QSize& textureSize,
                                                  qreal timelineFrame,
                                                  qreal effectFrame,
                                                  const PlaybackTimingContext& timing)
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
        const qreal spacing = qBound<qreal>(0.1, clip.tilingSpacing, 8.0);
        const qreal minDimension = std::max<qreal>(1.0, std::min(outputRect.width(), outputRect.height()));
        const qreal baseTileW = outputRect.width() / static_cast<qreal>(qBound(1, count, 96));
        const qreal tileW = std::max<qreal>(2.0, baseTileW * scale);
        const qreal tileH = std::max<qreal>(2.0, tileW / std::max<qreal>(0.001, aspect));
        const qreal stepX = std::max<qreal>(1.0, tileW * spacing);
        const qreal stepY = std::max<qreal>(1.0, tileH * spacing);
        const qreal phaseX = std::fmod(temporalFrame * speed * tileW * 0.015, stepX);
        const qreal phaseY = std::fmod(temporalFrame * speed * tileH * 0.006, stepY);
        const qreal normalizedPhaseX = phaseX < 0.0 ? phaseX + stepX : phaseX;
        const qreal normalizedPhaseY = phaseY < 0.0 ? phaseY + stepY : phaseY;

        if (clip.tilingPattern == ClipTilingPattern::Encircle) {
            const QPointF center = outputRect.center();
            const qreal radius = minDimension * 0.34 * spacing;
            const qreal phase = temporalFrame * speed * 0.018;
            for (int i = 0; i < count; ++i) {
                const qreal angle = phase + (kTwoPi * static_cast<qreal>(i) / static_cast<qreal>(count));
                const QPointF p(center.x() + std::cos(angle) * radius,
                                center.y() + std::sin(angle) * radius);
                addDraw(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
            }
        } else if (clip.tilingPattern == ClipTilingPattern::SpiralXY ||
                   clip.tilingPattern == ClipTilingPattern::SpiralXZ ||
                   clip.tilingPattern == ClipTilingPattern::SpiralYZ) {
            const QPointF center = outputRect.center();
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
                    y = center.y() + ((t - 0.5) * outputRect.height() * 0.68);
                    sizeMultiplier = qBound<qreal>(0.45, 0.76 + (v / std::max<qreal>(1.0, maxRadius)) * 0.34, 1.18);
                } else if (clip.tilingPattern == ClipTilingPattern::SpiralYZ) {
                    x = center.x() + ((t - 0.5) * outputRect.width() * 0.68);
                    y = center.y() + u;
                    sizeMultiplier = qBound<qreal>(0.45, 0.76 + (v / std::max<qreal>(1.0, maxRadius)) * 0.34, 1.18);
                }
                const qreal drawW = tileW * sizeMultiplier;
                const qreal drawH = tileH * sizeMultiplier;
                addDraw(QRectF(x - drawW * 0.5, y - drawH * 0.5, drawW, drawH));
            }
        } else if (clip.tilingPattern == ClipTilingPattern::Diamond) {
            const QPointF center = outputRect.center();
            const int rings = qBound(1, static_cast<int>(std::ceil(std::sqrt(static_cast<qreal>(count)))), 10);
            int emitted = 0;
            addDraw(QRectF(center.x() - tileW * 0.5, center.y() - tileH * 0.5, tileW, tileH));
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
                    addDraw(QRectF(p.x() - tileW * 0.5, p.y() - tileH * 0.5, tileW, tileH));
                    if (++emitted >= count) {
                        break;
                    }
                }
            }
        } else {
            const int columns = qBound(1, count, 96);
            const qreal startY = clip.tilingWrap ? outputRect.top() - tileH + normalizedPhaseY : outputRect.top();
            const qreal endY = clip.tilingWrap ? outputRect.bottom() + tileH : outputRect.bottom() - tileH + 1.0;
            int row = 0;
            for (qreal y = startY; y < endY; y += stepY, ++row) {
                const qreal rowOffset =
                    (clip.effectAlternateDirection && (row % 2)) ? stepX * 0.5 : 0.0;
                const qreal startX = clip.tilingWrap
                    ? outputRect.left() - tileW + normalizedPhaseX - rowOffset
                    : outputRect.left() - rowOffset;
                const qreal endX = clip.tilingWrap
                    ? outputRect.right() + tileW
                    : outputRect.left() + (columns * stepX);
                for (qreal x = startX; x < endX; x += stepX) {
                    addDraw(QRectF(x, y, tileW, tileH));
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
    } else if (clip.effectPreset == ClipEffectPreset::MirrorRing ||
               clip.effectPreset == ClipEffectPreset::Tessellation) {
        VulkanEffectPipelinePlan::DrawPass pass;
        pass.outputRect = outputRect;
        pass.shaderMode = clip.effectPreset == ClipEffectPreset::MirrorRing
            ? kVulkanEffectModeMirrorRing
            : kVulkanEffectModeTessellation;
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
                                                      const QRectF& validTextureRectNorm,
                                                      const VulkanBackgroundFillMapping& mapping)
{
    VulkanDrawEffectState state;
    state.opacity = std::clamp(opacity, 0.0f, 1.0f);
    state.brightness = std::clamp(baseEffects.brightness + brightness, -1.0f, 1.0f);
    state.contrast = std::clamp(baseEffects.contrast, 0.0f, 4.0f);
    state.saturation = std::clamp(baseEffects.saturation * saturation, 0.0f, 3.0f);
    if (effect == BackgroundFillEffect::EdgeStretch ||
        effect == BackgroundFillEffect::ProgressiveEdgeStretch ||
        effect == BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch ||
        effect == BackgroundFillEffect::Tile ||
        effect == BackgroundFillEffect::Mirror) {
        Q_UNUSED(progressiveEdge);
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
