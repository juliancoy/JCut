#include "../vulkan_pipeline.h"
#include "../render_vulkan_shared.h"

#include <QtTest/QtTest>
#include <QFile>

#include <cstddef>

class VulkanDirectRenderParityTest : public QObject {
    Q_OBJECT

private slots:
    void sharedRenderStateBuildsVulkanPushValues()
    {
        TimelineClip::GradingKeyframe grade;
        grade.opacity = 0.5;
        grade.brightness = 0.125;
        grade.contrast = 1.25;
        grade.saturation = 0.75;
        grade.shadowsR = -0.25;
        grade.shadowsG = 0.125;
        grade.shadowsB = 0.375;
        grade.midtonesR = 0.5;
        grade.midtonesG = -0.125;
        grade.midtonesB = 0.25;
        grade.highlightsR = 0.75;
        grade.highlightsG = 0.625;
        grade.highlightsB = -0.5;

        const render_detail::VulkanDrawEffectState state =
            render_detail::vulkanDrawEffectStateForGrade(grade);

        QCOMPARE(state.opacity, 0.5f);
        QCOMPARE(state.brightness, 0.125f);
        QCOMPARE(state.contrast, 1.25f);
        QCOMPARE(state.saturation, 0.75f);
        QCOMPARE(state.shadows[0], -0.25f);
        QCOMPARE(state.shadows[1], 0.125f);
        QCOMPARE(state.shadows[2], 0.375f);
        QCOMPARE(state.shadows[3], 1.0f);
        QCOMPARE(state.midtones[0], 0.5f);
        QCOMPARE(state.midtones[1], -0.125f);
        QCOMPARE(state.midtones[2], 0.25f);
        QCOMPARE(state.highlights[0], 0.75f);
        QCOMPARE(state.highlights[1], 0.625f);
        QCOMPARE(state.highlights[2], -0.5f);
        QCOMPARE(state.highlights[3], 1.0f);
    }

    void sharedCurveLutHasShaderTextureShape()
    {
        const QByteArray lut = render_detail::vulkanIdentityCurveLutRgbaBytes();
        QCOMPARE(lut.size(), TimelineClip::kGradingCurveLutSize * 4);
        QCOMPARE(static_cast<unsigned char>(lut[0]), static_cast<unsigned char>(0));
        QCOMPARE(static_cast<unsigned char>(lut[3]), static_cast<unsigned char>(0));
        QCOMPARE(static_cast<unsigned char>(lut[lut.size() - 4]), static_cast<unsigned char>(255));
        QCOMPARE(static_cast<unsigned char>(lut[lut.size() - 1]), static_cast<unsigned char>(255));
    }

    void blurredFillBackgroundUsesShaderBlurSignal()
    {
        const render_detail::VulkanDrawEffectState state =
            render_detail::vulkanBlurredBackgroundEffectState(0.8f);

        QCOMPARE(state.opacity, 0.8f);
        QVERIFY2(state.midtones[3] < 0.0f,
                 "Blurred fill background must signal color blur without using the positive alpha-feather path.");
    }

    void pushConstantLayoutKeepsParityFlagsInPadding()
    {
        QCOMPARE(sizeof(VulkanPipeline::Push), size_t(128));
        QCOMPARE(offsetof(VulkanPipeline::Push, mvp), size_t(0));
        QCOMPARE(offsetof(VulkanPipeline::Push, brightness), size_t(64));
        QCOMPARE(offsetof(VulkanPipeline::Push, contrast), size_t(68));
        QCOMPARE(offsetof(VulkanPipeline::Push, saturation), size_t(72));
        QCOMPARE(offsetof(VulkanPipeline::Push, opacity), size_t(76));
        QCOMPARE(offsetof(VulkanPipeline::Push, shadows), size_t(80));
        QCOMPARE(offsetof(VulkanPipeline::Push, midtones), size_t(96));
        QCOMPARE(offsetof(VulkanPipeline::Push, highlights), size_t(112));
    }

    void directVulkanShaderRunsOpenGlGradeOrder()
    {
        QFile shader(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/effects.frag"));
        QVERIFY2(shader.open(QIODevice::ReadOnly), "Unable to open direct Vulkan effects shader.");
        const QString source = QString::fromUtf8(shader.readAll());

        QVERIFY2(source.contains(QStringLiteral("layout(set = 0, binding = 1) uniform sampler2D u_curve_lut")),
                 "Direct Vulkan presenter must bind the expected curve LUT resource.");
        QVERIFY2(source.contains(QStringLiteral("pc.u_shadows.a > 0.5")),
                 "Direct Vulkan presenter must use the push-constant curve-enabled flag.");
        QVERIFY2(source.contains(QStringLiteral("pc.u_midtones.a > 0.0")),
                 "Direct Vulkan presenter must use the push-constant mask-feather radius.");
        QVERIFY2(source.contains(QStringLiteral("pc.u_midtones.a < 0.0")),
                 "Direct Vulkan presenter must use the push-constant blurred-fill background signal.");
        QVERIFY2(source.contains(QStringLiteral("blurredFillSample")),
                 "Direct Vulkan presenter must blur the cover-fill background in shader.");

        const int shadowsPos = source.indexOf(QStringLiteral("rgb *= (1.0 + pc.u_shadows.rgb"));
        const int midtonesPos = source.indexOf(QStringLiteral("vec3 midtoneAdjust = pc.u_midtones.rgb"));
        const int highlightsPos = source.indexOf(QStringLiteral("rgb += pc.u_highlights.rgb"));
        const int curvePos = source.indexOf(QStringLiteral("texture(u_curve_lut"));
        const int contrastPos = source.indexOf(QStringLiteral("rgb = ((rgb - 0.5) * pc.u_contrast"));
        const int lumaRefreshPos = source.indexOf(QStringLiteral("float luma = lumaOf(rgb)"), contrastPos);
        const int saturationPos = source.indexOf(QStringLiteral("rgb = mix(vec3(luma), rgb, pc.u_saturation)"), lumaRefreshPos);

        QVERIFY2(shadowsPos >= 0, "Direct Vulkan shader must apply shadows.");
        QVERIFY2(midtonesPos > shadowsPos, "Direct Vulkan shader must apply midtones after shadows.");
        QVERIFY2(highlightsPos > midtonesPos, "Direct Vulkan shader must apply highlights after midtones.");
        QVERIFY2(curvePos > highlightsPos, "Direct Vulkan shader must apply curves after lift/gamma/gain.");
        QVERIFY2(contrastPos > curvePos, "Direct Vulkan shader must apply brightness/contrast after curves.");
        QVERIFY2(lumaRefreshPos > contrastPos, "Direct Vulkan shader must recompute luma after brightness/contrast.");
        QVERIFY2(saturationPos > lumaRefreshPos, "Direct Vulkan shader must apply saturation with refreshed luma.");
    }
};

QTEST_MAIN(VulkanDirectRenderParityTest)
#include "test_vulkan_direct_render_parity.moc"
