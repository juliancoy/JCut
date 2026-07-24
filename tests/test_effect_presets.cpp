#include <QtTest/QtTest>

#include "../clip_serialization.h"
#include "../editor_effect_presets.h"
#include "../editor_shared_effects.h"
#include "../editor_shared_keyframes.h"
#include "../render_vulkan_shared.h"
#include "../mask_sidecar.h"
#include "../mask_sidecar_core.h"
#include "../mask_frame_map_core.h"
#include "../transform_skip_aware_timing.h"
#include "../titles.h"
#include "../vulkan_effect_synth.h"
#include "mask_sidecar_test_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

struct MovingPresetCase {
    ClipEffectPreset preset = ClipEffectPreset::None;
    ClipTilingPattern tilingPattern = ClipTilingPattern::Grid;
    QString label;
};

TimelineClip makeMovingEffectClip(const MovingPresetCase& testCase)
{
    TimelineClip clip;
    clip.id = QStringLiteral("speed-through-effect-") + testCase.label;
    clip.filePath = QStringLiteral("/tmp/source.png");
    clip.trackIndex = 1;
    clip.startFrame = 0;
    clip.durationFrames = 60;
    clip.effectPreset = testCase.preset;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;
    clip.tilingPattern = testCase.tilingPattern;
    clip.transformSkipAwareTiming = true;
    clip.effectSkipAwareTiming = true;
    return clip;
}

}  // namespace

class TestEffectPresets : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void clipSerializationPersistsEffectPresetState();
    void textExtrudeModesSerializeAndMigrate();
    void maskFeatherFalloffProfilesShapeAlphaDifferently();
    void correctionPolygonsEraseMaskIntensity();
    void maskSidecarDiscoveryProvidesStableIdentityAndCoverage();
    void neutralMaskSidecarDiscoveryMatchesQtIdentityAndReadiness();
    void neutralMaskFrameMapValidatesOrdinalSidecarsAndSourceIdentity();
    void neutralMaskFrameMapCacheReusesValidationAndInvalidatesChanges();
    void genericAiMaskSidecarsBecomeNestedChildrenWithIndependentZLevels();
    void birefnetSidecarDiscoveryPreservesContinuousAlphaMetadata();
    void incompleteBiRefNetSidecarsStayDisabledAndUnmaterialized();
    void completedOrdinalSidecarWithInteriorHoleFailsClosed();
    void birefnetUxExposesExplicitContextActionsAndPreview();
    void clipSerializationMigratesLegacyEffectSpeechSync();
    void clipSerializationPersistsArpeggiatorEffectPresets();
    void effectPresetMetadataCoversSerializedSynthPresets();
    void trackEffectSettingsDoNotLeakIntoChildClips();
    void maskMatteSourceHistoryEffectsRenderInactive();
    void clipSerializationPersistsGeneratedClipRoleState();
    void maskMatteFactoryKeepsSourceTimingLocked();
    void maskSidecarAssociationUpdatesPathAndStableIdentityTogether();
    void maskMatteNormalizerRejectsUnsupportedParentsAndDeduplicates();
    void maskMatteNormalizerRepairsLegacyTimelineState();
    void maskMatteNormalizerFollowsSourceTimingEdits();
    void alternatingMotionBackgroundFactoryCreatesVisualOnlySynthClip();
    void sourceTilingFactoryCreatesVisualOnlySynthClip();
    void speakerTitleFactoryBuildsLowerThirdsForSpeakerChanges();
    void speakerTitleFlyInsApplyToSourceClipKeyframes();
    void speakerTitleFlyInsKeepOrganizationSeparateAndIndependentBoxWidth();
    void effectPipelinePassesThroughWhenPresetIsOff();
    void effectPipelineUsesGeneratedDrawsForTiling();
    void maskedRepeatUsesMaskGradeDrawsAndKeyframedOffsets();
    void maskedRepeatUsesSpeechFilterAwareTransformTiming();
    void temporalEffectsUseContiguousPlaybackTimeAcrossSegmentGaps();
    void temporalEffectsUsePlaybackTimeAcrossSpeechFilterGaps();
    void titleAnimationsUseContiguousPlaybackTimeAcrossSkips();
    void temporalEffectsStayOnRawClockDuringVisualSpeedThrough();
    void temporalMovingPresetOptionsStaySmoothAcrossMultipleSpeechBoundaries();
    void generatedDrawMvpKeepsVulkanYDownOrientation();
    void tickerPresetProducesAlternatingRowsAcrossOutput();
    void alternatingMotionBackgroundCoversOutputWithMovingRows();
    void sourceTilingPresetCoversOutputWithGrid();
    void sourceTilingPresetSupportsGeometricPatterns();
    void orbitPresetProducesRequestedCopiesAroundCenter();
    void freezePatternProducesHeldGridCopies();
    void stepRepeatProducesSequencedCopies();
    void directionalTrimTickerAnimatesWidthAndDirection();
    void vulkan3dSynthProducesDepthSortedShaderDraws();
    void newsLowerThirdPresetBuildsFlyInHoldFlyOutKeyframes();
    void speakerTitleWrapAroundSpeakerBuilds3DKeyframes();
    void generatedSpeakerTitlePlacementReplacesAndAvoidsTrackConflicts();
    void generatedSpeakerTitleResolvesToTranscriptParent();
};

void TestEffectPresets::init()
{
    setTransformSkipAwareTimelineRanges({});
}

void TestEffectPresets::cleanup()
{
    setTransformSkipAwareTimelineRanges({});
}

void TestEffectPresets::clipSerializationPersistsEffectPresetState()
{
    for (const BackgroundFillEffect effect : {
             BackgroundFillEffect::None,
             BackgroundFillEffect::EdgeStretch,
             BackgroundFillEffect::ProgressiveEdgeStretch,
             BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch,
             BackgroundFillEffect::Tile,
             BackgroundFillEffect::Mirror,
             BackgroundFillEffect::BlurCover}) {
        QCOMPARE(
            backgroundFillEffectFromString(backgroundFillEffectToString(effect)),
            effect);
    }
    TimelineClip clip;
    clip.id = QStringLiteral("logo");
    clip.filePath = QStringLiteral("logo.png");
    clip.label = QStringLiteral("Logo");
    clip.mediaType = ClipMediaType::Image;
    clip.maskEnabled = true;
    clip.maskFramesDir = QStringLiteral("/tmp/masks");
    clip.maskForegroundLayerEnabled = true;
    clip.maskRepeatEnabled = true;
    clip.maskRepeatDeltaX = 120.0;
    clip.maskRepeatDeltaY = -15.0;
    clip.edgeFillEffect =
        BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch;
    clip.edgeFillPixels = 24;
    clip.edgeFillPower = 3.25;
    clip.edgeFillOpacity = 0.72;
    clip.edgeFillBrightness = -0.15;
    clip.edgeFillSaturation = 1.35;
    clip.maskFeatherGamma = 2.4;
    clip.maskFeatherFalloff = 3;
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 32;
    clip.effectSpeed = 1.75;
    clip.effectScale = 0.85;
    clip.effectAlternateDirection = true;
    clip.transformSkipAwareTiming = true;
    clip.effectSkipAwareTiming = false;
    clip.tilingPattern = ClipTilingPattern::SpiralXY;
    clip.tilingSpacing = 1.4;
    clip.tilingWrap = false;
    clip.differenceReferenceFrames = 17;
    clip.differenceThreshold = 0.22;
    clip.differenceSoftness = 0.04;
    clip.temporalEchoCount = 7;
    clip.temporalEchoSpacingFrames = 3;
    clip.temporalEchoDecay = 0.58;
    clip.effectParameterSets.insert(
        QString::number(static_cast<int>(ClipEffectPreset::NeonGlow)),
        QJsonObject{{QStringLiteral("rows"), 11},
                    {QStringLiteral("speed"), 2.5},
                    {QStringLiteral("scale"), 1.75}});

    const QJsonObject json = editor::clipToJson(clip);
    QCOMPARE(json.value(QStringLiteral("maskForegroundLayerEnabled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("maskRepeatEnabled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("maskFeatherFalloff")).toInt(), 3);
    QCOMPARE(
        json.value(QStringLiteral("edgeFillEffect")).toString(),
        QStringLiteral("progressive_bidirectional_edge_stretch"));
    QCOMPARE(json.value(QStringLiteral("edgeFillPixels")).toInt(), 24);
    QVERIFY(std::abs(json.value(QStringLiteral("maskRepeatDeltaX")).toDouble() - 120.0) < 0.000001);
    QVERIFY(std::abs(json.value(QStringLiteral("maskRepeatDeltaY")).toDouble() + 15.0) < 0.000001);
    QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(), QStringLiteral("news_logo_ticker"));
    QCOMPARE(json.value(QStringLiteral("effectRows")).toInt(), 32);
    QCOMPARE(json.value(QStringLiteral("transformSkipAwareTiming")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("effectSkipAwareTiming")).toBool(), false);
    QCOMPARE(json.value(QStringLiteral("tilingPattern")).toString(), QStringLiteral("spiral"));
    QVERIFY(std::abs(json.value(QStringLiteral("tilingSpacing")).toDouble() - 1.4) < 0.000001);
    QCOMPARE(json.value(QStringLiteral("tilingWrap")).toBool(), false);
    QCOMPARE(json.value(QStringLiteral("differenceReferenceFrames")).toInt(), 17);
    QCOMPARE(json.value(QStringLiteral("temporalEchoCount")).toInt(), 7);

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.maskForegroundLayerEnabled, true);
    QCOMPARE(loaded.maskRepeatEnabled, true);
    QCOMPARE(loaded.maskFeatherFalloff, 3);
    QVERIFY(std::abs(loaded.maskFeatherGamma - 2.4) < 0.000001);
    QVERIFY(std::abs(loaded.maskRepeatDeltaX - 120.0) < 0.000001);
    QVERIFY(std::abs(loaded.maskRepeatDeltaY + 15.0) < 0.000001);
    QCOMPARE(
        loaded.edgeFillEffect,
        BackgroundFillEffect::ProgressiveBidirectionalEdgeStretch);
    QCOMPARE(loaded.edgeFillPixels, 24);
    QVERIFY(std::abs(loaded.edgeFillPower - 3.25) < 0.000001);
    QVERIFY(std::abs(loaded.edgeFillOpacity - 0.72) < 0.000001);
    QVERIFY(std::abs(loaded.edgeFillBrightness + 0.15) < 0.000001);
    QVERIFY(std::abs(loaded.edgeFillSaturation - 1.35) < 0.000001);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::NewsLogoTicker);
    QCOMPARE(loaded.effectRows, 32);
    QVERIFY(std::abs(loaded.effectSpeed - 1.75) < 0.000001);
    QVERIFY(std::abs(loaded.effectScale - 0.85) < 0.000001);
    QCOMPARE(loaded.effectAlternateDirection, true);
    QCOMPARE(loaded.transformSkipAwareTiming, true);
    QCOMPARE(loaded.effectSkipAwareTiming, false);
    QCOMPARE(loaded.tilingPattern, ClipTilingPattern::SpiralXY);
    QVERIFY(std::abs(loaded.tilingSpacing - 1.4) < 0.000001);
    QCOMPARE(loaded.tilingWrap, false);
    QCOMPARE(loaded.differenceReferenceFrames, 17);
    QVERIFY(std::abs(loaded.differenceThreshold - 0.22) < 0.000001);
    QVERIFY(std::abs(loaded.differenceSoftness - 0.04) < 0.000001);
    QCOMPARE(loaded.temporalEchoCount, 7);
    QCOMPARE(loaded.temporalEchoSpacingFrames, 3);
    QVERIFY(std::abs(loaded.temporalEchoDecay - 0.58) < 0.000001);
    const QJsonObject neonParameters = loaded.effectParameterSets
        .value(QString::number(static_cast<int>(ClipEffectPreset::NeonGlow))).toObject();
    QCOMPARE(neonParameters.value(QStringLiteral("rows")).toInt(), 11);
    QCOMPARE(neonParameters.value(QStringLiteral("speed")).toDouble(), 2.5);
    QCOMPARE(neonParameters.value(QStringLiteral("scale")).toDouble(), 1.75);
}

void TestEffectPresets::textExtrudeModesSerializeAndMigrate()
{
    using Mode = TimelineClip::TitleKeyframe::TextExtrudeMode;
    TimelineClip clip;
    TimelineClip::TitleKeyframe keyframe;
    keyframe.vulkan3DExtrudeEnabled = true;
    keyframe.textExtrudeMode = Mode::ErodedSolid;
    clip.titleKeyframes = {keyframe};
    clip.transcriptOverlay.textExtrudeMode = Mode::StackedCopies;
    clip.transcriptOverlay.textExtrudeDepth = 0.42;
    const TimelineClip loaded = editor::clipFromJson(editor::clipToJson(clip));
    QCOMPARE(loaded.titleKeyframes.constFirst().textExtrudeMode, Mode::ErodedSolid);
    QCOMPARE(loaded.transcriptOverlay.textExtrudeMode, Mode::StackedCopies);
    QCOMPARE(loaded.transcriptOverlay.textExtrudeDepth, 0.42);

    QJsonObject legacy = editor::clipToJson(clip);
    QJsonArray keyframes = legacy.value(QStringLiteral("titleKeyframes")).toArray();
    QJsonObject legacyKeyframe = keyframes.at(0).toObject();
    legacyKeyframe.remove(QStringLiteral("textExtrudeMode"));
    keyframes[0] = legacyKeyframe;
    legacy[QStringLiteral("titleKeyframes")] = keyframes;
    QCOMPARE(editor::clipFromJson(legacy).titleKeyframes.constFirst().textExtrudeMode,
             Mode::StackedCopies);
}

void TestEffectPresets::maskFeatherFalloffProfilesShapeAlphaDifferently()
{
    QImage source(5, 1, QImage::Format_ARGB32);
    source.fill(Qt::transparent);
    source.setPixel(2, 0, qRgba(255, 255, 255, 255));

    const QImage linear = applyMaskFeather(source, 1.0, 1.0, 1);
    const QImage power = applyMaskFeather(source, 1.0, 3.0, 0);
    const QImage smoother = applyMaskFeather(source, 1.0, 1.0, 3);
    const int linearAlpha = qAlpha(linear.pixel(1, 0));
    const int powerAlpha = qAlpha(power.pixel(1, 0));
    const int smootherAlpha = qAlpha(smoother.pixel(1, 0));
    QVERIFY(powerAlpha > linearAlpha);
    QVERIFY(smootherAlpha < linearAlpha);
}

