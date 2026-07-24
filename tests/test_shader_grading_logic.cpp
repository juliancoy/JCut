#include <QtTest/QtTest>

#include "../editor_shared_effects.h"
#include "../visual_effects_shader.h"
#include "mask_sidecar_test_utils.h"

#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

class TestShaderGradingLogic : public QObject {
    Q_OBJECT

private slots:
    void testGlShaderUsesSafePowForMidtones();
    void testGlShaderAppliesLumaCurveAsLumaScale();
    void testGlShaderRecomputesLuminanceBeforeSaturation();
    void testVulkanShaderUsesSafePowForMidtones();
    void testVulkanShaderAppliesLumaCurveAsLumaScale();
    void testVulkanShaderHasSeparateMaskCurveLut();
    void testVulkanMaskPreprocessShadersExist();
    void testVulkanMaskPrepareUsesBilinearResampling();
    void testVulkanMaskMorphShaderUsesMinMax();
    void testVulkanMaskBlurShaderIsSeparable();
    void testVulkanRenderersLoadRawMasksForGpuPreprocess();
    void testVulkanMaskComputeUsesDescriptorRings();
    void testVulkanShaderRecomputesLuminanceBeforeSaturation();
    void testNv12HandoffShaderStoresCanonicalRgba();
    void testCpuLumaCurvePreservesChroma();
    void testCpuMaskCurveGradesOnlyMaskedPixels();
    void testCpuMaskMatteCompositesOpacityAndShadow();
    void testDecodeOrdinalMaskWithoutFrameMapFailsClosed();
};

void TestShaderGradingLogic::testGlShaderUsesSafePowForMidtones()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSource());
    QVERIFY2(shader.contains(QStringLiteral("pow(max(rgb, vec3(0.0))")),
             "GL shader must clamp RGB non-negative before pow for midtone gamma.");
}

void TestShaderGradingLogic::testGlShaderAppliesLumaCurveAsLumaScale()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSource());
    QVERIFY2(shader.contains(QStringLiteral("float curveLuma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));")),
             "GL shader must compute luminance after RGB curves.");
    QVERIFY2(shader.contains(QStringLiteral("rgb *= remappedLuma / curveLuma;")),
             "GL shader must preserve chroma when applying the Brightness/Luma curve.");
    QVERIFY2(!shader.contains(QStringLiteral("rr = texture2D(u_curve_lut, vec2(clamp(rr")),
             "GL shader must not apply the luma curve independently to each RGB channel.");
}

void TestShaderGradingLogic::testGlShaderRecomputesLuminanceBeforeSaturation()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSource());
    const QString contrastLine =
        QStringLiteral("rgb = ((rgb - 0.5) * u_contrast) + 0.5 + vec3(u_brightness);");
    const QString lumaRefresh =
        QStringLiteral("luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));");
    const QString satMix = QStringLiteral("rgb = mix(vec3(luminance), rgb, u_saturation);");

    const int contrastPos = shader.indexOf(contrastLine);
    const int lumaPos = shader.indexOf(lumaRefresh, contrastPos);
    const int satPos = shader.indexOf(satMix, contrastPos);
    QVERIFY2(contrastPos >= 0, "GL shader contrast/brightness stage not found.");
    QVERIFY2(lumaPos > contrastPos, "GL shader must recompute luminance after contrast/brightness.");
    QVERIFY2(satPos > lumaPos, "GL shader saturation mix must use refreshed luminance.");
}

void TestShaderGradingLogic::testVulkanShaderUsesSafePowForMidtones()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSourceVulkan());
    QVERIFY2(shader.contains(QStringLiteral("pow(max(rgb, vec3(0.0))")),
             "Vulkan shader must clamp RGB non-negative before pow for midtone gamma.");
}

void TestShaderGradingLogic::testVulkanShaderAppliesLumaCurveAsLumaScale()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSourceVulkan());
    QVERIFY2(shader.contains(QStringLiteral("float curveLuma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));")),
             "Vulkan shader must compute luminance after RGB curves.");
    QVERIFY2(shader.contains(QStringLiteral("rgb *= remappedLuma / curveLuma;")),
             "Vulkan shader must preserve chroma when applying the Brightness/Luma curve.");
    QVERIFY2(!shader.contains(QStringLiteral("rr = texture(u_curve_lut, vec2(clamp(rr")),
             "Vulkan shader must not apply the luma curve independently to each RGB channel.");
}

