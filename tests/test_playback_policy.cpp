#include <QtTest/QtTest>

#include "../editor_shared.h"
#include "../playback_clock_coordinator.h"

#include <QFile>

namespace {

QString readSourceFile(const QString& relativePath)
{
    QFile file(QStringLiteral(JCUT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

}  // namespace

class TestPlaybackPolicy : public QObject {
    Q_OBJECT

private slots:
    void testClockSourceRoundTrip();
    void testAudioWarpRoundTrip();
    void testNormalizedAudioWarpMode();
    void testEffectiveWarpRate();
    void testSystemClockSourcePolicy();
    void testPitchPreservingAudioGatePolicy();
    void testPlaybackClockCoordinator();
    void testPlaybackDriftRetimeController();
    void testAudioFeedbackSampleStaysAbsoluteAcrossSpeechRanges();
    void testSystemClockDecisionCarriesTransportSample();
    void testPlayableSampleAtOrAfterAcrossSpeechRanges();
    void testActivePlaybackRuntimeConfigRealignsStreams();
};

void TestPlaybackPolicy::testClockSourceRoundTrip() {
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Auto)),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Audio)),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Timeline)),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(QStringLiteral("audio")),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(QStringLiteral("timeline")),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(QStringLiteral("unknown")),
             PlaybackClockSource::Auto);
}

void TestPlaybackPolicy::testAudioWarpRoundTrip() {
    QCOMPARE(playbackAudioWarpModeFromString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::Disabled)),
             PlaybackAudioWarpMode::Disabled);
    QCOMPARE(playbackAudioWarpModeFromString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::Varispeed)),
             PlaybackAudioWarpMode::Varispeed);
    QCOMPARE(playbackAudioWarpModeFromString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::TimeStretch)),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("time-stretch")),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("invalid")),
             PlaybackAudioWarpMode::Disabled);
}

void TestPlaybackPolicy::testEffectiveWarpRate() {
    QCOMPARE(effectivePlaybackAudioWarpRate(2.0, PlaybackAudioWarpMode::Disabled), 2.0);
    QCOMPARE(effectivePlaybackAudioWarpRate(0.05, PlaybackAudioWarpMode::Varispeed), 0.1);
    QCOMPARE(effectivePlaybackAudioWarpRate(4.0, PlaybackAudioWarpMode::TimeStretch), 3.0);
    QCOMPARE(effectivePlaybackAudioWarpRate(1.5, PlaybackAudioWarpMode::Varispeed), 1.5);
}

void TestPlaybackPolicy::testNormalizedAudioWarpMode() {
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.0, PlaybackAudioWarpMode::Disabled),
             PlaybackAudioWarpMode::Disabled);
    QCOMPARE(normalizedPlaybackAudioWarpMode(2.0, PlaybackAudioWarpMode::Disabled),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(normalizedPlaybackAudioWarpMode(3.0, PlaybackAudioWarpMode::Disabled),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.5, PlaybackAudioWarpMode::Disabled),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(normalizedPlaybackAudioWarpMode(2.0, PlaybackAudioWarpMode::TimeStretch),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.5, PlaybackAudioWarpMode::TimeStretch),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.5, PlaybackAudioWarpMode::Varispeed),
             PlaybackAudioWarpMode::TimeStretch);
}

void TestPlaybackPolicy::testSystemClockSourcePolicy() {
    QCOMPARE(playbackClockSourceToString(PlaybackClockSource::Auto), QStringLiteral("auto"));
    QCOMPARE(playbackClockSourceToString(PlaybackClockSource::Audio), QStringLiteral("auto"));
    QCOMPARE(playbackClockSourceToString(PlaybackClockSource::Timeline), QStringLiteral("auto"));
    QCOMPARE(playbackClockSourceLabel(PlaybackClockSource::Auto), QStringLiteral("System Clock"));
    QCOMPARE(playbackClockSourceLabel(PlaybackClockSource::Audio), QStringLiteral("System Clock"));
    QCOMPARE(playbackClockSourceLabel(PlaybackClockSource::Timeline), QStringLiteral("System Clock"));
}