void TestEffectPresets::correctionPolygonsEraseMaskIntensity()
{
    QImage source(8, 8, QImage::Format_Grayscale8);
    source.fill(200);

    TimelineClip::CorrectionPolygon active;
    active.pointsNormalized = {
        {0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
    TimelineClip::CorrectionPolygon disabled;
    disabled.enabled = false;
    disabled.pointsNormalized = {
        {0.0, 0.0}, {0.25, 0.0}, {0.25, 0.25}, {0.0, 0.25}};

    const QImage corrected = applyCorrectionPolygonsToMaskImage(
        source, {active, disabled});
    QCOMPARE(corrected.format(), QImage::Format_Grayscale8);
    QCOMPARE(corrected.constScanLine(4)[4], uchar{0});
    QCOMPARE(corrected.constScanLine(0)[0], uchar{200});
    QCOMPARE(source.constScanLine(4)[4], uchar{200});
}

void TestEffectPresets::maskSidecarDiscoveryProvidesStableIdentityAndCoverage()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("shot.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.close();
    const QString sidecarDir = temp.filePath(QStringLiteral("shot_sam3_person_binary_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    for (const int frame : {7, 8, 12}) {
        QFile image(QDir(sidecarDir).filePath(
            QStringLiteral("frame_%1.png").arg(frame, 6, 10, QLatin1Char('0'))));
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write("png");
    }
    TimelineClip clip;
    clip.filePath = mediaPath;
    const auto sidecars = editor::masks::discoverMaskSidecars(clip);
    QCOMPARE(sidecars.size(), 1);
    QCOMPARE(sidecars.constFirst().displayName, QStringLiteral("person"));
    QCOMPARE(sidecars.constFirst().frameCount, 3);
    QCOMPARE(sidecars.constFirst().firstFrame, int64_t(7));
    QCOMPARE(sidecars.constFirst().lastFrame, int64_t(12));
    QCOMPARE(sidecars.constFirst().id,
             editor::masks::stableMaskSidecarId(sidecarDir));
    QVERIFY(!sidecars.constFirst().isReadyForTimeline());
    QCOMPARE(sidecars.constFirst().readinessIssue, QStringLiteral("Frame map missing"));
}

void TestEffectPresets::neutralMaskSidecarDiscoveryMatchesQtIdentityAndReadiness()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("shot.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.write("source");
    media.close();
    const QString sidecarDir = temp.filePath(
        QStringLiteral("shot_sam3_person_binary_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    for (const int frame : {7, 8, 12}) {
        QFile image(QDir(sidecarDir).filePath(
            QStringLiteral("frame_%1.png").arg(frame, 6, 10, QLatin1Char('0'))));
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write("png");
    }
    TimelineClip clip;
    clip.filePath = mediaPath;
    const auto qtSidecars = editor::masks::discoverMaskSidecars(clip);
    const auto coreSidecars = jcut::masks::discoverMaskSidecarsCore(
        mediaPath.toStdString());
    QCOMPARE(qtSidecars.size(), 1);
    QCOMPARE(coreSidecars.size(), std::size_t(1));
    const auto& core = coreSidecars.front();
    QCOMPARE(QString::fromStdString(core.id), qtSidecars.front().id);
    QCOMPARE(QString::fromStdString(core.displayName), qtSidecars.front().displayName);
    QCOMPARE(core.frameCount, std::int64_t(qtSidecars.front().frameCount));
    QCOMPARE(core.firstFrame, qtSidecars.front().firstFrame);
    QCOMPARE(core.lastFrame, qtSidecars.front().lastFrame);
    QCOMPARE(core.decodeOrdinalFrames, qtSidecars.front().decodeOrdinalFrames);
    QCOMPARE(core.ready, qtSidecars.front().isReadyForTimeline());
}

void TestEffectPresets::neutralMaskFrameMapValidatesOrdinalSidecarsAndSourceIdentity()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString sourcePath = temp.filePath(QStringLiteral("source.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("source-v1"), qint64{9});
    source.close();

    const QString sidecarDir = temp.filePath(QStringLiteral("source_sam3_person_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    QFile map(QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(map.open(QIODevice::WriteOnly));
    QVERIFY(map.write("# source_frame\tmask_frame\n0\t0\n") > 0);
    map.close();
    QImage frame(2, 2, QImage::Format_Grayscale8);
    frame.fill(255);
    QVERIFY(frame.save(QDir(sidecarDir).filePath(QStringLiteral("frame_000001.png"))));
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(sidecarDir, sourcePath));
    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, sourcePath, false));

    const auto validated = jcut::masks::loadMaskFrameMapCore(
        sidecarDir.toStdString(), sourcePath.toStdString());
    QVERIFY2(validated.ordinalSidecar, validated.error.c_str());
    QVERIFY2(validated.metadataVerified, validated.error.c_str());
    QVERIFY2(validated.renderReady, validated.error.c_str());
    QCOMPARE(validated.mappedFrameCount, std::int64_t{1});
    QCOMPARE(jcut::masks::mappedMaskFrameForSourceFrameCore(
        sidecarDir.toStdString(), sourcePath.toStdString(), 0),
        std::optional<std::int64_t>{0});
    QVERIFY(!jcut::masks::mappedMaskFrameForSourceFrameCore(
        sidecarDir.toStdString(), sourcePath.toStdString(), 1));

    QVERIFY(source.open(QIODevice::Append));
    QCOMPARE(source.write("-changed"), qint64{8});
    source.close();
    const auto stale = jcut::masks::loadMaskFrameMapCore(
        sidecarDir.toStdString(), sourcePath.toStdString());
    QVERIFY(!stale.metadataVerified);
    QVERIFY(!stale.renderReady);
    QVERIFY(!jcut::masks::mappedMaskFrameForSourceFrameCore(
        sidecarDir.toStdString(), sourcePath.toStdString(), 0));

    const QString manualDir = temp.filePath(QStringLiteral("manual_mask"));
    QVERIFY(QDir().mkpath(manualDir));
    QVERIFY(frame.save(QDir(manualDir).filePath(QStringLiteral("frame_000006.png"))));
    QCOMPARE(jcut::masks::mappedMaskFrameForSourceFrameCore(
        manualDir.toStdString(), sourcePath.toStdString(), 5),
        std::optional<std::int64_t>{5});
    QVERIFY(jcut::masks::maskFramePathForSourceFrameCore(
        manualDir.toStdString(), sourcePath.toStdString(), 5));
}

void TestEffectPresets::neutralMaskFrameMapCacheReusesValidationAndInvalidatesChanges()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString sourcePath = temp.filePath(QStringLiteral("source.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("source-v1"), qint64{9});
    source.close();

    const QString sidecarDir =
        temp.filePath(QStringLiteral("source_sam3_person_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    QFile map(QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(map.open(QIODevice::WriteOnly));
    QVERIFY(map.write("# source_frame\tmask_frame\n0\t0\n") > 0);
    map.close();
    const QString framePath =
        QDir(sidecarDir).filePath(QStringLiteral("frame_000001.png"));
    QImage frame(2, 2, QImage::Format_Grayscale8);
    frame.fill(255);
    QVERIFY(frame.save(framePath));
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(
        sidecarDir, sourcePath));
    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, sourcePath, false));

    jcut::masks::clearMaskFrameMapCoreCache();
    const auto mappedFrame = [&]() {
        return jcut::masks::mappedMaskFrameForSourceFrameCore(
            sidecarDir.toStdString(), sourcePath.toStdString(), 0);
    };
    QCOMPARE(mappedFrame(), std::optional<std::int64_t>{0});
    auto stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.missCount, std::uint64_t{1});
    QCOMPARE(stats.validationCount, std::uint64_t{1});

    for (int repeat = 0; repeat < 5; ++repeat) {
        QCOMPARE(mappedFrame(), std::optional<std::int64_t>{0});
    }
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.hitCount, std::uint64_t{5});
    QCOMPARE(stats.validationCount, std::uint64_t{1});

    QVERIFY(QFile::remove(framePath));
    QVERIFY(!mappedFrame());
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.validationCount, std::uint64_t{2});

    QVERIFY(frame.save(framePath));
    QCOMPARE(mappedFrame(), std::optional<std::int64_t>{0});
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.validationCount, std::uint64_t{3});

    QFile emptyFrame(framePath);
    QVERIFY(emptyFrame.open(QIODevice::WriteOnly | QIODevice::Truncate));
    emptyFrame.close();
    QVERIFY(!jcut::masks::maskFramePathForSourceFrameCore(
        sidecarDir.toStdString(), sourcePath.toStdString(), 0));
    QVERIFY(frame.save(framePath));

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, sourcePath, false, false));
    QVERIFY(!mappedFrame());
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    const std::uint64_t incompleteValidationCount = stats.validationCount;

    QFile unrelated(QDir(sidecarDir).filePath(QStringLiteral("still_generating.tmp")));
    QVERIFY(unrelated.open(QIODevice::WriteOnly));
    unrelated.write("progress");
    unrelated.close();
    QVERIFY(!mappedFrame());
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.validationCount, incompleteValidationCount);

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, sourcePath, false, true));
    QCOMPARE(mappedFrame(), std::optional<std::int64_t>{0});
    stats = jcut::masks::maskFrameMapCoreCacheStats();
    QCOMPARE(stats.validationCount, incompleteValidationCount + 1);
}

void TestEffectPresets::genericAiMaskSidecarsBecomeNestedChildrenWithIndependentZLevels()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("shot.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.close();

    const QString samDir = temp.filePath(QStringLiteral("shot_sam3_person_binary_masks"));
    const QString genericDir = temp.filePath(QStringLiteral("shot_custom_ai_microphone_mattes"));
    for (const QString& directory : {samDir, genericDir}) {
        QVERIFY(QDir().mkpath(directory));
        QFile frame(QDir(directory).filePath(QStringLiteral("frame_000001.png")));
        QVERIFY(frame.open(QIODevice::WriteOnly));
        frame.write("png");
    }
    QFile samMap(QDir(samDir).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(samMap.open(QIODevice::WriteOnly));
    samMap.write("# source_frame\tmask_frame\n0\t0\n");
    samMap.close();
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(samDir, mediaPath));
    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        samDir, mediaPath, false));

    TimelineClip source;
    source.id = QStringLiteral("shot");
    source.filePath = mediaPath;
    source.label = QStringLiteral("Shot");
    source.mediaType = ClipMediaType::Video;
    source.trackIndex = 4;
    source.zLevel = 30;
    source.durationFrames = 120;
    QVector<TimelineClip> clips{source};

    QVERIFY(reconcileMaskMatteChildrenFromDisk(clips));
    QCOMPARE(clips.size(), 3);
    QVector<TimelineClip> children;
    for (const TimelineClip& clip : std::as_const(clips)) {
        if (clip.clipRole == ClipRole::MaskMatte) children.push_back(clip);
    }
    QCOMPARE(children.size(), 2);
    std::sort(children.begin(), children.end(), [](const TimelineClip& left, const TimelineClip& right) {
        return left.zLevel < right.zLevel;
    });
    QCOMPARE(children.at(0).trackIndex, source.trackIndex);
    QCOMPARE(children.at(1).trackIndex, source.trackIndex);
    QCOMPARE(children.at(0).linkedSourceClipId, source.id);
    QCOMPARE(children.at(1).linkedSourceClipId, source.id);
    QCOMPARE(children.at(0).zLevel, 31);
    QCOMPARE(children.at(1).zLevel, 32);
    QVERIFY(children.at(0).id != children.at(1).id);
    QVERIFY(children.at(0).maskSidecarAvailable);
    QVERIFY(children.at(1).maskSidecarAvailable);
    QVERIFY(!reconcileMaskMatteChildrenFromDisk(clips));
    QCOMPARE(clips.size(), 3);

    children[0].zLevel = 90;
    const QJsonObject serialized = editor::clipToJson(children[0]);
    QCOMPARE(serialized.value(QStringLiteral("zLevel")).toInt(), 90);
    QCOMPARE(editor::clipFromJson(serialized).zLevel, 90);
}

void TestEffectPresets::birefnetSidecarDiscoveryPreservesContinuousAlphaMetadata()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("portrait.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.close();
    const QString sidecarDir = temp.filePath(QStringLiteral("portrait_birefnet_alpha_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    QFile frame(QDir(sidecarDir).filePath(QStringLiteral("frame_000001.png")));
    QVERIFY(frame.open(QIODevice::WriteOnly));
    frame.write("png");
    frame.close();
    QFile frameMap(QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(frameMap.open(QIODevice::WriteOnly));
    frameMap.write("# source_frame\tmask_frame\n0\t0\n");
    frameMap.close();
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(sidecarDir, mediaPath));
    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, mediaPath, true));

    TimelineClip clip;
    clip.filePath = mediaPath;
    const auto sidecars = editor::masks::discoverMaskSidecars(clip);
    QCOMPARE(sidecars.size(), 1);
    QCOMPARE(sidecars.constFirst().displayName, QStringLiteral("alpha"));
    QCOMPARE(sidecars.constFirst().sourceType, QStringLiteral("birefnet_continuous_alpha"));
    QVERIFY(sidecars.constFirst().decodeOrdinalFrames);
    QVERIFY(sidecars.constFirst().frameIndexMapAvailable);
    QVERIFY(sidecars.constFirst().frameIndexMetadataAvailable);
    QCOMPARE(sidecars.constFirst().mappedFrameCount, int64_t(1));
    QCOMPARE(sidecars.constFirst().firstMappedSourceFrame, int64_t(0));
    QCOMPARE(sidecars.constFirst().lastMappedSourceFrame, int64_t(0));
    QVERIFY(sidecars.constFirst().completionConfirmed);
    QVERIFY(sidecars.constFirst().isReadyForTimeline());
    QVERIFY(sidecars.constFirst().readinessIssue.isEmpty());
}

