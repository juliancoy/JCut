#include <QtTest/QtTest>

#include <QFile>

#include "../editor_grading_core.h"
#include "../editor_runtime.h"
#include "../editor_shared.h"
#include "../render_vulkan_shared.h"

#include <array>
#include <limits>

namespace {

std::vector<jcut::EditorPoint> editorCurveFromQt(const QVector<QPointF>& points)
{
    std::vector<jcut::EditorPoint> result;
    result.reserve(static_cast<std::size_t>(points.size()));
    for (const QPointF& point : points) {
        result.push_back({point.x(), point.y()});
    }
    return result;
}

QVector<QPointF> qtCurveFromEditor(const std::vector<jcut::EditorPoint>& points)
{
    QVector<QPointF> result;
    result.reserve(static_cast<qsizetype>(points.size()));
    for (const jcut::EditorPoint& point : points) {
        result.push_back(QPointF(point.x, point.y));
    }
    return result;
}

jcut::EditorGradingKeyframe editorGradingKeyframeFromQt(
    const TimelineClip::GradingKeyframe& keyframe)
{
    jcut::EditorGradingKeyframe result;
    result.frame = keyframe.frame;
    result.brightness = keyframe.brightness;
    result.contrast = keyframe.contrast;
    result.saturation = keyframe.saturation;
    result.opacity = keyframe.opacity;
    result.linearInterpolation = keyframe.linearInterpolation;
    result.shadowsR = keyframe.shadowsR;
    result.shadowsG = keyframe.shadowsG;
    result.shadowsB = keyframe.shadowsB;
    result.midtonesR = keyframe.midtonesR;
    result.midtonesG = keyframe.midtonesG;
    result.midtonesB = keyframe.midtonesB;
    result.highlightsR = keyframe.highlightsR;
    result.highlightsG = keyframe.highlightsG;
    result.highlightsB = keyframe.highlightsB;
    result.curvePointsR = editorCurveFromQt(keyframe.curvePointsR);
    result.curvePointsG = editorCurveFromQt(keyframe.curvePointsG);
    result.curvePointsB = editorCurveFromQt(keyframe.curvePointsB);
    result.curvePointsLuma = editorCurveFromQt(keyframe.curvePointsLuma);
    result.curveThreePointLock = keyframe.curveThreePointLock;
    result.curveSmoothingEnabled = keyframe.curveSmoothingEnabled;
    return result;
}

void compareEditorCurveWithQt(const std::vector<jcut::EditorPoint>& actual,
                              const QVector<QPointF>& expected)
{
    QCOMPARE(static_cast<int>(actual.size()), expected.size());
    for (int index = 0; index < expected.size(); ++index) {
        QVERIFY(std::abs(actual.at(static_cast<std::size_t>(index)).x -
                         expected.at(index).x()) < 0.000001);
        QVERIFY(std::abs(actual.at(static_cast<std::size_t>(index)).y -
                         expected.at(index).y()) < 0.000001);
    }
}

void compareEditorGradeWithQt(const jcut::EditorGradingKeyframe& actual,
                              const TimelineClip::GradingKeyframe& expected)
{
    const auto compareValue = [](double left, qreal right) {
        QVERIFY(std::abs(left - static_cast<double>(right)) < 0.000001);
    };

    compareValue(actual.brightness, expected.brightness);
    compareValue(actual.contrast, expected.contrast);
    compareValue(actual.saturation, expected.saturation);
    compareValue(actual.opacity, expected.opacity);
    compareValue(actual.shadowsR, expected.shadowsR);
    compareValue(actual.shadowsG, expected.shadowsG);
    compareValue(actual.shadowsB, expected.shadowsB);
    compareValue(actual.midtonesR, expected.midtonesR);
    compareValue(actual.midtonesG, expected.midtonesG);
    compareValue(actual.midtonesB, expected.midtonesB);
    compareValue(actual.highlightsR, expected.highlightsR);
    compareValue(actual.highlightsG, expected.highlightsG);
    compareValue(actual.highlightsB, expected.highlightsB);
    compareEditorCurveWithQt(actual.curvePointsR, expected.curvePointsR);
    compareEditorCurveWithQt(actual.curvePointsG, expected.curvePointsG);
    compareEditorCurveWithQt(actual.curvePointsB, expected.curvePointsB);
    compareEditorCurveWithQt(actual.curvePointsLuma, expected.curvePointsLuma);
    QCOMPARE(actual.curveThreePointLock, expected.curveThreePointLock);
    QCOMPARE(actual.curveSmoothingEnabled, expected.curveSmoothingEnabled);
    QCOMPARE(actual.linearInterpolation, expected.linearInterpolation);
}

} // namespace

