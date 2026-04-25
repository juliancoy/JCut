#include <QtTest/QtTest>

#include "../editor_shared.h"

class TestPlaybackPolicy : public QObject {
    Q_OBJECT

private slots:
    void testClockSourceRoundTrip();
    void testAudioWarpRoundTrip();
    void testNormalizedAudioWarpMode();
    void testEffectiveWarpRate();
    void testAudioMasterClockPolicy();
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
             PlaybackAudioWarpMode::Varispeed);
    QCOMPARE(normalizedPlaybackAudioWarpMode(2.0, PlaybackAudioWarpMode::TimeStretch),
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

QTEST_MAIN(TestPlaybackPolicy)
#include "test_playback_policy.moc"