void TestEffectPresets::incompleteBiRefNetSidecarsStayDisabledAndUnmaterialized()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("portrait.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.close();

    const QString sidecarDir = temp.filePath(QStringLiteral("portrait_birefnet_alpha_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    QFile frame(QDir(sidecarDir).filePath(QStringLiteral("frame_000001.png")));
    QVERIFY(frame.open(QIODevice::WriteOnly));
    frame.write("png");
    frame.close();
    QFile frameMap(QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.tsv")));
    QVERIFY(frameMap.open(QIODevice::WriteOnly));
    frameMap.write("# source_frame\tmask_frame\n0\t0\n");
    frameMap.close();
    QVERIFY(mask_sidecar_test::writeSingleFrameMapMetadata(sidecarDir, mediaPath));

    const editor::masks::MaskSidecar sidecar =
        editor::masks::inspectMaskSidecar(sidecarDir, QStringLiteral("portrait"));
    QVERIFY(sidecar.isValid());
    QVERIFY(sidecar.decodeOrdinalFrames);
    QVERIFY(sidecar.frameIndexMapAvailable);
    QVERIFY(sidecar.frameIndexMetadataAvailable);
    QVERIFY(!sidecar.completionConfirmed);
    QVERIFY(!sidecar.isReadyForTimeline());
    QCOMPARE(sidecar.readinessIssue, QStringLiteral("Generation incomplete"));

    TimelineClip source;
    source.id = QStringLiteral("portrait");
    source.filePath = mediaPath;
    source.label = QStringLiteral("Portrait");
    source.mediaType = ClipMediaType::Video;
    source.durationFrames = 30;
    QVector<TimelineClip> clips{source};
    QVERIFY(!reconcileMaskMatteChildrenFromDisk(clips));
    QCOMPARE(clips.size(), 1);

    TimelineClip persisted = makeMaskMatteClip(source);
    persisted.id = QStringLiteral("persisted-incomplete-alpha");
    persisted.maskFramesDir = sidecarDir;
    persisted.generatedFromMaskId = sidecar.id;
    persisted.maskEnabled = true;
    clips.push_back(persisted);
    bool availabilityChanged = false;
    QVERIFY(!reconcileMaskMatteChildrenFromDisk(clips, &availabilityChanged));
    QVERIFY(availabilityChanged);
    QCOMPARE(clips.size(), 2);
    QVERIFY(clips.constLast().maskEnabled);
    QVERIFY(!clips.constLast().maskSidecarAvailable);
    QCOMPARE(clips.constLast().maskSidecarAvailabilityIssue,
             QStringLiteral("Generation incomplete"));

    QVERIFY(mask_sidecar_test::writeSingleFrameCompletion(
        sidecarDir, mediaPath, true));
    availabilityChanged = false;
    QVERIFY(reconcileMaskMatteChildrenFromDisk(clips, &availabilityChanged));
    QVERIFY(availabilityChanged);
    QVERIFY(clips.constLast().maskEnabled);
    QVERIFY(clips.constLast().maskSidecarAvailable);
    QVERIFY(clips.constLast().maskSidecarAvailabilityIssue.isEmpty());
}

void TestEffectPresets::completedOrdinalSidecarWithInteriorHoleFailsClosed()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString mediaPath = temp.filePath(QStringLiteral("source.mp4"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    media.write("source-content");
    media.close();

    const QString sidecarDir =
        temp.filePath(QStringLiteral("source_sam3_person_binary_masks"));
    QVERIFY(QDir().mkpath(sidecarDir));
    for (const int frame : {1, 3}) {
        QImage image(2, 2, QImage::Format_Grayscale8);
        image.fill(frame * 32);
        QVERIFY(image.save(QDir(sidecarDir).filePath(
            QStringLiteral("frame_%1.png").arg(
                frame, 6, 10, QLatin1Char('0')))));
    }
    const QString mapPath =
        QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.tsv"));
    QFile frameMap(mapPath);
    QVERIFY(frameMap.open(QIODevice::WriteOnly));
    frameMap.write("# source_frame\tmask_frame\n0\t0\n1\t1\n2\t2\n");
    frameMap.close();
    const QJsonObject identity = mask_sidecar_test::sourceIdentity(mediaPath);
    QVERIFY(!identity.isEmpty());
    QVERIFY(mask_sidecar_test::writeJson(
        QDir(sidecarDir).filePath(QStringLiteral("jcut_frame_map.json")),
        QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("jcut_frame_index_map_v2")},
            {QStringLiteral("status"), QStringLiteral("ready")},
            {QStringLiteral("frame_domain"),
             QStringLiteral("source_timestamp_to_generated_ordinal")},
            {QStringLiteral("source_identity"), identity},
            {QStringLiteral("output_fps"), QJsonValue::Null},
            {QStringLiteral("map_file"), QStringLiteral("jcut_frame_map.tsv")},
            {QStringLiteral("map_sha256"), mask_sidecar_test::fileSha256(mapPath)},
            {QStringLiteral("mapped_frame_count"), 3},
            {QStringLiteral("min_source_frame"), 0},
            {QStringLiteral("max_source_frame"), 2},
            {QStringLiteral("max_mask_frame"), 2},
            {QStringLiteral("expected_output_frame_count"), 3},
        }));
    QVERIFY(mask_sidecar_test::writeJson(
        QDir(sidecarDir).filePath(QStringLiteral("jcut_mask.json")),
        QJsonObject{
            {QStringLiteral("schema"), QStringLiteral("jcut_mask_sidecar_v1")},
            {QStringLiteral("complete"), true},
            {QStringLiteral("source_type"), QStringLiteral("sam3_binary_frames")},
            {QStringLiteral("frame_domain"), QStringLiteral("decode_ordinal")},
            {QStringLiteral("frame_index_map"), QStringLiteral("jcut_frame_map.tsv")},
            {QStringLiteral("frame_index_metadata"), QStringLiteral("jcut_frame_map.json")},
            {QStringLiteral("frame_map_sha256"), mask_sidecar_test::fileSha256(mapPath)},
            {QStringLiteral("expected_frame_count"), 3},
            {QStringLiteral("source_identity"), identity},
        }));

    const editor::masks::MaskSidecar sidecar =
        editor::masks::inspectMaskSidecar(
            sidecarDir, QStringLiteral("source"), mediaPath);
    QVERIFY(sidecar.isValid());
    QVERIFY(sidecar.completionConfirmed);
    QVERIFY(!sidecar.frameCoverageComplete);
    QVERIFY(!sidecar.isReadyForTimeline());
    QCOMPARE(sidecar.readinessIssue,
             QStringLiteral("Frame coverage incomplete"));

    TimelineClip clip;
    clip.filePath = mediaPath;
    clip.maskFramesDir = sidecarDir;
    QVERIFY(!editor::masks::hasReadyMaskSidecar(clip));
    QVERIFY2(rawClipMaskImage(clip, 0).isNull(),
             "Runtime must reject every frame when an ordinal sidecar has an interior hole");
    QVERIFY2(rawClipMaskImage(clip, 2).isNull(),
             "A present mask frame cannot bypass incomplete-artifact rejection");
}

void TestEffectPresets::birefnetUxExposesExplicitContextActionsAndPreview()
{
    auto readSource = [](const QString& relativePath) {
        QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
        if (!file.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(file.readAll());
    };
    const QString timelineMenu = readSource(QStringLiteral("timeline_widget_context_menu.cpp"));
    const QString clipsMenu = readSource(QStringLiteral("clips_tab.cpp"));
    const QString editor = readSource(QStringLiteral("editor_birefnet.cpp"));
    const QString shell = readSource(QStringLiteral("birefnet.sh"));
    const QString runner = readSource(QStringLiteral("birefnet_run.py"));
    const QString samRunner = readSource(QStringLiteral("sam3_run.py"));
    const QString frameMapHelper = readSource(QStringLiteral("jcut_frame_index_map.py"));
    const QString maskTab = readSource(QStringLiteral("mask_tab.cpp"));
    const QString inspector = readSource(QStringLiteral("inspector_pane.cpp"));

    QVERIFY2(timelineMenu.contains(QStringLiteral("Rotoscope")) &&
                 timelineMenu.contains(QStringLiteral("Run SAM 3...")) &&
                 timelineMenu.contains(QStringLiteral("Run BiRefNet...")) &&
                 timelineMenu.contains(QStringLiteral("rotoscopeSourceClipId")) &&
                 timelineMenu.contains(QStringLiteral("clipParent(*rotoscopeSource")),
             "Timeline right-click must expose explicit SAM and BiRefNet actions.");
    QVERIFY2(clipsMenu.contains(QStringLiteral("Run BiRefNet...")),
             "Clips-table right-click must expose BiRefNet.");
    QVERIFY2(editor.contains(QStringLiteral("Preview Frame %1")) &&
                 editor.contains(QStringLiteral("Composite Check")) &&
                 editor.contains(QStringLiteral("preview_source.png")) &&
                 editor.contains(QStringLiteral("Live result")) &&
                 editor.contains(QStringLiteral("jcut_live_preview.png")),
             "BiRefNet preflight must provide a visual source/alpha/composite preview.");
    QVERIFY2(runner.contains(QStringLiteral("--frame-index")) &&
                 runner.contains(QStringLiteral("preview_source.png")) &&
                 runner.contains(QStringLiteral("--live-preview")) &&
                 runner.contains(QStringLiteral("live_preview_strip")),
             "BiRefNet runner must support bounded and live source/alpha/composite previews.");
    QVERIFY2(shell.contains(QStringLiteral("jcut_frame_index_map.py")) &&
                 shell.contains(QStringLiteral("--source-frame")) &&
                 shell.contains(QStringLiteral("PREVIEW_ONLY")) &&
                 runner.contains(QStringLiteral("\"frame_domain\": \"decode_ordinal\"")) &&
                 runner.contains(QStringLiteral("\"frame_index_map\": \"jcut_frame_map.tsv\"")) &&
                 runner.contains(QStringLiteral("jcut_alpha.json")) &&
                 runner.contains(QStringLiteral("verify_contiguous_alpha_frames")) &&
                 samRunner.contains(QStringLiteral("from jcut_frame_index_map import")) &&
                 samRunner.contains(QStringLiteral("jcut_mask.json")) &&
                 samRunner.contains(QStringLiteral("verify_contiguous_mask_frames")) &&
                 frameMapHelper.contains(QStringLiteral("jcut_frame_index_map_v2")) &&
                 frameMapHelper.contains(QStringLiteral("source_identity")) &&
                 frameMapHelper.contains(QStringLiteral("map_sha256")) &&
                 frameMapHelper.contains(QStringLiteral("-show_frames")) &&
                 frameMapHelper.contains(QStringLiteral("best_effort_timestamp")) &&
                 frameMapHelper.contains(QStringLiteral("os.replace")),
             "Ordinal mask generators must share an atomic decoded-frame timestamp map.");
    QVERIFY2(editor.contains(QStringLiteral("Alpha tolerance")) &&
                 editor.contains(QStringLiteral("alpha_tolerance")) &&
                 runner.contains(QStringLiteral("--alpha-tolerance")) &&
                 runner.contains(QStringLiteral("apply_alpha_tolerance")),
             "BiRefNet alpha tolerance must be exposed consistently in preview and generation.");
    QVERIFY2(runner.contains(QStringLiteral("torch.OutOfMemoryError")) &&
                 runner.contains(QStringLiteral("jcut_error.json")) &&
                 runner.contains(QStringLiteral("JCUT_BIREFNET_ERROR_JSON=")) &&
                 runner.contains(QStringLiteral("completed_frames_preserved")),
             "BiRefNet must emit a structured, resumable CUDA OOM failure.");
    QVERIFY2(editor.contains(QStringLiteral("GPU Memory Exhausted")) &&
                 editor.contains(QStringLiteral("possible_memory_kill")) &&
                 editor.contains(QStringLiteral("exitCode == 137")) &&
                 editor.contains(QStringLiteral("error_kind")) &&
                 editor.contains(QStringLiteral("error_frame_index")),
             "BiRefNet UI and manifests must classify OOM and ambiguous SIGKILL failures.");
    QVERIFY2(editor.contains(QStringLiteral("added as a source-linked Mask Matte child")) &&
                 editor.contains(QStringLiteral("createOrReplaceMaskMatte")),
             "A completed BiRefNet run must offer an immediately usable downstream mask layer.");
    QVERIFY2(maskTab.contains(QStringLiteral("discoverMaskSidecars(clip)")) &&
                 maskTab.contains(QStringLiteral("soft alpha")),
             "The Masks tab must auto-discover and identify continuous-alpha sidecars.");
    QVERIFY2(maskTab.contains(QStringLiteral("setMaskSidecarAssociation")) &&
                 maskTab.contains(QStringLiteral("findMaskMatteChildForSidecar")) &&
                 maskTab.contains(QStringLiteral("materializeMaskMatteForSidecar")) &&
                 maskTab.contains(QStringLiteral("isMaskInspectorActive")) &&
                 maskTab.contains(QStringLiteral("setTreatmentControlsEnabled")) &&
                 maskTab.contains(QStringLiteral("Current sidecar (unavailable)")) &&
                 maskTab.contains(QStringLiteral("clip->clipRole == ClipRole::Media")) &&
                 maskTab.contains(QStringLiteral("selectClipById(existingChildId)")),
             "The Masks tab must update path and stable identity atomically, and select an "
             "existing or newly materialized sidecar child instead of editing its parent or "
             "retargeting another child.");
    QVERIFY2(!inspector.contains(QStringLiteral("SAM mask is foreground layer")) &&
                 !inspector.contains(QStringLiteral("SAM binary mask frames directory")),
             "Shared downstream mask controls must not be branded as SAM-only.");
}

void TestEffectPresets::clipSerializationMigratesLegacyEffectSpeechSync()
{
    TimelineClip clip;
    clip.id = QStringLiteral("legacy-logo");
    clip.filePath = QStringLiteral("legacy-logo.png");
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.transformSkipAwareTiming = true;
    clip.effectSkipAwareTiming = false;

    QJsonObject legacyJson = editor::clipToJson(clip);
    legacyJson.remove(QStringLiteral("effectSkipAwareTiming"));

    const TimelineClip loaded = editor::clipFromJson(legacyJson);
    QCOMPARE(loaded.transformSkipAwareTiming, true);
    QCOMPARE(loaded.effectSkipAwareTiming, true);
}

void TestEffectPresets::clipSerializationPersistsArpeggiatorEffectPresets()
{
    auto roundTripPreset = [](ClipEffectPreset preset, const QString& expectedJson) {
        TimelineClip clip;
        clip.id = expectedJson;
        clip.effectPreset = preset;
        const QJsonObject json = editor::clipToJson(clip);
        QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(), expectedJson);
        QCOMPARE(editor::clipFromJson(json).effectPreset, preset);
    };

    roundTripPreset(ClipEffectPreset::FreezePattern, QStringLiteral("freeze_pattern"));
    roundTripPreset(ClipEffectPreset::StepRepeat, QStringLiteral("step_repeat"));
    roundTripPreset(ClipEffectPreset::DirectionalTrimTicker, QStringLiteral("directional_trim_ticker"));
    roundTripPreset(ClipEffectPreset::SourceTile, QStringLiteral("source_tile"));
    roundTripPreset(ClipEffectPreset::Vulkan3DSynth, QStringLiteral("vulkan_3d_synth"));
    roundTripPreset(ClipEffectPreset::ProgressiveEdgeStretch, QStringLiteral("progressive_edge_stretch"));
    roundTripPreset(ClipEffectPreset::DifferenceMatte, QStringLiteral("difference_matte"));
    roundTripPreset(ClipEffectPreset::TemporalEcho, QStringLiteral("temporal_echo"));
    roundTripPreset(ClipEffectPreset::MirrorRing, QStringLiteral("mirror_ring"));
    roundTripPreset(ClipEffectPreset::Tessellation, QStringLiteral("tessellation"));
    roundTripPreset(ClipEffectPreset::Kaleidoscope, QStringLiteral("kaleidoscope"));
    roundTripPreset(ClipEffectPreset::HexagonalPrism, QStringLiteral("hexagonal_prism"));
    roundTripPreset(ClipEffectPreset::Droste, QStringLiteral("droste"));
    roundTripPreset(ClipEffectPreset::PolarTunnel, QStringLiteral("polar_tunnel"));
    roundTripPreset(ClipEffectPreset::TinyPlanet, QStringLiteral("tiny_planet"));
    roundTripPreset(ClipEffectPreset::InfiniteMirror, QStringLiteral("infinite_mirror"));
    roundTripPreset(ClipEffectPreset::QuadMirror, QStringLiteral("quad_mirror"));
    roundTripPreset(ClipEffectPreset::SlitScan, QStringLiteral("slit_scan"));
    roundTripPreset(ClipEffectPreset::DisplacementMap, QStringLiteral("displacement_map"));
    roundTripPreset(ClipEffectPreset::TwirlVortex, QStringLiteral("twirl_vortex"));
    roundTripPreset(ClipEffectPreset::RippleShockwave, QStringLiteral("ripple_shockwave"));
    roundTripPreset(ClipEffectPreset::PixelSorting, QStringLiteral("pixel_sorting"));
    roundTripPreset(ClipEffectPreset::DatamoshGlitch, QStringLiteral("datamosh_glitch"));
    roundTripPreset(ClipEffectPreset::RgbSplit, QStringLiteral("rgb_split"));
    roundTripPreset(ClipEffectPreset::HalftoneMosaic, QStringLiteral("halftone_mosaic"));
    roundTripPreset(ClipEffectPreset::GlassRefraction, QStringLiteral("glass_refraction"));
    roundTripPreset(ClipEffectPreset::SobelEdges, QStringLiteral("sobel_edges"));
    roundTripPreset(ClipEffectPreset::NeonGlow, QStringLiteral("neon_glow"));
    roundTripPreset(ClipEffectPreset::SpeakerMaskDilation, QStringLiteral("speaker_mask_dilation"));
    roundTripPreset(ClipEffectPreset::SpeakerMaskDilationPulse, QStringLiteral("speaker_mask_dilation_pulse"));
    roundTripPreset(ClipEffectPreset::SpeakerMaskDilationRings, QStringLiteral("speaker_mask_dilation_rings"));
}

void TestEffectPresets::effectPresetMetadataCoversSerializedSynthPresets()
{
    const QVector<EffectPresetUiOption> options = effectPresetUiOptions();
    auto hasPreset = [&options](ClipEffectPreset preset) {
        for (const EffectPresetUiOption& option : options) {
            if (option.preset == preset && !option.label.trimmed().isEmpty()) {
                return true;
            }
        }
        return false;
    };

    const QVector<ClipEffectPreset> serializedPresets{
        ClipEffectPreset::None,
        ClipEffectPreset::NewsLogoTicker,
        ClipEffectPreset::PersonOrbit,
        ClipEffectPreset::AlternatingMotionBackground,
        ClipEffectPreset::FreezePattern,
        ClipEffectPreset::StepRepeat,
        ClipEffectPreset::DirectionalTrimTicker,
        ClipEffectPreset::SourceTile,
        ClipEffectPreset::Vulkan3DSynth,
        ClipEffectPreset::ProgressiveEdgeStretch,
        ClipEffectPreset::DifferenceMatte,
        ClipEffectPreset::TemporalEcho,
        ClipEffectPreset::MirrorRing,
        ClipEffectPreset::Tessellation,
        ClipEffectPreset::Kaleidoscope,
        ClipEffectPreset::HexagonalPrism,
        ClipEffectPreset::Droste,
        ClipEffectPreset::PolarTunnel,
        ClipEffectPreset::TinyPlanet,
        ClipEffectPreset::InfiniteMirror,
        ClipEffectPreset::QuadMirror,
        ClipEffectPreset::SlitScan,
        ClipEffectPreset::DisplacementMap,
        ClipEffectPreset::TwirlVortex,
        ClipEffectPreset::RippleShockwave,
        ClipEffectPreset::PixelSorting,
        ClipEffectPreset::DatamoshGlitch,
        ClipEffectPreset::RgbSplit,
        ClipEffectPreset::HalftoneMosaic,
        ClipEffectPreset::GlassRefraction,
        ClipEffectPreset::SobelEdges,
        ClipEffectPreset::NeonGlow,
        ClipEffectPreset::SpeakerMaskDilation,
        ClipEffectPreset::SpeakerMaskDilationPulse,
        ClipEffectPreset::SpeakerMaskDilationRings,
    };

    QCOMPARE(options.size(), serializedPresets.size());
    for (ClipEffectPreset preset : serializedPresets) {
        QVERIFY(hasPreset(preset));
    }

    QVERIFY(effectPresetUsesDirectionalControl(ClipEffectPreset::NewsLogoTicker));
    QVERIFY(effectPresetUsesDirectionalControl(ClipEffectPreset::SourceTile));
    QVERIFY(effectPresetUsesDirectionalControl(ClipEffectPreset::Vulkan3DSynth));
    QVERIFY(effectPresetUsesDirectionalControl(ClipEffectPreset::ProgressiveEdgeStretch));
    QVERIFY(!effectPresetUsesDirectionalControl(ClipEffectPreset::FreezePattern));
    QVERIFY(effectPresetUsesTilingControls(ClipEffectPreset::SourceTile));
    QVERIFY(!effectPresetUsesTilingControls(ClipEffectPreset::Vulkan3DSynth));

    QCOMPARE(tilingPatternUiOptions().size(), 6);

    TimelineClip parameterized;
    parameterized.effectPreset = ClipEffectPreset::NeonGlow;
    parameterized.effectRows = 3;
    parameterized.effectScale = 2.25;
    parameterized.effectSpeed = 1.5;
    parameterized.tilingSpacing = 1.75;
    const auto neonPlan = render_detail::vulkanEffectPipelinePlan(
        parameterized, QRectF(0, 0, 1920, 1080), QSize(1920, 1080), 20.0, 12.0);
    QVERIFY(neonPlan.usesGeneratedDraws());
    QCOMPARE(neonPlan.generatedDraws.constFirst().shaderMode,
             render_detail::kVulkanEffectModeNeonGlow);
    QCOMPARE(neonPlan.generatedDraws.constFirst().effectParams[0], 2.25f);
    QCOMPARE(neonPlan.generatedDraws.constFirst().effectParams[1], 3.0f);
    QCOMPARE(neonPlan.generatedDraws.constFirst().effectParams[2], 18.0f);
    QCOMPARE(neonPlan.generatedDraws.constFirst().effectParams[3], 1.75f);
}

void TestEffectPresets::trackEffectSettingsDoNotLeakIntoChildClips()
{
    TimelineClip clip;
    clip.trackIndex = 1;
    clip.effectPreset = ClipEffectPreset::None;
    clip.effectRows = 3;

    QVector<TimelineTrack> tracks(2);
    tracks[1].effectPreset = ClipEffectPreset::SourceTile;
    tracks[1].effectRows = 7;
    tracks[1].effectSpeed = 0.25;
    tracks[1].effectScale = 1.5;
    tracks[1].effectAlternateDirection = false;
    tracks[1].tilingPattern = ClipTilingPattern::Diamond;
    tracks[1].tilingSpacing = 1.25;
    tracks[1].tilingWrap = false;
    tracks[1].differenceReferenceFrames = 9;
    tracks[1].differenceThreshold = 0.31;
    tracks[1].differenceSoftness = 0.08;
    tracks[1].temporalEchoCount = 6;
    tracks[1].temporalEchoSpacingFrames = 5;
    tracks[1].temporalEchoDecay = 0.44;

    const TimelineClip effective = clipWithTrackEffectSettings(clip, tracks);
    QCOMPARE(effective.effectPreset, ClipEffectPreset::None);
    QCOMPARE(effective.effectRows, 3);
    QCOMPARE(effective.effectSpeed, clip.effectSpeed);
    QCOMPARE(effective.effectScale, clip.effectScale);
    QCOMPARE(effective.effectAlternateDirection, clip.effectAlternateDirection);
    QCOMPARE(effective.tilingPattern, clip.tilingPattern);
    QCOMPARE(effective.tilingSpacing, clip.tilingSpacing);
    QCOMPARE(effective.tilingWrap, clip.tilingWrap);
    QCOMPARE(effective.differenceReferenceFrames, clip.differenceReferenceFrames);
    QCOMPARE(effective.temporalEchoCount, clip.temporalEchoCount);
    QCOMPARE(clip.effectPreset, ClipEffectPreset::None);
    QCOMPARE(clip.effectRows, 3);
}

void TestEffectPresets::maskMatteSourceHistoryEffectsRenderInactive()
{
    QVERIFY(!effectPresetSupportedForClipRole(
        ClipEffectPreset::DifferenceMatte, ClipRole::MaskMatte));
    QVERIFY(!effectPresetSupportedForClipRole(
        ClipEffectPreset::TemporalEcho, ClipRole::MaskMatte));
    QVERIFY(effectPresetSupportedForClipRole(
        ClipEffectPreset::SourceTile, ClipRole::MaskMatte));
    QVERIFY(effectPresetSupportedForClipRole(
        ClipEffectPreset::DifferenceMatte, ClipRole::Media));

    TimelineClip matte;
    matte.clipRole = ClipRole::MaskMatte;
    matte.effectPreset = ClipEffectPreset::DifferenceMatte;

    TimelineClip renderable = clipWithRenderableEffectSettings(matte, {});
    QCOMPARE(renderable.effectPreset, ClipEffectPreset::None);
    QCOMPARE(matte.effectPreset, ClipEffectPreset::DifferenceMatte);

    matte.effectPreset = ClipEffectPreset::TemporalEcho;
    renderable = clipWithRenderableEffectSettings(matte, {});
    QCOMPARE(renderable.effectPreset, ClipEffectPreset::None);
    QCOMPARE(matte.effectPreset, ClipEffectPreset::TemporalEcho);

    matte.effectPreset = ClipEffectPreset::SourceTile;
    QCOMPARE(clipWithRenderableEffectSettings(matte, {}).effectPreset,
             ClipEffectPreset::SourceTile);

    TimelineClip media = matte;
    media.clipRole = ClipRole::Media;
    media.effectPreset = ClipEffectPreset::DifferenceMatte;
    QCOMPARE(clipWithRenderableEffectSettings(media, {}).effectPreset,
             ClipEffectPreset::DifferenceMatte);
}

void TestEffectPresets::clipSerializationPersistsGeneratedClipRoleState()
{
    TimelineClip clip;
    clip.id = QStringLiteral("source-mask");
    clip.clipRole = ClipRole::MaskMatte;
    clip.linkedSourceClipId = QStringLiteral("source");
    clip.generatedFromMaskId = QStringLiteral("/tmp/source_sam3_person_binary_masks");
    clip.syncLockedToSource = true;
    clip.sourceTransformLocked = true;
    clip.filePath = QStringLiteral("source.mp4");
    clip.mediaType = ClipMediaType::Video;
    clip.maskEnabled = true;
    clip.maskFramesDir = clip.generatedFromMaskId;
    clip.maskSidecarAvailable = false;
    clip.maskSidecarAvailabilityIssue = QStringLiteral("Generation incomplete");
    clip.maskShowOnly = true;
    clip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;

    const QJsonObject json = editor::clipToJson(clip);
    QCOMPARE(json.value(QStringLiteral("clipRole")).toString(), QStringLiteral("mask_matte"));
    QCOMPARE(json.value(QStringLiteral("linkedSourceClipId")).toString(), QStringLiteral("source"));
    QCOMPARE(json.value(QStringLiteral("generatedFromMaskId")).toString(),
             QStringLiteral("/tmp/source_sam3_person_binary_masks"));
    QCOMPARE(json.value(QStringLiteral("syncLockedToSource")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("sourceTransformLocked")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(),
             QStringLiteral("alternating_motion_background"));
    QVERIFY(!json.contains(QStringLiteral("maskSidecarAvailable")));
    QVERIFY(!json.contains(QStringLiteral("maskSidecarAvailabilityIssue")));

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.clipRole, ClipRole::MaskMatte);
    QCOMPARE(loaded.linkedSourceClipId, QStringLiteral("source"));
    QCOMPARE(loaded.generatedFromMaskId, QStringLiteral("/tmp/source_sam3_person_binary_masks"));
    QCOMPARE(loaded.syncLockedToSource, true);
    QCOMPARE(loaded.sourceTransformLocked, true);
    QVERIFY(loaded.maskEnabled);
    QVERIFY(loaded.maskSidecarAvailable);
    QVERIFY(loaded.maskSidecarAvailabilityIssue.isEmpty());
    QVERIFY(!loaded.maskShowOnly);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::AlternatingMotionBackground);

    QJsonObject legacyJson = json;
    legacyJson.remove(QStringLiteral("sourceTransformLocked"));
    QVERIFY(!editor::clipFromJson(legacyJson).sourceTransformLocked);
}

void TestEffectPresets::maskMatteFactoryKeepsSourceTimingLocked()
{
    TimelineClip source;
    source.id = QStringLiteral("shot-01");
    source.filePath = QStringLiteral("/media/shot-01.mp4");
    source.label = QStringLiteral("Shot 01");
    source.mediaType = ClipMediaType::Video;
    source.hasAudio = true;
    source.audioEnabled = true;
    source.audioBusId = QStringLiteral("main");
    source.sourceFps = 23.976;
    source.sourceDurationFrames = 2400;
    source.sourceInFrame = 120;
    source.sourceInSubframeSamples = 9;
    source.startFrame = 300;
    source.startSubframeSamples = 11;
    source.durationFrames = 600;
    source.durationSubframeSamples = 13;
    source.trackIndex = 2;
    source.playbackRate = 0.75;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/shot-01_sam3_person_binary_masks");
    source.maskForegroundLayerEnabled = true;
    source.effectPreset = ClipEffectPreset::NewsLogoTicker;
    source.effectRows = 91;
    source.effectSkipAwareTiming = true;
    source.maskRepeatEnabled = true;
    source.maskRepeatDeltaX = 77.0;
    source.maskRepeatDeltaY = -33.0;
    source.opacity = 0.35;
    TimelineClip::OpacityKeyframe parentOpacity;
    parentOpacity.frame = 0;
    parentOpacity.opacity = 0.2;
    source.opacityKeyframes = {parentOpacity};
    TimelineClip::TitleKeyframe parentTitle;
    parentTitle.frame = 0;
    parentTitle.text = QStringLiteral("Do not clone");
    source.titleKeyframes = {parentTitle};
    source.transcriptOverlay.enabled = true;
    TimelineClip::CorrectionPolygon correction;
    correction.pointsNormalized = {{0.0, 0.0}, {1.0, 0.0}, {0.5, 1.0}};
    source.correctionPolygons = {correction};
    source.brightness = 0.35;
    source.contrast = 1.7;
    source.saturation = 0.4;
    TimelineClip::GradingKeyframe parentGrade;
    parentGrade.frame = 0;
    parentGrade.brightness = 0.6;
    parentGrade.contrast = 1.8;
    parentGrade.saturation = 0.3;
    parentGrade.curvePointsR = {{0.0, 0.2}, {1.0, 0.9}};
    parentGrade.curvePointsG = {{0.0, 0.1}, {1.0, 0.8}};
    parentGrade.curvePointsB = {{0.0, 0.3}, {1.0, 0.7}};
    parentGrade.curvePointsLuma = {{0.0, 0.15}, {1.0, 1.0}};
    source.gradingKeyframes = {parentGrade};

    const TimelineClip matte = makeMaskMatteClip(source);
    QCOMPARE(matte.id, QStringLiteral("shot-01-mask-matte"));
    QCOMPARE(matte.clipRole, ClipRole::MaskMatte);
    QCOMPARE(matte.linkedSourceClipId, source.id);
    QCOMPARE(matte.generatedFromMaskId,
             editor::masks::stableMaskSidecarId(source.maskFramesDir));
    QVERIFY(matte.syncLockedToSource);
    QVERIFY(matte.sourceTransformLocked);
    QVERIFY(matte.locked);
    QCOMPARE(matte.filePath, source.filePath);
    QCOMPARE(matte.mediaType, source.mediaType);
    QVERIFY(matte.videoEnabled);
    QCOMPARE(matte.sourceFps, source.sourceFps);
    QCOMPARE(matte.sourceDurationFrames, source.sourceDurationFrames);
    QCOMPARE(matte.sourceInFrame, source.sourceInFrame);
    QCOMPARE(matte.sourceInSubframeSamples, source.sourceInSubframeSamples);
    QCOMPARE(matte.startFrame, source.startFrame);
    QCOMPARE(matte.startSubframeSamples, source.startSubframeSamples);
    QCOMPARE(matte.durationFrames, source.durationFrames);
    QCOMPARE(matte.durationSubframeSamples, source.durationSubframeSamples);
    QCOMPARE(matte.trackIndex, source.trackIndex);
    QCOMPARE(matte.playbackRate, source.playbackRate);
    QVERIFY(!matte.hasAudio);
    QVERIFY(!matte.audioEnabled);
    QVERIFY(!matte.audioLinkedToVideo);
    QVERIFY(matte.maskEnabled);
    QCOMPARE(matte.maskFramesDir, source.maskFramesDir);
    QVERIFY(!matte.maskShowOnly);
    QVERIFY(!matte.maskForegroundLayerEnabled);
    QCOMPARE(matte.effectPreset, ClipEffectPreset::None);
    QCOMPARE(matte.effectRows, TimelineClip{}.effectRows);
    QCOMPARE(matte.effectSkipAwareTiming, TimelineClip{}.effectSkipAwareTiming);
    QCOMPARE(matte.maskRepeatEnabled, TimelineClip{}.maskRepeatEnabled);
    QCOMPARE(matte.maskRepeatDeltaX, TimelineClip{}.maskRepeatDeltaX);
    QCOMPARE(matte.maskRepeatDeltaY, TimelineClip{}.maskRepeatDeltaY);
    QCOMPARE(matte.opacity, 1.0);
    QVERIFY(matte.opacityKeyframes.isEmpty());
    QVERIFY(matte.titleKeyframes.isEmpty());
    QVERIFY(!matte.transcriptOverlay.enabled);
    QVERIFY(matte.correctionPolygons.isEmpty());
    QCOMPARE(matte.brightness, 0.0);
    QCOMPARE(matte.contrast, 1.0);
    QCOMPARE(matte.saturation, 1.0);
    QVERIFY(matte.gradingKeyframes.isEmpty());

    // Parent grading can change after the virtual child is established. Media
    // and transform ownership remain linked, but grading ownership does not.
    source.brightness = -0.4;
    source.contrast = 0.55;
    source.saturation = 1.9;
    source.gradingKeyframes[0].brightness = -0.7;
    const TimelineClip::GradingKeyframe childGrade =
        evaluateEffectiveClipGradingAtPosition(matte, {}, matte.startFrame);
    QCOMPARE(childGrade.brightness, 0.0);
    QCOMPARE(childGrade.contrast, 1.0);
    QCOMPARE(childGrade.saturation, 1.0);
    QVERIFY(!gradingUsesCurveLut(childGrade));
}

void TestEffectPresets::maskSidecarAssociationUpdatesPathAndStableIdentityTogether()
{
    TimelineClip matte;
    matte.clipRole = ClipRole::MaskMatte;
    matte.maskFramesDir = QStringLiteral("/tmp/old-mask");
    matte.generatedFromMaskId = QStringLiteral("stale-id");

    const QString newDirectory = QStringLiteral(" /tmp/new-mask ");
    QVERIFY(setMaskSidecarAssociation(matte, newDirectory));
    QCOMPARE(matte.maskFramesDir, QStringLiteral("/tmp/new-mask"));
    QCOMPARE(matte.generatedFromMaskId,
             editor::masks::stableMaskSidecarId(matte.maskFramesDir));
    QVERIFY(!setMaskSidecarAssociation(matte, matte.maskFramesDir));

    QVERIFY(setMaskSidecarAssociation(matte, QString()));
    QVERIFY(matte.maskFramesDir.isEmpty());
    QVERIFY(matte.generatedFromMaskId.isEmpty());
}

void TestEffectPresets::maskMatteNormalizerRejectsUnsupportedParentsAndDeduplicates()
{
    TimelineClip source;
    source.id = QStringLiteral("source");
    source.filePath = QStringLiteral("/media/source.mp4");
    source.mediaType = ClipMediaType::Video;
    source.clipRole = ClipRole::Media;
    source.durationFrames = 120;
    source.maskFramesDir = QStringLiteral("/tmp/source-person-mask");

    TimelineClip effectParent = source;
    effectParent.id = QStringLiteral("effect-parent");
    effectParent.clipRole = ClipRole::EffectSynth;
    TimelineClip titleParent = source;
    titleParent.id = QStringLiteral("title-parent");
    titleParent.clipRole = ClipRole::SpeakerTitle;
    TimelineClip imageParent = source;
    imageParent.id = QStringLiteral("image-parent");
    imageParent.mediaType = ClipMediaType::Image;

    TimelineClip retained = makeMaskMatteClip(source);
    retained.id = QStringLiteral("retained-child");
    retained.maskEnabled = false;
    retained.maskShowOnly = true;
    retained.brightness = 0.42;
    TimelineClip::GradingKeyframe retainedGrade;
    retainedGrade.frame = 0;
    retainedGrade.brightness = 0.73;
    retained.gradingKeyframes = {retainedGrade};

    TimelineClip duplicate = retained;
    duplicate.id = QStringLiteral("duplicate-child");
    duplicate.maskEnabled = true;
    duplicate.maskShowOnly = false;
    duplicate.brightness = -0.5;
    duplicate.gradingKeyframes[0].brightness = -0.8;

    TimelineClip effectChild = retained;
    effectChild.id = QStringLiteral("effect-child");
    effectChild.linkedSourceClipId = effectParent.id;
    TimelineClip titleChild = retained;
    titleChild.id = QStringLiteral("title-child");
    titleChild.linkedSourceClipId = titleParent.id;
    TimelineClip imageChild = retained;
    imageChild.id = QStringLiteral("image-child");
    imageChild.linkedSourceClipId = imageParent.id;

    QVector<TimelineClip> clips{
        source, effectParent, titleParent, imageParent,
        retained, duplicate, effectChild, titleChild, imageChild};
    QVERIFY(normalizeMaskMatteClips(clips));

    QVector<TimelineClip> children;
    for (const TimelineClip& clip : std::as_const(clips)) {
        if (clip.clipRole == ClipRole::MaskMatte) {
            children.push_back(clip);
        }
    }
    QCOMPARE(children.size(), 1);
    QCOMPARE(children.constFirst().id, retained.id);
    QVERIFY(!children.constFirst().maskEnabled);
    QVERIFY(children.constFirst().maskShowOnly);
    QCOMPARE(children.constFirst().brightness, retained.brightness);
    QCOMPARE(children.constFirst().gradingKeyframes.size(), 1);
    QCOMPARE(children.constFirst().gradingKeyframes.constFirst().brightness, 0.73);
    QCOMPARE(maskMatteChildIdForSidecar(
                 clips, source.id, retained.generatedFromMaskId),
             retained.id);
    QVERIFY(!normalizeMaskMatteClips(clips));
}

void TestEffectPresets::maskMatteNormalizerRepairsLegacyTimelineState()
{
    TimelineClip source;
    source.id = QStringLiteral("shot-01");
    source.filePath = QStringLiteral("/media/shot-01.mp4");
    source.mediaType = ClipMediaType::Video;
    source.sourceFps = 29.97;
    source.sourceDurationFrames = 10000;
    source.sourceInFrame = 71;
    source.startFrame = 71;
    source.durationFrames = 85000;
    source.playbackRate = 1.0;
    source.transcriptOverlay.showSpeakerTitle = true;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/shot-01_masks");
    source.maskFeather = 4.5;
    source.maskDilate = 2.0;
    source.maskErode = 1.0;
    source.maskBlur = 0.5;
    source.maskOpacity = 0.75;
    source.maskGradeEnabled = true;
    source.maskGradeBrightness = 0.2;
    source.maskGradeContrast = 1.3;
    source.maskGradeSaturation = 0.8;
    source.baseTranslationX = 12.0;
    source.baseTranslationY = -6.0;
    TimelineClip::TransformKeyframe sourceTransform;
    sourceTransform.frame = 12;
    sourceTransform.translationX = 22.0;
    sourceTransform.scaleX = 1.25;
    sourceTransform.scaleY = 1.25;
    source.transformKeyframes = {sourceTransform};
    source.maskShowOnly = true;
    source.maskForegroundLayerEnabled = false;

    TimelineClip matte = source;
    matte.id = QStringLiteral("shot-01-mask-matte");
    matte.clipRole = ClipRole::MaskMatte;
    matte.linkedSourceClipId = source.id;
    matte.startFrame = 0;
    matte.sourceInFrame = 0;
    matte.locked = false;
    matte.sourceTransformLocked = false;
    matte.maskShowOnly = true;
    matte.maskForegroundLayerEnabled = true;
    matte.maskFeather = 99.0;
    matte.maskDilate = 99.0;
    matte.maskOpacity = 1.0;
    matte.maskGradeEnabled = false;
    matte.effectPreset = ClipEffectPreset::NeonGlow;
    source.effectSkipAwareTiming = false;
    matte.effectSkipAwareTiming = true;
    matte.transformKeyframes.clear();

    QVector<TimelineClip> clips{source, matte};
    QVERIFY(normalizeMaskMatteClips(clips));

    QVERIFY(!clips[0].maskShowOnly);
    QVERIFY(!clips[0].maskForegroundLayerEnabled);
    QCOMPARE(clips[1].startFrame, clips[0].startFrame);
    QCOMPARE(clips[1].sourceInFrame, clips[0].sourceInFrame);
    QCOMPARE(clips[1].durationFrames, clips[0].durationFrames);
    QVERIFY(clips[1].locked);
    QVERIFY(clips[1].sourceTransformLocked);
    QVERIFY(clips[1].maskShowOnly);
    QVERIFY(!clips[1].maskForegroundLayerEnabled);
    QCOMPARE(clips[1].maskFramesDir, clips[0].maskFramesDir);
    QCOMPARE(clips[1].generatedFromMaskId,
             editor::masks::stableMaskSidecarId(clips[0].maskFramesDir));
    QCOMPARE(clips[1].maskFeather, 99.0);
    QCOMPARE(clips[1].maskDilate, 99.0);
    QCOMPARE(clips[1].maskErode, clips[0].maskErode);
    QCOMPARE(clips[1].maskBlur, clips[0].maskBlur);
    QCOMPARE(clips[1].maskOpacity, 1.0);
    QCOMPARE(clips[1].effectPreset, ClipEffectPreset::NeonGlow);
    QVERIFY(clips[1].effectSkipAwareTiming);
    QVERIFY(!clips[0].maskGradeEnabled);
    QVERIFY(!clips[1].maskGradeEnabled);
    QCOMPARE(clips[1].gradingKeyframes.size(), 1);
    QCOMPARE(clips[1].gradingKeyframes.constFirst().frame, int64_t(0));
    QCOMPARE(clips[1].gradingKeyframes.constFirst().brightness, 0.2);
    QCOMPARE(clips[1].gradingKeyframes.constFirst().contrast, 1.3);
    QCOMPARE(clips[1].gradingKeyframes.constFirst().saturation, 0.8);
    QCOMPARE(clips[1].baseTranslationX, clips[0].baseTranslationX);
    QCOMPARE(clips[1].baseTranslationY, clips[0].baseTranslationY);
    QCOMPARE(clips[1].transformKeyframes.size(), clips[0].transformKeyframes.size());
    QCOMPARE(clips[1].transformKeyframes.constFirst().translationX,
             clips[0].transformKeyframes.constFirst().translationX);
}

void TestEffectPresets::maskMatteNormalizerFollowsSourceTimingEdits()
{
    TimelineClip source;
    source.id = QStringLiteral("shot-01");
    source.filePath = QStringLiteral("/media/shot-01.mp4");
    source.mediaType = ClipMediaType::Video;
    source.sourceFps = 59.94;
    source.sourceDurationFrames = 12000;
    source.sourceInFrame = 240;
    source.sourceInSubframeSamples = 7;
    source.startFrame = 300;
    source.startSubframeSamples = 11;
    source.durationFrames = 900;
    source.durationSubframeSamples = 13;
    source.playbackRate = 1.25;
    source.maskFramesDir = QStringLiteral("/tmp/shot-01_masks");

    TimelineClip matte = makeMaskMatteClip(source);
    QVector<TimelineClip> clips{source, matte};

    // Model the edits which previously changed only the source clip: move,
    // left/right trim, and playback-rate adjustment.
    clips[0].startFrame = 420;
    clips[0].startSubframeSamples = 17;
    clips[0].sourceInFrame = 360;
    clips[0].sourceInSubframeSamples = 19;
    clips[0].durationFrames = 480;
    clips[0].durationSubframeSamples = 23;
    clips[0].playbackRate = 0.8;

    QVERIFY(normalizeMaskMatteClips(clips));
    QCOMPARE(clips[1].startFrame, clips[0].startFrame);
    QCOMPARE(clips[1].startSubframeSamples, clips[0].startSubframeSamples);
    QCOMPARE(clips[1].sourceInFrame, clips[0].sourceInFrame);
    QCOMPARE(clips[1].sourceInSubframeSamples, clips[0].sourceInSubframeSamples);
    QCOMPARE(clips[1].durationFrames, clips[0].durationFrames);
    QCOMPARE(clips[1].durationSubframeSamples, clips[0].durationSubframeSamples);
    QCOMPARE(clips[1].playbackRate, clips[0].playbackRate);
    QVERIFY(!normalizeMaskMatteClips(clips));
}

void TestEffectPresets::alternatingMotionBackgroundFactoryCreatesVisualOnlySynthClip()
{
    TimelineClip source;
    source.id = QStringLiteral("logo");
    source.filePath = QStringLiteral("/media/logo.png");
    source.label = QStringLiteral("Soul House");
    source.mediaType = ClipMediaType::Image;
    source.sourceInFrame = 0;
    source.startFrame = 120;
    source.durationFrames = 240;
    source.trackIndex = 1;
    source.hasAudio = true;
    source.audioEnabled = true;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/masks");

    const TimelineClip synth = makeAlternatingMotionBackgroundClip(source, 3);
    QVERIFY(!synth.id.isEmpty());
    QVERIFY(synth.id != source.id);
    QCOMPARE(synth.clipRole, ClipRole::EffectSynth);
    QCOMPARE(synth.linkedSourceClipId, source.id);
    QCOMPARE(synth.filePath, source.filePath);
    QCOMPARE(synth.startFrame, source.startFrame);
    QCOMPARE(synth.durationFrames, source.durationFrames);
    QCOMPARE(synth.trackIndex, 3);
    QVERIFY(!synth.hasAudio);
    QVERIFY(!synth.audioEnabled);
    QVERIFY(!synth.maskEnabled);
    QVERIFY(synth.maskFramesDir.isEmpty());
    QCOMPARE(synth.effectPreset, ClipEffectPreset::AlternatingMotionBackground);
    QCOMPARE(synth.effectRows, 8);
    QVERIFY(synth.effectAlternateDirection);
}

void TestEffectPresets::sourceTilingFactoryCreatesVisualOnlySynthClip()
{
    TimelineClip source;
    source.id = QStringLiteral("still-01");
    source.filePath = QStringLiteral("/media/still-01.png");
    source.label = QStringLiteral("Still 01");
    source.mediaType = ClipMediaType::Image;
    source.sourceInFrame = 0;
    source.startFrame = 240;
    source.durationFrames = 180;
    source.trackIndex = 2;
    source.hasAudio = true;
    source.audioEnabled = true;
    source.maskEnabled = true;
    source.maskFramesDir = QStringLiteral("/tmp/masks");

    const TimelineClip synth = makeSourceTilingClip(source, 4);
    QVERIFY(!synth.id.isEmpty());
    QVERIFY(synth.id != source.id);
    QCOMPARE(synth.clipRole, ClipRole::EffectSynth);
    QCOMPARE(synth.linkedSourceClipId, source.id);
    QCOMPARE(synth.filePath, source.filePath);
    QCOMPARE(synth.startFrame, source.startFrame);
    QCOMPARE(synth.durationFrames, source.durationFrames);
    QCOMPARE(synth.trackIndex, 4);
    QVERIFY(!synth.hasAudio);
    QVERIFY(!synth.audioEnabled);
    QVERIFY(!synth.maskEnabled);
    QVERIFY(synth.maskFramesDir.isEmpty());
    QCOMPARE(synth.effectPreset, ClipEffectPreset::SourceTile);
    QCOMPARE(synth.effectRows, 6);
    QCOMPARE(synth.effectSpeed, 0.0);
    QVERIFY(!synth.effectAlternateDirection);
}

void TestEffectPresets::speakerTitleFactoryBuildsLowerThirdsForSpeakerChanges()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "Unable to create temporary transcript directory.");
    const QString transcriptPath = dir.filePath(QStringLiteral("clip.transcript.json"));
    QFile file(transcriptPath);
    QVERIFY2(file.open(QIODevice::WriteOnly), "Unable to write transcript profile fixture.");
    file.write(R"({
        "speaker_profiles": {
            "S1": {
                "name": "Jane Doe",
                "organization": "Director",
                "logo_path": "/tmp/jane-logo.png",
                "primary_color": "#f4fbff",
                "secondary_color": "#102030",
                "accent_color": "#4ac7ff"
            },
            "S2": {"name": "John Roe", "organization": "Producer"}
        },
        "segments": []
    })");
    file.close();

    TimelineClip source;
    source.id = QStringLiteral("clip-01");
    source.mediaType = ClipMediaType::Video;
    source.sourceInFrame = 100;
    source.startFrame = 1000;
    source.durationFrames = 300;
    source.playbackRate = 1.0;

    TranscriptSection section;
    section.startFrame = 110;
    section.endFrame = 172;
    // Speaker identity must come from the introduction event itself. Transcript
    // timing can overlap, so resolving the profile again at a frame can select
    // the prior speaker and produce a stale title.
    section.words.push_back(TranscriptWord{110, 150, QStringLiteral("S1"), QStringLiteral("hello"), false});
    section.words.push_back(TranscriptWord{120, 122, QStringLiteral("S1"), QStringLiteral("again"), false});
    section.words.push_back(TranscriptWord{140, 142, QStringLiteral("S2"), QStringLiteral("there"), false});
    section.words.push_back(TranscriptWord{170, 172, QStringLiteral("S1"), QStringLiteral("back"), false});

    const QVector<TimelineClip> titles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        90,
        0);

    QCOMPARE(titles.size(), 3);
    QCOMPARE(titles.at(0).clipRole, ClipRole::SpeakerTitle);
    QCOMPARE(titles.at(0).linkedSourceClipId, source.id);
    QVERIFY(titles.at(0).syncLockedToSource);
    QCOMPARE(titles.at(0).mediaType, ClipMediaType::Title);
    QCOMPARE(titles.at(0).trackIndex, 4);
    QCOMPARE(titles.at(0).startFrame, int64_t(1010));
    QVERIFY(titles.at(0).durationFrames > 0);
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe\nDirector"));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().logoPath, QStringLiteral("/tmp/jane-logo.png"));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().color.name(QColor::HexRgb), QStringLiteral("#f4fbff"));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().windowColor.name(QColor::HexRgb), QStringLiteral("#102030"));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().windowFrameColor.name(QColor::HexRgb), QStringLiteral("#4ac7ff"));
    QCOMPARE(titles.at(1).startFrame, int64_t(1040));
    QCOMPARE(titles.at(1).titleKeyframes.constFirst().text, QStringLiteral("John Roe\nProducer"));
    QCOMPARE(titles.at(2).startFrame, int64_t(1070));
    QCOMPARE(titles.at(2).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe\nDirector"));

    const QVector<TimelineClip> delayedTitles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        90);
    QCOMPARE(delayedTitles.constFirst().startFrame, int64_t(1010));
    QCOMPARE(delayedTitles.constFirst().titleKeyframes.constFirst().frame, int64_t(0));
    QCOMPARE(delayedTitles.constFirst().titleKeyframes.constFirst().opacity, 0.0);

    SpeakerTitleFlyInSettings nameOnly;
    nameOnly.showSpeakerOrganization = false;
    const auto nameOnlyTitles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source, transcriptPath, QVector<TranscriptSection>{section}, 4, nameOnly);
    QCOMPARE(nameOnlyTitles.constFirst().titleKeyframes.constFirst().text,
             QStringLiteral("Jane Doe"));

    SpeakerTitleFlyInSettings organizationOnly;
    organizationOnly.showSpeakerName = false;
    const auto organizationOnlyTitles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source, transcriptPath, QVector<TranscriptSection>{section}, 4, organizationOnly);
    QCOMPARE(organizationOnlyTitles.constFirst().titleKeyframes.constFirst().text,
             QStringLiteral("Director"));

    SpeakerTitleFlyInSettings rightFly;
    rightFly.style = SpeakerTitleFlyInStyle::SlideFromRight;
    rightFly.titleStartDelayFrames = 0;
    rightFly.titleDurationFrames = 120;
    rightFly.flyInFrames = 12;
    rightFly.flyOutFrames = 18;
    rightFly.titleExtrude3D = true;
    rightFly.titleExtrudeDepth = 0.24;
    const QVector<TimelineClip> rightTitles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        rightFly);
    QCOMPARE(rightTitles.constFirst().titleKeyframes.size(), 4);
    QCOMPARE(rightTitles.constFirst().titleKeyframes.at(1).frame, int64_t(12));
    QVERIFY(rightTitles.constFirst().titleKeyframes.constFirst().translationX >
            rightTitles.constFirst().titleKeyframes.at(1).translationX);
    for (const TimelineClip::TitleKeyframe& keyframe : rightTitles.constFirst().titleKeyframes) {
        QVERIFY(keyframe.vulkan3DEnabled);
        QVERIFY(keyframe.vulkan3DExtrudeEnabled);
        QVERIFY(!keyframe.dropShadowEnabled);
        QCOMPARE(keyframe.dropShadowOpacity, 0.0);
        QCOMPARE(keyframe.vulkan3DExtrudeDepth, rightFly.titleExtrudeDepth);
    }

    SpeakerTitleFlyInSettings riseFly;
    riseFly.style = SpeakerTitleFlyInStyle::RiseFromBottom;
    riseFly.titleStartDelayFrames = 0;
    riseFly.titleDurationFrames = 90;
    const QVector<TimelineClip> riseTitles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        riseFly);
    QVERIFY(riseTitles.constFirst().titleKeyframes.constFirst().translationY >
            riseTitles.constFirst().titleKeyframes.at(1).translationY);

    TimelineClip repeatedSource = source;
    repeatedSource.sourceInFrame = 0;
    repeatedSource.startFrame = 100;
    repeatedSource.durationFrames = 400;
    TranscriptSection repeatedSection;
    repeatedSection.startFrame = 0;
    repeatedSection.endFrame = 300;
    repeatedSection.words = {
        TranscriptWord{0, 10, QStringLiteral("S1"), QStringLiteral("start"), false},
        TranscriptWord{100, 110, QStringLiteral("S1"), QStringLiteral("continue"), false},
        TranscriptWord{200, 210, QStringLiteral("S2"), QStringLiteral("reply"), false},
        TranscriptWord{290, 300, QStringLiteral("S2"), QStringLiteral("finish"), false},
    };
    SpeakerTitleFlyInSettings repeatedSettings;
    repeatedSettings.titleStartDelayFrames = 0;
    repeatedSettings.titleDurationFrames = 30;
    repeatedSettings.showAtSectionEnd = true;
    repeatedSettings.cadenceFrames = 120;
    const QVector<TimelineClip> repeatedTitles =
        makeSpeakerTitleClipsForTranscriptIntroductions(
            repeatedSource,
            transcriptPath,
            QVector<TranscriptSection>{repeatedSection},
            4,
            repeatedSettings);
    QCOMPARE(repeatedTitles.size(), 7);
    QVector<int64_t> repeatedStarts;
    for (const TimelineClip& titleClip : repeatedTitles) {
        repeatedStarts.push_back(titleClip.startFrame);
        QVERIFY(titleClip.durationFrames <= repeatedSettings.titleDurationFrames);
    }
    QCOMPARE(
        repeatedStarts,
        QVector<int64_t>({100, 181, 220, 300, 340, 371, 460}));
}

