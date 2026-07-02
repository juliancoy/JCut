#include <QtTest/QtTest>

#include "../clip_serialization.h"
#include "../editor_effect_presets.h"
#include "../editor_shared_effects.h"
#include "../render_vulkan_shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFile>
#include <QTemporaryDir>

class TestEffectPresets : public QObject {
    Q_OBJECT

private slots:
    void clipSerializationPersistsEffectPresetState();
    void clipSerializationPersistsArpeggiatorEffectPresets();
    void trackEffectSettingsOverlayOntoClipForRenderPipeline();
    void clipSerializationPersistsGeneratedClipRoleState();
    void samMaskMatteFactoryKeepsSourceTimingLocked();
    void alternatingMotionBackgroundFactoryCreatesVisualOnlySynthClip();
    void sourceTilingFactoryCreatesVisualOnlySynthClip();
    void speakerTitleFactoryBuildsLowerThirdsForSpeakerChanges();
    void effectPipelinePassesThroughWhenPresetIsOff();
    void effectPipelineUsesGeneratedDrawsForTiling();
    void maskedRepeatUsesMaskGradeDrawsAndKeyframedOffsets();
    void temporalEffectsUseContiguousPlaybackTimeAcrossSegmentGaps();
    void generatedDrawMvpKeepsVulkanYDownOrientation();
    void tickerPresetProducesAlternatingRowsAcrossOutput();
    void alternatingMotionBackgroundCoversOutputWithMovingRows();
    void sourceTilingPresetCoversOutputWithGrid();
    void sourceTilingPresetSupportsGeometricPatterns();
    void orbitPresetProducesRequestedCopiesAroundCenter();
    void freezePatternProducesHeldGridCopies();
    void stepRepeatProducesSequencedCopies();
    void directionalTrimTickerAnimatesWidthAndDirection();
    void newsLowerThirdPresetBuildsFlyInHoldFlyOutKeyframes();
    void generatedSpeakerTitlePlacementReplacesAndAvoidsTrackConflicts();
};

void TestEffectPresets::clipSerializationPersistsEffectPresetState()
{
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
    clip.effectPreset = ClipEffectPreset::NewsLogoTicker;
    clip.effectRows = 32;
    clip.effectSpeed = 1.75;
    clip.effectScale = 0.85;
    clip.effectAlternateDirection = true;
    clip.tilingPattern = ClipTilingPattern::SpiralXY;
    clip.tilingSpacing = 1.4;
    clip.tilingWrap = false;

    const QJsonObject json = editor::clipToJson(clip);
    QCOMPARE(json.value(QStringLiteral("maskForegroundLayerEnabled")).toBool(), true);
    QCOMPARE(json.value(QStringLiteral("maskRepeatEnabled")).toBool(), true);
    QVERIFY(std::abs(json.value(QStringLiteral("maskRepeatDeltaX")).toDouble() - 120.0) < 0.000001);
    QVERIFY(std::abs(json.value(QStringLiteral("maskRepeatDeltaY")).toDouble() + 15.0) < 0.000001);
    QCOMPARE(json.value(QStringLiteral("effectPreset")).toString(), QStringLiteral("news_logo_ticker"));
    QCOMPARE(json.value(QStringLiteral("effectRows")).toInt(), 32);
    QCOMPARE(json.value(QStringLiteral("tilingPattern")).toString(), QStringLiteral("spiral"));
    QVERIFY(std::abs(json.value(QStringLiteral("tilingSpacing")).toDouble() - 1.4) < 0.000001);
    QCOMPARE(json.value(QStringLiteral("tilingWrap")).toBool(), false);

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.maskForegroundLayerEnabled, true);
    QCOMPARE(loaded.maskRepeatEnabled, true);
    QVERIFY(std::abs(loaded.maskRepeatDeltaX - 120.0) < 0.000001);
    QVERIFY(std::abs(loaded.maskRepeatDeltaY + 15.0) < 0.000001);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::NewsLogoTicker);
    QCOMPARE(loaded.effectRows, 32);
    QVERIFY(std::abs(loaded.effectSpeed - 1.75) < 0.000001);
    QVERIFY(std::abs(loaded.effectScale - 0.85) < 0.000001);
    QCOMPARE(loaded.effectAlternateDirection, true);
    QCOMPARE(loaded.tilingPattern, ClipTilingPattern::SpiralXY);
    QVERIFY(std::abs(loaded.tilingSpacing - 1.4) < 0.000001);
    QCOMPARE(loaded.tilingWrap, false);
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
}

