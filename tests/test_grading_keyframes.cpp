#include <QtTest/QtTest>

#include "../editor_shared.h"

class TestGradingKeyframes : public QObject {
    Q_OBJECT

private slots:
    void testCompatibleCurvesInterpolateLinearly();
    void testCurveOnlyOpacityCleanupKeyframeIsPreserved();
};

void TestGradingKeyframes::testCompatibleCurvesInterpolateLinearly()
{
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Video;
    clip.startFrame = 100;
    clip.durationFrames = 11;

    TimelineClip::GradingKeyframe first;
    first.frame = 0;
    first.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.4}, {1.0, 1.0}};
    first.curvePointsG = defaultGradingCurvePoints();
    first.curvePointsB = defaultGradingCurvePoints();
    first.curvePointsLuma = defaultGradingCurvePoints();

    TimelineClip::GradingKeyframe second = first;
    second.frame = 10;
    second.linearInterpolation = true;
    second.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}};

    clip.gradingKeyframes = QVector<TimelineClip::GradingKeyframe>{first, second};

    const TimelineClip::GradingKeyframe evaluated = evaluateClipGradingAtFrame(clip, 105);

    QCOMPARE(evaluated.curvePointsR.size(), 3);
    QCOMPARE(evaluated.curvePointsR.at(1).x(), 0.5);
    QVERIFY(std::abs(evaluated.curvePointsR.at(1).y() - 0.6) < 0.000001);
}

void TestGradingKeyframes::testCurveOnlyOpacityCleanupKeyframeIsPreserved()
{
    TimelineClip clip;
    clip.mediaType = ClipMediaType::Video;
    clip.durationFrames = 11;
    clip.opacityKeyframes = QVector<TimelineClip::OpacityKeyframe>{
        TimelineClip::OpacityKeyframe{5, 1.0, true},
    };

    TimelineClip::GradingKeyframe first;
    first.frame = 0;
    first.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.4}, {1.0, 1.0}};
    first.curvePointsG = defaultGradingCurvePoints();
    first.curvePointsB = defaultGradingCurvePoints();
    first.curvePointsLuma = defaultGradingCurvePoints();

    TimelineClip::GradingKeyframe middle = first;
    middle.frame = 5;
    middle.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.9}, {1.0, 1.0}};

    TimelineClip::GradingKeyframe last = first;
    last.frame = 10;
    last.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}};

    clip.gradingKeyframes = QVector<TimelineClip::GradingKeyframe>{first, middle, last};

    normalizeClipGradingKeyframes(clip);

    QCOMPARE(clip.gradingKeyframes.size(), 3);
    QCOMPARE(clip.gradingKeyframes.at(1).frame, 5);
    QVERIFY(std::abs(clip.gradingKeyframes.at(1).curvePointsR.at(1).y() - 0.9) < 0.000001);
}

QTEST_MAIN(TestGradingKeyframes)
#include "test_grading_keyframes.moc"
