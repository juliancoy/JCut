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
    void correctionPolygonsStayInVulkanMaskPreparation()
    {
        TimelineClip::CorrectionPolygon polygon;
        polygon.pointsNormalized = {
            {0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
        const QByteArray storage =
            render_detail::vulkanMaskCorrectionStorageData({polygon});
        QCOMPARE(storage.size(), 6 * 4 * static_cast<int>(sizeof(float)));

        QFile shader(
            QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/mask_prepare.comp"));
        QVERIFY2(shader.open(QIODevice::ReadOnly),
                 "Unable to open Vulkan mask preparation shader.");
        const QString shaderSource = QString::fromUtf8(shader.readAll());
        QVERIFY(shaderSource.contains(QStringLiteral(
            "layout(std430, binding = 2) readonly buffer CorrectionStorage")));
        QVERIFY(shaderSource.contains(QStringLiteral(
            "float correctionCoverage(ivec2 dstCoord)")));
        QVERIFY(shaderSource.contains(QStringLiteral(
            "value *= 1.0 - correctionCoverage(dstCoord)")));

        QFile preview(
            QStringLiteral(JCUT_SOURCE_DIR "/direct_vulkan_preview_window.cpp"));
        QVERIFY2(preview.open(QIODevice::ReadOnly),
                 "Unable to open direct Vulkan preview source.");
        const QString previewSource = QString::fromUtf8(preview.readAll());
        QVERIFY(previewSource.contains(QStringLiteral(
            "maskOptions.correctionStorage")));
        QVERIFY(!previewSource.contains(QStringLiteral(
            "applyCorrectionPolygonsToMaskImage")));

        QFile exportRenderer(
            QStringLiteral(JCUT_SOURCE_DIR
                           "/offscreen_vulkan_renderer_backend.cpp"));
        QVERIFY2(exportRenderer.open(QIODevice::ReadOnly),
                 "Unable to open offscreen Vulkan renderer source.");
        const QString exportSource = QString::fromUtf8(exportRenderer.readAll());
        QFile maskPreprocessor(
            QStringLiteral(JCUT_SOURCE_DIR "/vulkan_mask_preprocessor.cpp"));
        QVERIFY2(maskPreprocessor.open(QIODevice::ReadOnly),
                 "Unable to open shared Vulkan mask preprocessor source.");
        const QString maskPreprocessorSource =
            QString::fromUtf8(maskPreprocessor.readAll());
        QVERIFY(maskPreprocessorSource.contains(QStringLiteral(
            "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER")));
        QVERIFY(exportSource.contains(QStringLiteral(
            "m_maskPreprocessor.record(")));
        QVERIFY(!exportSource.contains(QStringLiteral(
            "rgbaMaskImageForUpload")));
    }

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

        const render_detail::VulkanGradePayload payload =
            render_detail::vulkanGradePayloadForGrade(grade);
        QVERIFY(payload.curveLutApplied);
        QCOMPARE(payload.effects.shadows[3],
                 render_detail::kVulkanEffectModeCurve);
        QCOMPARE(payload.curveLutRgba,
                 render_detail::vulkanCurveLutRgbaBytes(grade));
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
        const render_detail::VulkanDrawEffectState edgeState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::EdgeStretch, 0.8f, -0.02f, 1.5f, 24, 2.5f);
        QCOMPARE(edgeState.opacity, 0.8f);
        QVERIFY(qAbs(edgeState.brightness + 0.02f) < 0.0001f);
        QCOMPARE(edgeState.contrast, 1.0f);
        QCOMPARE(edgeState.saturation, 1.5f);
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
                0.8f,
                -0.02f,
                1.5f,
                24,
                2.5f,
                QRectF(0.0, 0.0, 1.0, 1.0));
        QCOMPARE(progressiveEdgeState.midtones[0], 24.0f);
        QCOMPARE(progressiveEdgeState.midtones[1], 0.0f);
        QCOMPARE(progressiveEdgeState.midtones[2], 2.5f);
        QVERIFY2(progressiveEdgeState.highlights[3] < -2.5f &&
                     progressiveEdgeState.highlights[3] > -3.5f,
                 "Progressive edge stretch must have its own background fill mode signal.");

        const render_detail::VulkanDrawEffectState bidirectionalEdgeState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch,
                0.8f,
                -0.02f,
                1.5f,
                24,
                2.5f,
                QRectF(0.0, 0.0, 1.0, 1.0));
        QCOMPARE(
            bidirectionalEdgeState.highlights[3],
            render_detail::kVulkanEffectModeBackgroundProgressiveBidirectionalEdgeStretch);

        const render_detail::VulkanDrawEffectState tileState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::Tile,
                0.8f,
                -0.02f,
                1.5f,
                24,
                2.5f,
                QRectF(0.0, 0.0, 1.0, 1.0));
        QCOMPARE(
            tileState.highlights[3],
            render_detail::kVulkanEffectModeBackgroundTile);

        const render_detail::VulkanDrawEffectState mirrorState =
            render_detail::vulkanBackgroundFillEffectState(
                BackgroundFillEffect::Mirror, 0.8f, -0.02f, 1.5f, 24, 2.5f);
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

    void titlePreviewMvpPreservesQtColumnMajorStorage()
    {
        QFile renderer(QStringLiteral(JCUT_SOURCE_DIR "/vulkan_text_renderer.cpp"));
        QVERIFY2(renderer.open(QIODevice::ReadOnly),
                 "Unable to open Vulkan text renderer source.");
        const QString source = QString::fromUtf8(renderer.readAll());
        QVERIFY2(!source.contains(QStringLiteral("QMatrix4x4 outputMvpMatrix(outputMvp)")),
                 "Title MVP arrays must not use Qt's row-major pointer constructor.");
        QVERIFY2(source.contains(QStringLiteral(
                     "std::copy(outputMvp, outputMvp + 16, outputMvpMatrix.data())")),
                 "Title MVP arrays must preserve Vulkan/Qt column-major storage.");
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
        QVERIFY(render_detail::vulkanClipSupportsBackgroundFillSource(top));
        QVERIFY(!render_detail::vulkanClipSupportsBackgroundFillSource(maskMatte));

        TimelineClip independentMask = maskMatte;
        independentMask.effectPreset = ClipEffectPreset::None;
        QVERIFY(!render_detail::vulkanEffectPipelinePlan(
                     independentMask, QRectF(0.0, 0.0, 1080.0, 1920.0),
                     QSize(1920, 1080), 12.0, 12.0).usesGeneratedDraws());
    }

    void bidirectionalStretchUsesContinuousRoundedPerimeter()
    {
        QFile shader(QStringLiteral(JCUT_SOURCE_DIR "/shaders/vulkan/effects.frag"));
        QVERIFY(shader.open(QIODevice::ReadOnly));
        const QByteArray source = shader.readAll();
        QVERIFY(source.contains("progressiveBidirectionalEdgeStretchSample"));
        QVERIFY(source.contains("superellipseDirection"));
        QVERIFY(source.contains("float coreRadius"));
        QVERIFY(source.contains("float canvasRadius"));
        QVERIFY(source.contains("float overlayAlpha = smoothstep"));
        QVERIFY(source.contains("float angularStep"));
        QVERIFY(source.contains("sampleBidirectionalRing"));
        QCOMPARE(
            render_detail::kVulkanEffectModeBackgroundProgressiveBidirectionalEdgeStretch,
            -6.0f);
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
        const int postGradeHelper = source.indexOf(
            QStringLiteral("vec4 samplePostGradeTexture(vec2 uv)"));
        const int postGradeCurve = source.indexOf(
            QStringLiteral("rgb = applyCurveLut(rgb, false);"),
            postGradeHelper);
        const int postGradeBrightness = source.indexOf(
            QStringLiteral("frame.backgroundGrade.y"),
            postGradeHelper);
        const int fillSampling = source.indexOf(
            QStringLiteral("sampleBidirectionalRing"),
            postGradeHelper);
        QVERIFY2(postGradeHelper >= 0 &&
                     postGradeCurve > postGradeHelper &&
                     postGradeBrightness > postGradeCurve &&
                     fillSampling > postGradeBrightness,
                 "Edge Fill must sample fully graded clip color before its "
                 "spatial reconstruction.");
        QVERIFY2(source.contains(QStringLiteral("frame.backgroundShadows.rgb")) &&
                     source.contains(QStringLiteral("frame.backgroundMidtones.rgb")) &&
                     source.contains(QStringLiteral("frame.backgroundHighlights.rgb")) &&
                     source.contains(QStringLiteral("frame.backgroundGrade")),
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

        QVERIFY2(
            source.contains(QStringLiteral(
                "const BackgroundFillEffect effectiveFillEffect =")) &&
                source.contains(QStringLiteral("effectClip.edgeFillEffect")),
            "Direct Vulkan presenter must use the selected clip's Edge Fill.");
        QVERIFY2(
            source.contains(QStringLiteral(
                "status.gradePayload")) &&
                !source.contains(QStringLiteral(
                    "vulkanGradePayloadForGrade(status.grading)")),
            "Direct Vulkan presenter must consume the canonical complete "
            "grading payload prepared by the shared layer packet.");
        QVERIFY2(source.contains(QStringLiteral("baseEffects.shadows")) &&
                     source.contains(QStringLiteral("baseEffects.midtones")) &&
                     source.contains(QStringLiteral("baseEffects.highlights")) &&
                     source.contains(QStringLiteral("backgroundGrade")) &&
                     source.contains(QStringLiteral("frameUniformDynamicOffset")),
                 "Direct Vulkan presenter must pass the complete pre-fill grade through per-draw dynamic uniforms.");
        QVERIFY2(source.contains(QStringLiteral("effectClip.edgeFillOpacity")) &&
                     source.contains(QStringLiteral("effectClip.edgeFillBrightness")) &&
                     source.contains(QStringLiteral("effectClip.edgeFillSaturation")),
                 "Direct Vulkan presenter must use clip-owned fill grading.");
        QVERIFY2(source.contains(QStringLiteral("qBound(1, effectClip.edgeFillPixels, 512)")) &&
                     source.contains(QStringLiteral("qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0)")),
                 "Direct Vulkan presenter must use canonical Edge Fill geometry.");
        QVERIFY2(source.contains(QStringLiteral("effectiveFillEffect == BackgroundFillEffect::EdgeStretch")),
                 "Direct Vulkan presenter must default through the edge-stretch background path.");
        QVERIFY2(source.contains(QStringLiteral("BackgroundFillEffect::ProgressiveEdgeStretch")),
                 "Clip-owned progressive stretch must reuse its dedicated shader mode.");
        QVERIFY2(source.contains(QStringLiteral("progressiveRenderSpaceFill")),
                 "Progressive edge stretch must use render/output-space shader coordinates in preview.");
        QVERIFY2(source.contains(QStringLiteral("!(status && status->maskClipSource)")),
                 "Direct Vulkan presenter must reject decoded mask-source statuses for progressive edge stretch.");
        QVERIFY2(source.contains(QStringLiteral("render_detail::fitRectF(renderSourceSize, renderOutputSize)")),
                 "Progressive edge stretch preview mapping must be rebuilt in render space, not preview zoom space.");
        QVERIFY2(source.contains(QStringLiteral("? renderOutputSize")) &&
                     source.contains(QStringLiteral(": compositeRect.size().toSize()")),
                 "Progressive edge stretch must update the shader frame uniform with output size, not preview canvas size.");
        QVERIFY2(!source.contains(QStringLiteral("useCompositeTarget")) &&
                     !source.contains(QStringLiteral("finalCompositeStretch")),
                 "Direct Vulkan presenter must contain only the clip-owned Edge Fill path.");
        QVERIFY2(source.contains(QStringLiteral("recordSwapchainReadback(cb, &slot, swapSize)")),
                 "Direct Vulkan presenter must make the post-final-pass frame available to the pipeline tap.");
        QVERIFY2(source.contains(QStringLiteral("m_owner->pipelineThumbnailReadbackPending()")),
                 "Direct Vulkan presenter must record the pipeline tap only when review tooling requests it.");
        QVERIFY2(source.contains(QStringLiteral("effectiveFillEffect == BackgroundFillEffect::Mirror")),
                 "Direct Vulkan presenter must draw mirror fill across the full canvas.");
        QVERIFY2(source.contains(QStringLiteral("backgroundPush.highlights[3] = backgroundEffects.highlights[3]")),
                 "Direct Vulkan presenter must pass the background fill mode signal into the draw.");
        QVERIFY2(
            source.contains(QStringLiteral("effectClip.edgeFillEffect")) &&
                source.contains(QStringLiteral(
                    "BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch")) &&
                source.contains(QStringLiteral("bidirectionalEdgeDrawPending")),
            "Direct Vulkan preview must expose the bidirectional mode and draw its "
            "in-clip border overlay after the ordinary clip.");

        QFile previewSurface(QStringLiteral(JCUT_SOURCE_DIR "/vulkan_preview_surface.cpp"));
        QVERIFY2(previewSurface.open(QIODevice::ReadOnly), "Unable to open Vulkan preview surface.");
        const QString previewSurfaceSource = QString::fromUtf8(previewSurface.readAll());
        QVERIFY2(previewSurfaceSource.contains(QStringLiteral(
                     "clipWithResolvedTimingOwner(clip, m_interaction.clips)")) &&
                     previewSurfaceSource.contains(QStringLiteral(
                         "evaluateEffectiveVisualEffectsAtPosition(")),
                 "Preview status construction must evaluate effects through the resolved timing owner; "
                 "the direct presenter applies the shared progressive-edge layer policy.");
        QVERIFY2(previewSurfaceSource.contains(QStringLiteral(
                     "status.maskForegroundLayerEnabled = clip.maskForegroundLayerEnabled")) &&
                     source.contains(QStringLiteral(
                         "if (maskReady && status->maskForegroundLayerEnabled)")),
                 "Preview status and the direct presenter must retain independently graded mask foreground layers during progressive edge stretch.");

        QFile offscreen(QStringLiteral(JCUT_SOURCE_DIR "/offscreen_vulkan_renderer_backend.cpp"));
        QVERIFY2(offscreen.open(QIODevice::ReadOnly), "Unable to open offscreen Vulkan renderer.");
        const QString offscreenSource = QString::fromUtf8(offscreen.readAll());
        QVERIFY2(offscreenSource.contains(QStringLiteral("backgroundLayer.backgroundShadows")) &&
               offscreenSource.contains(QStringLiteral("updateFrameUniformForDraw(&layer")) &&
                     offscreenSource.contains(QStringLiteral("VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC")),
                 "Offscreen renderer must pass background grading through per-draw dynamic uniforms.");
        QVERIFY2(
            offscreenSource.contains(QStringLiteral(
                "backgroundLayer.gradePayload = layer.gradePayload")) &&
                offscreenSource.contains(QStringLiteral(
                    "backgroundLayer.gradePayload.effects = backgroundEffects")),
            "Offscreen fill layers must inherit the source clip's complete "
            "curve-LUT payload.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("if (layer.maskTextureEnabled &&")),
                 "Offscreen renderer must retain mask foreground layers during progressive edge stretch.");
        QVERIFY2(offscreenSource.contains(QStringLiteral("qBound(1, effectClip.edgeFillPixels, 512)")) &&
                     offscreenSource.contains(QStringLiteral("qBound<qreal>(0.25, effectClip.edgeFillPower, 8.0)")),
                 "Offscreen renderer must source geometry from canonical Edge Fill fields.");
        QVERIFY2(
            offscreenSource.contains(QStringLiteral("effectClip.edgeFillEffect")) &&
                offscreenSource.contains(QStringLiteral("bidirectionalEdgeLayerPending")) &&
                offscreenSource.contains(QStringLiteral(
                    "layers.push_back(bidirectionalEdgeLayer)")),
            "Offscreen rendering must composite the bidirectional border warp after "
            "the source layer so it affects the clip edge as well as blank space.");

        QFile editor(QStringLiteral(JCUT_SOURCE_DIR "/editor.cpp"));
        QVERIFY2(editor.open(QIODevice::ReadOnly), "Unable to open editor source.");
        const QString editorSource = QString::fromUtf8(editor.readAll());
        QFile outputTab(QStringLiteral(JCUT_SOURCE_DIR "/output_tab.cpp"));
        QVERIFY2(outputTab.open(QIODevice::ReadOnly), "Unable to open Output tab source.");
        const QString outputTabSource = QString::fromUtf8(outputTab.readAll());
        QFile projectState(QStringLiteral(JCUT_SOURCE_DIR "/project_state.cpp"));
        QVERIFY2(projectState.open(QIODevice::ReadOnly), "Unable to open project state source.");
        const QString projectStateSource = QString::fromUtf8(projectState.readAll());
        QVERIFY2(
            !editorSource.contains(
                QStringLiteral("migrateLegacyBackgroundProgressiveStretchToClipEffect")) &&
                !projectStateSource.contains(QStringLiteral("backgroundFillEffect")) &&
                !outputTabSource.contains(QStringLiteral("backgroundFillEffect")),
            "Output must not own, persist, or migrate fill effects; Edge Fill is clip-owned.");
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
