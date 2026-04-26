#include "editor_shared.h"

#include <QPainter>
#include <QPainterPath>
#include <QtGlobal>

#include <cmath>

namespace {
int clampChannel(int value) {
    return qBound(0, value, 255);
}

qreal catmullRom(qreal p0, qreal p1, qreal p2, qreal p3, qreal t) {
    const qreal t2 = t * t;
    const qreal t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

qreal effectTimelinePositionForClip(const TimelineClip& clip,
                                    qreal timelineFramePosition,
                                    const QVector<RenderSyncMarker>& markers) {
    if (!clip.transformSkipAwareTiming || markers.isEmpty()) {
        return timelineFramePosition;
    }

    const qreal maxLocalFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
    const qreal localTimelineFrame =
        qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxLocalFrame);
    const int64_t steppedLocalTimelineFrame =
        qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFrame)));
    const qreal fractional = localTimelineFrame - static_cast<qreal>(steppedLocalTimelineFrame);
    const int64_t adjustedLocalFrame =
        adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
    const qreal adjustedLocalFramePosition =
        qBound<qreal>(0.0, static_cast<qreal>(adjustedLocalFrame) + fractional, maxLocalFrame);
    return static_cast<qreal>(clip.startFrame) + adjustedLocalFramePosition;
}
}  // namespace

QVector<QPointF> defaultGradingCurvePoints() {
    return {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
}

QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points) {
    QVector<QPointF> normalized;
    normalized.reserve(points.size() + 2);
    for (const QPointF& point : points) {
        normalized.push_back(QPointF(qBound<qreal>(0.0, point.x(), 1.0),
                                     qBound<qreal>(0.0, point.y(), 1.0)));
    }
    std::sort(normalized.begin(), normalized.end(), [](const QPointF& a, const QPointF& b) {
        if (qFuzzyCompare(a.x() + 1.0, b.x() + 1.0)) {
            return a.y() < b.y();
        }
        return a.x() < b.x();
    });

    QVector<QPointF> deduped;
    deduped.reserve(normalized.size() + 2);
    for (const QPointF& point : normalized) {
        if (!deduped.isEmpty() && std::abs(deduped.constLast().x() - point.x()) <= 0.000001) {
            deduped.last().setY(point.y());
        } else {
            deduped.push_back(point);
        }
    }

    if (deduped.isEmpty() || deduped.constFirst().x() > 0.0 + 0.000001) {
        deduped.push_front(QPointF(0.0, 0.0));
    } else {
        deduped.first().setX(0.0);
        deduped.first().setY(qBound<qreal>(0.0, deduped.first().y(), 1.0));
    }
    if (deduped.constLast().x() < 1.0 - 0.000001) {
        deduped.push_back(QPointF(1.0, 1.0));
    } else {
        deduped.last().setX(1.0);
        deduped.last().setY(qBound<qreal>(0.0, deduped.last().y(), 1.0));
    }
    // Enforce a stable no-op baseline endpoint behavior.
    deduped.first().setY(0.0);
    deduped.last().setY(1.0);
    return deduped;
}

qreal sampleGradingCurveAt(const QVector<QPointF>& points, qreal xNorm, bool smoothingEnabled) {
    const QVector<QPointF> curve = sanitizeGradingCurvePoints(points);
    if (curve.size() < 2) {
        return qBound<qreal>(0.0, xNorm, 1.0);
    }
    const qreal x = qBound<qreal>(0.0, xNorm, 1.0);
    if (x <= curve.constFirst().x()) {
        return qBound<qreal>(0.0, curve.constFirst().y(), 1.0);
    }
    if (x >= curve.constLast().x()) {
        return qBound<qreal>(0.0, curve.constLast().y(), 1.0);
    }

    int right = 1;
    while (right < curve.size() && curve.at(right).x() < x) {
        ++right;
    }
    const int i1 = qBound(0, right - 1, curve.size() - 1);
    const int i2 = qBound(0, right, curve.size() - 1);
    const qreal x1 = curve.at(i1).x();
    const qreal x2 = curve.at(i2).x();
    const qreal denom = qMax<qreal>(0.000001, x2 - x1);
    const qreal t = qBound<qreal>(0.0, (x - x1) / denom, 1.0);
    if (!smoothingEnabled) {
        const qreal yLinear = curve.at(i1).y() + ((curve.at(i2).y() - curve.at(i1).y()) * t);
        return qBound<qreal>(0.0, yLinear, 1.0);
    }
    const int i0 = qMax(0, i1 - 1);
    const int i3 = qMin(curve.size() - 1, i2 + 1);
    const qreal y = catmullRom(curve.at(i0).y(), curve.at(i1).y(), curve.at(i2).y(), curve.at(i3).y(), t);
    return qBound<qreal>(0.0, y, 1.0);
}

QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points, int samples, bool smoothingEnabled) {
    const int count = qMax(2, samples);
    QVector<quint8> lut;
    lut.resize(count);
    const QVector<QPointF> curve = sanitizeGradingCurvePoints(points);
    for (int i = 0; i < count; ++i) {
        const qreal x = static_cast<qreal>(i) / static_cast<qreal>(count - 1);
        const qreal y = sampleGradingCurveAt(curve, x, smoothingEnabled);
        lut[i] = static_cast<quint8>(qBound(0, qRound(y * 255.0), 255));
    }
    return lut;
}

qreal evaluateEffectiveClipOpacityAtFrame(const TimelineClip& clip,
                                          const QVector<TimelineTrack>& tracks,
                                          int64_t timelineFrame) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtFrame(clip, timelineFrame);
}

qreal evaluateEffectiveClipOpacityAtPosition(const TimelineClip& clip,
                                             const QVector<TimelineTrack>& tracks,
                                             qreal timelineFramePosition) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtPosition(clip, timelineFramePosition);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip,
                                                                  const QVector<TimelineTrack>& tracks,
                                                                  int64_t timelineFrame) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtFrame(clip, timelineFrame);
    grade.opacity = evaluateEffectiveClipOpacityAtFrame(clip, tracks, timelineFrame);
    return grade;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtPosition(clip, timelineFramePosition);
    grade.opacity = evaluateEffectiveClipOpacityAtPosition(clip, tracks, timelineFramePosition);
    return grade;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    return evaluateEffectiveClipGradingAtFrame(clip, {}, timelineFrame);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    return evaluateEffectiveClipGradingAtPosition(clip, {}, timelineFramePosition);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtFrame(clip, tracks, timelineFrame);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelineFrame(clip, polygon, timelineFrame)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtPosition(clip, tracks, timelineFramePosition);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelinePosition(clip, polygon, timelineFramePosition)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame,
                                                             const QVector<RenderSyncMarker>& markers) {
    return evaluateEffectiveVisualEffectsAtPosition(
        clip, tracks, static_cast<qreal>(timelineFrame), markers);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers) {
    const qreal adjustedTimelinePosition =
        effectTimelinePositionForClip(clip, timelineFramePosition, markers);
    return evaluateEffectiveVisualEffectsAtPosition(clip, tracks, adjustedTimelinePosition);
}
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade) {
    const bool needsBasicGrade =
        !qFuzzyIsNull(grade.brightness) ||
        !qFuzzyCompare(grade.contrast, 1.0) ||
        !qFuzzyCompare(grade.saturation, 1.0) ||
        !qFuzzyCompare(grade.opacity, 1.0);
    const bool needsToneGrade =
        !qFuzzyIsNull(grade.shadowsR) || !qFuzzyIsNull(grade.shadowsG) || !qFuzzyIsNull(grade.shadowsB) ||
        !qFuzzyIsNull(grade.midtonesR) || !qFuzzyIsNull(grade.midtonesG) || !qFuzzyIsNull(grade.midtonesB) ||
        !qFuzzyIsNull(grade.highlightsR) || !qFuzzyIsNull(grade.highlightsG) || !qFuzzyIsNull(grade.highlightsB);

    const QVector<quint8> curveLutR =
        gradingCurveLut8(grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> curveLutG =
        gradingCurveLut8(grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> curveLutB =
        gradingCurveLut8(grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    const QVector<quint8> curveLutL =
        gradingCurveLut8(grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);

    if (source.isNull() || (!needsBasicGrade && !needsToneGrade &&
                            curveLutR.isEmpty() && curveLutG.isEmpty() && curveLutB.isEmpty())) {
        return source;
    }

    auto smoothShadows = [](float luma) { return std::pow(1.0f - luma, 2.0f); };
    auto smoothMidtones = [](float luma) { return 1.0f - std::abs(luma - 0.5f) * 2.0f; };
    auto smoothHighlights = [](float luma) { return std::pow(luma, 2.0f); };

    QImage graded = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < graded.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(graded.scanLine(y));
        for (int x = 0; x < graded.width(); ++x) {
            QColor color = QColor::fromRgba(row[x]);
            float h = 0.0f, s = 0.0f, l = 0.0f, a = 0.0f;
            color.getHslF(&h, &s, &l, &a);

            float rf = color.redF();
            float gf = color.greenF();
            float bf = color.blueF();

            // Calculate luminance for tone-based grading
            float luminance = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;

            // Apply Shadows (Lift)
            if (needsToneGrade) {
                float shadowWeight = smoothShadows(luminance);
                rf *= (1.0f + grade.shadowsR * shadowWeight);
                gf *= (1.0f + grade.shadowsG * shadowWeight);
                bf *= (1.0f + grade.shadowsB * shadowWeight);

                // Apply Midtones (Gamma)
                float midtoneWeight = smoothMidtones(luminance);
                rf = std::pow(rf, 1.0f / (1.0f + grade.midtonesR * midtoneWeight));
                gf = std::pow(gf, 1.0f / (1.0f + grade.midtonesG * midtoneWeight));
                bf = std::pow(bf, 1.0f / (1.0f + grade.midtonesB * midtoneWeight));

                // Apply Highlights (Gain)
                float highlightWeight = smoothHighlights(luminance);
                rf += grade.highlightsR * highlightWeight;
                gf += grade.highlightsG * highlightWeight;
                bf += grade.highlightsB * highlightWeight;
            }

            if (!curveLutR.isEmpty() && !curveLutG.isEmpty() && !curveLutB.isEmpty()) {
                const int ri = qBound(0, qRound(rf * 255.0f), 255);
                const int gi = qBound(0, qRound(gf * 255.0f), 255);
                const int bi = qBound(0, qRound(bf * 255.0f), 255);
                rf = static_cast<float>(curveLutR[ri]) / 255.0f;
                gf = static_cast<float>(curveLutG[gi]) / 255.0f;
                bf = static_cast<float>(curveLutB[bi]) / 255.0f;
                if (!curveLutL.isEmpty()) {
                    const int rmi = qBound(0, qRound(rf * 255.0f), 255);
                    const int gmi = qBound(0, qRound(gf * 255.0f), 255);
                    const int bmi = qBound(0, qRound(bf * 255.0f), 255);
                    rf = static_cast<float>(curveLutL[rmi]) / 255.0f;
                    gf = static_cast<float>(curveLutL[gmi]) / 255.0f;
                    bf = static_cast<float>(curveLutL[bmi]) / 255.0f;
                }
            }

            // Basic grading
            rf = qBound(0.0f, rf, 1.0f);
            gf = qBound(0.0f, gf, 1.0f);
            bf = qBound(0.0f, bf, 1.0f);

            int r = clampChannel(qRound(((rf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int g = clampChannel(qRound(((gf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int b = clampChannel(qRound(((bf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0f, static_cast<float>(s * grade.saturation), 1.0f);
            a = qBound(0.0f, static_cast<float>(a * grade.opacity), 1.0f);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip) {
    return applyClipGrade(source, evaluateEffectiveClipGradingAtFrame(clip, clip.startFrame));
}

QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects) {
    QImage output = applyClipGrade(source, effects.grading);
    if (!effects.correctionPolygons.isEmpty() && !output.isNull()) {
        output = output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.setBrush(Qt::black);
        painter.setPen(Qt::NoPen);
        const qreal width = qMax<qreal>(1.0, output.width());
        const qreal height = qMax<qreal>(1.0, output.height());
        for (const TimelineClip::CorrectionPolygon& polygon : effects.correctionPolygons) {
            if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
                continue;
            }
            QPainterPath path;
            const QPointF first(
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().x(), 1.0) * width,
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().y(), 1.0) * height);
            path.moveTo(first);
            for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
                const QPointF point(
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].x(), 1.0) * width,
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].y(), 1.0) * height);
                path.lineTo(point);
            }
            path.closeSubpath();
            painter.drawPath(path);
        }
        painter.end();
    }
    if (effects.maskFeather > 0.0) {
        output = applyMaskFeather(output, effects.maskFeather, effects.maskFeatherGamma);
    }
    return output;
}

QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma) {
    if (source.isNull() || featherRadius <= 0.0) {
        return source;
    }

    QImage feathered = source.convertToFormat(QImage::Format_ARGB32);
    const int radius = qRound(featherRadius);
    if (radius <= 0) {
        return source;
    }

    // Create a copy for reading
    const QImage sourceCopy = feathered.copy();
    const int width = feathered.width();
    const int height = feathered.height();

    // Box blur on the alpha channel with gamma curve
    const qreal gamma = qMax(0.01, featherGamma);
    for (int y = 0; y < height; ++y) {
        QRgb* destRow = reinterpret_cast<QRgb*>(feathered.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int alphaSum = 0;
            int pixelCount = 0;

            // Sample the box
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sampleY = qBound(0, y + dy, height - 1);
                const QRgb* srcRow = reinterpret_cast<const QRgb*>(sourceCopy.scanLine(sampleY));
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sampleX = qBound(0, x + dx, width - 1);
                    alphaSum += qAlpha(srcRow[sampleX]);
                    pixelCount++;
                }
            }

            // Box blur average
            const qreal blurredAlpha = static_cast<qreal>(alphaSum) / pixelCount / 255.0;
            // Apply gamma curve (1.0 = linear, <1.0 = sharper, >1.0 = softer)
            const qreal curvedAlpha = std::pow(blurredAlpha, 1.0 / gamma);
            const int newAlpha = qBound(0, qRound(curvedAlpha * 255.0), 255);
            const QRgb original = destRow[x];
            destRow[x] = qRgba(qRed(original), qGreen(original), qBlue(original), newAlpha);
        }
    }

    return feathered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
