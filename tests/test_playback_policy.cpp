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
    void testAudioMasterClockPolicy();
    void testPitchPreservingAudioGatePolicy();
    void testPlaybackClockCoordinator();
    void testPlaybackDriftRetimeController();
    void testAudioMasterClockSampleStaysAbsoluteAcrossSpeechRanges();
    void testAudioMasterSameFrameCarriesCanonicalSample();
    void testPlayableSampleAtOrAfterAcrossSpeechRanges();
    void testActivePlaybackRuntimeConfigRealignsStreams();
};

void TestPlaybackPolicy::testClockSourceRoundTrip() {
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Auto)),
             PlaybackClockSource::Auto);
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Audio)),
             PlaybackClockSource::Audio);
    QCOMPARE(playbackClockSourceFromString(playbackClockSourceToString(PlaybackClockSource::Timeline)),
             PlaybackClockSource::Timeline);
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

void TestPlaybackPolicy::testAudioMasterClockPolicy() {
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Auto,
                                       PlaybackAudioWarpMode::Varispeed,
                                       1.0,
                                       false));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Auto,
                                       PlaybackAudioWarpMode::Disabled,
                                       1.0,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Auto,
                                       PlaybackAudioWarpMode::Disabled,
                                       1.5,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Audio,
                                       PlaybackAudioWarpMode::Varispeed,
                                       2.0,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Audio,
                                       PlaybackAudioWarpMode::TimeStretch,
                                       1.0,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Timeline,
                                       PlaybackAudioWarpMode::Varispeed,
                                       1.0,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Timeline,
                                       PlaybackAudioWarpMode::TimeStretch,
                                       1.0,
                                       true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Timeline,
                                       PlaybackAudioWarpMode::TimeStretch,
                                       1.5,
                                       true));
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

    QVERIFY(!shouldHoldForPitchPreservingAudio(PlaybackAudioWarpMode::TimeStretch,
                                               1.5,
                                               true,
                                               false,
                                               true));
    QVERIFY(shouldHoldForPitchPreservingAudio(PlaybackAudioWarpMode::TimeStretch,
                                              1.5,
                                              true,
                                              true,
                                              true));
    QVERIFY(shouldHoldForPitchPreservingAudio(PlaybackAudioWarpMode::TimeStretch,
                                              1.5,
                                              true,
                                              false,
                                              false));
    QVERIFY(!shouldHoldForPitchPreservingAudio(PlaybackAudioWarpMode::TimeStretch,
                                               1.0,
                                               true,
                                               true,
                                               false));
}

void TestPlaybackPolicy::testPlaybackClockCoordinator() {
    editor::PlaybackClockInput input;
    input.pitchPreservingAudioRequired = true;
    input.audioMasterEnabled = true;
    input.audioClockAvailable = true;
    input.hasPlayableAudio = true;
    input.audioBlocked = true;
    input.audioReady = true;
    input.transportSample = frameToSamples(120);
    input.audioSample = frameToSamples(100);
    input.currentFrame = 120;
    input.totalFrames = 1000;
    input.audioClockStallTicks = 0;
    input.audioClockStallThresholdTicks = 2;
    editor::PlaybackClockDecision decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);
    QCOMPARE(decision.reason, QStringLiteral("audio_blocked"));
    QVERIFY(decision.resetTimerContinuity);

    input.audioBlocked = false;
    input.audioReady = false;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);
    QCOMPARE(decision.reason, QStringLiteral("retimed_audio_not_ready"));

    input.audioReady = true;
    input.audioSample = frameToSamples(80);
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);
    QCOMPARE(decision.reason, QStringLiteral("audio_clock_regressed"));

    input.pitchPreservingAudioRequired = false;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseAudioSample);
    QCOMPARE(decision.reason, QStringLiteral("audio_master_resync"));
    QVERIFY(decision.resetTimerContinuity);

    input.pitchPreservingAudioRequired = true;
    input.audioSample = frameToSamples(120);
    input.audioClockStallTicks = 1;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::WaitForAudioClock);

    input.audioClockStallTicks = 3;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::WaitForAudioClock);
    QCOMPARE(decision.reason, QStringLiteral("audio_clock_stalled"));

    input.audioSample = frameToSamples(121);
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseAudioSample);

    input.audioMasterEnabled = false;
    input.audioClockAvailable = true;
    input.hasPlayableAudio = true;
    input.transportSample = frameToSamples(140) + 17;
    input.audioSample = frameToSamples(10);
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTransportSample);
    QCOMPARE(decision.reason, QStringLiteral("monotonic_transport"));
    QCOMPARE(decision.sample, input.transportSample);
}