void TestEffectPresets::speakerTitleFlyInsApplyToSourceClipKeyframes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString transcriptPath = dir.filePath(QStringLiteral("clip.transcript.json"));
    QFile file(transcriptPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({
        "speaker_profiles": {
            "S1": {"name": "Jane Doe", "organization": "Director"},
            "S2": {"name": "John Roe", "organization": "Producer"}
        },
        "segments": []
    })");
    file.close();

    TimelineClip source;
    source.id = QStringLiteral("clip-01");
    source.mediaType = ClipMediaType::Video;
    source.sourceInFrame = 100;
    source.startFrame = 1000;
    source.durationFrames = 300;
    source.playbackRate = 1.0;

    TranscriptSection section;
    section.startFrame = 110;
    section.endFrame = 172;
    section.words.push_back(TranscriptWord{110, 112, QStringLiteral("S1"), QStringLiteral("hello"), false});
    section.words.push_back(TranscriptWord{140, 142, QStringLiteral("S2"), QStringLiteral("there"), false});

    SpeakerTitleFlyInSettings settings;
    settings.titleStartDelayFrames = 0;
    settings.titleDurationFrames = 90;
    settings.flyInFrames = 12;
    settings.flyOutFrames = 18;

    const int appliedCount = applySpeakerTitleFlyInsToSourceClip(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        settings);

    QCOMPARE(appliedCount, 2);
    QCOMPARE(source.mediaType, ClipMediaType::Video);
    QCOMPARE(source.clipRole, ClipRole::Media);
    QVERIFY(!source.titleKeyframes.isEmpty());
    QVERIFY(source.speakerTitleEngineActive);
    QVERIFY(!source.transcriptOverlay.showSpeakerTitle);
    QCOMPARE(source.titleKeyframes.constFirst().frame, int64_t(10));
    QCOMPARE(source.titleKeyframes.constFirst().text, QStringLiteral("Jane Doe\nDirector"));
    QVERIFY(source.titleKeyframes.constFirst().translationX < source.titleKeyframes.at(1).translationX);

    bool sawSecondSpeaker = false;
    for (const TimelineClip::TitleKeyframe& keyframe : source.titleKeyframes) {
        QVERIFY(keyframe.frame >= 0);
        QVERIFY(keyframe.frame < source.durationFrames);
        if (keyframe.text == QStringLiteral("John Roe\nProducer")) {
            sawSecondSpeaker = true;
            QVERIFY(keyframe.frame >= 40);
        }
    }
    QVERIFY(sawSecondSpeaker);
}

