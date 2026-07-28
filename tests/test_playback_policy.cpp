#include <QtTest/QtTest>

#include "../editor_shared.h"
#include "../playback_clock_coordinator.h"
#include "../playback_timing_context.h"

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
    void testAudioFeedbackProjectionUsesFeedbackAnchor();
    void testAudioFeedbackProjectionUsesSpeechRangeAnchor();
    void testSystemClockDecisionCarriesTransportSample();
    void testPlayableSampleAtOrAfterAcrossSpeechRanges();
    void testPlaybackSampleClampUsesExclusivePlayableEnd();
    void testDiscontinuousRangePrefetchStartsBeforeCrossfade();
    void testFrameCrossfadeMapsOutgoingTailToIncomingHead();
    void testFrameSmoothStepSpeedThroughMapsOutgoingTailAcrossGap();
    void testActivePlaybackRuntimeConfigRealignsStreams();
    void testPlaybackStartStopsCleanlyWhenFirstAdvanceHitsRangeEnd();
    void testPlaybackSyncDiagnosticsUseAudioFollowerDomain();
    void testSpeakerTitleRegenerationBypassesFullTimelineReset();
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
    QCOMPARE(playbackAudioWarpModeFromString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::RubberBand)),
             PlaybackAudioWarpMode::RubberBand);
    QCOMPARE(playbackAudioWarpModeFromString(playbackAudioWarpModeToString(PlaybackAudioWarpMode::RubberBandPassThroughFrequency)),
             PlaybackAudioWarpMode::RubberBandPassThroughFrequency);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("time-stretch")),
             PlaybackAudioWarpMode::TimeStretch);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("rubberband_100")),
             PlaybackAudioWarpMode::RubberBand);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("rubber-band-pass-through-frequency")),
             PlaybackAudioWarpMode::RubberBandPassThroughFrequency);
    QCOMPARE(playbackAudioWarpModeFromString(QStringLiteral("invalid")),
             PlaybackAudioWarpMode::Disabled);
}

void TestPlaybackPolicy::testEffectiveWarpRate() {
    QCOMPARE(effectivePlaybackAudioWarpRate(2.0, PlaybackAudioWarpMode::Disabled), 2.0);
    QCOMPARE(effectivePlaybackAudioWarpRate(0.05, PlaybackAudioWarpMode::Varispeed), 0.1);
    QCOMPARE(effectivePlaybackAudioWarpRate(4.0, PlaybackAudioWarpMode::TimeStretch), 3.0);
    QCOMPARE(effectivePlaybackAudioWarpRate(1.5, PlaybackAudioWarpMode::Varispeed), 1.5);
    QCOMPARE(effectivePlaybackAudioWarpRate(1.0, PlaybackAudioWarpMode::RubberBand), 1.0);
    QCOMPARE(effectivePlaybackAudioWarpRate(2.0, PlaybackAudioWarpMode::RubberBandPassThroughFrequency), 2.0);
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
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.0, PlaybackAudioWarpMode::RubberBand),
             PlaybackAudioWarpMode::RubberBand);
    QCOMPARE(normalizedPlaybackAudioWarpMode(1.5, PlaybackAudioWarpMode::RubberBand),
             PlaybackAudioWarpMode::RubberBand);
}

