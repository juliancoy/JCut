#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace {

struct StageBudget {
    QString name;
    qreal ms = 0.0;
};

struct FrameLossSample {
    qreal fps = 0.0;
    qreal frameIntervalMs = 0.0;
    int renderedFrames = 0;
    int droppedFrames = 0;
    qreal maxLatenessMs = 0.0;
    QString reason;
};

struct FrameLossProbeResult {
    QVector<FrameLossSample> samples;
    int firstLossIndex = -1;
};

qreal totalStageMs(const QVector<StageBudget>& stages)
{
    qreal total = 0.0;
    for (const StageBudget& stage : stages) {
        total += qMax<qreal>(0.0, stage.ms);
    }
    return total;
}

QString dominantStageReason(const QVector<StageBudget>& stages,
                            qreal frameIntervalMs,
                            qreal renderStartLagMs)
{
    StageBudget dominant;
    for (const StageBudget& stage : stages) {
        if (stage.ms > dominant.ms) {
            dominant = stage;
        }
    }

    const qreal total = totalStageMs(stages);
    if (total > frameIntervalMs) {
        return QStringLiteral("budget:%1_stage_dominates total_ms=%2 interval_ms=%3")
            .arg(dominant.name)
            .arg(total, 0, 'f', 3)
            .arg(frameIntervalMs, 0, 'f', 3);
    }
    if (renderStartLagMs > 0.001) {
        return QStringLiteral("backlog:previous_frame_missed_deadline");
    }
    return QStringLiteral("none");
}

FrameLossSample simulateRealtimePlaybackAtFps(qreal fps,
                                              const QVector<StageBudget>& stages,
                                              int frameCount = 180,
                                              int warmupFrames = 5)
{
    const qreal safeFps = qMax<qreal>(1.0, fps);
    const qreal intervalMs = 1000.0 / safeFps;
    const qreal frameWorkMs = totalStageMs(stages);
    qreal renderLaneAvailableMs = 0.0;

    FrameLossSample sample;
    sample.fps = safeFps;
    sample.frameIntervalMs = intervalMs;
    sample.renderedFrames = frameCount;

    for (int frame = 0; frame < frameCount; ++frame) {
        const qreal arrivalMs = static_cast<qreal>(frame) * intervalMs;
        const qreal startMs = qMax(arrivalMs, renderLaneAvailableMs);
        const qreal finishMs = startMs + frameWorkMs;
        const qreal deadlineMs = arrivalMs + intervalMs;
        renderLaneAvailableMs = finishMs;

        if (frame < warmupFrames) {
            continue;
        }

        const qreal latenessMs = finishMs - deadlineMs;
        if (latenessMs > 0.001) {
            ++sample.droppedFrames;
            sample.maxLatenessMs = qMax(sample.maxLatenessMs, latenessMs);
            if (sample.reason.isEmpty()) {
                sample.reason = dominantStageReason(stages, intervalMs, startMs - arrivalMs);
            }
        }
    }

    if (sample.reason.isEmpty()) {
        sample.reason = QStringLiteral("none");
    }
    return sample;
}

FrameLossProbeResult sweepFrameRates(const QVector<qreal>& fpsValues,
                                     const QVector<StageBudget>& stages)
{
    FrameLossProbeResult result;
    result.samples.reserve(fpsValues.size());
    for (const qreal fps : fpsValues) {
        FrameLossSample sample = simulateRealtimePlaybackAtFps(fps, stages);
        if (result.firstLossIndex < 0 && sample.droppedFrames > 0) {
            result.firstLossIndex = result.samples.size();
        }
        result.samples.push_back(sample);
    }
    return result;
}

QJsonObject sampleToJson(const FrameLossSample& sample)
{
    return QJsonObject{
        {QStringLiteral("fps"), sample.fps},
        {QStringLiteral("frame_interval_ms"), sample.frameIntervalMs},
        {QStringLiteral("rendered_frames"), sample.renderedFrames},
        {QStringLiteral("dropped_frames"), sample.droppedFrames},
        {QStringLiteral("max_lateness_ms"), sample.maxLatenessMs},
        {QStringLiteral("reason"), sample.reason},
    };
}

void logProbeResult(const QString& label, const FrameLossProbeResult& result)
{
    QJsonArray samples;
    for (const FrameLossSample& sample : result.samples) {
        samples.push_back(sampleToJson(sample));
    }
    QJsonObject root{
        {QStringLiteral("label"), label},
        {QStringLiteral("first_loss_index"), result.firstLossIndex},
        {QStringLiteral("samples"), samples},
    };
    if (result.firstLossIndex >= 0) {
        const FrameLossSample& firstLoss = result.samples.at(result.firstLossIndex);
        root.insert(QStringLiteral("first_lost_fps"), firstLoss.fps);
        root.insert(QStringLiteral("first_loss_reason"), firstLoss.reason);
    }
    qInfo().noquote() << QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class TestRealtimeFrameLossProbe : public QObject {
    Q_OBJECT

private slots:
    void findsFirstLostFrameRateForBalancedRenderWork();
    void explainsDecodeBoundLoss();
};

void TestRealtimeFrameLossProbe::findsFirstLostFrameRateForBalancedRenderWork()
{
    const QVector<qreal> fpsValues{24.0, 30.0, 48.0, 60.0, 72.0, 90.0, 96.0, 120.0, 144.0};
    const QVector<StageBudget> stages{
        {QStringLiteral("decode"), 3.0},
        {QStringLiteral("upload"), 2.0},
        {QStringLiteral("composite"), 5.0},
        {QStringLiteral("present"), 1.0},
    };

    const FrameLossProbeResult result = sweepFrameRates(fpsValues, stages);
    logProbeResult(QStringLiteral("balanced_render_work"), result);

    QVERIFY2(result.firstLossIndex >= 0, "probe must identify the first losing FPS");
    QCOMPARE(result.samples.at(result.firstLossIndex).fps, 96.0);
    QVERIFY2(result.samples.at(result.firstLossIndex - 1).droppedFrames == 0,
             "the FPS before the threshold should remain clean");
    QVERIFY2(result.samples.at(result.firstLossIndex).reason.contains(QStringLiteral("composite")),
             "the reported reason should name the dominant render stage");
}

void TestRealtimeFrameLossProbe::explainsDecodeBoundLoss()
{
    const QVector<qreal> fpsValues{24.0, 30.0, 48.0, 60.0, 72.0, 90.0};
    const QVector<StageBudget> stages{
        {QStringLiteral("decode"), 14.0},
        {QStringLiteral("upload"), 1.5},
        {QStringLiteral("composite"), 2.0},
        {QStringLiteral("present"), 1.0},
    };

    const FrameLossProbeResult result = sweepFrameRates(fpsValues, stages);
    logProbeResult(QStringLiteral("decode_bound_work"), result);

    QVERIFY2(result.firstLossIndex >= 0, "decode-bound work should have a measurable loss threshold");
    QCOMPARE(result.samples.at(result.firstLossIndex).fps, 60.0);
    QVERIFY2(result.samples.at(result.firstLossIndex).reason.contains(QStringLiteral("decode")),
             "the reported reason should distinguish decode-bound loss from presenter loss");
}

QTEST_MAIN(TestRealtimeFrameLossProbe)
#include "test_realtime_frame_loss_probe.moc"