void TestEffectPresets::speakerTitleFlyInsKeepOrganizationSeparateAndIndependentBoxWidth()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString transcriptPath = dir.filePath(QStringLiteral("clip.transcript.json"));
    QFile file(transcriptPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({
        "speaker_profiles": {
            "S1": {"name": "Jane Doe", "organization": "Director"}
        },
        "segments": []
    })");
    file.close();

    TimelineClip source;
    source.id = QStringLiteral("clip-01");
    source.mediaType = ClipMediaType::Video;
    source.sourceInFrame = 100;
    source.startFrame = 1000;
    source.durationFrames = 180;
    source.playbackRate = 1.0;

    TranscriptSection section;
    section.startFrame = 110;
    section.endFrame = 132;
    section.words.push_back(TranscriptWord{110, 112, QStringLiteral("S1"), QStringLiteral("hello"), false});

    SpeakerTitleFlyInSettings settings;
    settings.style = SpeakerTitleFlyInStyle::WrapAroundSpeaker;
    settings.titleStartDelayFrames = 0;
    settings.titleDurationFrames = 90;
    settings.flyInFrames = 12;
    settings.flyOutFrames = 18;
    settings.titleFontSize = 62.0;
    settings.titleBoxWidth = 940.0;
    settings.titleBackgroundEnabled = false;

    const int appliedCount = applySpeakerTitleFlyInsToSourceClip(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        settings);

    QCOMPARE(appliedCount, 1);
    QVERIFY(!source.titleKeyframes.isEmpty());
    for (const TimelineClip::TitleKeyframe& keyframe : source.titleKeyframes) {
        QCOMPARE(keyframe.text, QStringLiteral("Jane Doe\nDirector"));
        QCOMPARE(keyframe.text.split(QLatin1Char('\n')).size(), 2);
        QCOMPARE(keyframe.windowWidth, 940.0);
        QVERIFY(!keyframe.windowEnabled);
        QVERIFY(!keyframe.windowFrameEnabled);
        QVERIFY(keyframe.autoFitToOutput);
    }

    TimelineClip::TitleKeyframe longKeyframe = source.titleKeyframes.constFirst();
    longKeyframe.text = QStringLiteral("Cornelius Hairston — International Professionals Association");
    TimelineClip fittedClip;
    fittedClip.titleKeyframes = {longKeyframe};
    const EvaluatedTitle fitted = fitTitleToOutput(
        evaluateTitleAtLocalFrame(fittedClip, 0), QSize(300, 500));
    QVERIFY(fitted.fontSize < longKeyframe.fontSize);
    QVERIFY(measureTitleLayout(fitted).width <= 270.5);

    bool sawRequestedFontSize = false;
    for (const TimelineClip::TitleKeyframe& keyframe : source.titleKeyframes) {
        if (qFuzzyCompare(keyframe.fontSize, 62.0)) {
            sawRequestedFontSize = true;
            break;
        }
    }
    QVERIFY(sawRequestedFontSize);
}

