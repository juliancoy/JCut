#pragma once

#include "editor_document_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jcut {

struct EditorToneValues {
    double shadows = 0.0;
    double midtones = 0.0;
    double highlights = 0.0;
};

inline constexpr int kEditorGradingCurveLutSize = 256;

namespace grading_detail {

inline double qtBound(double minimum, double value, double maximum)
{
    // qBound(min, NaN, max) resolves to min through qMin/qMax. Keep the
    // neutral implementation total for JSON and ImGui text-entry inputs while
    // retaining the Qt editor's exact edge semantics.
    if (std::isnan(value)) {
        return minimum;
    }
    return std::max(minimum, std::min(maximum, value));
}

} // namespace grading_detail

inline std::vector<EditorPoint> sanitizeEditorGradingCurve(
    const std::vector<EditorPoint>& points)
{
    const auto fuzzyXEqual = [](double left, double right) {
        // Match qFuzzyCompare(left + 1.0, right + 1.0), as used by the Qt
        // grading implementation. The offset keeps zero-valued coordinates
        // on the relative comparison path.
        const double adjustedLeft = left + 1.0;
        const double adjustedRight = right + 1.0;
        return std::abs(adjustedLeft - adjustedRight) * 1.0e12 <=
            std::min(std::abs(adjustedLeft), std::abs(adjustedRight));
    };
    const auto pointLess = [&fuzzyXEqual](const EditorPoint& left,
                                          const EditorPoint& right) {
        if (fuzzyXEqual(left.x, right.x)) {
            return left.y < right.y;
        }
        return left.x < right.x;
    };

    std::vector<EditorPoint> normalized;
    normalized.reserve(points.size() + 2);
    for (const EditorPoint& point : points) {
        normalized.push_back({
            grading_detail::qtBound(0.0, point.x, 1.0),
            grading_detail::qtBound(-1.0, point.y, 2.0)});
    }
    std::sort(normalized.begin(), normalized.end(), pointLess);

    std::vector<EditorPoint> deduplicated;
    deduplicated.reserve(normalized.size() + 2);
    for (const EditorPoint& point : normalized) {
        if (!deduplicated.empty() &&
            std::abs(deduplicated.back().x - point.x) <= 0.000001) {
            deduplicated.back().y = point.y;
        } else {
            deduplicated.push_back(point);
        }
    }

    if (deduplicated.empty()) {
        return {{0.0, 0.0}, {1.0, 1.0}};
    }
    if (deduplicated.size() == 1) {
        const double extraX = deduplicated.front().x < 0.5 ? 1.0 : 0.0;
        deduplicated.push_back({extraX, deduplicated.front().y});
        std::sort(deduplicated.begin(), deduplicated.end(), pointLess);
    }
    return deduplicated;
}