void TestPlaybackPolicy::testFrameCrossfadeMapsOutgoingTailToIncomingHead()
{
    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{10, 19},
        ExportRangeSegment{40, 49},
    };
    timing.frameCrossfadeEnabled = true;
    timing.frameCrossfadeFrames = 4;

    PlaybackFrameCrossfade before = playbackFrameCrossfadeAtTimelineFrame(15.0, timing);
    QVERIFY(!before.active);

    PlaybackFrameCrossfade first = playbackFrameCrossfadeAtTimelineFrame(16.0, timing);
    QVERIFY(first.active);
    QCOMPARE(first.secondaryTimelineFrame, int64_t(40));
    QVERIFY(first.secondaryOpacity > 0.0f);
    QVERIFY(first.secondaryOpacity < 0.1f);

    PlaybackFrameCrossfade last = playbackFrameCrossfadeAtTimelineFrame(19.0, timing);
    QVERIFY(last.active);
    QCOMPARE(last.secondaryTimelineFrame, int64_t(43));
    QVERIFY(last.secondaryOpacity > 0.49f);
    QVERIFY(last.secondaryOpacity < 0.51f);

    PlaybackFrameCrossfade gap = playbackFrameCrossfadeAtTimelineFrame(30.0, timing);
    QVERIFY(!gap.active);

    PlaybackFrameCrossfade incoming = playbackFrameCrossfadeAtTimelineFrame(40.0, timing);
    QVERIFY(incoming.active);
    QCOMPARE(incoming.secondaryTimelineFrame, int64_t(16));
    QVERIFY(incoming.secondaryOpacity > 0.49f);
    QVERIFY(incoming.secondaryOpacity < 0.51f);

    PlaybackFrameCrossfade incomingEnd = playbackFrameCrossfadeAtTimelineFrame(43.0, timing);
    QVERIFY(incomingEnd.active);
    QCOMPARE(incomingEnd.secondaryTimelineFrame, int64_t(19));
    QVERIFY(incomingEnd.secondaryOpacity < 0.1f);

    PlaybackFrameCrossfade after = playbackFrameCrossfadeAtTimelineFrame(44.0, timing);
    QVERIFY(!after.active);
}

void TestPlaybackPolicy::testDiscontinuousRangePrefetchStartsBeforeCrossfade()
{
    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{10, 19},
        ExportRangeSegment{40, 49},
        ExportRangeSegment{50, 59},
    };

    QCOMPARE(upcomingNoncontiguousPlaybackRangeStart(14.0, timing, 4),
             int64_t(-1));
    QCOMPARE(upcomingNoncontiguousPlaybackRangeStart(15.0, timing, 4),
             int64_t(40));
    QCOMPARE(upcomingNoncontiguousPlaybackRangeStart(19.0, timing, 4),
             int64_t(40));
    QCOMPARE(upcomingNoncontiguousPlaybackRangeStart(40.0, timing, 4),
             int64_t(-1));
    QCOMPARE(upcomingNoncontiguousPlaybackRangeStart(55.0, timing, 4),
             int64_t(-1));
}

void TestPlaybackPolicy::testFrameSmoothStepSpeedThroughMapsOutgoingTailAcrossGap()
{
    PlaybackTimingContext timing;
    timing.playbackRanges = {
        ExportRangeSegment{10, 19},
        ExportRangeSegment{40, 49},
    };
    timing.frameTransitionMode = PlaybackFrameTransitionMode::SmoothStepSpeedThrough;
    timing.frameCrossfadeFrames = 4;

    QCOMPARE(playbackVisualTimelineFramePosition(15.0, timing), 15.0);

    const qreal first = playbackVisualTimelineFramePosition(16.0, timing);
    QVERIFY(first > 19.0);
    QVERIFY(first < 21.0);

    const qreal last = playbackVisualTimelineFramePosition(19.0, timing);
    QVERIFY(last > 29.0);
    QVERIFY(last < 30.0);

    QCOMPARE(playbackVisualTimelineFramePosition(30.0, timing), last);

    const qreal incoming = playbackVisualTimelineFramePosition(40.0, timing);
    QCOMPARE(incoming, last);

    const qreal incomingEnd = playbackVisualTimelineFramePosition(43.0, timing);
    QVERIFY(incomingEnd > 38.0);
    QVERIFY(incomingEnd < 40.0);

    QCOMPARE(playbackVisualTimelineFramePosition(44.0, timing), 44.0);
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
    QVERIFY(pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::RubberBand,
                                                    1.0,
                                                    true));
    QVERIFY(!pitchPreservingPlaybackRequiresAudioGate(PlaybackAudioWarpMode::RubberBand,
                                                     1.0,
                                                     false));

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
    input.deadbandSamples = 240.0;
    input.fullCorrectionSamples = 9600.0;
    input.maxCorrection = 0.08;
    input.smoothing = 1.0;

    input.driftSamples = 120;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.0);

    input.driftSamples = 9600;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.08);

    input.driftSamples = -9600;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 0.92);

    input.previousMultiplier = 1.0;
    input.driftSamples = 9600;
    input.smoothing = 0.25;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.02);

    input.enabled = false;
    input.previousMultiplier = 1.08;
    input.smoothing = 0.5;
    QCOMPARE(editor::evaluatePlaybackDriftRetimeMultiplier(input), 1.04);
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