void TestEffectPresets::trackEffectSettingsOverlayOntoClipForRenderPipeline()
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

    const TimelineClip effective = clipWithTrackEffectSettings(clip, tracks);
    QCOMPARE(effective.effectPreset, ClipEffectPreset::SourceTile);
    QCOMPARE(effective.effectRows, 7);
    QVERIFY(std::abs(effective.effectSpeed - 0.25) < 0.000001);
    QVERIFY(std::abs(effective.effectScale - 1.5) < 0.000001);
    QCOMPARE(effective.effectAlternateDirection, false);
    QCOMPARE(effective.tilingPattern, ClipTilingPattern::Diamond);
    QVERIFY(std::abs(effective.tilingSpacing - 1.25) < 0.000001);
    QCOMPARE(effective.tilingWrap, false);
    QCOMPARE(clip.effectPreset, ClipEffectPreset::None);
    QCOMPARE(clip.effectRows, 3);
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

    const TimelineClip loaded = editor::clipFromJson(json);
    QCOMPARE(loaded.clipRole, ClipRole::MaskMatte);
    QCOMPARE(loaded.linkedSourceClipId, QStringLiteral("source"));
    QCOMPARE(loaded.generatedFromMaskId, QStringLiteral("/tmp/source_sam3_person_binary_masks"));
    QCOMPARE(loaded.syncLockedToSource, true);
    QCOMPARE(loaded.sourceTransformLocked, true);
    QVERIFY(!loaded.maskShowOnly);
    QCOMPARE(loaded.effectPreset, ClipEffectPreset::AlternatingMotionBackground);

    QJsonObject legacyJson = json;
    legacyJson.remove(QStringLiteral("sourceTransformLocked"));
    QVERIFY(!editor::clipFromJson(legacyJson).sourceTransformLocked);
}

void TestEffectPresets::samMaskMatteFactoryKeepsSourceTimingLocked()
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

    const TimelineClip matte = makeSamMaskMatteClip(source);
    QCOMPARE(matte.id, QStringLiteral("shot-01-mask-matte"));
    QCOMPARE(matte.clipRole, ClipRole::MaskMatte);
    QCOMPARE(matte.linkedSourceClipId, source.id);
    QCOMPARE(matte.generatedFromMaskId, source.maskFramesDir);
    QVERIFY(matte.syncLockedToSource);
    QCOMPARE(matte.filePath, source.filePath);
    QCOMPARE(matte.mediaType, source.mediaType);
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
    section.words.push_back(TranscriptWord{120, 122, QStringLiteral("S1"), QStringLiteral("again"), false});
    section.words.push_back(TranscriptWord{140, 142, QStringLiteral("S2"), QStringLiteral("there"), false});
    section.words.push_back(TranscriptWord{170, 172, QStringLiteral("S1"), QStringLiteral("back"), false});

    const QVector<TimelineClip> titles = makeSpeakerTitleClipsForTranscriptIntroductions(
        source,
        transcriptPath,
        QVector<TranscriptSection>{section},
        4,
        90);

    QCOMPARE(titles.size(), 3);
    QCOMPARE(titles.at(0).clipRole, ClipRole::SpeakerTitle);
    QCOMPARE(titles.at(0).linkedSourceClipId, source.id);
    QVERIFY(titles.at(0).syncLockedToSource);
    QCOMPARE(titles.at(0).mediaType, ClipMediaType::Title);
    QCOMPARE(titles.at(0).trackIndex, 4);
    QCOMPARE(titles.at(0).startFrame, int64_t(1010));
    QCOMPARE(titles.at(0).durationFrames, int64_t(90));
    QCOMPARE(titles.at(0).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe - Director"));
    QCOMPARE(titles.at(1).startFrame, int64_t(1040));
    QCOMPARE(titles.at(1).titleKeyframes.constFirst().text, QStringLiteral("John Roe - Producer"));
    QCOMPARE(titles.at(2).startFrame, int64_t(1070));
    QCOMPARE(titles.at(2).titleKeyframes.constFirst().text, QStringLiteral("Jane Doe - Director"));
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
        QStringLiteral("Speaker Titles"));

    QVERIFY(result.changed);
    QCOMPARE(result.removedCount, 1);
    QCOMPARE(result.insertedCount, 2);
    QCOMPARE(result.firstInsertedClipId, QStringLiteral("title-a"));
    QCOMPARE(tracks.size(), 3);
    QCOMPARE(tracks.at(1).name, QStringLiteral("Speaker Titles"));
    QCOMPARE(tracks.at(2).name, QStringLiteral("Speaker Titles 2"));

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
            if (clip.id == QStringLiteral("title-a")) {
                titleATrack = clip.trackIndex;
            } else if (clip.id == QStringLiteral("title-b")) {
                titleBTrack = clip.trackIndex;
            }
        }
    }
    QCOMPARE(sourceTitleCount, 2);
    QVERIFY(keptUnrelated);
    QCOMPARE(titleATrack, 1);
    QCOMPARE(titleBTrack, 2);
}

QTEST_MAIN(TestEffectPresets)
#include "test_effect_presets.moc"
