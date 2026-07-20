#include <QtTest/QtTest>

#include "../editor_preview_edit_helpers.h"
#include "../editor_shared_keyframes.h"

class TestPreviewEditHelpers : public QObject {
    Q_OBJECT

private slots:
    void testPreviewResizeCreatesTemporalTransformKeyframe();
    void testPreviewResizeUpdatesTemporalTransformKeyframe();
    void testPreviewResizeKeyframeStoresRelativeToBaseTransform();
    void testPreviewMoveCommitsAsStaticBaseTransform();
};

namespace {

TimelineClip makeVideoClipWithPreviewKeyframeArtifacts() {
    TimelineClip clip;
    clip.id = QStringLiteral("clip-preview-edit");
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 0;
    clip.durationFrames = 367;
    clip.sourceDurationFrames = 367;
    clip.sourceFrameSize = QSize(1280, 720);
    clip.baseTranslationX = 0.0;
    clip.baseTranslationY = 0.0;
    clip.baseScaleX = 1.0;
    clip.baseScaleY = 1.0;

    TimelineClip::TransformKeyframe frame0;
    frame0.frame = 0;
    frame0.translationX = 232.0;
    frame0.translationY = 129.0;
    frame0.scaleX = 1.0;
    frame0.scaleY = 1.0;

    TimelineClip::TransformKeyframe frame162;
    frame162.frame = 162;
    frame162.translationX = -318.0;
    frame162.translationY = -68.0;
    frame162.scaleX = 1.89;
    frame162.scaleY = 1.89;

    clip.transformKeyframes = {frame0, frame162};
    normalizeClipTransformKeyframes(clip);
    return clip;
}

void verifyIdentityTransformKeyframe(const TimelineClip::TransformKeyframe& keyframe) {
    QCOMPARE(keyframe.frame, int64_t{0});
    QCOMPARE(keyframe.translationX, 0.0);
    QCOMPARE(keyframe.translationY, 0.0);
    QCOMPARE(keyframe.rotation, 0.0);
    QCOMPARE(keyframe.scaleX, 1.0);
    QCOMPARE(keyframe.scaleY, 1.0);
    QCOMPARE(keyframe.maskRepeatDeltaX, 0.0);
    QCOMPARE(keyframe.maskRepeatDeltaY, 0.0);
}

} // namespace

void TestPreviewEditHelpers::testPreviewResizeCreatesTemporalTransformKeyframe() {
    TimelineClip clip = makeVideoClipWithPreviewKeyframeArtifacts();
    clip.transformKeyframes.removeLast();

    QVERIFY(commitPreviewTransform(
        clip,
        162,
        -120.0,
        48.0,
        1.75,
        1.75,
        false));

    QCOMPARE(clip.baseTranslationX, 0.0);
    QCOMPARE(clip.baseTranslationY, 0.0);
    QCOMPARE(clip.baseScaleX, 1.0);
    QCOMPARE(clip.baseScaleY, 1.0);
    QCOMPARE(clip.transformKeyframes.size(), 2);
    QCOMPARE(clip.transformKeyframes.constLast().frame, int64_t{162});
    QCOMPARE(clip.transformKeyframes.constLast().translationX, -120.0);
    QCOMPARE(clip.transformKeyframes.constLast().translationY, 48.0);
    QCOMPARE(clip.transformKeyframes.constLast().scaleX, 1.75);
    QCOMPARE(clip.transformKeyframes.constLast().scaleY, 1.75);

    const TimelineClip::TransformKeyframe start =
        evaluateClipTransformAtFrame(clip, 0);
    const TimelineClip::TransformKeyframe resized =
        evaluateClipTransformAtFrame(clip, 162);
    QCOMPARE(start.translationX, 232.0);
    QCOMPARE(start.translationY, 129.0);
    QCOMPARE(start.scaleX, 1.0);
    QCOMPARE(start.scaleY, 1.0);
    QCOMPARE(resized.translationX, -120.0);
    QCOMPARE(resized.translationY, 48.0);
    QCOMPARE(resized.scaleX, 1.75);
    QCOMPARE(resized.scaleY, 1.75);
}

