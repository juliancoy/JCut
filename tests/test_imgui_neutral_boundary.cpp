#include "core/offscreen_vulkan_frame.h"
#include "editor_grading_core.h"
#include "editor_runtime.h"
#include "image_sequence_directory.h"
#include "imgui_audio_runtime.h"
#include "imgui_vulkan_frame_importer.h"
#include "standalone_preview_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using AudioSynchronizeMember =
    void (jcut::ImGuiAudioRuntime::*)(const jcut::EditorDocumentCore&,
                                      const std::string&);
using AudioStatusMember =
    jcut::ImGuiAudioStatus (jcut::ImGuiAudioRuntime::*)() const;
using FrameImportMember =
    bool (jcut::imgui::VulkanFrameImporter::*)(
        const render_detail::OffscreenVulkanFrame&,
        std::string*);
using PreviewRenderFunction =
    jcut::standalone_render::PreviewRenderResult (*)(
        const jcut::standalone_render::PreviewRenderRequest&);

static_assert(std::is_same_v<
              decltype(&jcut::ImGuiAudioRuntime::synchronize),
              AudioSynchronizeMember>);
static_assert(std::is_same_v<
              decltype(&jcut::ImGuiAudioRuntime::status),
              AudioStatusMember>);
static_assert(std::is_same_v<
              decltype(&jcut::imgui::VulkanFrameImporter::importFrame),
              FrameImportMember>);
static_assert(std::is_same_v<
              decltype(&jcut::standalone_render::renderPreviewFrame),
              PreviewRenderFunction>);
static_assert(std::is_same_v<
              decltype(render_detail::OffscreenVulkanFrame::queueFamilyIndex),
              std::uint32_t>);

} // namespace