void TestShaderGradingLogic::testVulkanShaderHasSeparateMaskCurveLut()
{
    QFile shaderFile(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/effects.frag"));
    QVERIFY2(shaderFile.open(QIODevice::ReadOnly), "Unable to open Vulkan effects shader.");
    const QString shader = QString::fromUtf8(shaderFile.readAll());

    QVERIFY2(shader.contains(QStringLiteral("layout(set = 0, binding = 3) uniform sampler2D u_mask_curve_lut;")),
             "Vulkan effects shader must bind a separate mask grade curve LUT.");
    QVERIFY2(shader.contains(QStringLiteral("texture(u_mask_curve_lut")),
             "Vulkan effects shader must sample the separate mask grade curve LUT.");
    QVERIFY2(shader.contains(QStringLiteral("bool maskCurveEnabled = maskOverlay && pc.u_midtones.a < -0.5;")),
             "Mask grade pass must explicitly select the mask curve LUT.");
}

void TestShaderGradingLogic::testVulkanMaskPreprocessShadersExist()
{
    for (const QString& shaderPath : {
             QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_prepare.comp"),
             QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_morph.comp"),
             QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_blur.comp"),
         }) {
        QFile shaderFile(shaderPath);
        QVERIFY2(shaderFile.open(QIODevice::ReadOnly),
                 qPrintable(QStringLiteral("Unable to open %1").arg(shaderPath)));
        const QString shader = QString::fromUtf8(shaderFile.readAll());
        QVERIFY2(shader.contains(QStringLiteral("layout(local_size_x = 16, local_size_y = 16")),
                 qPrintable(QStringLiteral("%1 must be a tiled compute shader.").arg(shaderPath)));
        QVERIFY2(shader.contains(QStringLiteral("writeonly uniform image2D u_output_mask")),
                 qPrintable(QStringLiteral("%1 must write the processed mask on GPU.").arg(shaderPath)));
    }
}

void TestShaderGradingLogic::testVulkanMaskPrepareUsesBilinearResampling()
{
    QFile shaderFile(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_prepare.comp"));
    QVERIFY2(shaderFile.open(QIODevice::ReadOnly), "Unable to open mask prepare shader.");
    const QString shader = QString::fromUtf8(shaderFile.readAll());

    QVERIFY2(shader.contains(QStringLiteral("bilinearMaskValue")) &&
                 shader.contains(QStringLiteral("mix(top, bottom, fraction.y)")),
             "Mask scaling must preserve continuous alpha with bilinear reconstruction.");
    QVERIFY2(shader.contains(QStringLiteral("- vec2(0.5)")),
             "Mask scaling must map between texel centers.");
}

void TestShaderGradingLogic::testVulkanMaskMorphShaderUsesMinMax()
{
    QFile shaderFile(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_morph.comp"));
    QVERIFY2(shaderFile.open(QIODevice::ReadOnly), "Unable to open mask morph shader.");
    const QString shader = QString::fromUtf8(shaderFile.readAll());

    QVERIFY2(shader.contains(QStringLiteral("max(value, sampleValue)")),
             "Mask dilate must be implemented as a GPU max filter.");
    QVERIFY2(shader.contains(QStringLiteral("min(value, sampleValue)")),
             "Mask erode must be implemented as a GPU min filter.");
    QVERIFY2(shader.contains(QStringLiteral("pc.radius")),
             "Mask morph shader must honor the requested radius.");
}

void TestShaderGradingLogic::testVulkanMaskBlurShaderIsSeparable()
{
    QFile shaderFile(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_blur.comp"));
    QVERIFY2(shaderFile.open(QIODevice::ReadOnly), "Unable to open mask blur shader.");
    const QString shader = QString::fromUtf8(shaderFile.readAll());

    QVERIFY2(shader.contains(QStringLiteral("pc.horizontal")),
             "Mask blur shader must expose horizontal/vertical passes.");
    QVERIFY2(shader.contains(QStringLiteral("exp(-0.5")) &&
                 shader.contains(QStringLiteral("total / weightTotal")),
             "Mask blur shader must use a normalized Gaussian kernel on GPU.");
}

void TestShaderGradingLogic::testVulkanRenderersLoadRawMasksForGpuPreprocess()
{
    for (const QString& sourcePath : {
             QStringLiteral(JCUT_SOURCE_DIR "/vulkan_preview_surface.cpp"),
             QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"),
         }) {
        QFile sourceFile(sourcePath);
        QVERIFY2(sourceFile.open(QIODevice::ReadOnly),
                 qPrintable(QStringLiteral("Unable to open %1").arg(sourcePath)));
        const QString source = QString::fromUtf8(sourceFile.readAll());
        QVERIFY2(source.contains(QStringLiteral("rawClipMaskImage")),
                 qPrintable(QStringLiteral("%1 must load raw masks for GPU preprocessing.").arg(sourcePath)));
        QVERIFY2(!source.contains(QStringLiteral("preparedClipMaskImage(clip")),
                 qPrintable(QStringLiteral("%1 must not CPU-prepare Vulkan masks.").arg(sourcePath)));
    }

    QFile exportSource(QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"));
    QVERIFY2(exportSource.open(QIODevice::ReadOnly), "Unable to open offscreen Vulkan renderer.");
    const QString exportText = QString::fromUtf8(exportSource.readAll());
    QVERIFY2(!exportText.contains(QStringLiteral("maskUpload = maskUpload.scaled")),
             "Vulkan export must not CPU-scale masks before GPU preprocessing.");
}

void TestShaderGradingLogic::testVulkanMaskComputeUsesDescriptorRings()
{
    QFile directHeader(QStringLiteral(JCUT_SOURCE_DIR "/vulkan_resources.h"));
    QFile directSource(QStringLiteral(JCUT_SOURCE_DIR "/vulkan_resources.cpp"));
    QFile exportSource(QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"));
    QVERIFY2(directHeader.open(QIODevice::ReadOnly), "Unable to open Vulkan resources header.");
    QVERIFY2(directSource.open(QIODevice::ReadOnly), "Unable to open Vulkan resources source.");
    QVERIFY2(exportSource.open(QIODevice::ReadOnly), "Unable to open offscreen Vulkan renderer.");

    const QString directHeaderText = QString::fromUtf8(directHeader.readAll());
    const QString directText = QString::fromUtf8(directSource.readAll());
    const QString exportText = QString::fromUtf8(exportSource.readAll());
    const QRegularExpression singleMutableSetMember(
        QStringLiteral("\\bVkDescriptorSet\\s+m_maskComputeDescriptorSet\\b"));

    QVERIFY2(!directHeaderText.contains(singleMutableSetMember),
             "Direct preview mask compute must not keep one mutable descriptor set for all passes.");
    QVERIFY2(!exportText.contains(singleMutableSetMember),
             "Vulkan export mask compute must not keep one mutable descriptor set for all passes.");
    QVERIFY2(directText.contains(QStringLiteral("convertToFormat(QImage::Format_RGBA8888)")) &&
                 !directText.contains(QStringLiteral("Format_RGBA8888_Premultiplied")),
             "Direct Vulkan media upload must preserve straight RGBA so grading runs before premultiplication.");

    QVERIFY2(directHeaderText.contains(QStringLiteral("kMaskComputeDescriptorSetCount = 128")),
             "Direct preview must keep enough mask compute descriptor sets for one recorded command buffer.");
    QVERIFY2(directHeaderText.contains(
                 QStringLiteral("std::array<VkDescriptorSet, kMaskComputeDescriptorSetCount> m_maskComputeDescriptorSets")),
             "Direct preview must store mask compute descriptor sets in a ring.");
    QVERIFY2(directText.contains(QStringLiteral(
                 "VkDescriptorSet computeDescriptorSet = m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex];")),
             "Direct preview must snapshot the descriptor set used by each mask compute dispatch.");
    QVERIFY2(directText.contains(QStringLiteral(
                 "m_maskComputeDescriptorSetIndex = (m_maskComputeDescriptorSetIndex + 1) % m_maskComputeDescriptorSets.size();")),
             "Direct preview must advance the mask compute descriptor ring per dispatch.");
    QVERIFY2(directText.contains(QStringLiteral("vkCmdBindDescriptorSets(commandBuffer")) &&
                 directText.contains(QStringLiteral("&computeDescriptorSet")),
             "Direct preview must bind the per-dispatch mask compute descriptor set.");

    QVERIFY2(exportText.contains(QStringLiteral("kMaskComputeDescriptorSetCount = 128")),
             "Vulkan export must keep enough mask compute descriptor sets for one recorded command buffer.");
    QVERIFY2(exportText.contains(
                 QStringLiteral("std::array<VkDescriptorSet, kMaskComputeDescriptorSetCount> m_maskComputeDescriptorSets")),
             "Vulkan export must store mask compute descriptor sets in a ring.");
    QVERIFY2(exportText.contains(QStringLiteral(
                 "computeDescriptorSet =\n        m_maskComputeDescriptorSets[m_maskComputeDescriptorSetIndex];")),
             "Vulkan export must snapshot the descriptor set used by each mask compute dispatch.");
    QVERIFY2(exportText.contains(QStringLiteral(
                 "(m_maskComputeDescriptorSetIndex + 1) % kMaskComputeDescriptorSetCount")),
             "Vulkan export must advance the mask compute descriptor ring per dispatch.");
    QVERIFY2(exportText.contains(QStringLiteral("vkCmdBindDescriptorSets(m_commandBuffer")) &&
                 exportText.contains(QStringLiteral("&computeDescriptorSet")),
             "Vulkan export must bind the per-dispatch mask compute descriptor set.");
}

void TestShaderGradingLogic::testVulkanShaderRecomputesLuminanceBeforeSaturation()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSourceVulkan());
    const QString contrastLine =
        QStringLiteral("rgb = ((rgb - 0.5) * p.u_contrast) + 0.5 + vec3(p.u_brightness);");
    const QString lumaRefresh =
        QStringLiteral("luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));");
    const QString satMix = QStringLiteral("rgb = mix(vec3(luminance), rgb, p.u_saturation);");

    const int contrastPos = shader.indexOf(contrastLine);
    const int lumaPos = shader.indexOf(lumaRefresh, contrastPos);
    const int satPos = shader.indexOf(satMix, contrastPos);
    QVERIFY2(contrastPos >= 0, "Vulkan shader contrast/brightness stage not found.");
    QVERIFY2(lumaPos > contrastPos, "Vulkan shader must recompute luminance after contrast/brightness.");
    QVERIFY2(satPos > lumaPos, "Vulkan shader saturation mix must use refreshed luminance.");
}

void TestShaderGradingLogic::testNv12HandoffShaderStoresCanonicalRgba()
{
    QFile shaderFile(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/nv12_buffer_to_rgba.comp"));
    QVERIFY2(shaderFile.open(QIODevice::ReadOnly), "Unable to open NV12 handoff shader.");
    const QString shader = QString::fromUtf8(shaderFile.readAll());

    QVERIFY2(shader.contains(QStringLiteral("imageStore(outImage, ivec2(p), vec4(rgb, 1.0));")),
             "NV12 handoff shader must write canonical RGBA after YUV conversion.");
    QVERIFY2(!shader.contains(QStringLiteral("vec4(rgb.g, rgb.r, rgb.b")),
             "NV12 handoff shader must not swap red and green channels.");
    QVERIFY2(shader.contains(QStringLiteral("pc.fullRange")),
             "NV12 handoff shader must honor full-range source metadata.");
    QVERIFY2(shader.contains(QStringLiteral("pc.colorMatrix")),
             "NV12 handoff shader must honor source color-matrix metadata.");
    QVERIFY2(shader.contains(QStringLiteral("pc.colorMatrix == 2")) &&
                 shader.contains(QStringLiteral("1.4746 * v")),
             "NV12 handoff shader must support BT.2020 non-constant-luminance sources.");
    QVERIFY2(shader.contains(QStringLiteral("pc.chromaSwap")),
             "NV12 handoff shader must support decoder paths that expose VU chroma order.");
    QVERIFY2(shader.contains(QStringLiteral("uint chromaX = p.x & ~1u;")) &&
                 shader.contains(QStringLiteral("loadUV(chromaX + 1u, p.y)")),
             "NV12 handoff shader must read both bytes from each interleaved UV pair.");
    QVERIFY2(shader.contains(QStringLiteral("uint byteOffset = (y >> 1u) * uint(pc.uvPitch) + x;")),
             "NV12 UV byte loader must not mask away the second chroma byte.");
    QVERIFY2(shader.contains(QStringLiteral("1.402 * v")),
             "NV12 handoff shader must support full-range BT.601/SMPTE170M sources.");
    QVERIFY2(
        shader.contains(QStringLiteral("applyBaseGrade(")) &&
            shader.contains(QStringLiteral("applyTonalGrade(")) &&
            shader.contains(QStringLiteral("applyCurves(")) &&
            shader.contains(QStringLiteral("pc.brightness")) &&
            shader.contains(QStringLiteral("pc.contrast")) &&
            shader.contains(QStringLiteral("pc.saturation")) &&
            shader.contains(QStringLiteral("pc.shadowsR")) &&
            shader.contains(QStringLiteral("pc.midtonesG")) &&
            shader.contains(QStringLiteral("pc.highlightsB")) &&
            shader.indexOf(QStringLiteral("nv12ToRgb(yValue")) <
                shader.indexOf(QStringLiteral(
                    "imageStore(outImage")),
        "NV12 hardware handoff must apply neutral tonal, curve, and base BCS grading "
        "on GPU after color conversion and before storing canonical RGBA.");
    const int tonalCall = shader.indexOf(
        QStringLiteral("applyTonalGrade(\n        nv12ToRgb"));
    const int baseCall = shader.indexOf(
        QStringLiteral("applyBaseGrade(applyCurves(applyTonalGrade("));
    QVERIFY2(tonalCall >= 0 && baseCall >= 0,
             "NV12 handoff must apply tonal and curve grading before base BCS grading.");
    QVERIFY2(
        shader.contains(QStringLiteral(
            "max(rgb, vec3(0.0))")) &&
            shader.contains(QStringLiteral(
                "max(vec3(0.01), vec3(1.0) + midtones * midtoneWeight)")),
        "NV12 tonal midtones must use the same safe-pow bounds as CPU grading.");
    QVERIFY2(
        shader.contains(QStringLiteral(
            "layout(std430, binding = 3) readonly buffer CurveLut")) &&
            shader.contains(QStringLiteral(
                "curveByte(redIndex, 0u)")) &&
            shader.contains(QStringLiteral(
                "curveByte(greenIndex, 8u)")) &&
            shader.contains(QStringLiteral(
                "curveByte(blueIndex, 16u)")) &&
            shader.contains(QStringLiteral(
                "float mappedLuma = curveByte(lumaIndex, 24u);")) &&
            shader.contains(QStringLiteral(
                "rgb *= mappedLuma / mappedInput;")),
        "NV12 direct grading must apply independent RGB LUTs followed by "
        "the compositor's chroma-preserving luma LUT.");
    const int tonalStage = shader.indexOf(
        QStringLiteral("applyTonalGrade("));
    const int curveStage = shader.indexOf(
        QStringLiteral("applyCurves(applyTonalGrade("));
    const int baseStage = shader.indexOf(
        QStringLiteral("applyBaseGrade(applyCurves("));
    QVERIFY2(
        tonalStage >= 0 && curveStage >= 0 && baseStage >= 0,
        "NV12 grading stages must be nested in tonal, curve, then base order.");
}

void TestShaderGradingLogic::testCpuLumaCurvePreservesChroma()
{
    QImage source(1, 1, QImage::Format_ARGB32);
    source.fill(qRgba(24, 42, 180, 255));

    TimelineClip::GradingKeyframe grade;
    grade.curvePointsR = defaultGradingCurvePoints();
    grade.curvePointsG = defaultGradingCurvePoints();
    grade.curvePointsB = defaultGradingCurvePoints();
    grade.curvePointsLuma = QVector<QPointF>{{0.0, 0.18}, {1.0, 1.0}};
    grade.curveSmoothingEnabled = false;

    const QImage graded = applyClipGrade(source, grade).convertToFormat(QImage::Format_ARGB32);
    const QColor color = QColor::fromRgba(graded.pixel(0, 0));

    QVERIFY2(color.blue() > color.red(),
             qPrintable(QStringLiteral("Luma curve desaturated blue pixel: r=%1 g=%2 b=%3")
                            .arg(color.red())
                            .arg(color.green())
                            .arg(color.blue())));
    QVERIFY2(color.blue() > color.green(),
             qPrintable(QStringLiteral("Luma curve desaturated blue pixel: r=%1 g=%2 b=%3")
                            .arg(color.red())
                            .arg(color.green())
                            .arg(color.blue())));
}

void TestShaderGradingLogic::testCpuMaskCurveGradesOnlyMaskedPixels()
{
    QTemporaryDir maskDir;
    QVERIFY2(maskDir.isValid(), "Unable to create temporary mask directory.");

    QImage mask(2, 1, QImage::Format_Grayscale8);
    mask.scanLine(0)[0] = 255;
    mask.scanLine(0)[1] = 0;
    QVERIFY2(mask.save(maskDir.filePath(QStringLiteral("frame_000001.png"))),
             "Unable to write temporary mask frame.");

    QImage source(2, 1, QImage::Format_ARGB32);
    source.setPixel(0, 0, qRgba(64, 80, 160, 255));
    source.setPixel(1, 0, qRgba(64, 80, 160, 255));

    TimelineClip clip;
    clip.mediaType = ClipMediaType::Video;
    clip.maskEnabled = true;
    clip.maskFramesDir = maskDir.path();
    clip.maskOpacity = 1.0;
    clip.maskGradeEnabled = true;
    clip.maskGradeCurvePointsR = defaultGradingCurvePoints();
    clip.maskGradeCurvePointsG = defaultGradingCurvePoints();
    clip.maskGradeCurvePointsB = defaultGradingCurvePoints();
    clip.maskGradeCurvePointsLuma = QVector<QPointF>{{0.0, 0.25}, {1.0, 1.0}};
    clip.maskGradeCurveSmoothingEnabled = false;

    TimelineClip::GradingKeyframe normalGrade;
    normalGrade.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {1.0, 0.75}};
    normalGrade.curvePointsG = defaultGradingCurvePoints();
    normalGrade.curvePointsB = defaultGradingCurvePoints();
    normalGrade.curvePointsLuma = defaultGradingCurvePoints();
    normalGrade.curveSmoothingEnabled = false;

    const QImage normalOnly = applyClipGrade(source, normalGrade).convertToFormat(QImage::Format_ARGB32);
    const QImage masked = applyClipMaskEffectsToImage(source, clip, 0, normalGrade)
                              .convertToFormat(QImage::Format_ARGB32);

    const QColor maskedPixel = QColor::fromRgba(masked.pixel(0, 0));
    const QColor unmaskedPixel = QColor::fromRgba(masked.pixel(1, 0));
    const QColor normalPixel = QColor::fromRgba(normalOnly.pixel(1, 0));

    QVERIFY2(maskedPixel != normalPixel,
             "Mask curve grading did not change the fully masked pixel.");
    QCOMPARE(unmaskedPixel, normalPixel);
}

void TestShaderGradingLogic::testCpuMaskMatteCompositesOpacityAndShadow()
{
    QTemporaryDir maskDir;
    QVERIFY(maskDir.isValid());
    QImage mask(5, 1, QImage::Format_Grayscale8);
    mask.fill(0);
    mask.scanLine(0)[1] = 255;
    QVERIFY(mask.save(maskDir.filePath(QStringLiteral("frame_000001.png"))));

    QImage source(5, 1, QImage::Format_ARGB32);
    source.fill(qRgba(240, 220, 200, 255));
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Video;
    clip.clipRole = ClipRole::MaskMatte;
    clip.maskEnabled = true;
    clip.maskFramesDir = maskDir.path();
    clip.maskOpacity = 0.5;
    clip.maskDropShadowEnabled = true;
    clip.maskDropShadowRadius = 0.0;
    clip.maskDropShadowOffsetX = 2.0;
    clip.maskDropShadowOffsetY = 0.0;
    clip.maskDropShadowOpacity = 0.5;

    const QImage result = applyClipMaskEffectsToImage(
        source, clip, 0, TimelineClip::GradingKeyframe{}).convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(qAlpha(result.pixel(0, 0)), 0);
    QVERIFY(qAlpha(result.pixel(1, 0)) >= 126 && qAlpha(result.pixel(1, 0)) <= 129);
    QVERIFY(qAlpha(result.pixel(3, 0)) >= 126 && qAlpha(result.pixel(3, 0)) <= 129);
    QCOMPARE(qRed(result.pixel(3, 0)), 0);
    QCOMPARE(qGreen(result.pixel(3, 0)), 0);
    QCOMPARE(qBlue(result.pixel(3, 0)), 0);
}

void TestShaderGradingLogic::testDecodeOrdinalMaskWithoutFrameMapFailsClosed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString maskDirectory =
        root.filePath(QStringLiteral("shot_birefnet_alpha_masks"));
    QVERIFY(QDir().mkpath(maskDirectory));
    const QString sourcePath = root.filePath(QStringLiteral("shot.mp4"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.write("source-a");
    sourceFile.close();

    QImage mask(2, 1, QImage::Format_Grayscale8);
    mask.fill(255);
    QVERIFY(mask.save(QDir(maskDirectory).filePath(
        QStringLiteral("frame_000001.png"))));

    TimelineClip clip;
    clip.mediaType = ClipMediaType::Video;
    clip.filePath = sourcePath;
    clip.maskEnabled = true;
    clip.maskFramesDir = maskDirectory;
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QFile frameMap(QDir(maskDirectory).filePath(
        QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(frameMap.open(QIODevice::WriteOnly));
    frameMap.write("# source_frame\tmask_frame\n0\t0\n");
    frameMap.close();
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(
        maskDirectory, sourcePath));
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        maskDirectory, sourcePath, true, false));
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        maskDirectory, sourcePath, true, true));
    const QImage initialMask = rawClipMaskImage(clip, 0);
    QVERIFY(!initialMask.isNull());
    QCOMPARE(initialMask.convertToFormat(QImage::Format_Grayscale8)
                 .constScanLine(0)[0],
             uchar{255});
    QImage updatedMask(2, 1, QImage::Format_Grayscale8);
    updatedMask.fill(0);
    QVERIFY(updatedMask.save(QDir(maskDirectory).filePath(
        QStringLiteral("frame_000001.png"))));
    const QImage reloadedMask = rawClipMaskImage(clip, 0);
    QVERIFY(!reloadedMask.isNull());
    QCOMPARE(reloadedMask.convertToFormat(QImage::Format_Grayscale8)
                 .constScanLine(0)[0],
             uchar{0});
    QVERIFY(rawClipMaskImage(clip, 1).isNull());

    // Durable binding is content-based: a byte-identical copy at a different
    // path/inode remains the same source after its full digest is verified.
    const QString copiedSourcePath = root.filePath(QStringLiteral("copied.mp4"));
    QFile copiedSource(copiedSourcePath);
    QVERIFY(copiedSource.open(QIODevice::WriteOnly));
    copiedSource.write("source-a");
    copiedSource.close();
    clip.filePath = copiedSourcePath;
    QVERIFY(!rawClipMaskImage(clip, 0).isNull());

    const QString otherSourcePath = root.filePath(QStringLiteral("other.mp4"));
    QFile otherSource(otherSourcePath);
    QVERIFY(otherSource.open(QIODevice::WriteOnly));
    otherSource.write("source-b"); // Same size, different content.
    otherSource.close();
    clip.filePath = otherSourcePath;
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    // A map makes the sidecar ordinal regardless of its directory name. It
    // must not bypass completion gating, and schema-less legacy completion is
    // not sufficient to bind the artifact to this exact source/map pair.
    const QString customDirectory =
        root.filePath(QStringLiteral("shot_custom_ai_subject_mattes"));
    QVERIFY(QDir().mkpath(customDirectory));
    QVERIFY(mask.save(QDir(customDirectory).filePath(
        QStringLiteral("frame_000001.png"))));
    QFile customMap(QDir(customDirectory).filePath(
        QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(customMap.open(QIODevice::WriteOnly));
    customMap.write("# source_frame\tmask_frame\n0\t0\n");
    customMap.close();
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(
        customDirectory, sourcePath));
    clip.filePath = sourcePath;
    clip.maskFramesDir = customDirectory;
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QVERIFY(mask_sidecar_test::writeJson(
        QDir(customDirectory).filePath(QStringLiteral("jcut_alpha.json")),
        QJsonObject{
            {QStringLiteral("source_type"),
             QStringLiteral("birefnet_continuous_alpha")},
            {QStringLiteral("frame_domain"), QStringLiteral("decode_ordinal")},
        }));
    QVERIFY(rawClipMaskImage(clip, 0).isNull());

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        customDirectory, sourcePath, true, true));
    QVERIFY(!rawClipMaskImage(clip, 0).isNull());
}

QTEST_MAIN(TestShaderGradingLogic)
#include "test_shader_grading_logic.moc"