class TestGradingKeyframes : public QObject {
    Q_OBJECT

private slots:
    void testNeutralEvaluatorMatchesQtGradeSemantics();
    void testCompatibleCurvesInterpolateLinearly();
    void testCurveOnlyOpacityCleanupKeyframeIsPreserved();
    void testIdentityCurvesDoNotActivateCurveLut();
    void testNonIdentityCurveActivatesCurveLut();
    void testCurveSanitizerPreservesGraphAdjustmentRange();
    void testNeutralCurveMathMatchesQtWrappers();
    void testNeutralCurveNormalizationMatchesQtWorkflow();
    void testBrightnessCurveLutDoesNotCollapseToWhite();
    void testVulkanBrightnessCurveLutMatchesCpuLut();
    void testVulkanDrawStateDoesNotEnableCurveByDefault();
    void testSpeakerGradeOverridesRatherThanCombinesWithClipGrade();
    void testGradingTabKeepsDisplayedFramesClipLocal();
};

void TestGradingKeyframes::testNeutralEvaluatorMatchesQtGradeSemantics()
{
    TimelineClip qtClip;
    qtClip.startFrame = 100;
    qtClip.durationFrames = 21;
    qtClip.opacity = 0.13;

    TimelineClip::GradingKeyframe first;
    first.frame = 0;
    first.brightness = -0.4;
    first.contrast = 0.8;
    first.saturation = 0.6;
    first.opacity = 0.91;
    first.shadowsR = -0.9;
    first.shadowsG = -0.7;
    first.shadowsB = -0.5;
    first.midtonesR = -0.3;
    first.midtonesG = -0.1;
    first.midtonesB = 0.1;
    first.highlightsR = 0.3;
    first.highlightsG = 0.5;
    first.highlightsB = 0.7;
    first.curvePointsR = {{0.0, 0.0}, {0.5, 0.2}, {1.0, 1.0}};
    first.curvePointsG = {{0.0, 0.1}, {0.5, 0.4}, {1.0, 0.9}};
    first.curvePointsB = {{0.0, 0.0}, {0.4, 0.3}, {1.0, 1.0}};
    first.curvePointsLuma = {{0.0, -0.2}, {1.0, 1.2}};
    first.curveThreePointLock = true;
    first.curveSmoothingEnabled = false;

    TimelineClip::GradingKeyframe second;
    second.frame = 10;
    second.brightness = 0.4;
    second.contrast = 1.6;
    second.saturation = 1.4;
    second.opacity = 0.92;
    second.shadowsR = 0.9;
    second.shadowsG = 0.7;
    second.shadowsB = 0.5;
    second.midtonesR = 0.3;
    second.midtonesG = 0.1;
    second.midtonesB = -0.1;
    second.highlightsR = -0.3;
    second.highlightsG = -0.5;
    second.highlightsB = -0.7;
    second.curvePointsR = {{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}};
    // Both a different point count and a different X topology must retain the
    // previous curve during interpolation, matching Qt's fallback behavior.
    second.curvePointsG = {{0.0, 0.0}, {1.0, 1.0}};
    second.curvePointsB = {{0.0, 0.0}, {0.6, 0.7}, {1.0, 1.0}};
    second.curvePointsLuma = {{0.0, 0.2}, {1.0, 0.8}};
    second.curveThreePointLock = false;
    second.curveSmoothingEnabled = true;
    second.linearInterpolation = true;

    TimelineClip::GradingKeyframe third = second;
    third.frame = 20;
    third.brightness = 0.9;
    third.shadowsR = 1.4;
    third.midtonesG = 1.2;
    third.highlightsB = -1.1;
    third.opacity = 0.93;
    third.curveThreePointLock = true;
    third.curveSmoothingEnabled = false;
    third.linearInterpolation = false;

    qtClip.gradingKeyframes = {first, second, third};
    qtClip.opacityKeyframes = {
        TimelineClip::OpacityKeyframe{0, 0.2, true},
        TimelineClip::OpacityKeyframe{10, 0.8, true},
        TimelineClip::OpacityKeyframe{20, 0.4, false},
    };

    jcut::EditorClip editorClip;
    editorClip.durationFrames = qtClip.durationFrames;
    editorClip.opacity = qtClip.opacity;
    editorClip.gradingKeyframes = {
        editorGradingKeyframeFromQt(first),
        editorGradingKeyframeFromQt(second),
        editorGradingKeyframeFromQt(third),
    };
    editorClip.opacityKeyframes = {
        jcut::EditorOpacityKeyframe{0, 0.2, true},
        jcut::EditorOpacityKeyframe{10, 0.8, true},
        jcut::EditorOpacityKeyframe{20, 0.4, false},
    };

    for (const int64_t localFrame : {int64_t{0}, int64_t{5}, int64_t{10},
                                     int64_t{15}, int64_t{20}}) {
        const TimelineClip::GradingKeyframe qtResult =
            evaluateClipGradingAtFrame(qtClip, qtClip.startFrame + localFrame);
        const jcut::EditorGradingKeyframe editorResult =
            jcut::evaluateEditorClipGradingAtLocalFrame(editorClip, localFrame);
        compareEditorGradeWithQt(editorResult, qtResult);

        if (localFrame == 15) {
            // Qt exposes the held source keyframe's frame, while the neutral
            // API consistently reports the queried local frame.
            QCOMPARE(qtResult.frame, int64_t{10});
            QCOMPARE(editorResult.frame, int64_t{15});
        } else {
            QCOMPARE(editorResult.frame, qtResult.frame);
        }
    }

    const jcut::EditorGradingKeyframe midpoint =
        jcut::evaluateEditorClipGradingAtLocalFrame(editorClip, 5);
    QVERIFY(std::abs(midpoint.curvePointsR.at(1).y - 0.5) < 0.000001);
    QCOMPARE(static_cast<int>(midpoint.curvePointsG.size()),
             first.curvePointsG.size());
    QVERIFY(std::abs(midpoint.curvePointsG.at(1).y - first.curvePointsG.at(1).y()) <
            0.000001);
    QCOMPARE(static_cast<int>(midpoint.curvePointsB.size()),
             first.curvePointsB.size());
    QVERIFY(std::abs(midpoint.curvePointsB.at(1).x - first.curvePointsB.at(1).x()) <
            0.000001);
    QVERIFY(midpoint.curveThreePointLock);
    QVERIFY(!midpoint.curveSmoothingEnabled);
    QVERIFY(std::abs(midpoint.opacity - 0.5) < 0.000001);
    QVERIFY(std::abs(midpoint.opacity - first.opacity) > 0.1);

    const jcut::EditorGradingKeyframe held =
        jcut::evaluateEditorClipGradingAtLocalFrame(editorClip, 15);
    QVERIFY(std::abs(held.brightness - second.brightness) < 0.000001);
    QVERIFY(std::abs(held.shadowsR - second.shadowsR) < 0.000001);
    QVERIFY(std::abs(held.midtonesG - second.midtonesG) < 0.000001);
    QVERIFY(std::abs(held.highlightsB - second.highlightsB) < 0.000001);
    QVERIFY(std::abs(held.opacity - 0.8) < 0.000001);
}

