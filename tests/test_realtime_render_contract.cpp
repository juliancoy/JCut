#include <QtTest/QtTest>

#include "../editor_shared_keyframes.h"
#include "../editor_shared_effects.h"
#include "../editor_shared_media.h"
#include "../editor_shared_render_sync.h"
#include "../export_timing.h"
#include "../timeline_fps.h"

#include <QFile>
#include <QRegularExpression>
#include <cmath>

namespace {

QVector<qreal> outputTimelinePositions(int64_t startFrame, int64_t endFrame, qreal outputFps)
{
    const int64_t outputFrameCount =
        jcut::export_timing::outputFrameCountForTimelineRange(startFrame, endFrame, outputFps, 1.0);

    QVector<qreal> positions;
    positions.reserve(static_cast<int>(outputFrameCount));
    for (int64_t outputFrame = 0; outputFrame < outputFrameCount; ++outputFrame) {
        const jcut::export_timing::ExportFrameTiming timing =
            jcut::export_timing::frameTimingForOutputFrame(
                outputFrame,
                startFrame,
                endFrame,
                outputFps,
                1.0);
        positions.push_back(static_cast<qreal>(timing.timelineFramePosition));
    }
    return positions;
}

TimelineClip makeMappedClip(qreal sourceFps)
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip-render-contract");
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 30;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 60;
    clip.sourceFps = sourceFps;
    clip.playbackRate = 1.0;
    return clip;
}

} // namespace

class TestRealtimeRenderContract : public QObject {
    Q_OBJECT

private slots:
    void outputFpsSamplingProducesFractionalTimelinePositions();
    void exportFrameTimingNamesOutputTimeAndTimelineDomains();
    void fractionalSourceMappingDoesNotDuplicateThirtyFpsFrames();
    void renderTransformsInterpolateAtOutputFpsPositions();
    void childTransformLockUsesSourceTransformWhenEnabled();
    void childMaskMatteOwnsIndependentKeyframedGrade();
    void hiddenParentStillProvidesMediaForVisibleMaskMatte();
    void exportLoopPassesFractionalPositionToRenderer();
};

void TestRealtimeRenderContract::outputFpsSamplingProducesFractionalTimelinePositions()
{
    const QVector<qreal> positions = outputTimelinePositions(0, 29, 60.0);

    QCOMPARE(positions.size(), 60);
    QCOMPARE(positions.constFirst(), 0.0);
    QVERIFY2(std::abs(positions.at(1) - 0.5) < 0.000001,
             "60 fps output must sample between 30 fps edit frames");
    QVERIFY2(std::abs(positions.at(2) - 1.0) < 0.000001,
             "output PTS mapping must advance by output-frame duration");
    QCOMPARE(positions.constLast(), 29.0);
}

void TestRealtimeRenderContract::exportFrameTimingNamesOutputTimeAndTimelineDomains()
{
    const std::int64_t outputFrameCount =
        jcut::export_timing::outputFrameCountForTimelineRange(100, 129, 60.0, 0.5);
    QCOMPARE(outputFrameCount, std::int64_t(120));

    const jcut::export_timing::ExportFrameTiming timing =
        jcut::export_timing::frameTimingForOutputFrame(3, 100, 129, 60.0, 0.5);

    QCOMPARE(timing.outputFrame, std::int64_t(3));
    QVERIFY2(std::abs(timing.outputTimeSeconds - 0.05) < 0.000001,
             "export timing must start from explicit output PTS seconds");
    QVERIFY2(std::abs(timing.timelineFramePosition - 100.75) < 0.000001,
             "export output time must project through timeline fps and playback speed");
    QCOMPARE(timing.timelineFrame, std::int64_t(100));

    const jcut::export_timing::ExportFrameTiming clamped =
        jcut::export_timing::frameTimingForOutputFrame(999, 100, 129, 60.0, 1.0);
    QCOMPARE(clamped.timelineFramePosition, 129.0);
    QCOMPARE(clamped.timelineFrame, std::int64_t(129));
}

void TestRealtimeRenderContract::fractionalSourceMappingDoesNotDuplicateThirtyFpsFrames()
{
    const TimelineClip clip = makeMappedClip(60.0);
    const QVector<qreal> positions = outputTimelinePositions(0, 29, 60.0);

    QVector<qreal> sourcePositions;
    sourcePositions.reserve(positions.size());
    for (const qreal timelinePosition : positions) {
        sourcePositions.push_back(
            sourceFramePositionForClipAtTimelinePosition(clip, timelinePosition, {}));
    }

    QVERIFY2(std::abs(sourcePositions.at(0) - 0.0) < 0.000001,
             "first output frame should map to first source frame");
    QVERIFY2(std::abs(sourcePositions.at(1) - 1.0) < 0.000001,
             "second 60 fps output frame should map to the next 60 fps source frame");
    QVERIFY2(std::abs(sourcePositions.at(2) - 2.0) < 0.000001,
             "source mapping must not duplicate every other source frame at 60 fps");

    for (int i = 1; i < sourcePositions.size(); ++i) {
        QVERIFY2(sourcePositions.at(i) >= sourcePositions.at(i - 1),
                 "source positions must be monotonic during real-time render sampling");
        QVERIFY2(std::abs((sourcePositions.at(i) - sourcePositions.at(i - 1)) - 1.0) < 0.000001 ||
                     i == sourcePositions.size() - 1,
                 "60 fps output from 60 fps media should advance one source frame per output PTS");
    }
}