void TestPlaybackPolicy::testAudioFeedbackProjectionUsesFeedbackAnchor() {
    const QVector<ExportRangeSegment> ranges;
    const int64_t anchorTimelineSample = frameToSamples(300);
    const int64_t anchorFeedbackSample = frameToSamples(40);

    QCOMPARE(editor::projectAudioFeedbackSampleToTimelineSample(
                 anchorFeedbackSample,
                 anchorTimelineSample,
                 anchorFeedbackSample,
                 ranges),
             anchorTimelineSample);
    QCOMPARE(editor::projectAudioFeedbackSampleToTimelineSample(
                 anchorFeedbackSample + frameToSamples(12),
                 anchorTimelineSample,
                 anchorFeedbackSample,
                 ranges),
             anchorTimelineSample + frameToSamples(12));
}

void TestPlaybackPolicy::testAudioFeedbackProjectionUsesSpeechRangeAnchor() {
    const QVector<ExportRangeSegment> ranges{
        ExportRangeSegment{100, 109},
        ExportRangeSegment{200, 209},
        ExportRangeSegment{300, 309},
    };
    const int64_t anchorTimelineSample = frameToSamples(200);
    const int64_t anchorFeedbackSample = frameToSamples(80);

    QCOMPARE(editor::projectAudioFeedbackSampleToTimelineSample(
                 anchorFeedbackSample,
                 anchorTimelineSample,
                 anchorFeedbackSample,
                 ranges),
             anchorTimelineSample);
    QCOMPARE(editor::projectAudioFeedbackSampleToTimelineSample(
                 anchorFeedbackSample + frameToSamples(5),
                 anchorTimelineSample,
                 anchorFeedbackSample,
                 ranges),
             frameToSamples(205));
    QCOMPARE(editor::projectAudioFeedbackSampleToTimelineSample(
                 anchorFeedbackSample + frameToSamples(12),
                 anchorTimelineSample,
                 anchorFeedbackSample,
                 ranges),
             frameToSamples(302));
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

void TestPlaybackPolicy::testPlaybackSampleClampUsesExclusivePlayableEnd()
{
    const QString source = readSourceFile(QStringLiteral("editor_playback.cpp"));
    QVERIFY2(!source.isEmpty(), "editor_playback.cpp must be readable");
    QVERIFY2(source.contains(QStringLiteral("playableTimelineEndSampleExclusive()")),
             "playback sample clamping must use the exclusive end sample so playback can reach range_end");
    QVERIFY2(!source.contains(QStringLiteral(
                 "qBound<int64_t>(0, samplePosition, frameToSamples(lastPlayableFrame()))")),
             "clamping to the first sample of the last frame can stall playback on the last content frame");
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
    const QString playbackDebug = readSourceFile(QStringLiteral("playback_debug.h"));
    QVERIFY2(playbackDebug.contains(QStringLiteral("QElapsedTimer")) &&
                 playbackDebug.contains(QStringLiteral("timer.elapsed()")) &&
                 !playbackDebug.contains(QStringLiteral("currentMSecsSinceEpoch")),
             "playback transport time must use a monotonic elapsed timer, not wall clock time");
    QVERIFY2(playback.contains(QStringLiteral("sub_sample_tick")) &&
                 !playback.contains(QStringLiteral("llround(speed * static_cast<qreal>(kSamplesPerFrame)")),
             "sub-sample playback timer ticks must not manufacture a full frame advance");
    QVERIFY2(playback.contains(QStringLiteral(
                 "reconcileActivePlaybackAudioState(activePlaybackReconfigured)")),
             "active runtime changes must request stream realignment");
    QVERIFY2(playback.contains(QStringLiteral(
                 "m_audioEngine->seekToTimelineSample(m_transportTimelineSample)")),
             "already-running audio must seek to the exact transport sample "
             "when runtime playback policy changes");
    QVERIFY2(playback.contains(QStringLiteral("int64_t playbackStartFrame")) &&
                 playback.contains(QStringLiteral(
                     "m_audioEngine->startAtTimelineSample(playbackStartSample)")) &&
                 playback.contains(QStringLiteral("requestPlaybackAudioWarmup()")),
             "playback start must use the exact transport-derived sample for "
             "audio follower startup or asynchronous warmup");
    QVERIFY2(playback.contains(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) &&
                 playback.indexOf(QStringLiteral("playbackStartSample = playableSampleAtOrAfter")) <
                     playback.indexOf(QStringLiteral("int64_t playbackStartFrame")) &&
                 playback.indexOf(QStringLiteral("int64_t playbackStartFrame")) <
                     playback.indexOf(QStringLiteral(
                         "m_audioEngine->startAtTimelineSample(playbackStartSample)")),
             "playback start must resolve export/speech ranges before deriving "
             "the shared audio/video start position");
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
             "retimed-audio warmup must not restore a selectable audio clock "
             "playback path");
    QVERIFY2(playback.contains(QStringLiteral("requestPlaybackAudioWarmup()")) &&
                 playback.contains(QStringLiteral("audio follower waiting for re-timed audio")) &&
                 playback.contains(QStringLiteral("reconcileActivePlaybackAudioState(true)")),
             "missing pitch-preserving audio at playback start must warm as an "
             "audio follower and attach when ready");
    QVERIFY2(!playback.contains(QStringLiteral("startup gated: waiting for audio")) &&
                 !playback.contains(QStringLiteral("startup gated: waiting for re-timed audio")) &&
                 !playback.contains(QStringLiteral("transport playback waiting while pitch-preserving audio warms")),
             "audio readiness must not gate or hold the system-clock transport");
    QVERIFY2(!playback.contains(QStringLiteral("warmPlaybackLookahead(")) &&
                 !playback.contains(QStringLiteral("video_not_ready")),
             "video decoder lookahead readiness must not gate system-clock playback startup");
    QVERIFY2(playback.contains(QStringLiteral("audioOutputUnavailableForPlayback()")) &&
                 playback.contains(QStringLiteral("audioOutputStatusText()")),
             "playback overlay must distinguish audio-output failures from "
             "retimed-audio generation or video buffering");
    QVERIFY2(playback.contains(QStringLiteral("updateRubberBandProgressDialog")) &&
                 playback.contains(QStringLiteral("QProgressDialog")) &&
                 playback.contains(QStringLiteral("Rubber Band Audio Retiming")) &&
                 playback.contains(QStringLiteral("timeStretchProgressSnapshot()")) &&
                 playback.contains(QStringLiteral("Current: %2")) &&
                 playback.contains(QStringLiteral("%1 of %2 complete, %3 remaining")),
             "Rubber Band generation must show one aggregate progress dialog "
             "with the current clip and remaining clip count");
    const QString setup = readSourceFile(QStringLiteral("editor_setup.cpp"));
    QVERIFY2(setup.contains(QStringLiteral("rubberBandGenerationActive")) &&
                 setup.contains(QStringLiteral("m_rubberBandProgressDialog")) &&
                 setup.contains(QStringLiteral("updatePlaybackStatusOverlay()")),
             "main-thread heartbeat must keep the Rubber Band progress dialog "
             "updated and close it after generation finishes");
    const QString audio = readSourceFile(QStringLiteral("audio_engine.cpp"));
    QVERIFY2(audio.contains(QStringLiteral("audio_output_unavailable")) &&
                 audio.contains(QStringLiteral("audio_output_status")) &&
                 audio.contains(QStringLiteral("Audio output unavailable")),
             "audio diagnostics must expose a clear output-device/stream "
             "status separate from Rubber Band readiness");
    QVERIFY2(audio.contains(QStringLiteral("TimeStretchProgressSnapshot")) &&
                 audio.contains(QStringLiteral("time_stretch_progress_total_clips")) &&
                 audio.contains(QStringLiteral("time_stretch_progress_remaining_clips")) &&
                 audio.contains(QStringLiteral("TimeStretchJobProgress")) &&
                 audio.contains(QStringLiteral("markTimeStretchJob")),
             "audio diagnostics must expose aggregate Rubber Band progress "
             "from an explicit job tracker, not decode queue inspection");
    QVERIFY2(audio.contains(QStringLiteral("job.state == TimeStretchJobReadingSidecar")),
             "loading or validating an existing Rubber Band sidecar must not "
             "be presented as new artifact generation");
    QVERIFY2(!playback.contains(QStringLiteral("PlaybackClockInput{")),
             "PlaybackClockInput must use named field assignment so adding "
             "clock fields cannot silently shift audio/current-frame values");
}

