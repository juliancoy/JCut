#include <QtTest/QtTest>

#include "../editor_shared_effects.h"
#include "../visual_effects_shader.h"

#include <QRegularExpression>
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
    void testVulkanMaskMorphShaderUsesMinMax();
    void testVulkanMaskBlurShaderIsSeparable();
    void testVulkanRenderersLoadRawMasksForGpuPreprocess();
    void testVulkanMaskComputeUsesDescriptorRings();
    void testVulkanShaderRecomputesLuminanceBeforeSaturation();
    void testNv12HandoffShaderStoresCanonicalRgba();
    void testCpuLumaCurvePreservesChroma();
    void testCpuMaskCurveGradesOnlyMaskedPixels();
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
    QVERIFY2(shader.contains(QStringLiteral("total / float(count)")),
             "Mask blur shader must average samples on GPU.");
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
    QVERIFY2(shader.contains(QStringLiteral("pc.chromaSwap")),
             "NV12 handoff shader must support decoder paths that expose VU chroma order.");
    QVERIFY2(shader.contains(QStringLiteral("uint chromaX = p.x & ~1u;")) &&
                 shader.contains(QStringLiteral("loadUV(chromaX + 1u, p.y)")),
             "NV12 handoff shader must read both bytes from each interleaved UV pair.");
    QVERIFY2(shader.contains(QStringLiteral("uint byteOffset = (y >> 1u) * uint(pc.uvPitch) + x;")),
             "NV12 UV byte loader must not mask away the second chroma byte.");
    QVERIFY2(shader.contains(QStringLiteral("1.402 * v")),
             "NV12 handoff shader must support full-range BT.601/SMPTE170M sources.");
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

QTEST_MAIN(TestShaderGradingLogic)
#include "test_shader_grading_logic.moc"