void TestRealtimeRenderContract::renderTransformsInterpolateAtOutputFpsPositions()
{
    TimelineClip clip = makeMappedClip(30.0);
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.transformKeyframes = QVector<TimelineClip::TransformKeyframe>{
        TimelineClip::TransformKeyframe{0, QString(), 0.0, 0.0, 0.0, 1.0, 1.0, true},
        TimelineClip::TransformKeyframe{1, QString(), 10.0, 20.0, 0.0, 1.0, 1.0, true},
    };

    const TimelineClip::TransformKeyframe atHalfFrame =
        evaluateClipRenderTransformAtPosition(clip, 0.5, QSize(1920, 1080));

    QVERIFY2(std::abs(atHalfFrame.translationX - 5.0) < 0.000001,
             "render transforms must evaluate at fractional output-frame positions");
    QVERIFY2(std::abs(atHalfFrame.translationY - 10.0) < 0.000001,
             "fractional render transform evaluation prevents visible half-frame stepping");
}

void TestRealtimeRenderContract::childTransformLockUsesSourceTransformWhenEnabled()
{
    TimelineClip source = makeMappedClip(30.0);
    source.id = QStringLiteral("source");
    source.baseTranslationX = 100.0;
    source.baseTranslationY = 25.0;
    source.baseRotation = 7.5;
    source.baseScaleX = 1.5;
    source.baseScaleY = 1.25;

    TimelineClip child = makeMappedClip(30.0);
    child.id = QStringLiteral("child");
    child.linkedSourceClipId = source.id;
    child.baseTranslationX = -200.0;
    child.baseTranslationY = -75.0;
    child.baseRotation = -12.0;
    child.baseScaleX = 0.5;
    child.baseScaleY = 0.5;

    const QVector<TimelineClip> clips{source, child};
    const TimelineClip::TransformKeyframe unlocked =
        evaluateClipRenderTransformWithSourceLockAtPosition(
            child, clips, 10.0, {}, QSize(1920, 1080));
    QCOMPARE(unlocked.translationX, child.baseTranslationX);
    QCOMPARE(unlocked.translationY, child.baseTranslationY);

    child.sourceTransformLocked = true;
    const QVector<TimelineClip> lockedClips{source, child};
    const TimelineClip::TransformKeyframe locked =
        evaluateClipRenderTransformWithSourceLockAtPosition(
            child, lockedClips, 10.0, {}, QSize(1920, 1080));
    QCOMPARE(locked.translationX, source.baseTranslationX);
    QCOMPARE(locked.translationY, source.baseTranslationY);
    QCOMPARE(locked.rotation, source.baseRotation);
    QCOMPARE(locked.scaleX, source.baseScaleX);
    QCOMPARE(locked.scaleY, source.baseScaleY);
}

void TestRealtimeRenderContract::childMaskMatteOwnsIndependentKeyframedGrade()
{
    TimelineClip source = makeMappedClip(30.0);
    source.id = QStringLiteral("source");
    source.gradingKeyframes = {
        TimelineClip::GradingKeyframe{0, 0.1, 1.0, 1.0},
    };

    TimelineClip matte = source;
    matte.id = QStringLiteral("source-mask-matte");
    matte.clipRole = ClipRole::MaskMatte;
    matte.linkedSourceClipId = source.id;
    matte.gradingKeyframes = {
        TimelineClip::GradingKeyframe{0, 0.0, 1.0, 1.0},
        TimelineClip::GradingKeyframe{10, 0.8, 1.4, 0.6},
    };

    const TimelineClip::GradingKeyframe sourceGrade =
        evaluateEffectiveClipGradingAtPosition(source, {}, 5.0);
    const TimelineClip::GradingKeyframe matteGrade =
        evaluateEffectiveClipGradingAtPosition(matte, {}, 5.0);

    QVERIFY(std::abs(sourceGrade.brightness - 0.1) < 0.000001);
    QVERIFY2(std::abs(matteGrade.brightness - 0.4) < 0.000001,
             "virtual matte brightness must interpolate on the matte's own grading timeline");
    QVERIFY2(std::abs(matteGrade.contrast - 1.2) < 0.000001,
             "virtual matte contrast must not fall back to the linked source grade");
    QVERIFY2(std::abs(matteGrade.saturation - 0.8) < 0.000001,
             "virtual matte saturation must not fall back to the linked source grade");
    QVERIFY2(std::abs(sourceGrade.brightness - matteGrade.brightness) > 0.1,
             "grading the virtual matte must leave the linked source grade independent");
}