void TestPlaybackPolicy::testPlaybackStartStopsCleanlyWhenFirstAdvanceHitsRangeEnd()
{
    const QString playback = readSourceFile(QStringLiteral("editor_playback.cpp"));
    QVERIFY2(!playback.isEmpty(), "editor_playback.cpp must be readable");

    const qsizetype start = playback.indexOf(QStringLiteral("void EditorWindow::setPlaybackActive"));
    QVERIFY2(start >= 0, "setPlaybackActive must be readable");
    const qsizetype advance = playback.indexOf(QStringLiteral("advanceFrame();"), start);
    const qsizetype guard = playback.indexOf(QStringLiteral("if (!playbackActive())"), advance);
    const qsizetype timerStart = playback.indexOf(QStringLiteral("m_playbackTimer.start();"), advance);
    QVERIFY2(advance >= 0 && guard > advance && timerStart > guard,
             "playback start must exit if initial advanceFrame() stops at range_end "
             "before arming the timer");
    QVERIFY2(playback.contains(QStringLiteral("playbackStartFrame >= lastPlayableFrame()")) &&
                 playback.contains(QStringLiteral("playbackStartSample = ranges.isEmpty()\n                ? 0")),
             "pressing Play from the last visible frame must restart at the first "
             "playable frame instead of arming a stalled end-of-range timer");
    QVERIFY2(playback.contains(QStringLiteral("playing == playbackActive() && (playing || !m_playbackTimer.isActive())")),
             "stopping playback must still stop an active timer even if the "
             "fast playback flag is already false");
    QVERIFY2(playback.contains(QStringLiteral(
                 "const bool videoReady = m_preview->preparePlaybackAdvanceSample(nextSample)")) &&
                 playback.contains(QStringLiteral("video_follower_waiting")) &&
                 playback.contains(QStringLiteral("transport_not_blocked")) &&
                 !playback.contains(QStringLiteral(
                     "if (!m_preview->preparePlaybackAdvanceSample(nextSample)")),
             "video readiness is follower telemetry and must not hold or "
             "redefine the monotonic transport clock");
    const qsizetype sampleApplyStart =
        playback.indexOf(QStringLiteral("void EditorWindow::setCurrentPlaybackSample"));
    const qsizetype setFrameStart =
        playback.indexOf(QStringLiteral("void EditorWindow::setCurrentFrame"), sampleApplyStart);
    const QString sampleApplyBody =
        sampleApplyStart >= 0 && setFrameStart > sampleApplyStart
            ? playback.mid(sampleApplyStart, setFrameStart - sampleApplyStart)
            : QString();
    QVERIFY2(sampleApplyBody.contains(QStringLiteral(
                 "m_preview->setCurrentPlaybackSample(boundedSample)")) &&
                 sampleApplyBody.contains(QStringLiteral(
                     "const bool syncQtPlaybackFollowers")) &&
                 sampleApplyBody.contains(QStringLiteral(
                     "m_timeline->setCurrentFrame(bounded)")) &&
                 sampleApplyBody.indexOf(QStringLiteral(
                     "m_preview->setCurrentPlaybackSample(boundedSample)")) <
                     sampleApplyBody.indexOf(QStringLiteral(
                         "const bool syncQtPlaybackFollowers")),
             "preview playback must consume every transport sample before "
             "coalescing timeline/slider/label UI followers");
    QVERIFY2(sampleApplyBody.contains(QStringLiteral(
                 "syncQtPlaybackFollowers &&")) &&
                 sampleApplyBody.contains(QStringLiteral(
                     "m_lastPlaybackChromeSyncMs = tickNowMs")),
             "during playback, table and chrome followers must be throttled "
             "so Qt repaint/layout work cannot dominate the transport tick");
    const QString setup = readSourceFile(QStringLiteral("editor_setup.cpp"));
    QVERIFY2(
        setup.contains(
            QStringLiteral("\"playback_timer_active\"), playbackIsActive")) &&
            !setup.contains(QStringLiteral(
                "const bool playbackTimerActive = m_playbackTimer.isActive()")),
        "worker-thread health telemetry must use the canonical atomic playback "
        "state and must not read a GUI-thread QTimer");
}

