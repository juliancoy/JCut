#include <QtTest/QtTest>

#include "../audio_time_stretch_cache.h"
#include "../editor_shared.h"
#include "../playback_clock_coordinator.h"

#include <cmath>

namespace {

TimelineClip makeSixtyFpsClip()
{
    TimelineClip clip;
    clip.id = QStringLiteral("clip-temporal");
    clip.mediaType = ClipMediaType::Video;
    clip.hasAudio = true;
    clip.sourceFps = 60.0;
    clip.startFrame = 90;
    clip.durationFrames = 9000;
    clip.sourceInFrame = 0;
    clip.sourceDurationFrames = 18000;
    clip.playbackRate = 1.0;
    return clip;
}

int64_t canonicalSampleForWallSeconds(const TimelineClip& clip, double wallSeconds, double playbackSpeed)
{
    const int64_t startSample = clipTimelineStartSamples(clip);
    return startSample + static_cast<int64_t>(
        std::llround(wallSeconds * static_cast<double>(kAudioSampleRate) * playbackSpeed));
}

void verifyDerivedDomainsShareSourceSeconds(const TimelineClip& clip, int64_t timelineSample)
{
    const int64_t sourceSample = sourceSampleForClipAtTimelineSample(clip, timelineSample, {});
    const int64_t mediaSourceFrame = sourceFrameForClipAtTimelineSample(clip, timelineSample, {});
    const int64_t transcriptFrame = transcriptFrameForClipAtTimelineSample(clip, timelineSample, {});
    const double sourceSeconds =
        static_cast<double>(sourceSample) / static_cast<double>(kAudioSampleRate);
    const double mediaSeconds =
        static_cast<double>(mediaSourceFrame) / static_cast<double>(clip.sourceFps);
    const double transcriptSeconds =
        static_cast<double>(transcriptFrame) / static_cast<double>(kTimelineFps);

    QVERIFY2(std::abs(mediaSeconds - sourceSeconds) <= (1.0 / clip.sourceFps),
             "media source frame must derive from the canonical timeline sample");
    QVERIFY2(std::abs(transcriptSeconds - sourceSeconds) <= (1.0 / kTimelineFps),
             "transcript frame must derive from the same canonical timeline sample");
}

}  // namespace

class TestTemporalSyncContract : public QObject {
    Q_OBJECT

private slots:
    void derivedDomainsStayLockedAtPlaybackSpeeds();
    void renderSyncMarkersFeedVideoAndTranscriptMapping();
    void pitchPreservingClockGatePreventsTimerFallback();
};

void TestTemporalSyncContract::derivedDomainsStayLockedAtPlaybackSpeeds()
{
    const TimelineClip clip = makeSixtyFpsClip();
    for (const double playbackSpeed : {1.0, 1.25, 1.5}) {
        for (const double wallSeconds : {0.25, 1.0, 2.5}) {
            const int64_t timelineSample =
                canonicalSampleForWallSeconds(clip, wallSeconds, playbackSpeed);
            verifyDerivedDomainsShareSourceSeconds(clip, timelineSample);

            const int64_t sourceSample =
                sourceSampleForClipAtTimelineSample(clip, timelineSample, {});
            const int64_t sourceEndSampleExclusive =
                sourceSample + static_cast<int64_t>(std::ceil(1024.0 * playbackSpeed));
            const int64_t cacheStart =
                audioTimeStretchCacheSampleForSourceSample(sourceSample, playbackSpeed);
            const int64_t cacheEnd =
                audioTimeStretchCacheEndSampleForSourceEndSample(sourceEndSampleExclusive,
                                                                 playbackSpeed);
            const int64_t cacheFrameCount = qMax<int64_t>(1, cacheEnd - cacheStart);
            QVERIFY2(audioTimeStretchSegmentCoversSourceRange(cacheStart,
                                                              cacheFrameCount,
                                                              sourceSample,
                                                              sourceEndSampleExclusive,
                                                              playbackSpeed),
                     "retimed cache segment must cover the whole requested mix range");
            QVERIFY2(!audioTimeStretchSegmentCoversSourceRange(cacheStart,
                                                               qMax<int64_t>(0, cacheFrameCount - 1),
                                                               sourceSample,
                                                               sourceEndSampleExclusive,
                                                               playbackSpeed),
                     "partial retimed cache segments must not be accepted");
        }
    }
}

void TestTemporalSyncContract::renderSyncMarkersFeedVideoAndTranscriptMapping()
{
    const TimelineClip clip = makeSixtyFpsClip();
    const int64_t timelineSample = frameToSamples(clip.startFrame + 120);

    const int64_t sourceWithoutMarker =
        sourceFrameForClipAtTimelineSample(clip, timelineSample, {});
    const int64_t transcriptWithoutMarker =
        transcriptFrameForClipAtTimelineSample(clip, timelineSample, {});

    const QVector<RenderSyncMarker> markers{
        RenderSyncMarker{clip.id, clip.startFrame + 60, RenderSyncAction::SkipFrame, 10},
    };
    const int64_t sourceWithMarker =
        sourceFrameForClipAtTimelineSample(clip, timelineSample, markers);
    const int64_t transcriptWithMarker =
        transcriptFrameForClipAtTimelineSample(clip, timelineSample, markers);

    QVERIFY(sourceWithMarker > sourceWithoutMarker);
    QVERIFY(transcriptWithMarker > transcriptWithoutMarker);

    const double sourceSeconds =
        static_cast<double>(sourceWithMarker) / static_cast<double>(clip.sourceFps);
    const double transcriptSeconds =
        static_cast<double>(transcriptWithMarker) / static_cast<double>(kTimelineFps);
    QVERIFY2(std::abs(sourceSeconds - transcriptSeconds) <= (1.0 / kTimelineFps),
             "video and transcript mapping must consume the same render-sync-adjusted source time");
}

void TestTemporalSyncContract::pitchPreservingClockGatePreventsTimerFallback()
{
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
    input.audioClockStallTicks = 8;
    input.audioClockStallThresholdTicks = 2;

    editor::PlaybackClockDecision decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);

    input.audioBlocked = false;
    input.audioReady = false;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);

    input.audioReady = true;
    input.audioSample = frameToSamples(80);
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::HoldForPitchPreservingAudio);

    input.pitchPreservingAudioRequired = false;
    decision = editor::evaluatePlaybackClock(input);
    QCOMPARE(decision.action, editor::PlaybackClockAction::UseTimelineTimer);
}

QTEST_MAIN(TestTemporalSyncContract)
#include "test_temporal_sync_contract.moc"