namespace grading_detail {

inline double catmullRom(double p0,
                         double p1,
                         double p2,
                         double p3,
                         double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

} // namespace grading_detail

inline double sampleEditorGradingCurveAt(
    const std::vector<EditorPoint>& points,
    double xNorm,
    bool smoothingEnabled = true)
{
    const std::vector<EditorPoint> curve =
        sanitizeEditorGradingCurve(points);
    if (curve.size() < 2) {
        return grading_detail::qtBound(0.0, xNorm, 1.0);
    }

    const double x = grading_detail::qtBound(0.0, xNorm, 1.0);
    if (x <= curve.front().x) {
        return grading_detail::qtBound(0.0, curve.front().y, 1.0);
    }
    if (x >= curve.back().x) {
        return grading_detail::qtBound(0.0, curve.back().y, 1.0);
    }

    std::size_t right = 1;
    while (right < curve.size() && curve[right].x < x) {
        ++right;
    }
    const std::size_t i1 = right - 1;
    const std::size_t i2 = std::min(right, curve.size() - 1);
    const double x1 = curve[i1].x;
    const double x2 = curve[i2].x;
    const double denominator = std::max(0.000001, x2 - x1);
    const double t = grading_detail::qtBound(
        0.0, (x - x1) / denominator, 1.0);
    if (!smoothingEnabled) {
        const double linear =
            curve[i1].y + ((curve[i2].y - curve[i1].y) * t);
        return grading_detail::qtBound(0.0, linear, 1.0);
    }

    const std::size_t i0 = i1 > 0 ? i1 - 1 : 0;
    const std::size_t i3 = std::min(curve.size() - 1, i2 + 1);
    return grading_detail::qtBound(
        0.0,
        grading_detail::catmullRom(
            curve[i0].y, curve[i1].y, curve[i2].y, curve[i3].y, t),
        1.0);
}

inline std::vector<std::uint8_t> editorGradingCurveLut8(
    const std::vector<EditorPoint>& points,
    int samples = kEditorGradingCurveLutSize,
    bool smoothingEnabled = true)
{
    const int count = std::max(2, samples);
    std::vector<std::uint8_t> lut(static_cast<std::size_t>(count));
    const std::vector<EditorPoint> curve =
        sanitizeEditorGradingCurve(points);
    for (int index = 0; index < count; ++index) {
        const double x = static_cast<double>(index) /
            static_cast<double>(count - 1);
        const double y = sampleEditorGradingCurveAt(
            curve, x, smoothingEnabled);
        // sampleEditorGradingCurveAt() clamps to [0, 1], so this is the
        // positive-input equivalent of Qt's qRound(y * 255.0).
        const int quantized = std::clamp(
            static_cast<int>((y * 255.0) + 0.5), 0, 255);
        lut[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(quantized);
    }
    return lut;
}

inline std::vector<EditorPoint> composeEditorGradingCurves(
    const std::vector<EditorPoint>& channel,
    const std::vector<EditorPoint>& luma,
    bool smoothingEnabled,
    int samples = kEditorGradingCurveLutSize)
{
    const int count = std::max(2, samples);
    const std::vector<std::uint8_t> channelLut =
        editorGradingCurveLut8(channel, count, smoothingEnabled);
    const std::vector<std::uint8_t> lumaLut =
        editorGradingCurveLut8(luma, count, smoothingEnabled);
    std::vector<EditorPoint> composed;
    composed.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        // The definitive 256-sample LUT uses the byte value directly as its
        // next lookup index. Scale that byte only for smaller diagnostic LUTs.
        const int channelValue = std::clamp(
            (static_cast<int>(channelLut[static_cast<std::size_t>(index)]) *
                 (count - 1) +
             127) /
                255,
            0,
            count - 1);
        composed.push_back({
            static_cast<double>(index) / static_cast<double>(count - 1),
            static_cast<double>(lumaLut[static_cast<std::size_t>(channelValue)]) /
                255.0,
        });
    }
    return composed;
}

inline std::vector<EditorPoint> simplifyEditorGradingCurve(
    const std::vector<EditorPoint>& points,
    bool smoothingEnabled,
    int maximumPoints = 12,
    int samples = kEditorGradingCurveLutSize,
    int maximumLutError = 1)
{
    const int count = std::max(2, samples);
    const int pointLimit = std::clamp(maximumPoints, 2, count);
    const int errorLimit = std::max(0, maximumLutError);
    const std::vector<EditorPoint> sanitized =
        sanitizeEditorGradingCurve(points);
    const std::vector<std::uint8_t> target =
        editorGradingCurveLut8(sanitized, count, smoothingEnabled);
    std::vector<int> knots{0, count - 1};
    std::vector<EditorPoint> candidate;
    while (static_cast<int>(knots.size()) <= pointLimit) {
        candidate.clear();
        candidate.reserve(knots.size());
        for (const int index : knots) {
            candidate.push_back({
                static_cast<double>(index) / static_cast<double>(count - 1),
                static_cast<double>(target[static_cast<std::size_t>(index)]) /
                    255.0,
            });
        }

        const std::vector<std::uint8_t> approximation =
            editorGradingCurveLut8(candidate, count, smoothingEnabled);
        int worstIndex = -1;
        int worstError = 0;
        for (int index = 1; index < count - 1; ++index) {
            const int error = std::abs(
                static_cast<int>(target[static_cast<std::size_t>(index)]) -
                static_cast<int>(approximation[static_cast<std::size_t>(index)]));
            if (error > worstError) {
                worstError = error;
                worstIndex = index;
            }
        }
        if (worstError <= errorLimit ||
            static_cast<int>(knots.size()) == pointLimit ||
            worstIndex < 0) {
            return candidate;
        }
        knots.push_back(worstIndex);
        std::sort(knots.begin(), knots.end());
    }
    return candidate;
}

inline void normalizeEditorGradingCurves(
    EditorGradingKeyframe& keyframe,
    int maximumPoints = 12)
{
    const std::vector<EditorPoint> luma =
        sanitizeEditorGradingCurve(keyframe.curvePointsLuma);
    keyframe.curvePointsR = simplifyEditorGradingCurve(
        composeEditorGradingCurves(
            keyframe.curvePointsR, luma, keyframe.curveSmoothingEnabled),
        false,
        maximumPoints);
    keyframe.curvePointsG = simplifyEditorGradingCurve(
        composeEditorGradingCurves(
            keyframe.curvePointsG, luma, keyframe.curveSmoothingEnabled),
        false,
        maximumPoints);
    keyframe.curvePointsB = simplifyEditorGradingCurve(
        composeEditorGradingCurves(
            keyframe.curvePointsB, luma, keyframe.curveSmoothingEnabled),
        false,
        maximumPoints);
    keyframe.curvePointsLuma = {{0.0, 0.0}, {1.0, 1.0}};
    keyframe.curveSmoothingEnabled = false;
    keyframe.curveThreePointLock = false;
}

inline std::vector<EditorPoint> editorThreePointCurveFromToneValues(
    double shadows,
    double midtones,
    double highlights)
{
    return sanitizeEditorGradingCurve({
        {0.0, grading_detail::qtBound(0.0, shadows * 0.25, 1.0)},
        {0.5, grading_detail::qtBound(
                  0.0, 0.5 + (midtones * 0.20), 1.0)},
        {1.0, grading_detail::qtBound(
                  0.0, 1.0 + (highlights * 0.25), 1.0)},
    });
}

inline EditorToneValues editorToneValuesFromThreePointCurve(
    const std::vector<EditorPoint>& points)
{
    std::vector<EditorPoint> sanitized =
        sanitizeEditorGradingCurve(points);
    if (sanitized.size() < 3) {
        sanitized = {{0.0, 0.0}, {1.0, 1.0}};
    }

    const double shadows = (sanitized.front().y - 0.0) / 0.25;
    const double midtones =
        (sanitized[sanitized.size() / 2].y - 0.5) / 0.20;
    const double highlights = (sanitized.back().y - 1.0) / 0.25;
    return {
        grading_detail::qtBound(-2.0, shadows, 2.0),
        grading_detail::qtBound(-2.0, midtones, 2.0),
        grading_detail::qtBound(-2.0, highlights, 2.0),
    };
}

inline void synchronizeEditorThreePointGradingCurves(
    EditorGradingKeyframe& keyframe)
{
    keyframe.curvePointsR = editorThreePointCurveFromToneValues(
        keyframe.shadowsR, keyframe.midtonesR, keyframe.highlightsR);
    keyframe.curvePointsG = editorThreePointCurveFromToneValues(
        keyframe.shadowsG, keyframe.midtonesG, keyframe.highlightsG);
    keyframe.curvePointsB = editorThreePointCurveFromToneValues(
        keyframe.shadowsB, keyframe.midtonesB, keyframe.highlightsB);
}

inline void synchronizeEditorThreePointGradingCurves(
    EditorGradingKeyframe* keyframe)
{
    if (keyframe) {
        synchronizeEditorThreePointGradingCurves(*keyframe);
    }
}

} // namespace jcut
