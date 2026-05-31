#include <QtTest/QtTest>

#include "../editor_shared.h"
#include "../playback_clock_coordinator.h"

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
    void testPlayableSampleAtOrAfterAcrossSpeechRanges();
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
    QVERIFY(shouldUseAudioMasterClock(PlaybackClockSource::Auto,
                                      PlaybackAudioWarpMode::Disabled,
                                      1.0,
                                      true));
    QVERIFY(shouldUseAudioMasterClock(PlaybackClockSource::Auto,
                                      PlaybackAudioWarpMode::Disabled,
                                      1.5,
                                      true));
    QVERIFY(shouldUseAudioMasterClock(PlaybackClockSource::Audio,
                                      PlaybackAudioWarpMode::Varispeed,
                                      2.0,
                                      true));
    QVERIFY(!shouldUseAudioMasterClock(PlaybackClockSource::Timeline,
                                       PlaybackAudioWarpMode::Varispeed,
                                       1.0,
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
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTimelineTimer);
    QCOMPARE(decision.reason, QStringLiteral("audio_clock_stalled"));

    input.audioSample = frameToSamples(121);
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseAudioSample);
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

QTEST_MAIN(TestPlaybackPolicy)
#include "test_playback_policy.moc"