void TestEffectPresets::effectPipelinePassesThroughWhenPresetIsOff()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::None;

    const render_detail::VulkanEffectPipelinePlan plan =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            QRectF(0.0, 0.0, 800.0, 450.0),
            QSize(200, 100),
            10.0);

    QVERIFY(plan.mode == render_detail::VulkanEffectPipelinePlan::Mode::PassThrough);
    QVERIFY(!plan.usesGeneratedDraws());
    QVERIFY(plan.generatedDraws.isEmpty());
    QVERIFY(plan.generatedDrawRects().isEmpty());
}

void TestEffectPresets::effectPipelineUsesGeneratedDrawsForTiling()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::SourceTile;
    clip.effectRows = 4;

    const render_detail::VulkanEffectPipelinePlan plan =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            QRectF(0.0, 0.0, 800.0, 450.0),
            QSize(200, 100),
            10.0);

    QVERIFY(plan.mode == render_detail::VulkanEffectPipelinePlan::Mode::GeneratedDraws);
    QVERIFY(plan.usesGeneratedDraws());
    QVERIFY(!plan.generatedDraws.isEmpty());
    QCOMPARE(plan.generatedDrawRects().size(), plan.generatedDraws.size());
    QCOMPARE(plan.generatedDrawRects().constFirst(), plan.generatedDraws.constFirst().outputRect);
}

void TestEffectPresets::maskedRepeatUsesMaskGradeDrawsAndKeyframedOffsets()
{
    TimelineClip clip;
    clip.maskEnabled = true;
    clip.maskFramesDir = QStringLiteral("/tmp/masks");
    clip.maskRepeatEnabled = true;
    clip.maskRepeatDeltaX = 100.0;
    clip.maskRepeatDeltaY = 20.0;
    clip.effectPreset = ClipEffectPreset::None;
    clip.effectRows = 3;
    clip.durationFrames = 20;
    TimelineClip::TransformKeyframe first;
    first.frame = 0;
    first.maskRepeatDeltaX = 0.0;
    first.maskRepeatDeltaY = 0.0;
    TimelineClip::TransformKeyframe second;
    second.frame = 10;
    second.maskRepeatDeltaX = 100.0;
    second.maskRepeatDeltaY = 20.0;
    clip.transformKeyframes = {first, second};

    const QRectF seedRect(200.0, 100.0, 80.0, 40.0);
    const render_detail::VulkanEffectPipelinePlan plan =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            seedRect,
            QSize(200, 100),
            5.0);

    QVERIFY(plan.mode == render_detail::VulkanEffectPipelinePlan::Mode::GeneratedDraws);
    QCOMPARE(plan.generatedDraws.size(), 3);
    for (const render_detail::VulkanEffectPipelinePlan::DrawPass& pass : plan.generatedDraws) {
        QCOMPARE(pass.shaderMode, render_detail::kVulkanEffectModeMaskGrade);
    }
    QCOMPARE(plan.generatedDraws.at(0).outputRect, seedRect.translated(-150.0, -30.0));
    QCOMPARE(plan.generatedDraws.at(1).outputRect, seedRect);
    QCOMPARE(plan.generatedDraws.at(2).outputRect, seedRect.translated(150.0, 30.0));
}

void TestEffectPresets::maskedRepeatUsesSpeechFilterAwareTransformTiming()
{
    TimelineClip clip;
    clip.id = QStringLiteral("speech-aware-repeat");
    clip.filePath = QStringLiteral("/tmp/source.mp4");
    clip.startFrame = 0;
    clip.durationFrames = 40;
    clip.effectPreset = ClipEffectPreset::None;
    clip.maskEnabled = true;
    clip.maskRepeatEnabled = true;
    clip.maskFramesDir = QStringLiteral("/tmp/masks");
    clip.effectRows = 3;
    clip.transformSkipAwareTiming = true;

    TimelineClip::TransformKeyframe first;
    first.frame = 0;
    first.maskRepeatDeltaX = 0.0;
    first.maskRepeatDeltaY = 0.0;
    TimelineClip::TransformKeyframe second;
    second.frame = 20;
    second.maskRepeatDeltaX = 200.0;
    second.maskRepeatDeltaY = 40.0;
    clip.transformKeyframes = {first, second};

    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 29},
    };
    timing.frameTransitionMode = PlaybackFrameTransitionMode::SmoothStepSpeedThrough;
    timing.frameCrossfadeFrames = 4;

    const QRectF seedRect(200.0, 100.0, 80.0, 40.0);
    const render_detail::VulkanEffectPipelinePlan plan =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            seedRect,
            QSize(200, 100),
            10.0,
            -1.0,
            timing);
    const TimelineClip::TransformKeyframe expectedTransform =
        evaluateClipTransformAtPosition(clip, 10.0, timing);

    QVERIFY(plan.mode == render_detail::VulkanEffectPipelinePlan::Mode::GeneratedDraws);
    QCOMPARE(plan.generatedDraws.size(), 3);
    QCOMPARE(plan.generatedDraws.at(0).outputRect,
             seedRect.translated(-expectedTransform.maskRepeatDeltaX,
                                 -expectedTransform.maskRepeatDeltaY));
    QCOMPARE(plan.generatedDraws.at(1).outputRect, seedRect);
    QCOMPARE(plan.generatedDraws.at(2).outputRect,
             seedRect.translated(expectedTransform.maskRepeatDeltaX,
                                 expectedTransform.maskRepeatDeltaY));
}

void TestEffectPresets::temporalEffectsUseContiguousPlaybackTimeAcrossSegmentGaps()
{
    TimelineClip first;
    first.id = QStringLiteral("segment-a");
    first.filePath = QStringLiteral("/tmp/source.png");
    first.trackIndex = 2;
    first.startFrame = 0;
    first.durationFrames = 30;
    first.effectPreset = ClipEffectPreset::NewsLogoTicker;
    first.effectRows = 4;
    first.effectSpeed = 1.0;

    TimelineClip second = first;
    second.id = QStringLiteral("segment-b");
    second.startFrame = 100;
    const QVector<TimelineClip> clips{first, second};
    const QRectF output(0.0, 0.0, 800.0, 450.0);
    const QSize sourceSize(200, 100);

    const qreal secondPlayFrame =
        render_detail::clipEffectPlaybackFramePosition(second, clips, 100.0);
    QCOMPARE(secondPlayFrame, 30.0);

    const QVector<QRectF> contiguousRects =
        render_detail::vulkanEffectPipelinePlan(
            second,
            output,
            sourceSize,
            100.0,
            secondPlayFrame).generatedDrawRects();
    const QVector<QRectF> expectedRects =
        render_detail::vulkanEffectPipelinePlan(
            first,
            output,
            sourceSize,
            30.0,
            30.0).generatedDrawRects();
    const QVector<QRectF> wallTimeRects =
        render_detail::vulkanEffectPipelinePlan(
            second,
            output,
            sourceSize,
            100.0,
            100.0).generatedDrawRects();

    QCOMPARE(contiguousRects, expectedRects);
    QVERIFY(contiguousRects != wallTimeRects);

    TimelineClip trackFirst = first;
    trackFirst.effectPreset = ClipEffectPreset::None;
    TimelineClip trackSecond = second;
    trackSecond.effectPreset = ClipEffectPreset::None;
    const QVector<TimelineClip> trackClips{trackFirst, trackSecond};
    QVector<TimelineTrack> tracks(3);
    tracks[2].effectPreset = ClipEffectPreset::NewsLogoTicker;
    tracks[2].effectRows = 4;
    tracks[2].effectSpeed = 1.0;

    const TimelineClip effectiveTrackSecond =
        clipWithTrackEffectSettings(trackSecond, tracks);
    const qreal trackSecondPlayFrame =
        render_detail::clipEffectPlaybackFramePosition(
            effectiveTrackSecond, trackClips, 100.0, tracks);
    QCOMPARE(trackSecondPlayFrame, 30.0);
}

void TestEffectPresets::temporalEffectsUsePlaybackTimeAcrossSpeechFilterGaps()
{
    TimelineClip clip;
    clip.id = QStringLiteral("speech-filtered-effect");
    clip.filePath = QStringLiteral("/tmp/source.png");
    clip.trackIndex = 1;
    clip.startFrame = 0;
    clip.durationFrames = 40;
    clip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.effectAlternateDirection = true;
    clip.transformSkipAwareTiming = false;
    clip.effectSkipAwareTiming = true;

    setTransformSkipAwareTimelineRanges({
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 29},
    });
    const PlaybackTimingContext timing{{
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 29},
    }};
    setTransformSkipAwareTimelineRanges({});

    const QVector<TimelineClip> clips{clip};
    const QRectF output(0.0, 0.0, 800.0, 450.0);
    const QSize sourceSize(200, 100);
    const qreal playFrame =
        render_detail::clipEffectPlaybackFramePosition(clip, clips, 20.0, timing);
    QCOMPARE(playFrame, 10.0);

    const QVector<QRectF> playTimeRects =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            20.0,
            playFrame).generatedDrawRects();
    const QVector<QRectF> expectedRects =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            10.0,
            10.0).generatedDrawRects();
    const QVector<QRectF> wallTimeRects =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            20.0,
            20.0).generatedDrawRects();

    QCOMPARE(playTimeRects, expectedRects);
    QVERIFY(playTimeRects != wallTimeRects);

    TimelineClip unsyncedClip = clip;
    unsyncedClip.effectSkipAwareTiming = false;
    QCOMPARE(render_detail::clipEffectPlaybackFramePosition(unsyncedClip, {unsyncedClip}, 20.0, timing), 20.0);
}