void TestPlaybackPolicy::testPitchPreservingAudioGatePolicy() {
    QVERIFY(!pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::TimeStretch,
                                                     1.0,
                                                     true));
    QVERIFY(!pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::TimeStretch,
                                                     1.5,
                                                     false));
    QVERIFY(pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::Disabled,
                                                    1.5,
                                                    true));
    QVERIFY(pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::TimeStretch,
                                                    1.5,
                                                    true));

}

void TestPlaybackPolicy::testPlaybackClockCoordinator() {
    editor::PlaybackClockInput input;
    input.transportSample = frameToSamples(120);
    input.totalFrames = 1000;
    editor::PlaybackClockDecision decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTransportSample);
    QCOMPARE(decision.reason, QStringLiteral("system_clock_transport"));
    QCOMPARE(decision.sample, input.transportSample);
    QCOMPARE(decision.frame, static_cast<int64_t>(120));
    QVERIFY(!decision.resetTimerContinuity);

    input.transportSample = frameToSamples(140) + 17;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTransportSample);
    QCOMPARE(decision.reason, QStringLiteral("system_clock_transport"));
    QCOMPARE(decision.sample, input.transportSample);
}

void TestPlaybackPolicy::testPlaybackDriftRetimeController() {
    editor::PlaybackDriftRetimeInput input;
    input.enabled = true;
    input.previousMultiplier = 1.0;
    input.deadbandSamples = 3840.0;
    input.fullCorrectionSamples = 60000.0;
    input.maxCorrection = 0.01;
    input.smoothing = 1.0;

    input.driftSamples = 2400;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.0);

    input.driftSamples = 60000;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.01);

    input.driftSamples = -60000;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 0.99);

    input.previousMultiplier = 1.0;
    input.driftSamples = 60000;
    input.smoothing = 0.25;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.0025);

    input.enabled = false;
    input.previousMultiplier = 1.01;
    input.smoothing = 0.5;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.005);
}

void TestPlaybackPolicy::testAudioFeedbackSampleStaysAbsoluteAcrossSpeechRanges() {
    const QVector<ExportRangeSegment> ranges{
        ExportRangeSegment{100, 109},
        ExportRangeSegment{200, 209},
    };

    const int64_t audibleAbsoluteSample = frameToSamples(205);
    QCOMPARE(editor::audioFeedbackSampleToTimelineSample(audibleAbsoluteSample),
             audibleAbsoluteSample);

    const int64_t skippedGapSample = frameToSamples(150);
    bool atOrPastEnd = false;
    QCOMPARE(playableSampleAtOrAfter(
                 editor::audioFeedbackSampleToTimelineSample(skippedGapSample),
                 ranges,
                 &atOrPastEnd),
             frameToSamples(200));
    QVERIFY(!atOrPastEnd);
}

void TestPlaybackPolicy::testSystemClockDecisionCarriesTransportSample() {
    editor::PlaybackClockInput input;
    input.transportSample = frameToSamples(120);
    input.totalFrames = 1000;

    const editor::PlaybackClockDecision decision =
        editor::evaluatePlaybackClock(input);

    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTransportSample);
    QCOMPARE(decision.sample, input.transportSample);
    QCOMPARE(decision.frame, static_cast<int64_t>(120));
}

void TestPlaybackPolicy::testPlayableSampleAtOrAfterAcrossSpeechRanges() {
    const QVector<ExportRangeSegment> ranges{
        ExportRangeSegment{10, 19},
        ExportRangeSegment{30, 39},
        ExportRangeSegment{50, 59},
    };

    bool atOrPastEnd = false;
    QCOMPARE(playableSampleAtOrAfter(frameToSamples(5), ranges, &atOrPastEnd),
             frameToSamples(10));
    QVERIFY(!atOrPastEnd);

    QCOMPARE(playableSampleAtOrAfter(frameToSamples(12), ranges, &atOrPastEnd),
             frameToSamples(12));
    QVERIFY(!atOrPastEnd);

    QCOMPARE(playableSampleAtOrAfter(frameToSamples(25), ranges, &atOrPastEnd),
             frameToSamples(30));
    QVERIFY(!atOrPastEnd);

    const int64_t lastPlayableSample = frameToSamples(60) - 1;
    QCOMPARE(playableSampleAtOrAfter(frameToSamples(65), ranges, &atOrPastEnd),
             lastPlayableSample);
    QVERIFY(atOrPastEnd);
}