void TestRealtimeRenderContract::hiddenParentStillProvidesMediaForVisibleMaskMatte()
{
    TimelineClip source = makeMappedClip(30.0);
    source.id = QStringLiteral("hidden-parent");
    source.videoEnabled = false;

    TimelineClip matte = source;
    matte.id = QStringLiteral("visible-mask-child");
    matte.clipRole = ClipRole::MaskMatte;
    matte.linkedSourceClipId = source.id;
    matte.videoEnabled = true;

    const QVector<TimelineClip> clips{source, matte};
    QVERIFY(!clipVisualPlaybackEnabled(source, {}));
    QVERIFY(clipVisualPlaybackEnabled(matte, {}));
    QVERIFY2(clipProvidesMediaForVisibleMaskMatte(source, clips, {}),
             "a hidden parent must remain a decode provider for its visible child mask clip");
    QVERIFY2(clipContributesVisualMedia(source, clips, {}),
             "shared render policy must retain a hidden source for its visible mask matte");

    TimelineClip synth = matte;
    synth.id = QStringLiteral("visible-effect-child");
    synth.clipRole = ClipRole::EffectSynth;
    QVERIFY(clipIsChildOf(synth, source));
    QVERIFY(clipChildPlaybackEnabled(synth, {}));
    QVERIFY2(clipHasVisibleChild(source, {source, synth}, {}),
             "decode ownership must consider every visible child role");
    QVERIFY(clipContributesVisualMedia(source, {source, synth}, {}));
    ClipParentChildIndex relationships;
    relationships.rebuild({source, synth}, 42);
    QCOMPARE(relationships.timelineRevision(), quint64(42));
    QVERIFY(relationships.hasVisibleChild(source, {source, synth}, {}));
    QVERIFY(clipContributesVisualMedia(source, {source, synth}, {}, &relationships));

    matte.videoEnabled = false;
    QVERIFY2(!clipProvidesMediaForVisibleMaskMatte(source, {source, matte}, {}),
             "a hidden child mask must not keep its parent active as a media provider");
    QVERIFY(!clipContributesVisualMedia(source, {source, matte}, {}));
}

void TestRealtimeRenderContract::exportLoopPassesFractionalPositionToRenderer()
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QStringLiteral("/render_export.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "render_export.cpp must be readable");
    const QString source = QString::fromUtf8(file.readAll());

    QVERIFY2(source.contains(QStringLiteral("const qreal timelineFramePosition")),
             "export must derive a fractional timeline position from output PTS");
    QVERIFY2(source.contains(QStringLiteral("frameTimingForOutputFrame")),
             "export must use the shared output-time timing helper");
    QVERIFY2(source.contains(QStringLiteral("exportFrameTiming.timelineFramePosition")),
             "export must pass the helper's explicit fractional timeline position through to rendering");
    static const QRegularExpression fractionalRenderCall(
        QStringLiteral("activeRenderer->renderFrameToOutput\\s*\\(\\s*request\\s*,\\s*timelineFramePosition"));
    QVERIFY2(fractionalRenderCall.match(source).hasMatch(),
             "Vulkan export must render the fractional timeline position, not the floored edit frame");
    QVERIFY2(!source.contains(QStringLiteral("renderTimelineFrameToOutput(request,")),
             "export must not contain the removed CPU render fallback path");
    QVERIFY2(!source.contains(QStringLiteral("renderTranscriptOverlay(")),
             "Vulkan export must not run a post-render CPU transcript overlay pass");
    QVERIFY2(!source.contains(QStringLiteral("!hasTranscriptOverlay")),
             "Transcript overlays must not disable direct Vulkan handoff or GPU color conversion");

    QFile vulkanFile(QStringLiteral(JCUT_SOURCE_DIR) + QStringLiteral("/offscreen_vulkan_renderer_backend.cpp"));
    QVERIFY2(vulkanFile.open(QIODevice::ReadOnly),
             "offscreen_vulkan_renderer_backend.cpp must be readable");
    const QString vulkanSource = QString::fromUtf8(vulkanFile.readAll());
    QVERIFY2(vulkanSource.contains(QStringLiteral("const bool gpuOutputOnly = (readbackMs == nullptr)")),
             "Vulkan export must make GPU-output mode explicit");
    QVERIFY2(!vulkanSource.contains(QStringLiteral("CPU-raster title layer")) &&
                 vulkanSource.contains(QStringLiteral("textInputs.title3D.push_back(title)")),
             "All titles must use the Vulkan text path, including GPU-only export");
    QVERIFY2(vulkanSource.contains(QStringLiteral("uploadFrame(layer.frameHandle, false")) &&
                 vulkanSource.contains(QStringLiteral("return false;\n        }\n      }\n      QImage rgba")),
             "GPU-output render path must not fall back to CPU image uploads when hardware handoff fails");
}

QTEST_MAIN(TestRealtimeRenderContract)
#include "test_realtime_render_contract.moc"