void TestPlaybackPolicy::testPlaybackSyncDiagnosticsUseAudioFollowerDomain()
{
    const QString routes =
        readSourceFile(QStringLiteral("control_server_worker_routes.cpp"));
    const QString probe =
        readSourceFile(QStringLiteral("tests/live_playback_sync_probe.py"));
    QVERIFY2(!routes.isEmpty() && !probe.isEmpty(),
             "playback sync diagnostic sources must be readable");
    QVERIFY2(
        routes.contains(QStringLiteral(
            "\"projected_audio_feedback_timeline_frame\"")) &&
            routes.contains(QStringLiteral(
                "\"projected_audio_feedback_timeline_sample\"")) &&
            routes.contains(QStringLiteral("\"transport_timeline_sample\"")) &&
            routes.contains(QStringLiteral("\"audio_clock_available\"")) &&
            routes.contains(QStringLiteral("\"has_playable_audio\"")) &&
            !routes.contains(QStringLiteral(
                "\"projected_audio_clock_absolute_frame\"")),
        "playback sync must expose the audio follower projected into the "
        "canonical timeline domain, not read nonexistent audio-clock fields");
    QVERIFY2(
        probe.contains(QStringLiteral(
            "projected_audio_feedback_timeline_frame")) &&
            probe.contains(QStringLiteral(
                "projected_audio_feedback_timeline_sample")) &&
            probe.contains(QStringLiteral(
                "transport_timeline_sample")) &&
            probe.contains(QStringLiteral(
                "TIMELINE_FPS\n                / TIMELINE_SAMPLE_RATE")) &&
            probe.contains(QStringLiteral(
                "/playback/telemetry")) &&
            !probe.contains(QStringLiteral(
                "/playback/sync")) &&
            probe.count(QStringLiteral(
                "{\"id\": \"transport.play\"}")) >= 2 &&
            !probe.contains(QStringLiteral(
                "{\"id\": \"transport.pause\"}")) &&
            probe.contains(QStringLiteral(
                "not clip.get(\"linkedSourceClipId\")")) &&
            !probe.contains(QStringLiteral(
                "expected_source_frame_for_clip")) &&
            !probe.contains(QStringLiteral(
                "video_source_drift")),
        "the live sync probe must measure canonical sample-domain follower "
        "feedback through UI-free telemetry, compare the renderer's canonical "
        "requested/presented handoff instead of reimplementing temporal "
        "mapping, toggle the real Wayland transport widget, and select the "
        "media parent rather than a generated mask child");
}