void TestGradingKeyframes::testGradingTabKeepsDisplayedFramesClipLocal()
{
    QFile source(QStringLiteral(JCUT_SOURCE_DIR "/grading_tab.cpp"));
    QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text),
             "grading_tab.cpp must be readable");
    const QByteArray contents = source.readAll();

    QVERIFY2(contents.contains("evaluateDisplayedGrading(*clip, displayedLocalFrame)"),
             "Grade refresh must evaluate a clip-local frame");
    QVERIFY2(contents.contains("evaluateDisplayedGrading(*clip, primaryFrame)"),
             "Grade table selection must evaluate the selected clip-local frame");
    QVERIFY2(!contents.contains("evaluateDisplayedGrading(*clip, clip->startFrame"),
             "Grade-tab callers must not add the clip start before the display helper adds it");
}

void TestGradingKeyframes::testSpeakerGradeOverridesRatherThanCombinesWithClipGrade()
{
    TimelineClip::GradingKeyframe master;
    master.frame = 17;
    master.brightness = 0.6;
    master.contrast = 1.8;
    master.saturation = 0.4;
    master.opacity = 0.75;
    master.linearInterpolation = false;
    master.curvePointsR = {{0.0, 0.2}, {1.0, 0.8}};
    master.curvePointsLuma = {{0.0, 0.1}, {1.0, 1.0}};

    TimelineClip::GradingKeyframe person;
    person.brightness = -0.2;
    person.contrast = 1.1;
    person.saturation = 1.3;

    const TimelineClip::GradingKeyframe result =
        gradingWithSpeakerOverride(master, person);
    QCOMPARE(result.brightness, person.brightness);
    QCOMPARE(result.contrast, person.contrast);
    QCOMPARE(result.saturation, person.saturation);
    QVERIFY(!gradingUsesCurveLut(result));
    QCOMPARE(result.opacity, master.opacity);
    QCOMPARE(result.frame, master.frame);
    QCOMPARE(result.linearInterpolation, master.linearInterpolation);
}

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

