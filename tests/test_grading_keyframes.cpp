#include <QtTest/QtTest>

#include "../editor_shared.h"
#include "../render_vulkan_shared.h"

class TestGradingKeyframes : public QObject {
    Q_OBJECT

private slots:
    void testCompatibleCurvesInterpolateLinearly();
    void testCurveOnlyOpacityCleanupKeyframeIsPreserved();
    void testIdentityCurvesDoNotActivateCurveLut();
    void testNonIdentityCurveActivatesCurveLut();
    void testCurveSanitizerPreservesGraphAdjustmentRange();
    void testBrightnessCurveLutDoesNotCollapseToWhite();
    void testVulkanBrightnessCurveLutMatchesCpuLut();
    void testVulkanDrawStateDoesNotEnableCurveByDefault();
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

void TestGradingKeyframes::testIdentityCurvesDoNotActivateCurveLut()
{
    TimelineClip::GradingKeyframe grade;
    grade.curvePointsR = defaultGradingCurvePoints();
    grade.curvePointsG = defaultGradingCurvePoints();
    grade.curvePointsB = defaultGradingCurvePoints();
    grade.curvePointsLuma = defaultGradingCurvePoints();

    QVERIFY(!gradingUsesCurveLut(grade));
}

void TestGradingKeyframes::testNonIdentityCurveActivatesCurveLut()
{
    TimelineClip::GradingKeyframe grade;
    grade.curvePointsR = QVector<QPointF>{{0.0, 0.0}, {0.5, 0.65}, {1.0, 1.0}};
    grade.curvePointsG = defaultGradingCurvePoints();
    grade.curvePointsB = defaultGradingCurvePoints();
    grade.curvePointsLuma = defaultGradingCurvePoints();

    QVERIFY(gradingUsesCurveLut(grade));
}

void TestGradingKeyframes::testCurveSanitizerPreservesGraphAdjustmentRange()
{
    const QVector<QPointF> sanitized = sanitizeGradingCurvePoints({
        QPointF(-0.2, -0.75),
        QPointF(0.5, 1.5),
        QPointF(1.2, 2.75),
    });

    QCOMPARE(sanitized.size(), 3);
    QCOMPARE(sanitized.at(0), QPointF(0.0, -0.75));
    QCOMPARE(sanitized.at(1), QPointF(0.5, 1.5));
    QCOMPARE(sanitized.at(2), QPointF(1.0, 2.0));
}

void TestGradingKeyframes::testBrightnessCurveLutDoesNotCollapseToWhite()
{
    const QVector<QPointF> adjustedCurve{
        QPointF(0.0, -0.25),
        QPointF(0.5, 0.55),
        QPointF(1.0, 1.25),
    };

    const QVector<quint8> lut = gradingCurveLut8(adjustedCurve, 16, false);

    QCOMPARE(lut.size(), 16);
    QCOMPARE(lut.constFirst(), static_cast<quint8>(0));
    QCOMPARE(lut.constLast(), static_cast<quint8>(255));
    QVERIFY2(std::any_of(lut.constBegin(), lut.constEnd(), [](quint8 value) {
                 return value > 0 && value < 255;
             }),
             "Brightness curve LUT collapsed to all black or all white instead of preserving the curve ramp.");
}

void TestGradingKeyframes::testVulkanBrightnessCurveLutMatchesCpuLut()
{
    TimelineClip::GradingKeyframe grade;
    grade.curvePointsR = defaultGradingCurvePoints();
    grade.curvePointsG = defaultGradingCurvePoints();
    grade.curvePointsB = defaultGradingCurvePoints();
    grade.curvePointsLuma = {
        QPointF(0.0, -0.25),
        QPointF(0.5, 0.55),
        QPointF(1.0, 1.25),
    };
    grade.curveSmoothingEnabled = false;

    const QVector<quint8> lumaLut =
        gradingCurveLut8(grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, false);
    const QByteArray vulkanLut = render_detail::vulkanCurveLutRgbaBytes(grade);

    QCOMPARE(vulkanLut.size(), TimelineClip::kGradingCurveLutSize * 4);
    for (int i = 0; i < TimelineClip::kGradingCurveLutSize; ++i) {
        QCOMPARE(static_cast<quint8>(vulkanLut.at((i * 4) + 3)), lumaLut.at(i));
    }
    QVERIFY2(std::any_of(lumaLut.constBegin(), lumaLut.constEnd(), [](quint8 value) {
                 return value > 0 && value < 255;
             }),
             "Vulkan brightness/Luma LUT alpha channel collapsed instead of matching CPU grading LUT.");
}

void TestGradingKeyframes::testVulkanDrawStateDoesNotEnableCurveByDefault()
{
    const TimelineClip::GradingKeyframe grade;
    const render_detail::VulkanDrawEffectState state =
        render_detail::vulkanDrawEffectStateForGrade(grade);

    QCOMPARE(state.shadows[3], render_detail::kVulkanEffectModeNormal);
}

QTEST_MAIN(TestGradingKeyframes)
#include "test_grading_keyframes.moc"