void TestPlaybackPolicy::testSpeakerTitleRegenerationBypassesFullTimelineReset()
{
    const QString editorTabs = readSourceFile(QStringLiteral("editor_tabs.cpp"));
    QVERIFY2(!editorTabs.isEmpty(), "editor_tabs.cpp must be readable");

    const QString marker = QStringLiteral("Transcript • Speaker Introductions");
    const qsizetype markerIndex = editorTabs.indexOf(marker);
    QVERIFY2(markerIndex >= 0, "speaker-title dependency must still be wired");

    const qsizetype lambdaStart = editorTabs.lastIndexOf(
        QStringLiteral("replaceGeneratedClipsForSource("), markerIndex);
    QVERIFY2(lambdaStart >= 0,
             "speaker-title dependency must call the narrow TimelineWidget "
             "generated-clip replacement API");

    const qsizetype lambdaEnd = editorTabs.indexOf(
        QStringLiteral("scheduleSaveState();"), markerIndex);
    QVERIFY2(lambdaEnd > lambdaStart,
             "speaker-title dependency body must be bounded");

    const QString dependencyBody =
        editorTabs.mid(lambdaStart, lambdaEnd - lambdaStart);
    QVERIFY2(!dependencyBody.contains(QStringLiteral("setClips(")),
             "regenerating Animated Speaker Titles must not go through "
             "TimelineWidget::setClips(); that broad mutation path runs mask "
             "sidecar reconciliation and audio-source refresh");
    QVERIFY2(!dependencyBody.contains(QStringLiteral("setTracks(")),
             "regenerating Animated Speaker Titles must not reset every track; "
             "generated children are a narrow in-place timeline mutation");

    const QString timelineCore =
        readSourceFile(QStringLiteral("timeline_widget_core.cpp"));
    QVERIFY2(!timelineCore.isEmpty(), "timeline_widget_core.cpp must be readable");
    const qsizetype methodIndex = timelineCore.indexOf(
        QStringLiteral("TimelineWidget::replaceGeneratedClipsForSource"));
    QVERIFY2(methodIndex >= 0,
             "TimelineWidget must expose a generated-child replacement "
             "mutation boundary");
    const qsizetype methodEnd = timelineCore.indexOf(
        QStringLiteral("TimelineWidget::setDeferMaskSidecarReconciliation"),
        methodIndex);
    QVERIFY2(methodEnd > methodIndex,
             "generated-child replacement method must be bounded");
    const QString methodBody = timelineCore.mid(methodIndex, methodEnd - methodIndex);
    QVERIFY2(!methodBody.contains(QStringLiteral("reconcileMaskMatteChildrenFromDisk")),
             "speaker-title regeneration must not trigger filesystem "
             "mask-sidecar reconciliation");
    QVERIFY2(!methodBody.contains(QStringLiteral("refreshClipAudioSource")),
             "speaker-title regeneration must not refresh audio sources for "
             "unrelated media clips");

    const QString speakersWiring =
        readSourceFile(QStringLiteral("speakers_tab_wiring.cpp"));
    QVERIFY2(!speakersWiring.isEmpty(), "speakers_tab_wiring.cpp must be readable");
    QVERIFY2(speakersWiring.contains(
                 QStringLiteral("m_speakerTitleRegenerationTimer->setInterval(200)")),
             "speaker-title parameter edits must be debounced instead of "
             "regenerating on each UI event");
    QVERIFY2(speakersWiring.contains(QStringLiteral(
                 "auto refreshEnabledFlyIn = [this]() { "
                 "scheduleSpeakerTitleRegeneration(); };")),
             "speaker-title parameter edits must route through the debounce "
             "boundary");
    const QString speakersTab =
        readSourceFile(QStringLiteral("speakers_tab.cpp"));
    const QString transcriptTabHeader =
        readSourceFile(QStringLiteral("transcript_tab.h"));
    QVERIFY2(
        speakersTab.contains(QStringLiteral(
            "void SpeakersTab::refreshSpeakerTitleControlState()")) &&
            speakersTab.contains(QStringLiteral(
                "m_speakerDeps.getActiveTranscriptSnapshot")) &&
            speakersTab.contains(QStringLiteral(
                "control->setChecked(clip && clip->speakerTitleEngineActive)")) &&
            transcriptTabHeader.contains(QStringLiteral(
                "void activeTranscriptSnapshotChanged();")) &&
            editorTabs.contains(QStringLiteral(
                "&TranscriptTab::activeTranscriptSnapshotChanged")) &&
            !speakersTab.contains(QStringLiteral(
                "m_widgets.speakerOverlayCreateTitleClipsButton->setEnabled(\n"
                "            canRunClipActions")),
        "Transcript-hosted speaker-title controls must derive state from the "
        "active Transcript snapshot and selected source clip, not from the "
        "hidden Speakers-tab refresh session");
}

QTEST_MAIN(TestPlaybackPolicy)
#include "test_playback_policy.moc"