void TestGradingKeyframes::testNeutralCurveMathMatchesQtWrappers()
{
    const QVector<QPointF> qtCurve{
        QPointF(1.2, 1.4),
        QPointF(0.3, 0.75),
        QPointF(0.3000005, 0.2),
        QPointF(-0.2, -0.4),
    };
    const std::vector<jcut::EditorPoint> editorCurve =
        editorCurveFromQt(qtCurve);
    const std::vector<jcut::EditorPoint> sanitizedEditorCurve =
        jcut::sanitizeEditorGradingCurve(editorCurve);
    compareEditorCurveWithQt(
        sanitizedEditorCurve,
        sanitizeGradingCurvePoints(qtCurve));
    QCOMPARE(sanitizedEditorCurve.size(), std::size_t(3));
    QCOMPARE(sanitizedEditorCurve[0].x, 0.0);
    QCOMPARE(sanitizedEditorCurve[0].y, -0.4);
    QCOMPARE(sanitizedEditorCurve[1].x, 0.3);
    QCOMPARE(sanitizedEditorCurve[1].y, 0.2);
    QCOMPARE(sanitizedEditorCurve[2].x, 1.0);
    QCOMPARE(sanitizedEditorCurve[2].y, 1.4);

    const std::array<std::array<quint8, 16>, 2> goldenLuts{{
        {{0, 0, 0, 0, 34, 66, 95, 124, 153, 182, 211, 240,
          255, 255, 255, 255}},
        {{0, 0, 0, 0, 26, 63, 89, 120, 153, 187, 222, 255,
          255, 255, 255, 255}},
    }};

    for (const bool smoothingEnabled : {false, true}) {
        for (const int samples : {2, 16, jcut::kEditorGradingCurveLutSize}) {
            const QVector<quint8> qtLut =
                gradingCurveLut8(qtCurve, samples, smoothingEnabled);
            const std::vector<std::uint8_t> editorLut =
                jcut::editorGradingCurveLut8(
                    editorCurve, samples, smoothingEnabled);
            QCOMPARE(qtLut.size(), static_cast<qsizetype>(editorLut.size()));
            for (qsizetype index = 0; index < qtLut.size(); ++index) {
                QCOMPARE(
                    qtLut.at(index),
                    static_cast<quint8>(editorLut.at(
                        static_cast<std::size_t>(index))));
            }
            if (samples == 16) {
                const auto& golden = goldenLuts[smoothingEnabled ? 1 : 0];
                for (std::size_t index = 0; index < golden.size(); ++index) {
                    QCOMPARE(editorLut[index], golden[index]);
                }
            }
        }
        for (const double x : {-0.2, 0.0, 0.125, 0.5, 0.875, 1.0, 1.2}) {
            QVERIFY(std::abs(
                        sampleGradingCurveAt(qtCurve, x, smoothingEnabled) -
                        jcut::sampleEditorGradingCurveAt(
                            editorCurve, x, smoothingEnabled)) < 0.000001);
        }
    }

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<jcut::EditorPoint> nanCurve =
        jcut::sanitizeEditorGradingCurve({{nan, nan}});
    QCOMPARE(nanCurve.size(), std::size_t(2));
    QCOMPARE(nanCurve[0].x, 0.0);
    QCOMPARE(nanCurve[0].y, -1.0);
    QCOMPARE(nanCurve[1].x, 1.0);
    QCOMPARE(nanCurve[1].y, -1.0);
    QCOMPARE(jcut::sampleEditorGradingCurveAt(
                 {{0.0, 0.0}, {1.0, 1.0}}, nan, false),
             0.0);
    const std::vector<std::uint8_t> nanLut =
        jcut::editorGradingCurveLut8({{nan, nan}}, 16, true);
    QVERIFY(std::all_of(nanLut.begin(), nanLut.end(),
                        [](std::uint8_t value) { return value == 0; }));
}

