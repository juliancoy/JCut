#include "../vulkan_pipeline.h"
#include "../background_fill_effect.h"
#include "../render_vulkan_shared.h"
#include "../preview_view_transform.h"

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
        QCOMPARE(state.shadows[3], render_detail::kVulkanEffectModeNormal);
        QCOMPARE(state.midtones[0], 0.5f);
        QCOMPARE(state.midtones[1], -0.125f);
        QCOMPARE(state.midtones[2], 0.25f);
        QCOMPARE(state.highlights[0], 0.75f);
        QCOMPARE(state.highlights[1], 0.625f);
        QCOMPARE(state.highlights[2], -0.5f);
        QCOMPARE(state.highlights[3], 1.0f);

        grade.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {1.0, 0.8}};
        const render_detail::VulkanDrawEffectState curveState =
            render_detail::vulkanDrawEffectStateForGrade(grade);
        QCOMPARE(curveState.shadows[3], render_detail::kVulkanEffectModeCurve);
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

    void backgroundFillEffectsUseSelectableShaderSignals()
    {
        render_detail::VulkanDrawEffectState baseState;
        baseState.brightness = 0.12f;
        baseState.contrast = 1.3f;
        baseState.saturation = 0.7f;
        const render_detail::VulkanDrawEffectState edgeState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::EdgeStretch, baseState, 0.8f, -0.02f, 1.5f, 24, false, 2.5f);
        QCOMPARE(edgeState.opacity, 0.8f);
        QVERIFY(qAbs(edgeState.brightness - 0.1f) < 0.0001f);
        QCOMPARE(edgeState.contrast, 1.3f);
        QVERIFY(qAbs(edgeState.saturation - 1.05f) < 0.0001f);
        QCOMPARE(edgeState.midtones[0], 24.0f);
        QCOMPARE(edgeState.midtones[1], 0.0f);
        QCOMPARE(edgeState.midtones[2], 2.5f);
        QCOMPARE(edgeState.shadows[0], 0.5f);
        QCOMPARE(edgeState.shadows[1], 0.5f);
        QCOMPARE(edgeState.shadows[2], 0.5f);
        QCOMPARE(edgeState.shadows[3], 0.5f);
        QVERIFY2(edgeState.highlights[3] < -1.5f,
                 "Edge-stretch background fill must signal row-wise edge sampling.");

        const render_detail::VulkanDrawEffectState progressiveEdgeState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::ProgressiveEdgeStretch,
                baseState,
                0.8f,
                -0.02f,
                1.5f,
                24,
                false,
                2.5f,
                QRectF(0.0, 0.0, 1.0, 1.0));
        QCOMPARE(progressiveEdgeState.midtones[0], 24.0f);
        QCOMPARE(progressiveEdgeState.midtones[1], 0.0f);
        QCOMPARE(progressiveEdgeState.midtones[2], 2.5f);
        QVERIFY2(progressiveEdgeState.highlights[3] < -2.5f &&
                     progressiveEdgeState.highlights[3] > -3.5f,
                 "Progressive edge stretch must have its own background fill mode signal.");

        const render_detail::VulkanDrawEffectState mirrorState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::Mirror, baseState, 0.8f, -0.02f, 1.5f, 24, true, 2.5f);
        QCOMPARE(mirrorState.shadows[0], 0.5f);
        QCOMPARE(mirrorState.shadows[1], 0.5f);
        QCOMPARE(mirrorState.shadows[2], 0.5f);
        QCOMPARE(mirrorState.shadows[3], 0.5f);
        QVERIFY2(mirrorState.highlights[3] < -3.5f,
                 "Mirror background fill must signal reflected source sampling.");

        const render_detail::VulkanDrawEffectState blurState =
            render_detail::vulkanBlurredBackgroundEffectState(0.8f);

        QCOMPARE(blurState.opacity, 0.8f);
        QVERIFY2(blurState.highlights[3] < -0.5f && blurState.highlights[3] > -1.5f,
                 "Blurred fill background must signal blur cover mode.");
        QVERIFY2(blurState.midtones[3] < 0.0f,
                 "Blurred fill background must carry a negative shader blur radius.");
    }

    void backgroundFillMappingTracksAffineTransform()
    {
        QTransform transform;
        transform.translate(640.0, 360.0);
        transform.rotate(30.0);
        transform.scale(1.5, -0.75);
        const auto mapping = render_detail::vulkanBackgroundFillMapping(
            transform, QRectF(-100.0, -200.0, 200.0, 400.0), QSize(1280, 720));
        QVERIFY(qAbs(mapping.centerXNorm - 0.5f) < 0.0001f);
        QVERIFY(qAbs(mapping.centerYNorm - 0.5f) < 0.0001f);
        QVERIFY(qAbs(mapping.halfWidthOverOutputHeight - (150.0f / 720.0f)) < 0.0001f);
        QVERIFY(qAbs(mapping.signedHalfHeightOverOutputHeight - (-150.0f / 720.0f)) < 0.0001f);
        QVERIFY(qAbs(mapping.rotationRadians - 0.5235988f) < 0.0001f);
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

    void exportHardwareFrameSpeakerTargetDoesNotInvertY()
    {
        const QSize outputSize(1080, 1920);
        const QRectF fitted(0.0, 656.25, 1080.0, 607.5);
        const QPointF translation(3365.8771668733616, 196.669655606035);
        const QPointF scale(7.166943719443477, 7.166943719443477);
        const QPointF sampledFaceNorm(0.06514895968345152, 0.4019114220245886);
        const QPointF expectedTarget(540.0, 729.6);

        PreviewClipGeometry previewGeometry =
            PreviewViewTransform::clipGeometry(
                fitted,
                QPointF(1.0, 1.0),
                translation,
                0.0,
                scale);
        previewGeometry.clipToScreen.scale(1.0, -1.0);
        float previewMvp[16] = {};
        render_detail::vulkanMvpForPreviewTransform(previewGeometry.clipToScreen,
                                                    previewGeometry.localRect,
                                                    outputSize,
                                                    previewMvp);

        auto mappedOutputPoint = [&outputSize](const float mvp[16], const QPointF& norm) {
            const float shaderX = static_cast<float>((norm.x() * 2.0) - 1.0);
            const float shaderY = static_cast<float>((norm.y() * 2.0) - 1.0);
            const float ndcX = (mvp[0] * shaderX) + (mvp[4] * shaderY) + mvp[12];
            const float ndcY = (mvp[1] * shaderX) + (mvp[5] * shaderY) + mvp[13];
            return QPointF(((static_cast<qreal>(ndcX) + 1.0) * outputSize.width()) / 2.0,
                           ((static_cast<qreal>(ndcY) + 1.0) * outputSize.height()) / 2.0);
        };

        const QPointF previewPoint =
            mappedOutputPoint(previewMvp, QPointF(sampledFaceNorm.x(), 1.0 - sampledFaceNorm.y()));

        QVERIFY2(std::abs(previewPoint.x() - expectedTarget.x()) < 0.5,
                 qPrintable(QStringLiteral("preview face X target mismatch: got %1 expected %2")
                                .arg(previewPoint.x(), 0, 'f', 3)
                                .arg(expectedTarget.x(), 0, 'f', 3)));
        QVERIFY2(std::abs(previewPoint.y() - expectedTarget.y()) < 0.5,
                 qPrintable(QStringLiteral("preview face Y target mismatch: got %1 expected %2")
                                .arg(previewPoint.y(), 0, 'f', 3)
                                .arg(expectedTarget.y(), 0, 'f', 3)));

        float exportMvp[16] = {};
        render_detail::vulkanMvpForExportVideoLayer(fitted,
                                                    translation,
                                                    outputSize,
                                                    0.0,
                                                    scale,
                                                    exportMvp);
        const QPointF exportPoint = mappedOutputPoint(exportMvp, sampledFaceNorm);
        QVERIFY2(std::abs(exportPoint.x() - expectedTarget.x()) < 0.5,
                 qPrintable(QStringLiteral("canonical export face X target mismatch: got %1 expected %2")
                                .arg(exportPoint.x(), 0, 'f', 3)
                                .arg(expectedTarget.x(), 0, 'f', 3)));
        QVERIFY2(std::abs(exportPoint.y() - expectedTarget.y()) < 0.5,
                 qPrintable(QStringLiteral("canonical export face Y target mismatch: got %1 expected %2")
                                .arg(exportPoint.y(), 0, 'f', 3)
                                .arg(expectedTarget.y(), 0, 'f', 3)));
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
        QVERIFY2(source.contains(QStringLiteral("pc.u_highlights.a < -1.5")),
                 "Direct Vulkan presenter must use the push-constant edge-stretch background signal.");
        QVERIFY2(source.contains(QStringLiteral("edgeStretchFillSample")),
                 "Direct Vulkan presenter must drag edge pixels across missing background rows.");
        QVERIFY2(source.contains(QStringLiteral("validMin")) &&
                     source.contains(QStringLiteral("mappedUv")),
                 "Edge stretch must sample within explicit decoded-frame crop bounds.");
        QVERIFY2(source.contains(QStringLiteral("binding = 4")) &&
                     source.contains(QStringLiteral("frame.outputSize")) &&
                     !source.contains(QStringLiteral("dFdx(uv.x)")),
                 "Background transforms must use explicit frame uniforms, not fragment derivatives.");
        QVERIFY2(source.contains(QStringLiteral("blurredFillSample")),
                 "Direct Vulkan presenter must blur the cover-fill background in shader.");
        QVERIFY2(source.contains(QStringLiteral("float curveLuma = lumaOf(rgb);")),
                 "Direct Vulkan shader must compute luminance after RGB curve channels.");
        QVERIFY2(source.contains(QStringLiteral("rgb *= remappedLuma / curveLuma;")),
                 "Direct Vulkan shader must preserve chroma when applying the Brightness/Luma curve.");
        QVERIFY2(!source.contains(QStringLiteral("rr = texture(u_curve_lut, vec2(clamp(rr")),
                 "Direct Vulkan shader must not apply the luma curve independently to each RGB channel.");

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

    void directVulkanPresenterPassesBackgroundFillState()
    {
        QFile renderer(QStringLiteral(JCUT_SOURCE_DIR "/direct_vulkan_preview_window.cpp"));
        QVERIFY2(renderer.open(QIODevice::ReadOnly), "Unable to open direct Vulkan preview renderer.");
        const QString source = QString::fromUtf8(renderer.readAll());

        QVERIFY2(source.contains(QStringLiteral("const BackgroundFillEffect fillEffect = state->backgroundFillEffect")),
                 "Direct Vulkan presenter must use the selectable background fill effect.");
        QVERIFY2(source.contains(QStringLiteral("vulkanDrawEffectStateForGrade(status->grading)")),
                 "Direct Vulkan presenter must derive background fill from the main grading state.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillOpacity)")),
                 "Direct Vulkan presenter must use the output background fill opacity.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillBrightness)")),
                 "Direct Vulkan presenter must use the output background fill brightness.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillSaturation)")),
                 "Direct Vulkan presenter must use the output background fill saturation.");
        QVERIFY2(source.contains(QStringLiteral("state->backgroundFillEdgePixels")),
                 "Direct Vulkan presenter must use the output background fill edge pixel band.");
        QVERIFY2(source.contains(QStringLiteral("state->backgroundFillEdgeProgressive")),
                 "Direct Vulkan presenter must use the output background fill progressive mode.");
        QVERIFY2(source.contains(QStringLiteral("state->backgroundFillEdgePower")),
                 "Direct Vulkan presenter must use the output background fill edge curve.");
        QVERIFY2(source.contains(QStringLiteral("fillEffect == BackgroundFillEffect::EdgeStretch")),
                 "Direct Vulkan presenter must default through the edge-stretch background path.");
        QVERIFY2(source.contains(QStringLiteral("fillEffect == BackgroundFillEffect::ProgressiveEdgeStretch")),
                 "Direct Vulkan presenter must draw progressive edge stretch across the full canvas.");
        QVERIFY2(source.contains(QStringLiteral("fillEffect == BackgroundFillEffect::Mirror")),
                 "Direct Vulkan presenter must draw mirror fill across the full canvas.");
        QVERIFY2(source.contains(QStringLiteral("backgroundPush.highlights[3] = backgroundEffects.highlights[3]")),
                 "Direct Vulkan presenter must pass the background fill mode signal into the draw.");
    }
};

QTEST_MAIN(VulkanDirectRenderParityTest)
#include "test_vulkan_direct_render_parity.moc"