void TestPreviewEditHelpers::testPreviewResizeUpdatesTemporalTransformKeyframe() {
    TimelineClip clip = makeVideoClipWithPreviewKeyframeArtifacts();
    clip.transformKeyframes.last().rotation = 12.0;
    clip.transformKeyframes.last().maskRepeatDeltaX = 3.0;
    clip.transformKeyframes.last().maskRepeatDeltaY = -4.0;

    QVERIFY(commitPreviewTransform(
        clip,
        162,
        -120.0,
        48.0,
        1.75,
        1.75,
        false));

    QCOMPARE(clip.transformKeyframes.size(), 2);
    QCOMPARE(clip.transformKeyframes.constLast().frame, int64_t{162});
    QCOMPARE(clip.transformKeyframes.constLast().translationX, -120.0);
    QCOMPARE(clip.transformKeyframes.constLast().translationY, 48.0);
    QCOMPARE(clip.transformKeyframes.constLast().scaleX, 1.75);
    QCOMPARE(clip.transformKeyframes.constLast().scaleY, 1.75);
    QCOMPARE(clip.transformKeyframes.constLast().rotation, 12.0);
    QCOMPARE(clip.transformKeyframes.constLast().maskRepeatDeltaX, 3.0);
    QCOMPARE(clip.transformKeyframes.constLast().maskRepeatDeltaY, -4.0);
}

void TestPreviewEditHelpers::testPreviewResizeKeyframeStoresRelativeToBaseTransform() {
    TimelineClip clip = makeVideoClipWithPreviewKeyframeArtifacts();
    clip.transformKeyframes.clear();
    clip.baseTranslationX = 10.0;
    clip.baseTranslationY = -20.0;
    clip.baseScaleX = 2.0;
    clip.baseScaleY = 2.0;

    QVERIFY(commitPreviewTransform(
        clip,
        90,
        -120.0,
        48.0,
        1.75,
        1.75,
        false));

    QCOMPARE(clip.transformKeyframes.size(), 2);
    verifyIdentityTransformKeyframe(clip.transformKeyframes.constFirst());
    QCOMPARE(clip.transformKeyframes.constLast().frame, int64_t{90});
    QCOMPARE(clip.transformKeyframes.constLast().translationX, -130.0);
    QCOMPARE(clip.transformKeyframes.constLast().translationY, 68.0);
    QCOMPARE(clip.transformKeyframes.constLast().scaleX, 0.875);
    QCOMPARE(clip.transformKeyframes.constLast().scaleY, 0.875);

    const TimelineClip::TransformKeyframe start =
        evaluateClipTransformAtFrame(clip, 0);
    const TimelineClip::TransformKeyframe resized =
        evaluateClipTransformAtFrame(clip, 90);
    QCOMPARE(start.translationX, 10.0);
    QCOMPARE(start.translationY, -20.0);
    QCOMPARE(start.scaleX, 2.0);
    QCOMPARE(start.scaleY, 2.0);
    QCOMPARE(resized.translationX, -120.0);
    QCOMPARE(resized.translationY, 48.0);
    QCOMPARE(resized.scaleX, 1.75);
    QCOMPARE(resized.scaleY, 1.75);
}

void TestPreviewEditHelpers::testPreviewMoveCommitsAsStaticBaseTransform() {
    TimelineClip clip = makeVideoClipWithPreviewKeyframeArtifacts();
    clip.baseScaleX = 1.5;
    clip.baseScaleY = 1.5;

    QVERIFY(commitPreviewMove(clip, 162, 32.0, -44.0, false));

    QCOMPARE(clip.baseTranslationX, 32.0);
    QCOMPARE(clip.baseTranslationY, -44.0);
    QCOMPARE(clip.baseScaleX, 1.5);
    QCOMPARE(clip.baseScaleY, 1.5);
    QCOMPARE(clip.transformKeyframes.size(), 1);
    verifyIdentityTransformKeyframe(clip.transformKeyframes.constFirst());
}

QTEST_MAIN(TestPreviewEditHelpers)
#include "test_preview_edit_helpers.moc"