void TestPlaybackPolicy::testActivePlaybackRuntimeConfigRealignsStreams()
{
    const QString playback = readSourceFile(QStringLiteral("editor_playback.cpp"));
    QVERIFY2(!playback.isEmpty(), "editor_playback.cpp must be readable");

    QVERIFY2(playback.contains(QStringLiteral("activePlaybackReconfigured")),
             "active runtime playback changes must be explicitly detected");
    QVERIFY2(playback.contains(QStringLiteral("m_lastTimelineAdvanceTickMs = nowMs()")) &&
                 playback.contains(QStringLiteral("m_timelineAdvanceCarrySamples = 0.0")),
             "active clock/speed changes must reset transport timer continuity");
    QVERIFY2(playback.contains(QStringLiteral(
                 "reconcileActivePlaybackAudioState(activePlaybackReconfigured)")),
             "active runtime changes must request stream realignment");
    QVERIFY2(playback.contains(QStringLiteral("m_audioEngine->seek(currentFrame)")),
             "already-running audio must seek to the transport playhead when "
             "runtime playback policy changes");
    QVERIFY2(playback.contains(QStringLiteral("const int64_t playbackStartFrame")) &&
                 playback.contains(QStringLiteral("m_audioEngine->start(playbackStartFrame)")) &&
                 playback.contains(QStringLiteral("m_audioEngine->warmPlaybackAudio(\n                    playbackStartFrame")),
             "playback start must use one transport-derived start frame for audio "
             "warmup and stream start");
    QVERIFY2(playback.contains(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) &&
                 playback.indexOf(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) <
                     playback.indexOf(QStringLiteral("const int64_t playbackStartFrame")) &&
                 playback.indexOf(QStringLiteral("const int64_t playbackStartFrame")) <
                     playback.indexOf(QStringLiteral("m_audioEngine->start(playbackStartFrame)")),
             "playback start must resolve export/speech ranges before deriving "
             "the shared audio/video start frame");
    const QString removedSelectableClockHook =
        QStringLiteral("shouldUse") + QStringLiteral("Audio") +
        QStringLiteral("Master") + QStringLiteral("Clock");
    QVERIFY2(!playback.contains(removedSelectableClockHook),
             "runtime playback must not retain a selectable audio clock hook");
    QVERIFY2(playback.contains(QStringLiteral("updateAudioDriftRetime()")) &&
                 playback.contains(QStringLiteral("setPlaybackDriftRetimeRate")),
             "transport-clock playback must smoothly retime follower audio "
             "instead of only seeking on drift");
    QVERIFY2(!playback.contains(QStringLiteral("playbackActive() && ") + removedSelectableClockHook + QStringLiteral("()")),
             "retimed-audio warmup must not stop active system-clock transport "
             "playback");
    QVERIFY2(playback.contains(QStringLiteral("requestPlaybackAudioWarmup(false)")) &&
                 playback.contains(QStringLiteral("continuing transport playback while re-timed audio warms")) &&
                 playback.contains(QStringLiteral("reconcileActivePlaybackAudioState(true)")),
             "missing pitch-preserving audio at playback start must warm in "
             "the background and rejoin active playback when ready");
    QVERIFY2(!playback.contains(QStringLiteral("PlaybackClockInput{")),
             "PlaybackClockInput must use named field assignment so adding "
             "clock fields cannot silently shift audio/current-frame values");
}

QTEST_MAIN(TestPlaybackPolicy)
#include "test_playback_policy.moc"
