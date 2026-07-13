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
    void temporalEffectsUseZeroCopyVulkanContracts()
    {
        QCOMPARE(render_detail::kVulkanEffectModeDifferenceMatte, 5.0f);
        QFile shader(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/effects.frag"));
        QVERIFY2(shader.open(QIODevice::ReadOnly), "Unable to open Vulkan effects shader.");
        const QString shaderSource = QString::fromUtf8(shader.readAll());
        QVERIFY(shaderSource.contains(QStringLiteral("texture(u_mask, v_texCoord).rgb")));
        QVERIFY(shaderSource.contains(QStringLiteral("smoothstep(threshold - softness")));

        QFile preview(QStringLiteral(JCUT_SOURCE_DIR "/direct_vulkan_preview_window.cpp"));
        QVERIFY2(preview.open(QIODevice::ReadOnly), "Unable to open direct Vulkan preview source.");
        const QString previewSource = QString::fromUtf8(preview.readAll());
        QVERIFY(previewSource.contains(QStringLiteral("#differenceReference")));
        QVERIFY(previewSource.contains(QStringLiteral("#temporalEcho%1")));
        QVERIFY(previewSource.contains(QStringLiteral("bindAuxiliaryImage")));

        QFile exportRenderer(QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"));
        QVERIFY2(exportRenderer.open(QIODevice::ReadOnly), "Unable to open offscreen Vulkan renderer source.");
        const QString exportSource = QString::fromUtf8(exportRenderer.readAll());
        QVERIFY(exportSource.contains(QStringLiteral("referenceFrameHandoff->uploadFrame")));
        QVERIFY(exportSource.contains(QStringLiteral("effectPreset == ClipEffectPreset::TemporalEcho")));
        QVERIFY(exportSource.contains(QStringLiteral("context.preferHardwareFrames")));
    }

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
        QCOMPARE(edgeState.shadows[2], 1.0f);
        QCOMPARE(edgeState.shadows[3], 1.0f);
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
        QCOMPARE(mirrorState.shadows[2], 1.0f);
        QCOMPARE(mirrorState.shadows[3], 1.0f);
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
        QVERIFY(qAbs(mapping.outputHeightOverSourceWidth - (720.0f / 300.0f)) < 0.0001f);
        QVERIFY(qAbs(mapping.signedOutputHeightOverSourceHeight - (-720.0f / 300.0f)) < 0.0001f);
        QVERIFY(qAbs(mapping.rotationRadians - 0.5235988f) < 0.0001f);
    }

    void progressiveEdgeStretchHorizontalSamplingIsPreviewZoomInvariant()
    {
        const QRectF surfaceRect(0.0, 0.0, 1280.0, 720.0);
        const QSize outputSize(1920, 1080);
        const QSize sourceSize(1080, 1920);
        const QPointF outputPoint(90.0, 540.0);

        auto horizontalFillT = [&](qreal previewZoom) {
            const PreviewViewTransform view(
                surfaceRect, outputSize, 36.0, previewZoom, QPointF());
            const QRectF fitted = view.fittedClipRect(sourceSize, sourceSize);
            const PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
                fitted, view.outputScale(), QPointF(), 0.0, QPointF(1.0, 1.0));
            const auto mapping = render_detail::vulkanBackgroundFillMapping(
                geometry.clipToScreen, geometry.localRect, view.targetRect());

            const QPointF screenPoint = view.outputToScreen(outputPoint);
            const qreal uvX = (screenPoint.x() - view.targetRect().left()) /
                view.targetRect().width();
            const qreal outputAspect = view.targetRect().width() /
                view.targetRect().height();
            const qreal sourceUvX =
                ((uvX - mapping.centerXNorm) * outputAspect *
                 mapping.outputHeightOverSourceWidth) + 0.5;
            const qreal leftOverflow = qMax<qreal>(
                0.0001,
                mapping.centerXNorm * outputAspect *
                    mapping.outputHeightOverSourceWidth);
            return qBound<qreal>(0.0, -sourceUvX / leftOverflow, 1.0);
        };

        const qreal zoomOneFillT = horizontalFillT(1.0);
        const qreal zoomTwoFillT = horizontalFillT(2.0);
        QVERIFY2(qAbs(zoomOneFillT - zoomTwoFillT) < 0.0001,
                 qPrintable(QStringLiteral(
                     "Horizontal progressive sampling changed with preview zoom: "
                     "zoom 1=%1, zoom 2=%2")
                                .arg(zoomOneFillT, 0, 'f', 6)
                                .arg(zoomTwoFillT, 0, 'f', 6)));
    }

    void progressiveStretchIsClipOwnedLayerPolicy()
    {
        TimelineClip top;
        top.id = QStringLiteral("top");
        top.filePath = QStringLiteral("/tmp/top.mp4");
        top.mediaType = ClipMediaType::Video;
        top.startFrame = 0;
        top.durationFrames = 100;
        top.trackIndex = 3;

        TimelineClip hidden = top;
        hidden.id = QStringLiteral("hidden");
        hidden.trackIndex = 2;

        TimelineClip bottom = top;
        bottom.id = QStringLiteral("bottom");
        bottom.trackIndex = 1;

        TimelineClip audio = top;
        audio.id = QStringLiteral("audio");
        audio.mediaType = ClipMediaType::Audio;
        audio.trackIndex = 0;

        TimelineClip maskMatte = top;
        maskMatte.id = QStringLiteral("mask");
        maskMatte.clipRole = ClipRole::MaskMatte;
        maskMatte.trackIndex = 4;

        QVector<TimelineTrack> tracks(4);
        tracks[2].visualMode = TrackVisualMode::Hidden;
        const QVector<TimelineClip> ordered{top, hidden, bottom, audio, maskMatte};

        Q_UNUSED(tracks);
        Q_UNUSED(ordered);
        QVERIFY(render_detail::vulkanClipSupportsProgressiveEdgeStretchSource(top));
        QVERIFY(!render_detail::vulkanClipSupportsProgressiveEdgeStretchSource(maskMatte));

        top.effectPreset = ClipEffectPreset::ProgressiveEdgeStretch;
        const render_detail::VulkanProgressiveEdgeStretchLayerPolicy mediaPolicy =
            render_detail::vulkanProgressiveEdgeStretchLayerPolicy(top, {});
        QVERIFY(mediaPolicy.presetActive);
        QVERIFY(mediaPolicy.sourceEligible);
        QVERIFY(mediaPolicy.drawBackground);

        maskMatte.effectPreset = ClipEffectPreset::ProgressiveEdgeStretch;
        const render_detail::VulkanProgressiveEdgeStretchLayerPolicy mattePolicy =
            render_detail::vulkanProgressiveEdgeStretchLayerPolicy(maskMatte, {});
        QVERIFY(mattePolicy.presetActive);
        QVERIFY(!mattePolicy.sourceEligible);
        QVERIFY(!mattePolicy.drawBackground);

        TimelineClip trackDriven = top;
        trackDriven.effectPreset = ClipEffectPreset::None;
        QVector<TimelineTrack> progressiveTracks(5);
        progressiveTracks[trackDriven.trackIndex].effectPreset =
            ClipEffectPreset::ProgressiveEdgeStretch;
        const render_detail::VulkanProgressiveEdgeStretchLayerPolicy trackPolicy =
            render_detail::vulkanProgressiveEdgeStretchLayerPolicy(trackDriven, progressiveTracks);
        QVERIFY(!trackPolicy.presetActive);
        QVERIFY(!trackPolicy.drawBackground);
        TimelineClip independentMask = maskMatte;
        independentMask.effectPreset = ClipEffectPreset::None;
        const render_detail::VulkanProgressiveEdgeStretchLayerPolicy independentMaskPolicy =
            render_detail::vulkanProgressiveEdgeStretchLayerPolicy(independentMask, {});
        QVERIFY(!independentMaskPolicy.presetActive);
        QVERIFY(!independentMaskPolicy.sourceEligible);
        QVERIFY(!independentMaskPolicy.drawBackground);
        QVERIFY(!render_detail::vulkanEffectPipelinePlan(
                     independentMask, QRectF(0.0, 0.0, 1080.0, 1920.0),
                     QSize(1920, 1080), 12.0, 12.0).usesGeneratedDraws());
    }

    void finalCompositeProgressiveStretchUsesScreenTextureSampling()
    {
        QFile shader(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/effects.frag"));
        QVERIFY(shader.open(QIODevice::ReadOnly));
        const QByteArray source = shader.readAll();
        QVERIFY(source.contains("finalCompositeProgressiveEdgeStretchFill"));
        QVERIFY(source.contains("sampleCompositeScreen"));
        QVERIFY(source.contains("vec2 frameOutputSize = frame.outputSizeAndInverse.xy"));
        QVERIFY(source.contains("vec2 rawValidMin = pc.u_highlights.xy"));
        QVERIFY(source.contains("vec2 sampleMin = clamp(min(rawValidMin, rawValidMax), vec2(0.0), vec2(1.0))"));
        QVERIFY(source.contains("vec2 validSpan = max(sampleMax - sampleMin, vec2(0.0))"));
        QVERIFY(source.contains("safeClampRange"));
        QVERIFY(source.contains("vec2 edgePixelBasis = sampleCompositeScreen ? frameOutputSize : texSize * sampleSpan"));
        QVERIFY(source.contains("if (progressive && !sampleCompositeScreen && insideClipBounds)"));
        QVERIFY(source.contains("vec2 compositeUv = rawValidMin + screenUv * rawValidSpan"));
        QVERIFY(source.contains("if (resolvedSample.a <= 0.01)"));
        QVERIFY(source.contains("vec2 outside = max(max(-originalSourceUv, originalSourceUv - vec2(1.0))"));
        QVERIFY(source.contains("inwardStep.x = originalSourceUv.x < 0.5 ? 1.0 : -1.0"));
        QVERIFY(source.contains("inwardStep.y = originalSourceUv.y < 0.5 ? 1.0 : -1.0"));
        QVERIFY(source.contains("for (int i = 1; i <= 1024; ++i)"));
        QVERIFY(source.contains("if (searchSample.a > 0.01)"));
        QVERIFY(source.contains("if (finalCompositeProgressiveEdgeStretchFill && sourceAlpha > 0.01)"));
        QVERIFY(source.contains("sourceAlpha = 1.0"));
        QVERIFY(!source.contains("textureInteriorClamp(compositeUv)"));
        QCOMPARE(render_detail::kVulkanEffectModeFinalCompositeProgressiveEdgeStretch, -5.0f);
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
                     source.contains(QStringLiteral("frame.outputSizeAndInverse")) &&
                     !source.contains(QStringLiteral("dFdx(uv.x)")),
                 "Background transforms must use explicit frame uniforms, not fragment derivatives.");
        QVERIFY2(source.contains(QStringLiteral("blurredFillSample")),
                 "Direct Vulkan presenter must blur the cover-fill background in shader.");
        QVERIFY2(source.contains(QStringLiteral("float curveLuma = lumaOf(rgb);")),
                 "Direct Vulkan shader must compute luminance after RGB curve channels.");
        QVERIFY2(source.contains(QStringLiteral("rgb *= remappedLuma / curveLuma;")),
                 "Direct Vulkan shader must preserve chroma when applying the Brightness/Luma curve.");
        QVERIFY2(source.contains(QStringLiteral("} else if (backgroundFill)")) &&
                     source.contains(QStringLiteral("rgb = applyCurveLut(rgb, false);")),
                 "Direct Vulkan shader must apply the clip curve LUT to background/stretch fills.");
        QVERIFY2(source.contains(QStringLiteral("frame.backgroundShadows.rgb")) &&
                     source.contains(QStringLiteral("frame.backgroundMidtones.rgb")) &&
                     source.contains(QStringLiteral("frame.backgroundHighlights.rgb")),
                 "Background/stretch fills must apply full tonal grading vectors from per-draw uniforms.");
        QVERIFY2(source.contains(QStringLiteral("return vec4(0.0);")) &&
                     source.contains(QStringLiteral("insideClipBounds")),
                 "Progressive edge stretch backgrounds must be transparent inside the clip bounds.");
        QVERIFY2(source.contains(QStringLiteral("vec2 safeClampRange")),
                 "Direct Vulkan shader must not build inverted clamp ranges for tiny source spans.");
        QVERIFY2(!source.contains(QStringLiteral("rr = texture(u_curve_lut, vec2(clamp(rr")),
                 "Direct Vulkan shader must not apply the luma curve independently to each RGB channel.");

        const int shadowsPos = source.indexOf(QStringLiteral("rgb *= (1.0 + pc.u_shadows.rgb"));
        const int midtonesPos = source.indexOf(QStringLiteral("vec3 midtoneAdjust = pc.u_midtones.rgb"));
        const int highlightsPos = source.indexOf(QStringLiteral("rgb += pc.u_highlights.rgb"));
        const int curvePos = source.indexOf(QStringLiteral("rgb = applyCurveLut(rgb, maskCurveEnabled)"));
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
        QVERIFY2(source.contains(QStringLiteral("baseEffects.shadows")) &&
                     source.contains(QStringLiteral("baseEffects.midtones")) &&
                     source.contains(QStringLiteral("baseEffects.highlights")) &&
                     source.contains(QStringLiteral("frameUniformDynamicOffset")),
                 "Direct Vulkan presenter must pass full background grading through per-draw dynamic uniforms.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillOpacity)")),
                 "Direct Vulkan presenter must use the output background fill opacity.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillBrightness)")),
                 "Direct Vulkan presenter must use the output background fill brightness.");
        QVERIFY2(source.contains(QStringLiteral("static_cast<float>(state->backgroundFillSaturation)")),
                 "Direct Vulkan presenter must use the output background fill saturation.");
        QVERIFY2(source.contains(QStringLiteral("progressiveEdgeStretchEffect")) &&
                     source.contains(QStringLiteral("qBound(1, effectClip.effectRows, 512)")),
                 "Direct Vulkan presenter must use clip effect rows as the progressive edge pixel band.");
        QVERIFY2(source.contains(QStringLiteral("state->backgroundFillEdgeProgressive")),
                 "Direct Vulkan presenter must use the output background fill progressive mode.");
        QVERIFY2(source.contains(QStringLiteral("qBound<qreal>(0.25, effectClip.effectScale, 8.0)")),
                 "Direct Vulkan presenter must use clip effect scale as the progressive edge curve.");
        QVERIFY2(source.contains(QStringLiteral("effectiveFillEffect == BackgroundFillEffect::EdgeStretch")),
                 "Direct Vulkan presenter must default through the edge-stretch background path.");
        QVERIFY2(source.contains(QStringLiteral("render_detail::vulkanProgressiveEdgeStretchLayerPolicy(clip, state->tracks)")),
                 "Direct Vulkan presenter must apply progressive edge stretch through the shared layer policy.");
        QVERIFY2(source.contains(QStringLiteral("progressiveEdgeStretchEffect")),
                 "Direct Vulkan presenter must identify the clip-basis progressive stretch effect.");
        QVERIFY2(source.contains(QStringLiteral("BackgroundFillEffect::ProgressiveEdgeStretch")),
                 "Clip-basis progressive stretch must reuse the progressive edge stretch shader mode.");
        QVERIFY2(source.contains(QStringLiteral("progressiveStretchOwnsClipBackground")),
                 "Progressive stretch ownership must be explicit so source/generated effects cannot leak through.");
        QVERIFY2(source.contains(QStringLiteral("foregroundEffectClip.effectPreset = ClipEffectPreset::None")) &&
                     source.contains(QStringLiteral("foregroundEffectClip.maskRepeatEnabled = false")),
                 "A clip used as the progressive stretch source must not also run generated foreground effects.");
        QVERIFY2(source.contains(QStringLiteral("(progressiveEdgeStretchEffect || !backgroundFilled)")),
                 "Clip-basis progressive stretch must not be suppressed by an earlier global background fill.");
        QVERIFY2(source.contains(QStringLiteral("if (!progressiveStretchOwnsClipBackground)")),
                 "Only global background fills should consume the once-per-frame background fill guard.");
        QVERIFY2(source.contains(QStringLiteral("progressiveRenderSpaceFill")),
                 "Progressive edge stretch must use render/output-space shader coordinates in preview.");
        QVERIFY2(source.contains(QStringLiteral("render_detail::vulkanProgressiveEdgeStretchLayerPolicy(clip, state->tracks)")),
                 "Direct Vulkan presenter must use the shared progressive edge stretch layer policy.");
        QVERIFY2(source.contains(QStringLiteral("!(status && status->maskClipSource)")),
                 "Direct Vulkan presenter must reject decoded mask-source statuses for progressive edge stretch.");
        QVERIFY2(source.contains(QStringLiteral("render_detail::fitRectF(renderSourceSize, renderOutputSize)")),
                 "Progressive edge stretch preview mapping must be rebuilt in render space, not preview zoom space.");
        QVERIFY2(source.contains(QStringLiteral("? renderOutputSize")) &&
                     source.contains(QStringLiteral(": compositeRect.size().toSize()")),
                 "Progressive edge stretch must update the shader frame uniform with output size, not preview canvas size.");
        QVERIFY2(source.contains(QStringLiteral("const bool useCompositeTarget = false")),
                 "Direct Vulkan presenter must not route clip-basis progressive stretch through a final composite target.");
        QVERIFY2(source.contains(QStringLiteral("recordSwapchainReadback(cb, &slot, swapSize)")),
                 "Direct Vulkan presenter must make the post-final-pass frame available to the pipeline tap.");
        QVERIFY2(source.contains(QStringLiteral("m_owner->pipelineThumbnailReadbackPending()")),
                 "Direct Vulkan presenter must record the pipeline tap only when review tooling requests it.");
        QVERIFY2(source.contains(QStringLiteral("effectiveFillEffect == BackgroundFillEffect::Mirror")),
                 "Direct Vulkan presenter must draw mirror fill across the full canvas.");
        QVERIFY2(source.contains(QStringLiteral("backgroundPush.highlights[3] = backgroundEffects.highlights[3]")),
                 "Direct Vulkan presenter must pass the background fill mode signal into the draw.");

        QFile previewSurface(QStringLiteral(JCUT_SOURCE_DIR "/vulkan_preview_surface.cpp"));
        QVERIFY2(previewSurface.open(QIODevice::ReadOnly), "Unable to open Vulkan preview surface.");
        const QString previewSurfaceSource = QString::fromUtf8(previewSurface.readAll());
        QVERIFY2(previewSurfaceSource.contains(QStringLiteral("render_detail::vulkanProgressiveEdgeStretchLayerPolicy(clip, m_interaction.tracks)")),
                 "Preview must use the shared progressive edge stretch layer policy for mask foreground suppression.");
        QVERIFY2(previewSurfaceSource.contains(QStringLiteral("maskForegroundStatusBySourceId.insert")),
                 "Preview must retain independently graded mask foreground layers during progressive edge stretch.");

        QFile offscreen(QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"));
        QVERIFY2(offscreen.open(QIODevice::ReadOnly), "Unable to open offscreen Vulkan renderer.");
        const QString offscreenSource = QString::fromUtf8(offscreen.readAll());
        QVERIFY2(offscreenSource.contains(QStringLiteral("vulkanProgressiveEdgeStretchLayerPolicy(clip, request.tracks)")),
                 "Offscreen renderer must use the shared progressive edge stretch layer policy.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("backgroundLayer.backgroundShadows")) &&
                     offscreenSource.contains(QStringLiteral("updateFrameUniformForDraw(&layer)")) &&
                     offscreenSource.contains(QStringLiteral("VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC")),
                 "Offscreen renderer must pass background grading through per-draw dynamic uniforms.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("progressiveEdgeStretchEffect = progressiveStretchPolicy.drawBackground")),
                 "Offscreen renderer must identify the clip-basis progressive stretch effect from the shared policy.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("foregroundEffectClip.effectPreset = ClipEffectPreset::None")) &&
                     offscreenSource.contains(QStringLiteral("foregroundEffectClip.maskRepeatEnabled = false")),
                 "Offscreen progressive stretch source clips must not also run generated foreground effects.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("if (layer.maskTextureEnabled &&")),
                 "Offscreen renderer must retain mask foreground layers during progressive edge stretch.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("(progressiveEdgeStretchEffect || !backgroundFilled)")),
                 "Offscreen clip-basis progressive stretch must not be suppressed by an earlier global background fill.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("if (!progressiveStretchOwnsClipBackground)")),
                 "Offscreen global background fill guard must not consume clip-owned progressive stretch effects.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("qBound(1, effectClip.effectRows, 512)")) &&
                     offscreenSource.contains(QStringLiteral("qBound<qreal>(0.25, effectClip.effectScale, 8.0)")),
                 "Offscreen renderer must source progressive edge parameters from the selected clip effect.");

        QFile editor(QStringLiteral(JCUT_SOURCE_DIR "/editor.cpp"));
        QVERIFY2(editor.open(QIODevice::ReadOnly), "Unable to open editor source.");
        const QString editorSource = QString::fromUtf8(editor.readAll());
        QVERIFY2(editorSource.contains(QStringLiteral("migrateLegacyBackgroundProgressiveStretchToClipEffect")) &&
                     editorSource.contains(QStringLiteral("target.effectPreset = ClipEffectPreset::ProgressiveEdgeStretch")) &&
                     editorSource.contains(QStringLiteral("backgroundFillEdgeProgressive = false")),
                 "Legacy background progressive state must be normalized into the clip-owned effect.");
    }

    void vulkanShaderBuildDoesNotDependOnlyOnOutputTimestamps()
    {
        QFile cmake(QStringLiteral(JCUT_SOURCE_DIR "/CMakeLists.txt"));
        QVERIFY2(cmake.open(QIODevice::ReadOnly), "Unable to open CMakeLists.txt.");
        const QString source = QString::fromUtf8(cmake.readAll());
        QVERIFY2(source.contains(QStringLiteral("add_custom_target(jcut_vulkan_shader_${shader_target_name}")),
                 "Each Vulkan shader must be backed by an always-runnable target so stale SPIR-V cannot survive preserved source mtimes.");
        QVERIFY2(source.contains(QStringLiteral("BYPRODUCTS \"${out_file}\"")),
                 "Always-runnable shader targets must still declare their SPIR-V byproducts.");
        QVERIFY2(source.contains(QStringLiteral("add_custom_target(jcut_vulkan_shaders DEPENDS ${JCUT_VULKAN_SHADER_TARGETS})")),
                 "The aggregate Vulkan shader target must depend on the per-shader targets.");
    }
};

QTEST_MAIN(VulkanDirectRenderParityTest)
#include "test_vulkan_direct_render_parity.moc"
