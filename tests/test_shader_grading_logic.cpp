#include <QtTest/QtTest>

#include "../visual_effects_shader.h"

class TestShaderGradingLogic : public QObject {
    Q_OBJECT

private slots:
    void testGlShaderUsesSafePowForMidtones();
    void testGlShaderRecomputesLuminanceBeforeSaturation();
    void testVulkanShaderUsesSafePowForMidtones();
    void testVulkanShaderRecomputesLuminanceBeforeSaturation();
    void testNv12HandoffShaderStoresCanonicalRgba();
};

void TestShaderGradingLogic::testGlShaderUsesSafePowForMidtones()
{
    const QString shader = QString::fromUtf8(editor::visualEffectsFragmentShaderSource());
    QVERIFY2(shader.contains(QStringLiteral("pow(max(rgb, vec3(0.0))")),
             "GL shader must clamp RGB non-negative before pow for midtone gamma.");
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

QTEST_MAIN(TestShaderGradingLogic)
#include "test_shader_grading_logic.moc"