void TestPlaybackPolicy::testPlaybackDriftRetimeController() {
    editor::PlaybackDriftRetimeInput input;
    input.enabled = true;
    input.previousMultiplier = 1.0;
    input.deadbandSamples = 2400.0;
    input.fullCorrectionSamples = 24000.0;
    input.maxCorrection = 0.02;
    input.smoothing = 1.0;

    input.driftSamples = 1200;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.0);

    input.driftSamples = 24000;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.02);

    input.driftSamples = -24000;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 0.98);

    input.previousMultiplier = 1.0;
    input.driftSamples = 24000;
    input.smoothing = 0.25;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.005);

    input.enabled = false;
    input.previousMultiplier = 1.02;
    input.smoothing = 0.5;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.01);
}

void TestPlaybackPolicy::testAudioMasterClockSampleStaysAbsoluteAcrossSpeechRanges() {
    const QVector<ExportRangeSegment> ranges{
        ExportRangeSegment{100, 109},
        ExportRangeSegment{200, 209},
    };

    const int64_t audibleAbsoluteSample = frameToSamples(205);
    QCOMPARE(editor::audioMasterClockSampleToTimelineSample(audibleAbsoluteSample),
             audibleAbsoluteSample);

    const int64_t skippedGapSample = frameToSamples(150);
    bool atOrPastEnd = false;
    QCOMPARE(playableSampleAtOrAfter(
                 editor::audioMasterClockSampleToTimelineSample(skippedGapSample),
                 ranges,
                 &atOrPastEnd),
             frameToSamples(200));
    QVERIFY(!atOrPastEnd);
}

void TestPlaybackPolicy::testAudioMasterSameFrameCarriesCanonicalSample() {
    editor::PlaybackClockInput input;
    input.audioMasterEnabled = true;
    input.audioClockAvailable = true;
    input.hasPlayableAudio = true;
    input.transportSample = frameToSamples(120);
    input.audioSample = frameToSamples(120) + (kSamplesPerFrame / 2);
    input.currentFrame = 120;
    input.totalFrames = 1000;

    const editor::PlaybackClockDecision decision =
        editor::evaluatePlaybackClock(input);

    QCOMPARE(decision.action, editor::PlaybackClockAction::WaitForAudioClock);
    QCOMPARE(decision.sample, input.audioSample);
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
             "already-running audio must seek to the canonical playhead when "
             "runtime playback policy changes");
    QVERIFY2(playback.contains(QStringLiteral("const int64_t playbackStartFrame")) &&
                 playback.contains(QStringLiteral("m_audioEngine->start(playbackStartFrame)")) &&
                 playback.contains(QStringLiteral("m_audioEngine->warmPlaybackAudio(\n                    playbackStartFrame")),
             "playback start must use one canonical start frame for audio "
             "warmup and stream start");
    QVERIFY2(playback.contains(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) &&
                 playback.indexOf(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) <
                     playback.indexOf(QStringLiteral("const int64_t playbackStartFrame")) &&
                 playback.indexOf(QStringLiteral("const int64_t playbackStartFrame")) <
                     playback.indexOf(QStringLiteral("m_audioEngine->start(playbackStartFrame)")),
             "playback start must resolve export/speech ranges before deriving "
             "the shared audio/video start frame");
    QVERIFY2(playback.contains(QStringLiteral("bool EditorWindow::shouldUseAudioMasterClock() const")) &&
                 playback.contains(QStringLiteral("return false;")),
             "runtime playback must stay transport-clocked even when the UI "
             "clock source is Audio");
    QVERIFY2(playback.contains(QStringLiteral("updateAudioDriftRetime()")) &&
                 playback.contains(QStringLiteral("setPlaybackDriftRetimeRate")),
             "transport-clock playback must smoothly retime follower audio "
             "instead of only seeking on drift");
    QVERIFY2(playback.contains(QStringLiteral("playbackActive() && shouldUseAudioMasterClock()")),
             "retimed-audio warmup must not stop active transport-clock "
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