void TestGradingKeyframes::testNeutralCurveNormalizationMatchesQtWorkflow()
{
    jcut::EditorGradingKeyframe grade;
    grade.frame = 17;
    grade.brightness = -0.3;
    grade.shadowsR = 0.4;
    grade.linearInterpolation = false;
    grade.curvePointsR = {{0.0, 0.0}, {1.0, 1.0}};
    grade.curvePointsG = {{0.0, 0.0}, {1.0, 1.0}};
    grade.curvePointsB = {{0.0, 0.0}, {1.0, 1.0}};
    grade.curvePointsLuma = {{0.0, 0.1}, {1.0, 1.0}};
    grade.curveThreePointLock = true;
    grade.curveSmoothingEnabled = false;
    const jcut::EditorGradingKeyframe original = grade;

    const QVector<quint8> qtChannelLut = gradingCurveLut8(
        qtCurveFromEditor(original.curvePointsR),
        TimelineClip::kGradingCurveLutSize,
        original.curveSmoothingEnabled);
    const QVector<quint8> qtLumaLut = gradingCurveLut8(
        qtCurveFromEditor(original.curvePointsLuma),
        TimelineClip::kGradingCurveLutSize,
        original.curveSmoothingEnabled);
    jcut::normalizeEditorGradingCurves(grade);

    QCOMPARE(grade.frame, original.frame);
    QCOMPARE(grade.brightness, original.brightness);
    QCOMPARE(grade.shadowsR, original.shadowsR);
    QCOMPARE(grade.linearInterpolation, original.linearInterpolation);
    QVERIFY(!grade.curveThreePointLock);
    QVERIFY(!grade.curveSmoothingEnabled);
    QCOMPARE(grade.curvePointsLuma.size(), std::size_t(2));
    QCOMPARE(grade.curvePointsLuma.front().x, 0.0);
    QCOMPARE(grade.curvePointsLuma.front().y, 0.0);
    QCOMPARE(grade.curvePointsLuma.back().x, 1.0);
    QCOMPARE(grade.curvePointsLuma.back().y, 1.0);
    QCOMPARE(grade.curvePointsR.size(), std::size_t(2));
    QCOMPARE(grade.curvePointsR.front().x, 0.0);
    QCOMPARE(grade.curvePointsR.front().y, 26.0 / 255.0);
    QCOMPARE(grade.curvePointsR.back().x, 1.0);
    QCOMPARE(grade.curvePointsR.back().y, 1.0);
    QVERIFY(grade.curvePointsG.size() <= 12);
    QVERIFY(grade.curvePointsB.size() <= 12);

    const QVector<quint8> normalizedQtLut = gradingCurveLut8(
        qtCurveFromEditor(grade.curvePointsR),
        TimelineClip::kGradingCurveLutSize,
        false);
    int maximumError = 0;
    for (int index = 0; index < TimelineClip::kGradingCurveLutSize; ++index) {
        const quint8 expected = qtLumaLut.at(qtChannelLut.at(index));
        maximumError = qMax(
            maximumError,
            std::abs(static_cast<int>(expected) -
                     static_cast<int>(normalizedQtLut.at(index))));
    }
    QVERIFY(maximumError <= 1);
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
