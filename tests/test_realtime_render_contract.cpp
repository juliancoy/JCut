#include <QtTest/QtTest>

#include "../editor_shared_keyframes.h"
#include "../editor_shared_render_sync.h"
#include "../timeline_fps.h"

#include <QFile>
#include <cmath>

namespace {

QVector<qreal> outputTimelinePositions(int64_t startFrame, int64_t endFrame, qreal outputFps)
{
    const qreal safeOutputFps = qMax<qreal>(1.0, outputFps);
    const qreal timelineFramesPerOutputFrame =
        static_cast<qreal>(kTimelineFps) / safeOutputFps;
    const qreal durationSeconds =
        static_cast<qreal>(endFrame - startFrame + 1) / static_cast<qreal>(kTimelineFps);
    const int64_t outputFrameCount =
        qMax<int64_t>(1, static_cast<int64_t>(std::ceil(durationSeconds * safeOutputFps)));

    QVector<qreal> positions;
    positions.reserve(static_cast<int>(outputFrameCount));
    for (int64_t outputFrame = 0; outputFrame < outputFrameCount; ++outputFrame) {
        positions.push_back(qMin<qreal>(
            static_cast<qreal>(endFrame),
            static_cast<qreal>(startFrame) +
                (static_cast<qreal>(outputFrame) * timelineFramesPerOutputFrame)));
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
    void fractionalSourceMappingDoesNotDuplicateThirtyFpsFrames();
    void renderTransformsInterpolateAtOutputFpsPositions();
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
        TimelineClip::TransformKeyframe{0, 0.0, 0.0, 0.0, 1.0, 1.0, true},
        TimelineClip::TransformKeyframe{1, 10.0, 20.0, 0.0, 1.0, 1.0, true},
    };

    const TimelineClip::TransformKeyframe atHalfFrame =
        evaluateClipRenderTransformAtPosition(clip, 0.5, QSize(1920, 1080));

    QVERIFY2(std::abs(atHalfFrame.translationX - 5.0) < 0.000001,
             "render transforms must evaluate at fractional output-frame positions");
    QVERIFY2(std::abs(atHalfFrame.translationY - 10.0) < 0.000001,
             "fractional render transform evaluation prevents visible half-frame stepping");
}

void TestRealtimeRenderContract::exportLoopPassesFractionalPositionToRenderer()
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QStringLiteral("/render_export.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "render_export.cpp must be readable");
    const QString source = QString::fromUtf8(file.readAll());

    QVERIFY2(source.contains(QStringLiteral("const qreal timelineFramePosition")),
             "export must derive a fractional timeline position from output PTS");
    QVERIFY2(source.contains(QStringLiteral("activeRenderer->renderFrameToOutput(request,\n                                                      timelineFramePosition")),
             "GPU export must render the fractional timeline position, not the floored edit frame");
    QVERIFY2(source.contains(QStringLiteral("renderTimelineFrameToOutput(request,\n                                              timelineFramePosition")),
             "CPU export must render the fractional timeline position, not the floored edit frame");
}

QTEST_MAIN(TestRealtimeRenderContract)
#include "test_realtime_render_contract.moc"