void TestEffectPresets::titleAnimationsUseContiguousPlaybackTimeAcrossSkips()
{
    TimelineClip clip = createDefaultTitleClip(0, 1, 100);
    auto first = clip.titleKeyframes.first();
    first.frame = 0;
    first.translationX = 0.0;
    first.opacity = 0.0;
    first.fontSize = 20.0;
    first.vulkan3DYawDegrees = 0.0;
    first.vulkan3DPitchDegrees = 0.0;
    first.vulkan3DRollDegrees = 0.0;

    auto last = first;
    last.frame = 40;
    last.linearInterpolation = true;
    last.translationX = 40.0;
    last.opacity = 1.0;
    last.fontSize = 60.0;
    last.vulkan3DYawDegrees = 80.0;
    last.vulkan3DPitchDegrees = 120.0;
    last.vulkan3DRollDegrees = 160.0;
    clip.titleKeyframes = {first, last};

    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 49},
    };

    const EvaluatedTitle before = evaluateTitleAtTimelinePosition(clip, 9.0, timing);
    const EvaluatedTitle after = evaluateTitleAtTimelinePosition(clip, 20.0, timing);
    const EvaluatedTitle fractional = evaluateTitleAtTimelinePosition(clip, 20.5, timing);

    QCOMPARE(before.x, 9.0);
    QCOMPARE(after.x, 10.0);
    QCOMPARE(fractional.x, 10.5);
    QCOMPARE(after.opacity, 0.25);
    QCOMPARE(after.fontSize, 30.0);
    QCOMPARE(after.vulkan3DYawDegrees, 20.0);
    QCOMPARE(after.vulkan3DPitchDegrees, 30.0);
    QCOMPARE(after.vulkan3DRollDegrees, 40.0);
    QCOMPARE(after.x - before.x, 1.0);
}

void TestEffectPresets::temporalEffectsStayOnRawClockDuringVisualSpeedThrough()
{
    TimelineClip clip;
    clip.id = QStringLiteral("speed-through-effect");
    clip.filePath = QStringLiteral("/tmp/source.png");
    clip.trackIndex = 1;
    clip.startFrame = 0;
    clip.durationFrames = 40;
    clip.effectPreset = ClipEffectPreset::SourceTile;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.transformSkipAwareTiming = true;
    clip.effectSkipAwareTiming = true;

    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 29},
    };
    timing.frameTransitionMode = PlaybackFrameTransitionMode::SmoothStepSpeedThrough;
    timing.frameCrossfadeFrames = 4;

    const QVector<TimelineClip> clips{clip};
    const qreal rawFrame7 =
        render_detail::clipEffectPlaybackFramePosition(clip, clips, 7.0, timing);
    const qreal rawFrame8 =
        render_detail::clipEffectPlaybackFramePosition(clip, clips, 8.0, timing);
    QVERIFY(rawFrame8 > rawFrame7);

    const qreal visualFrame7 = playbackVisualTimelineFramePosition(7.0, timing);
    const qreal visualFrame8 = playbackVisualTimelineFramePosition(8.0, timing);
    QVERIFY(visualFrame7 > 9.0);
    QVERIFY(visualFrame7 < 11.0);
    QVERIFY(visualFrame8 > visualFrame7);
    QCOMPARE(render_detail::clipEffectPlaybackFramePosition(clip, clips, visualFrame7, timing), 10.0);
    QCOMPARE(render_detail::clipEffectPlaybackFramePosition(clip, clips, visualFrame8, timing), 10.0);
    QCOMPARE(playbackVisualTimelineFramePosition(15.0, timing), playbackVisualTimelineFramePosition(9.0, timing));

    const QRectF output(0.0, 0.0, 800.0, 450.0);
    const QSize sourceSize(200, 100);
    const QVector<QRectF> smoothFrame7 =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            visualFrame7,
            rawFrame7).generatedDrawRects();
    const QVector<QRectF> smoothFrame8 =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            visualFrame8,
            rawFrame8).generatedDrawRects();
    const QVector<QRectF> frozenFrame8 =
        render_detail::vulkanEffectPipelinePlan(
            clip,
            output,
            sourceSize,
            visualFrame8,
            10.0).generatedDrawRects();

    QVERIFY(smoothFrame8 != smoothFrame7);
    QVERIFY(smoothFrame8 != frozenFrame8);
}

void TestEffectPresets::temporalMovingPresetOptionsStaySmoothAcrossMultipleSpeechBoundaries()
{
    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{0, 9},
        ExportRangeSegment{20, 29},
        ExportRangeSegment{40, 49},
    };
    timing.frameTransitionMode = PlaybackFrameTransitionMode::SmoothStepSpeedThrough;
    timing.frameCrossfadeFrames = 4;

    const QVector<MovingPresetCase> movingPresets{
        {ClipEffectPreset::NewsLogoTicker, ClipTilingPattern::Grid, QStringLiteral("news")},
        {ClipEffectPreset::AlternatingMotionBackground, ClipTilingPattern::Grid, QStringLiteral("alternating")},
        {ClipEffectPreset::SourceTile, ClipTilingPattern::Grid, QStringLiteral("tile")},
        {ClipEffectPreset::PersonOrbit, ClipTilingPattern::Grid, QStringLiteral("orbit")},
    };
    const QVector<QPair<qreal, qreal>> boundarySamples{
        {7.0, 8.0},
        {27.0, 28.0},
    };
    const QRectF output(0.0, 0.0, 800.0, 450.0);
    const QSize sourceSize(200, 100);

    for (const MovingPresetCase& preset : movingPresets) {
        const TimelineClip clip = makeMovingEffectClip(preset);
        const QVector<TimelineClip> clips{clip};
        for (const QPair<qreal, qreal>& samples : boundarySamples) {
            const qreal beforeRawClock =
                render_detail::clipEffectPlaybackFramePosition(clip, clips, samples.first, timing);
            const qreal afterRawClock =
                render_detail::clipEffectPlaybackFramePosition(clip, clips, samples.second, timing);
            QVERIFY2(afterRawClock > beforeRawClock,
                     qPrintable(QStringLiteral("%1 raw effect clock must advance across boundary sample %2 -> %3")
                                    .arg(preset.label)
                                    .arg(samples.first)
                                    .arg(samples.second)));

            const qreal beforeVisualFrame = playbackVisualTimelineFramePosition(samples.first, timing);
            const qreal afterVisualFrame = playbackVisualTimelineFramePosition(samples.second, timing);
            QVERIFY(afterVisualFrame > beforeVisualFrame);
            const qreal frozenClock =
                render_detail::clipEffectPlaybackFramePosition(clip, clips, afterVisualFrame, timing);

            const QVector<QRectF> beforeRects =
                render_detail::vulkanEffectPipelinePlan(
                    clip,
                    output,
                    sourceSize,
                    beforeVisualFrame,
                    beforeRawClock).generatedDrawRects();
            const QVector<QRectF> afterRects =
                render_detail::vulkanEffectPipelinePlan(
                    clip,
                    output,
                    sourceSize,
                    afterVisualFrame,
                    afterRawClock).generatedDrawRects();
            const QVector<QRectF> frozenRects =
                render_detail::vulkanEffectPipelinePlan(
                    clip,
                    output,
                    sourceSize,
                    afterVisualFrame,
                    frozenClock).generatedDrawRects();

            QVERIFY2(!beforeRects.isEmpty() && !afterRects.isEmpty(),
                     qPrintable(QStringLiteral("%1 must generate visible effect draws").arg(preset.label)));
            QVERIFY2(afterRects != beforeRects,
                     qPrintable(QStringLiteral("%1 must keep moving across speech boundary").arg(preset.label)));
            QVERIFY2(afterRects != frozenRects,
                     qPrintable(QStringLiteral("%1 must not use the visual gap frame as the effect clock").arg(preset.label)));
        }
    }
}

void TestEffectPresets::generatedDrawMvpKeepsVulkanYDownOrientation()
{
    float mvp[16]{};
    render_detail::vulkanMvpForOutputRectMaybeFlippedY(
        QRectF(0.0, 0.0, 100.0, 50.0),
        QSize(100, 100),
        0.0,
        false,
        mvp);

    auto transformedY = [&mvp](float x, float y) {
        return (mvp[1] * x) + (mvp[5] * y) + mvp[13];
    };

    const float topY = transformedY(-1.0f, -1.0f);
    const float bottomY = transformedY(-1.0f, 1.0f);
    QVERIFY2(topY < bottomY, "Generated draw MVP must map source top above source bottom in Vulkan coordinates.");

    render_detail::vulkanMvpForOutputRectMaybeFlippedY(
        QRectF(0.0, 0.0, 100.0, 50.0),
        QSize(100, 100),
        0.0,
        true,
        mvp);
    const float flippedTopY = transformedY(-1.0f, -1.0f);
    const float flippedBottomY = transformedY(-1.0f, 1.0f);
    QVERIFY(flippedTopY > flippedBottomY);
}

void TestEffectPresets::tickerPresetProducesAlternatingRowsAcrossOutput()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 1080.0, 1920.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 0.0);
    const QVector<QRectF> frame10 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 10.0);

    QVERIFY(frame0.size() > clip.effectRows);
    QCOMPARE(frame0.constFirst().height(), output.height() / clip.effectRows * 0.78);
    QVERIFY(frame0.constFirst().width() > frame0.constFirst().height());
    QVERIFY(std::abs(frame10.constFirst().x() - frame0.constFirst().x()) > 0.001);

    const qreal rowHeight = output.height() / clip.effectRows;
    auto firstRectInRow = [&](const QVector<QRectF>& rects, int row) {
        const qreal rowTop = output.top() + row * rowHeight;
        const qreal rowBottom = rowTop + rowHeight;
        for (const QRectF& rect : rects) {
            if (rect.center().y() >= rowTop && rect.center().y() < rowBottom) {
                return rect;
            }
        }
        return QRectF();
    };
    const QRectF row0Frame0 = firstRectInRow(frame0, 0);
    const QRectF row0Frame10 = firstRectInRow(frame10, 0);
    const QRectF row1Frame0 = firstRectInRow(frame0, 1);
    const QRectF row1Frame10 = firstRectInRow(frame10, 1);
    QVERIFY(row0Frame0.isValid());
    QVERIFY(row1Frame0.isValid());
    QVERIFY((row0Frame10.x() - row0Frame0.x()) * (row1Frame10.x() - row1Frame0.x()) < 0.0);
}

void TestEffectPresets::alternatingMotionBackgroundCoversOutputWithMovingRows()
{
    TimelineClip clip;
    clip.clipRole = ClipRole::EffectSynth;
    clip.effectPreset = ClipEffectPreset::AlternatingMotionBackground;
    clip.effectRows = 5;
    clip.effectSpeed = 1.2;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 1280.0, 720.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(256, 128), 0.0);
    const QVector<QRectF> frame30 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(256, 128), 30.0);

    QVERIFY(frame0.size() > clip.effectRows);
    QVERIFY(frame0.constFirst().height() > output.height() / clip.effectRows);
    QVERIFY(frame0.constFirst().width() > frame0.constFirst().height());
    QVERIFY(std::abs(frame30.constFirst().x() - frame0.constFirst().x()) > 0.001);

    const qreal rowHeight = output.height() / clip.effectRows;
    auto firstRectInRow = [&](const QVector<QRectF>& rects, int row) {
        const qreal rowTop = output.top() + row * rowHeight;
        const qreal rowBottom = rowTop + rowHeight;
        for (const QRectF& rect : rects) {
            if (rect.center().y() >= rowTop && rect.center().y() < rowBottom) {
                return rect;
            }
        }
        return QRectF();
    };

    const QRectF row0Frame0 = firstRectInRow(frame0, 0);
    const QRectF row0Frame30 = firstRectInRow(frame30, 0);
    const QRectF row1Frame0 = firstRectInRow(frame0, 1);
    const QRectF row1Frame30 = firstRectInRow(frame30, 1);
    QVERIFY(row0Frame0.isValid());
    QVERIFY(row1Frame0.isValid());
    QVERIFY((row0Frame30.x() - row0Frame0.x()) * (row1Frame30.x() - row1Frame0.x()) < 0.0);
}

void TestEffectPresets::sourceTilingPresetCoversOutputWithGrid()
{
    TimelineClip clip;
    clip.clipRole = ClipRole::EffectSynth;
    clip.effectPreset = ClipEffectPreset::SourceTile;
    clip.effectRows = 4;
    clip.effectSpeed = 0.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = false;

    const QRectF output(0.0, 0.0, 800.0, 400.0);
    const QVector<QRectF> rects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 0.0);

    QVERIFY(rects.size() >= 12);
    QCOMPARE(rects.constFirst().width(), output.width() / clip.effectRows);
    QCOMPARE(rects.constFirst().height(), rects.constFirst().width() / 2.0);

    bool coversLeft = false;
    bool coversRight = false;
    bool coversTop = false;
    bool coversBottom = false;
    for (const QRectF& rect : rects) {
        coversLeft = coversLeft || rect.left() <= output.left();
        coversRight = coversRight || rect.right() >= output.right();
        coversTop = coversTop || rect.top() <= output.top();
        coversBottom = coversBottom || rect.bottom() >= output.bottom();
    }
    QVERIFY(coversLeft);
    QVERIFY(coversRight);
    QVERIFY(coversTop);
    QVERIFY(coversBottom);

    clip.effectSpeed = 1.0;
    const QVector<QRectF> moved =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(200, 100), 10.0);
    QVERIFY(!moved.isEmpty());
    QVERIFY(moved.constFirst().topLeft() != rects.constFirst().topLeft());
}

void TestEffectPresets::sourceTilingPresetSupportsGeometricPatterns()
{
    TimelineClip clip;
    clip.clipRole = ClipRole::EffectSynth;
    clip.effectPreset = ClipEffectPreset::SourceTile;
    clip.effectRows = 9;
    clip.effectSpeed = 0.0;
    clip.effectScale = 1.0;
    clip.tilingSpacing = 1.2;

    const QRectF output(0.0, 0.0, 900.0, 900.0);
    clip.tilingPattern = ClipTilingPattern::Encircle;
    QVector<QRectF> rects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    QCOMPARE(rects.size(), clip.effectRows);
    const QPointF center = output.center();
    qreal minDistance = std::numeric_limits<qreal>::max();
    qreal maxDistance = 0.0;
    for (const QRectF& rect : rects) {
        const qreal distance = std::hypot(rect.center().x() - center.x(), rect.center().y() - center.y());
        minDistance = std::min(minDistance, distance);
        maxDistance = std::max(maxDistance, distance);
    }
    QVERIFY(std::abs(maxDistance - minDistance) < 0.001);

    clip.tilingPattern = ClipTilingPattern::SpiralXY;
    rects = render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    QCOMPARE(rects.size(), clip.effectRows);
    minDistance = std::numeric_limits<qreal>::max();
    maxDistance = 0.0;
    for (const QRectF& rect : rects) {
        const qreal distance = std::hypot(rect.center().x() - center.x(), rect.center().y() - center.y());
        minDistance = std::min(minDistance, distance);
        maxDistance = std::max(maxDistance, distance);
    }
    QVERIFY(minDistance < 0.001);
    QVERIFY(maxDistance > output.width() * 0.25);

    clip.tilingPattern = ClipTilingPattern::SpiralXZ;
    const QVector<QRectF> xzRects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    QCOMPARE(xzRects.size(), clip.effectRows);
    QVERIFY(xzRects.constFirst().height() != xzRects.constLast().height());

    clip.tilingPattern = ClipTilingPattern::SpiralYZ;
    const QVector<QRectF> yzRects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    QCOMPARE(yzRects.size(), clip.effectRows);
    QVERIFY(yzRects.constFirst().width() != yzRects.constLast().width());
    QVERIFY(std::abs(yzRects.constFirst().center().x() - yzRects.constLast().center().x()) > output.width() * 0.4);

    clip.tilingPattern = ClipTilingPattern::Diamond;
    rects = render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    QCOMPARE(rects.size(), clip.effectRows);
    bool hasHorizontalPoint = false;
    bool hasVerticalPoint = false;
    for (const QRectF& rect : rects) {
        hasHorizontalPoint = hasHorizontalPoint || std::abs(rect.center().y() - center.y()) < 0.001;
        hasVerticalPoint = hasVerticalPoint || std::abs(rect.center().x() - center.x()) < 0.001;
    }
    QVERIFY(hasHorizontalPoint);
    QVERIFY(hasVerticalPoint);
}

void TestEffectPresets::orbitPresetProducesRequestedCopiesAroundCenter()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::PersonOrbit;
    clip.effectRows = 12;
    clip.effectSpeed = 0.0;
    clip.effectScale = 1.0;

    const QRectF output(0.0, 0.0, 1080.0, 1920.0);
    const QVector<QRectF> rects =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);

    QCOMPARE(rects.size(), 12);
    const QPointF center = output.center();
    qreal minDistance = std::numeric_limits<qreal>::max();
    qreal maxDistance = 0.0;
    for (const QRectF& rect : rects) {
        const qreal dx = rect.center().x() - center.x();
        const qreal dy = rect.center().y() - center.y();
        const qreal distance = std::sqrt((dx * dx) + (dy * dy));
        minDistance = std::min(minDistance, distance);
        maxDistance = std::max(maxDistance, distance);
    }
    QVERIFY(minDistance > 0.0);
    QVERIFY(maxDistance > minDistance);
}