int main()
{
    // Exercise the genuinely neutral implementation library. Adapter-backed
    // PIMPL types above are checked only through unevaluated member signatures.
    jcut::EditorRuntime runtime;
    const jcut::CommandResult renameResult =
        runtime.execute(jcut::SetProjectNameCommand{"neutral-boundary-smoke"});
    if (!renameResult.applied ||
        runtime.snapshot().projectName != "neutral-boundary-smoke") {
        std::cerr << "neutral EditorRuntime command path failed\n";
        return 1;
    }
    if (jcut::isImageSequenceDirectory(std::filesystem::path{})) {
        std::cerr << "empty media path was detected as an image sequence\n";
        return 2;
    }

    const jcut::ImGuiAudioStatus audioStatus;
    const render_detail::OffscreenVulkanFrame frame;
    const jcut::imgui::VulkanExternalImage externalImage;
    const jcut::standalone_render::PreviewRenderRequest previewRequest;
    const jcut::standalone_render::PreviewRenderResult previewResult;

    if (audioStatus.initialized || audioStatus.playbackActive || frame.valid ||
        externalImage.size.valid() || previewRequest.outputSize.valid() ||
        previewResult.success) {
        std::cerr << "neutral boundary defaults are not inert\n";
        return 3;
    }

    const auto nearlyEqual = [](double left, double right) {
        return std::abs(left - right) <= 0.000001;
    };
    jcut::EditorClip gradingClip;
    gradingClip.durationFrames = 101;
    gradingClip.brightness = -0.25;
    gradingClip.contrast = 1.25;
    gradingClip.saturation = 0.75;
    gradingClip.opacity = 0.6;
    const jcut::EditorGradingKeyframe baseGrade =
        jcut::evaluateEditorClipGradingAtLocalFrame(gradingClip, 25);
    if (baseGrade.frame != 25 || !nearlyEqual(baseGrade.brightness, -0.25) ||
        !nearlyEqual(baseGrade.opacity, 0.6) ||
        baseGrade.curvePointsR.size() != 2) {
        std::cerr << "neutral base grading evaluation failed\n";
        return 4;
    }

    jcut::EditorGradingKeyframe firstGrade;
    firstGrade.frame = 0;
    firstGrade.brightness = 0.0;
    firstGrade.shadowsR = -1.0;
    firstGrade.midtonesG = 0.0;
    firstGrade.highlightsB = 0.5;
    firstGrade.curvePointsR = {{0.0, 0.0}, {0.5, 0.2}, {1.0, 1.0}};
    firstGrade.curvePointsG = {{0.0, 0.1}, {1.0, 0.9}};
    firstGrade.curveThreePointLock = true;
    firstGrade.curveSmoothingEnabled = false;
    jcut::EditorGradingKeyframe lastGrade = firstGrade;
    lastGrade.frame = 100;
    lastGrade.brightness = 1.0;
    lastGrade.shadowsR = 1.0;
    lastGrade.midtonesG = 1.0;
    lastGrade.highlightsB = 1.5;
    lastGrade.curvePointsR = {{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}};
    // A different topology retains the previous curve during interpolation.
    lastGrade.curvePointsG = {{0.0, 0.0}, {0.4, 0.5}, {1.0, 1.0}};
    lastGrade.curveThreePointLock = false;
    lastGrade.curveSmoothingEnabled = true;
    gradingClip.gradingKeyframes = {lastGrade, firstGrade};
    gradingClip.opacityKeyframes = {{0, 0.2, true}, {100, 0.8, true}};

    const jcut::EditorGradingKeyframe midpoint =
        jcut::evaluateEditorClipGradingAtLocalFrame(gradingClip, 50);
    if (midpoint.frame != 50 || !nearlyEqual(midpoint.brightness, 0.5) ||
        !nearlyEqual(midpoint.shadowsR, 0.0) ||
        !nearlyEqual(midpoint.midtonesG, 0.5) ||
        !nearlyEqual(midpoint.highlightsB, 1.0) ||
        !nearlyEqual(midpoint.opacity, 0.5) ||
        midpoint.curvePointsR.size() != 3 ||
        !nearlyEqual(midpoint.curvePointsR[1].y, 0.5) ||
        midpoint.curvePointsG.size() != 2 ||
        !nearlyEqual(midpoint.curvePointsG.front().y, 0.1) ||
        !midpoint.curveThreePointLock || midpoint.curveSmoothingEnabled) {
        std::cerr << "neutral interpolated grading evaluation failed\n";
        return 5;
    }

    gradingClip.gradingKeyframes.front().linearInterpolation = false;
    const jcut::EditorGradingKeyframe held =
        jcut::evaluateEditorClipGradingAtLocalFrame(gradingClip, 50);
    if (!nearlyEqual(held.brightness, firstGrade.brightness) ||
        !nearlyEqual(held.shadowsR, firstGrade.shadowsR) ||
        !nearlyEqual(held.opacity, 0.5)) {
        std::cerr << "neutral held grading evaluation failed\n";
        return 6;
    }

    const jcut::EditorGradingKeyframe endpoint =
        jcut::evaluateEditorClipGradingAtLocalFrame(gradingClip, 500);
    if (endpoint.frame != 100 || !nearlyEqual(endpoint.brightness, 1.0) ||
        endpoint.curveThreePointLock ||
        !endpoint.curveSmoothingEnabled || !nearlyEqual(endpoint.opacity, 0.8)) {
        std::cerr << "neutral endpoint grading evaluation failed\n";
        return 7;
    }

    const std::vector<jcut::EditorPoint> sanitizedCurve =
        jcut::sanitizeEditorGradingCurve({
            {1.5, 4.0}, {0.5, -2.0}, {0.5000005, 0.25}, {-0.5, 0.4}});
    if (sanitizedCurve.size() != 3 ||
        !nearlyEqual(sanitizedCurve[0].x, 0.0) ||
        !nearlyEqual(sanitizedCurve[0].y, 0.4) ||
        !nearlyEqual(sanitizedCurve[1].x, 0.5) ||
        !nearlyEqual(sanitizedCurve[1].y, 0.25) ||
        !nearlyEqual(sanitizedCurve[2].x, 1.0) ||
        !nearlyEqual(sanitizedCurve[2].y, 2.0)) {
        std::cerr << "neutral grading curve sanitization failed\n";
        return 8;
    }
    const std::vector<jcut::EditorPoint> defaultCurve =
        jcut::sanitizeEditorGradingCurve({});
    const std::vector<jcut::EditorPoint> singletonCurve =
        jcut::sanitizeEditorGradingCurve({{0.75, 0.3}});
    if (defaultCurve.size() != 2 ||
        !nearlyEqual(defaultCurve.front().x, 0.0) ||
        !nearlyEqual(defaultCurve.back().y, 1.0) ||
        singletonCurve.size() != 2 ||
        !nearlyEqual(singletonCurve.front().x, 0.0) ||
        !nearlyEqual(singletonCurve.front().y, 0.3) ||
        !nearlyEqual(singletonCurve.back().x, 0.75) ||
        !nearlyEqual(singletonCurve.back().y, 0.3)) {
        std::cerr << "neutral grading curve minimum topology failed\n";
        return 9;
    }

    const std::vector<jcut::EditorPoint> toneCurve =
        jcut::editorThreePointCurveFromToneValues(1.2, -1.25, -1.6);
    const jcut::EditorToneValues recoveredTones =
        jcut::editorToneValuesFromThreePointCurve(toneCurve);
    if (toneCurve.size() != 3 || !nearlyEqual(toneCurve[0].y, 0.3) ||
        !nearlyEqual(toneCurve[1].y, 0.25) ||
        !nearlyEqual(toneCurve[2].y, 0.6) ||
        !nearlyEqual(recoveredTones.shadows, 1.2) ||
        !nearlyEqual(recoveredTones.midtones, -1.25) ||
        !nearlyEqual(recoveredTones.highlights, -1.6)) {
        std::cerr << "neutral grading tone mapping failed\n";
        return 10;
    }
    const jcut::EditorToneValues identityFallback =
        jcut::editorToneValuesFromThreePointCurve({{0.0, 0.2}});
    if (!nearlyEqual(identityFallback.shadows, 0.0) ||
        !nearlyEqual(identityFallback.midtones, 2.0) ||
        !nearlyEqual(identityFallback.highlights, 0.0)) {
        std::cerr << "neutral grading tone fallback diverged from Qt\n";
        return 11;
    }

    jcut::EditorGradingKeyframe synchronizedGrade;
    synchronizedGrade.shadowsR = 1.0;
    synchronizedGrade.midtonesR = -1.0;
    synchronizedGrade.highlightsR = -1.0;
    synchronizedGrade.shadowsG = 0.5;
    synchronizedGrade.midtonesG = 0.5;
    synchronizedGrade.highlightsG = -0.5;
    synchronizedGrade.shadowsB = 2.0;
    synchronizedGrade.midtonesB = -2.0;
    synchronizedGrade.highlightsB = -2.0;
    synchronizedGrade.curvePointsLuma = {{0.0, 0.2}, {1.0, 0.8}};
    jcut::synchronizeEditorThreePointGradingCurves(&synchronizedGrade);
    if (synchronizedGrade.curvePointsR.size() != 3 ||
        !nearlyEqual(synchronizedGrade.curvePointsR[0].y, 0.25) ||
        !nearlyEqual(synchronizedGrade.curvePointsR[1].y, 0.3) ||
        !nearlyEqual(synchronizedGrade.curvePointsR[2].y, 0.75) ||
        synchronizedGrade.curvePointsG.size() != 3 ||
        synchronizedGrade.curvePointsB.size() != 3 ||
        synchronizedGrade.curvePointsLuma.size() != 2 ||
        !nearlyEqual(synchronizedGrade.curvePointsLuma.front().y, 0.2) ||
        !nearlyEqual(synchronizedGrade.curvePointsLuma.back().y, 0.8)) {
        std::cerr << "neutral grading curve synchronization failed\n";
        return 12;
    }

    const std::vector<jcut::EditorPoint> lutCurve{
        {0.0, -0.25}, {0.3, 0.8}, {0.75, 0.2}, {1.0, 1.25}};
    const std::vector<std::uint8_t> smoothedLut =
        jcut::editorGradingCurveLut8(lutCurve);
    if (smoothedLut.size() !=
            static_cast<std::size_t>(jcut::kEditorGradingCurveLutSize) ||
        smoothedLut != jcut::editorGradingCurveLut8(lutCurve) ||
        smoothedLut.front() != 0 || smoothedLut.back() != 255) {
        std::cerr << "neutral grading LUT is not deterministic\n";
        return 13;
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<jcut::EditorPoint> boundedNanCurve =
        jcut::sanitizeEditorGradingCurve({{nan, nan}});
    const std::vector<std::uint8_t> boundedNanLut =
        jcut::editorGradingCurveLut8({{nan, nan}}, 16, true);
    if (boundedNanCurve.size() != 2 ||
        !nearlyEqual(boundedNanCurve.front().x, 0.0) ||
        !nearlyEqual(boundedNanCurve.front().y, -1.0) ||
        !nearlyEqual(boundedNanCurve.back().x, 1.0) ||
        !nearlyEqual(boundedNanCurve.back().y, -1.0) ||
        !std::all_of(boundedNanLut.begin(), boundedNanLut.end(),
                     [](std::uint8_t value) { return value == 0; })) {
        std::cerr << "neutral grading curve did not preserve Qt NaN bounds\n";
        return 14;
    }

    jcut::EditorGradingKeyframe normalizationInput;
    normalizationInput.frame = 37;
    normalizationInput.brightness = -0.2;
    normalizationInput.contrast = 1.4;
    normalizationInput.saturation = 0.7;
    normalizationInput.opacity = 0.65;
    normalizationInput.linearInterpolation = false;
    normalizationInput.shadowsR = -0.5;
    normalizationInput.midtonesG = 0.75;
    normalizationInput.highlightsB = 1.25;
    normalizationInput.curvePointsR = {{0.0, 0.0}, {1.0, 0.8}};
    normalizationInput.curvePointsG = {{0.0, 0.1}, {1.0, 0.9}};
    normalizationInput.curvePointsB = {{0.0, 0.0}, {1.0, 0.8}};
    normalizationInput.curvePointsLuma = {{0.0, 0.1}, {1.0, 1.0}};
    normalizationInput.curveThreePointLock = true;
    normalizationInput.curveSmoothingEnabled = true;
    const jcut::EditorGradingKeyframe normalizationOriginal =
        normalizationInput;
    const std::vector<std::uint8_t> originalLumaLut =
        jcut::editorGradingCurveLut8(
            normalizationOriginal.curvePointsLuma,
            jcut::kEditorGradingCurveLutSize,
            normalizationOriginal.curveSmoothingEnabled);
    jcut::normalizeEditorGradingCurves(normalizationInput);
    if (normalizationInput.frame != normalizationOriginal.frame ||
        !nearlyEqual(normalizationInput.brightness, normalizationOriginal.brightness) ||
        !nearlyEqual(normalizationInput.contrast, normalizationOriginal.contrast) ||
        !nearlyEqual(normalizationInput.saturation, normalizationOriginal.saturation) ||
        !nearlyEqual(normalizationInput.opacity, normalizationOriginal.opacity) ||
        normalizationInput.linearInterpolation != normalizationOriginal.linearInterpolation ||
        !nearlyEqual(normalizationInput.shadowsR, normalizationOriginal.shadowsR) ||
        !nearlyEqual(normalizationInput.midtonesG, normalizationOriginal.midtonesG) ||
        !nearlyEqual(normalizationInput.highlightsB, normalizationOriginal.highlightsB)) {
        std::cerr << "curve normalization changed non-curve grade fields\n";
        return 14;
    }
    if (normalizationInput.curveThreePointLock ||
        normalizationInput.curveSmoothingEnabled ||
        normalizationInput.curvePointsLuma.size() != 2 ||
        !nearlyEqual(normalizationInput.curvePointsLuma.front().x, 0.0) ||
        !nearlyEqual(normalizationInput.curvePointsLuma.front().y, 0.0) ||
        !nearlyEqual(normalizationInput.curvePointsLuma.back().x, 1.0) ||
        !nearlyEqual(normalizationInput.curvePointsLuma.back().y, 1.0) ||
        normalizationInput.curvePointsR.size() > 12 ||
        normalizationInput.curvePointsG.size() > 12 ||
        normalizationInput.curvePointsB.size() > 12) {
        std::cerr << "curve normalization did not bound/reset its curve state\n";
        return 15;
    }

    const auto composedLutError = [&originalLumaLut](
                                      const std::vector<jcut::EditorPoint>& original,
                                      const std::vector<jcut::EditorPoint>& normalized) {
        const std::vector<std::uint8_t> channelLut =
            jcut::editorGradingCurveLut8(
                original, jcut::kEditorGradingCurveLutSize, true);
        const std::vector<std::uint8_t> normalizedLut =
            jcut::editorGradingCurveLut8(
                normalized, jcut::kEditorGradingCurveLutSize, false);
        int maximumError = 0;
        for (std::size_t index = 0; index < channelLut.size(); ++index) {
            const std::uint8_t expected = originalLumaLut[channelLut[index]];
            maximumError = std::max(
                maximumError,
                std::abs(static_cast<int>(expected) -
                         static_cast<int>(normalizedLut[index])));
        }
        return maximumError;
    };
    if (composedLutError(
            normalizationOriginal.curvePointsR,
            normalizationInput.curvePointsR) > 1 ||
        composedLutError(
            normalizationOriginal.curvePointsG,
            normalizationInput.curvePointsG) > 1 ||
        composedLutError(
            normalizationOriginal.curvePointsB,
            normalizationInput.curvePointsB) > 1) {
        std::cerr << "curve normalization changed the composed RGB/Luma LUT\n";
        return 16;
    }

    const auto curvesEqual = [&nearlyEqual](
                                 const std::vector<jcut::EditorPoint>& left,
                                 const std::vector<jcut::EditorPoint>& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (!nearlyEqual(left[index].x, right[index].x) ||
                !nearlyEqual(left[index].y, right[index].y)) {
                return false;
            }
        }
        return true;
    };
    jcut::EditorGradingKeyframe repeatedNormalization = normalizationOriginal;
    jcut::normalizeEditorGradingCurves(repeatedNormalization);
    if (!curvesEqual(
            repeatedNormalization.curvePointsR,
            normalizationInput.curvePointsR) ||
        !curvesEqual(
            repeatedNormalization.curvePointsG,
            normalizationInput.curvePointsG) ||
        !curvesEqual(
            repeatedNormalization.curvePointsB,
            normalizationInput.curvePointsB)) {
        std::cerr << "curve normalization knot selection is not deterministic\n";
        return 17;
    }

    return 0;
}