void TestEffectPresets::freezePatternProducesHeldGridCopies()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::FreezePattern;
    clip.effectRows = 9;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;

    const QRectF output(0.0, 0.0, 900.0, 900.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    const QVector<QRectF> frame3 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 3.0);
    const QVector<QRectF> frame16 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 16.0);

    QCOMPARE(frame0.size(), 9);
    QCOMPARE(frame3.size(), 9);
    QCOMPARE(frame16.size(), 9);
    QCOMPARE(frame0.constFirst(), frame3.constFirst());
    QVERIFY(frame16.constFirst() != frame0.constFirst());
}

void TestEffectPresets::stepRepeatProducesSequencedCopies()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::StepRepeat;
    clip.effectRows = 6;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;

    const QRectF output(0.0, 0.0, 600.0, 400.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(120, 60), 0.0);
    const QVector<QRectF> frame12 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(120, 60), 12.0);

    QCOMPARE(frame0.size(), 6);
    QCOMPARE(frame12.size(), 6);
    QVERIFY(frame0.constFirst().x() != frame12.constFirst().x());

    clip.effectSpeed = -1.0;
    const QVector<QRectF> reverse =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(120, 60), 0.0);
    QCOMPARE(reverse.size(), 6);
    QVERIFY(reverse.constFirst().x() > frame0.constFirst().x());
}

void TestEffectPresets::directionalTrimTickerAnimatesWidthAndDirection()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::DirectionalTrimTicker;
    clip.effectRows = 4;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 800.0, 400.0);
    const QVector<QRectF> frame0 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 0.0);
    const QVector<QRectF> frame10 =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 10.0);

    QVERIFY(frame0.size() > clip.effectRows);
    QVERIFY(frame10.size() > clip.effectRows);
    QVERIFY(frame10.constFirst().width() != frame0.constFirst().width());

    clip.effectAlternateDirection = false;
    clip.effectSpeed = 1.0;
    const QVector<QRectF> forward =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 1.0);
    clip.effectSpeed = -1.0;
    const QVector<QRectF> reverse =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 1.0);
    QVERIFY(forward.size() > clip.effectRows);
    QVERIFY(reverse.size() > clip.effectRows);
    QVERIFY(forward.constFirst().x() != reverse.constFirst().x());

    clip.effectAlternateDirection = true;
    clip.effectSpeed = 1.0;
    const QVector<QRectF> alternating =
        render_detail::vulkanPresetEffectRects(clip, output, QSize(100, 100), 1.0);
    QVERIFY(alternating.size() > clip.effectRows);
}

void TestEffectPresets::vulkan3dSynthProducesDepthSortedShaderDraws()
{
    TimelineClip clip;
    clip.effectPreset = ClipEffectPreset::Vulkan3DSynth;
    clip.effectRows = 16;
    clip.effectSpeed = 1.0;
    clip.effectScale = 1.0;
    clip.effectAlternateDirection = true;

    const QRectF output(0.0, 0.0, 1280.0, 720.0);
    const render_detail::VulkanEffectPipelinePlan plan =
        render_detail::vulkanEffectPipelinePlan(clip, output, QSize(200, 100), 24.0);

    QCOMPARE(plan.generatedDraws.size(), clip.effectRows);
    QVERIFY(plan.mode == render_detail::VulkanEffectPipelinePlan::Mode::GeneratedDraws);

    qreal previousDepth = std::numeric_limits<qreal>::max();
    bool sawRotation = false;
    bool sawOpacityVariation = false;
    float firstOpacity = plan.generatedDraws.constFirst().opacityMultiplier;
    for (const render_detail::VulkanEffectPipelinePlan::DrawPass& pass : plan.generatedDraws) {
        QCOMPARE(pass.shaderMode, render_detail::kVulkanEffectModeSynth3D);
        QVERIFY(pass.depthSortKey <= previousDepth);
        previousDepth = pass.depthSortKey;
        sawRotation = sawRotation || std::abs(pass.rotationDegrees) > 0.001;
        sawOpacityVariation = sawOpacityVariation ||
            std::abs(pass.opacityMultiplier - firstOpacity) > 0.001f;
    }
    QVERIFY(sawRotation);
    QVERIFY(sawOpacityVariation);

    const QVector<render_detail::VulkanEffectPipelinePlan::DrawPass> direct =
        render_detail::vulkanSynth3DDrawPasses(render_detail::VulkanSynth3DParams{
            .outputRect = output,
            .sourceAspect = 2.0,
            .copyCount = 16,
            .scale = 1.0,
            .speed = 1.0,
            .timelineFrame = 24.0,
            .alternateHandedness = true,
        });
    QCOMPARE(direct.size(), plan.generatedDraws.size());
    QCOMPARE(direct.constFirst().shaderMode, render_detail::kVulkanEffectModeSynth3D);
}

void TestEffectPresets::newsLowerThirdPresetBuildsFlyInHoldFlyOutKeyframes()
{
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Jane Doe");
    clip.durationFrames = 120;

    QVERIFY(applyNewsLowerThirdFlyInPreset(clip));
    QCOMPARE(clip.titleKeyframes.size(), 4);
    QCOMPARE(clip.titleKeyframes.at(0).frame, int64_t(0));
    QVERIFY(clip.titleKeyframes.at(1).frame > clip.titleKeyframes.at(0).frame);
    QVERIFY(clip.titleKeyframes.at(2).frame > clip.titleKeyframes.at(1).frame);
    QCOMPARE(clip.titleKeyframes.at(3).frame, int64_t(119));
    QCOMPARE(clip.titleKeyframes.at(0).text, QStringLiteral("Jane Doe"));
    QVERIFY(clip.titleKeyframes.at(0).translationX < clip.titleKeyframes.at(1).translationX);
    QVERIFY(clip.titleKeyframes.at(3).translationX > clip.titleKeyframes.at(2).translationX);
    QCOMPARE(clip.titleKeyframes.at(0).opacity, 0.0);
    QCOMPARE(clip.titleKeyframes.at(1).opacity, 1.0);
    QCOMPARE(clip.titleKeyframes.at(2).opacity, 1.0);
    QCOMPARE(clip.titleKeyframes.at(3).opacity, 0.0);
    QVERIFY(clip.titleKeyframes.at(1).windowEnabled);
}

void TestEffectPresets::speakerTitleWrapAroundSpeakerBuilds3DKeyframes()
{
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Title;
    clip.label = QStringLiteral("Jane Doe - Director");
    clip.durationFrames = 120;

    TimelineClip::TitleKeyframe base;
    base.text = QStringLiteral("Jane Doe - Director");
    base.translationY = 0.68;
    base.fontSize = 48.0;
    base.windowEnabled = true;
    base.windowOpacity = 0.72;
    base.windowFrameEnabled = true;
    base.windowFrameOpacity = 0.85;
    clip.titleKeyframes = {base};

    SpeakerTitleFlyInSettings settings;
    settings.style = SpeakerTitleFlyInStyle::WrapAroundSpeaker;
    settings.flyInFrames = 30;
    settings.flyOutFrames = 15;
    settings.wrapRadius = 0.80;
    settings.wrapDepth = 0.75;
    settings.wrapStartAngleDegrees = -110.0;
    settings.wrapEndAngleDegrees = 110.0;
    settings.wrapPitchDegrees = 12.0;
    settings.wrapRollDegrees = 10.0;
    settings.rotationStartXDegrees = 45.0;
    settings.rotationStartYDegrees = 30.0;
    settings.rotationStartZDegrees = 25.0;
    settings.titleTextMaterialStyle = TimelineClip::TitleKeyframe::MaterialStyle::DiagonalStripes;
    settings.titleBorderMaterialStyle = TimelineClip::TitleKeyframe::MaterialStyle::Neon;
    settings.titleTextPatternImagePath = QStringLiteral("/tmp/text-pattern.png");
    settings.titleBorderPatternImagePath = QStringLiteral("/tmp/border-pattern.png");
    settings.titlePatternScale = 0.65;
    settings.titleExtrude3D = true;
    settings.titleExtrudeDepth = 0.24;
    settings.titleBevelScale = 0.80;

    QVERIFY(applyNewsLowerThirdFlyInPreset(clip, settings));
    constexpr int kExpectedOrbitKeyframes = 19;
    QVERIFY(clip.titleKeyframes.size() >= kExpectedOrbitKeyframes + 3);
    QCOMPARE(clip.titleKeyframes.constFirst().opacity, 0.0);
    qreal minOpacity = 1.0;
    qreal maxOrbitX = 0.0;
    bool sawPitchedOrRolledY = false;
    bool sawYawChange = false;
    const qreal firstYaw = clip.titleKeyframes.at(0).vulkan3DYawDegrees;
    for (int i = 0; i < kExpectedOrbitKeyframes; ++i) {
        const TimelineClip::TitleKeyframe& keyframe = clip.titleKeyframes.at(i);
        QVERIFY(keyframe.vulkan3DEnabled);
        QVERIFY(keyframe.vulkan3DExtrudeEnabled);
        QCOMPARE(keyframe.vulkan3DExtrudeDepth, settings.titleExtrudeDepth);
        QCOMPARE(keyframe.vulkan3DBevelScale, settings.titleBevelScale);
        QCOMPARE(keyframe.textMaterialStyle, settings.titleTextMaterialStyle);
        QCOMPARE(keyframe.windowFrameMaterialStyle, settings.titleBorderMaterialStyle);
        QCOMPARE(keyframe.textPatternImagePath, settings.titleTextPatternImagePath);
        QCOMPARE(keyframe.windowFramePatternImagePath, settings.titleBorderPatternImagePath);
        minOpacity = qMin(minOpacity, keyframe.opacity);
        maxOrbitX = qMax(maxOrbitX, std::abs(keyframe.translationX));
        sawPitchedOrRolledY = sawPitchedOrRolledY || std::abs(keyframe.translationY - 0.92) > 0.025;
        sawYawChange = sawYawChange || std::abs(keyframe.vulkan3DYawDegrees - firstYaw) > 1.0;
        QVERIFY(std::abs(keyframe.vulkan3DYawDegrees) <=
                62.1 + std::abs(settings.rotationStartYDegrees));
    }
    QVERIFY(minOpacity < 0.12);
    QVERIFY(maxOrbitX > 0.55);
    QVERIFY(sawPitchedOrRolledY);
    QVERIFY(sawYawChange);
    QCOMPARE(clip.titleKeyframes.constFirst().vulkan3DPitchDegrees,
             settings.wrapPitchDegrees + settings.rotationStartXDegrees);
    QCOMPARE(clip.titleKeyframes.constFirst().vulkan3DRollDegrees,
             settings.wrapRollDegrees + settings.rotationStartZDegrees);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes - 1).vulkan3DPitchDegrees,
             settings.wrapPitchDegrees);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes - 1).vulkan3DRollDegrees,
             settings.wrapRollDegrees);
    QVERIFY(clip.titleKeyframes.at(kExpectedOrbitKeyframes).vulkan3DEnabled);
    QVERIFY(clip.titleKeyframes.at(kExpectedOrbitKeyframes).vulkan3DExtrudeEnabled);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes).translationX, 0.0);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes).vulkan3DPitchDegrees, 0.0);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes).vulkan3DYawDegrees, 0.0);
    QCOMPARE(clip.titleKeyframes.at(kExpectedOrbitKeyframes).vulkan3DRollDegrees, 0.0);

    TimelineClip narrowClip = clip;
    narrowClip.titleKeyframes = {base};
    SpeakerTitleFlyInSettings narrowSettings = settings;
    narrowSettings.wrapRadius = 0.36;
    QVERIFY(applyNewsLowerThirdFlyInPreset(narrowClip, narrowSettings));
    qreal narrowMaxOrbitX = 0.0;
    for (int i = 0; i < kExpectedOrbitKeyframes; ++i) {
        narrowMaxOrbitX = qMax(narrowMaxOrbitX, std::abs(narrowClip.titleKeyframes.at(i).translationX));
    }
    QVERIFY(narrowMaxOrbitX < maxOrbitX);
}

void TestEffectPresets::generatedSpeakerTitlePlacementReplacesAndAvoidsTrackConflicts()
{
    QVector<TimelineTrack> tracks;
    TimelineTrack mediaTrack;
    mediaTrack.name = QStringLiteral("Track 1");
    tracks.push_back(mediaTrack);
    TimelineTrack titleTrack;
    titleTrack.name = QStringLiteral("Speaker Titles");
    titleTrack.audioEnabled = false;
    tracks.push_back(titleTrack);

    TimelineClip source;
    source.id = QStringLiteral("source");
    source.trackIndex = 0;
    source.startFrame = 0;
    source.durationFrames = 300;

    TimelineClip stale;
    stale.id = QStringLiteral("old-title");
    stale.clipRole = ClipRole::SpeakerTitle;
    stale.linkedSourceClipId = source.id;
    stale.mediaType = ClipMediaType::Title;
    stale.trackIndex = 1;
    stale.startFrame = 10;
    stale.durationFrames = 90;
    stale.zLevel = 75;
    stale.zLevelUserSet = true;

    TimelineClip unrelated = stale;
    unrelated.id = QStringLiteral("other-source-title");
    unrelated.linkedSourceClipId = QStringLiteral("other-source");
    unrelated.startFrame = 200;

    QVector<TimelineClip> clips{source, stale, unrelated};

    TimelineClip titleA;
    titleA.id = QStringLiteral("title-a");
    titleA.clipRole = ClipRole::SpeakerTitle;
    titleA.linkedSourceClipId = source.id;
    titleA.mediaType = ClipMediaType::Title;
    titleA.startFrame = 20;
    titleA.durationFrames = 90;

    TimelineClip titleB = titleA;
    titleB.id = QStringLiteral("title-b");
    titleB.startFrame = 50;

    const GeneratedClipPlacementResult result = replaceGeneratedClipsForSource(
        clips,
        tracks,
        source.id,
        ClipRole::SpeakerTitle,
        QVector<TimelineClip>{titleA, titleB},
        QStringLiteral("Transcript • Speaker Introductions"));

    QVERIFY(result.changed);
    QCOMPARE(result.removedCount, 1);
    QCOMPARE(result.insertedCount, 2);
    QCOMPARE(result.firstInsertedClipId, QStringLiteral("title-a"));
    QCOMPARE(tracks.size(), 3);
    QCOMPARE(tracks.at(1).name, QStringLiteral("Speaker Titles"));
    QCOMPARE(tracks.at(2).name,
             QStringLiteral("↳ Transcript • Speaker Introductions"));
    QVERIFY(tracks.at(2).generatedChildTrack);
    QCOMPARE(tracks.at(2).parentClipId, source.id);
    QCOMPARE(tracks.at(2).childClipId,
             QStringLiteral("title-a"));

    int sourceTitleCount = 0;
    int titleATrack = -1;
    int titleBTrack = -1;
    bool keptUnrelated = false;
    for (const TimelineClip& clip : clips) {
        QVERIFY(clip.id != QStringLiteral("old-title"));
        if (clip.id == QStringLiteral("other-source-title")) {
            keptUnrelated = true;
        }
        if (clip.clipRole == ClipRole::SpeakerTitle && clip.linkedSourceClipId == source.id) {
            ++sourceTitleCount;
            QCOMPARE(clip.zLevel, 75);
            QVERIFY(clip.zLevelUserSet);
            if (clip.id == QStringLiteral("title-a")) {
                titleATrack = clip.trackIndex;
            } else if (clip.id == QStringLiteral("title-b")) {
                titleBTrack = clip.trackIndex;
            }
        }
    }
    QCOMPARE(sourceTitleCount, 2);
    QVERIFY(keptUnrelated);
    QCOMPARE(titleATrack, 2);
    QCOMPARE(titleBTrack, 2);
}

void TestEffectPresets::generatedSpeakerTitleResolvesToTranscriptParent()
{
    TimelineClip transcript;
    transcript.id = QStringLiteral("transcript-parent");
    transcript.mediaType = ClipMediaType::Video;
    transcript.filePath = QStringLiteral("/tmp/interview.mp4");

    TimelineClip introduction = createDefaultTitleClip(20, 2, 40);
    introduction.clipRole = ClipRole::SpeakerTitle;
    introduction.linkedSourceClipId = transcript.id;

    const QVector<TimelineClip> clips{transcript, introduction};
    const TimelineClip* parent = clipParent(clips.at(1), clips);
    QVERIFY(parent);
    QCOMPARE(parent->id, transcript.id);
    QCOMPARE(parent->filePath, transcript.filePath);
    QVERIFY(!clipParent(clips.at(0), clips));
    const ClipSelectionContext context = clipSelectionContext(&clips.at(1), clips);
    QCOMPARE(context.selected->id, introduction.id);
    QCOMPARE(context.parent->id, transcript.id);
    QCOMPARE(context.owner()->id, transcript.id);
}

QTEST_MAIN(TestEffectPresets)
#include "test_effect_presets.moc"
