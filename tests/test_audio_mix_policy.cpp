#include "../audio_engine.h"
#include "../audio_clip_fade.h"
#include "../audio_mix_readiness.h"
#include "../capabilities_detector.h"
#include "../render_internal.h"
#include "../master_output_audio_delay.h"

#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

namespace {

int countBackendCallbacks(void* output,
                          void*,
                          unsigned int frameCount,
                          double,
                          unsigned int,
                          void* userData) {
  auto* callbackCount = static_cast<std::atomic<int>*>(userData);
  callbackCount->fetch_add(1, std::memory_order_relaxed);
  if (output) {
    std::memset(output, 0,
                static_cast<size_t>(frameCount) * 2 * sizeof(int16_t));
  }
  return 0;
}

class BlockingStopAudioBackend final : public AudioOutputBackend {
public:
  bool initialize(const AudioOutputBackendConfig&) override {
    m_open = true;
    m_running = true;
    return true;
  }
  void shutdown() override {
    m_open = false;
    m_running = false;
  }
  bool start() override {
    m_running = true;
    return true;
  }
  bool stop(bool) override {
    ++stopCalls;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    m_running = false;
    return true;
  }
  bool isOpen() const override { return m_open; }
  bool isRunning() const override { return m_running; }
  bool supportsSeamlessReprime() const override { return true; }
  int64_t latencyFrames() const override { return 0; }
  uint64_t connectionRevision() const override {
    return revision.load(std::memory_order_acquire);
  }
  AudioOutputBackendInfo info() const override { return {}; }
  std::string lastError() const override { return {}; }

  std::atomic<int> stopCalls{0};
  std::atomic<uint64_t> revision{1};

private:
  std::atomic<bool> m_open{false};
  std::atomic<bool> m_running{false};
};

} // namespace

// Multi-stream mix readiness policy (TIME.md "Multi-Stream Audio Readiness"):
// a starved clip degrades per-clip instead of stalling ready clips, and the
// chunk blocks only when no active clip can contribute.
class TestAudioMixPolicy : public QObject {
  Q_OBJECT

private slots:
  void testPolicyHelpers();
  void testStarvedClipDoesNotBlockReadyClip();
  void testOnlyStarvedClipBlocksChunk();
  void testSpeechFilterRangesAreDerivedFromExportRanges();
  void testSpeechFilterFadeModesShapeBoundaryGain();
  void testExportMixerUsesSharedSpeechFilterFadeBlend();
  void testMasterOutputAudioDelayIsSignedAndDurationPreserving();
  void testSpliceSecondaryTapStopsAtClipEnd();
  void testAudioDynamicsAreAppliedPerClipBeforeMix();
  void testTrackGainMuteAndSoloAffectMix();
  void testExportMixerUsesPreviewClipFadePolicy();
  void testTimelineStateAtFrameRejectsStaleMixerGeneration();
  void testAudioSourceCacheInvalidationVersionsQueuedWork();
  void testStaleTimeStretchStateRejectedAfterSourceInvalidation();
  void testWarmupPermanentFailureIsSourceGenerationScoped();
  void testRingBufferClearWaitsForReaderAndRejectsReadsDuringReset();
  void testOutputStreamStartWaitsForPrimeBuffer();
  void testPendingOutputRebasePreservesExactSampleAndIsOneShot();
  void testAudioFollowerSnapshotIsCoherent();
  void testCapabilitiesRankNativeBackendBeforePortableFallback();
  void testCapabilitiesDescribeVideoDecoderBackends();
  void testSeamlessBackendSeekNeverCallsBlockingDeviceStop();
  void testRecoveredBackendRebasesToAuthoritativeTransport();
  void testDetectedBackendDrivesLiveCallbackWhenDeviceIsAvailable();

private:
  static TimelineClip makeAudioClip(const QString &id, const QString &path,
                                    int64_t startFrame,
                                    int64_t durationFrames);
  static AudioEngine::AudioClipCacheEntry
  makeCacheEntry(int64_t sourceStartSample, int64_t frameCount, float value);
};

void TestAudioMixPolicy::testMasterOutputAudioDelayIsSignedAndDurationPreserving()
{
  const std::vector<float> input{
      1.0f, 11.0f,
      2.0f, 12.0f,
      3.0f, 13.0f,
      4.0f, 14.0f,
      5.0f, 15.0f};

  jcut::audio::MasterOutputAudioDelay delayed(2, 1000, 2);
  std::vector<float> delayedOutput = delayed.process(input.data(), 2);
  const std::vector<float> delayedSecond = delayed.process(input.data() + 4, 3);
  delayedOutput.insert(
      delayedOutput.end(), delayedSecond.begin(), delayedSecond.end());
  const std::vector<float> delayedTail = delayed.finish();
  QVERIFY(delayedTail.empty());
  QCOMPARE(delayedOutput.size(), input.size());
  QVERIFY(delayedOutput == std::vector<float>({
      0.0f, 0.0f,
      0.0f, 0.0f,
      1.0f, 11.0f,
      2.0f, 12.0f,
      3.0f, 13.0f}));

  jcut::audio::MasterOutputAudioDelay advanced(-2, 1000, 2);
  std::vector<float> advancedOutput = advanced.process(input.data(), 1);
  const std::vector<float> advancedSecond = advanced.process(input.data() + 2, 4);
  advancedOutput.insert(
      advancedOutput.end(), advancedSecond.begin(), advancedSecond.end());
  const std::vector<float> advancedTail = advanced.finish();
  advancedOutput.insert(
      advancedOutput.end(), advancedTail.begin(), advancedTail.end());
  QCOMPARE(advancedOutput.size(), input.size());
  QVERIFY(advancedOutput == std::vector<float>({
      3.0f, 13.0f,
      4.0f, 14.0f,
      5.0f, 15.0f,
      0.0f, 0.0f,
      0.0f, 0.0f}));
}

TimelineClip TestAudioMixPolicy::makeAudioClip(const QString &id,
                                               const QString &path,
                                               int64_t startFrame,
                                               int64_t durationFrames) {
  TimelineClip clip;
  clip.id = id;
  clip.filePath = path;
  clip.mediaType = ClipMediaType::Audio;
  clip.hasAudio = true;
  clip.audioEnabled = true;
  clip.startFrame = startFrame;
  clip.durationFrames = durationFrames;
  clip.sourceInFrame = 0;
  clip.sourceDurationFrames = durationFrames;
  return clip;
}

AudioEngine::AudioClipCacheEntry
TestAudioMixPolicy::makeCacheEntry(int64_t sourceStartSample,
                                   int64_t frameCount, float value) {
  AudioEngine::AudioClipCacheEntry entry;
  entry.sourceStartSample = sourceStartSample;
  entry.samples = QVector<float>(static_cast<int>(frameCount) * 2, value);
  entry.sampleRate = 48000;
  entry.channelCount = 2;
  entry.valid = true;
  entry.fullyDecoded = true;
  return entry;
}

void TestAudioMixPolicy::testPolicyHelpers() {
  QVERIFY(mixPrepareMustBlock(0, 1, 0));
  QVERIFY(mixPrepareMustBlock(0, 0, 1));
  QVERIFY(!mixPrepareMustBlock(0, 0, 0));
  QVERIFY(!mixPrepareMustBlock(1, 1, 1));

  QVERIFY(mixFrameMustBlock(true, false, true));
  QVERIFY(!mixFrameMustBlock(true, true, true));
  QVERIFY(!mixFrameMustBlock(true, true, false));
  QVERIFY(!mixFrameMustBlock(false, false, false));

  QVERIFY(spliceSecondaryTapWithinClip(100, 100, 200));
  QVERIFY(spliceSecondaryTapWithinClip(199, 100, 200));
  QVERIFY(!spliceSecondaryTapWithinClip(99, 100, 200));
  QVERIFY(!spliceSecondaryTapWithinClip(200, 100, 200));
}

void TestAudioMixPolicy::testOutputStreamStartWaitsForPrimeBuffer() {
  constexpr int periodFrames = 1024;
  constexpr int64_t streamLatencyFrames = 6141;
  const size_t primeSamples =
      AudioEngine::outputPrimeTargetSamples(
          periodFrames, streamLatencyFrames);
  QCOMPARE(
      primeSamples,
      static_cast<size_t>(8192 * 2));
  QVERIFY(AudioEngine::outputPrimeCapacitySufficient(
      periodFrames, streamLatencyFrames));
  QVERIFY(!AudioEngine::outputStreamCanStart(
      false, true, primeSamples, primeSamples));
  QVERIFY(!AudioEngine::outputStreamCanStart(
      true, false, primeSamples, primeSamples));
  QVERIFY(!AudioEngine::outputStreamCanStart(
      true, true, primeSamples - 1, primeSamples));
  QVERIFY(AudioEngine::outputStreamCanStart(
      true, true, primeSamples, primeSamples));
  QVERIFY(!AudioEngine::outputPrimeNeedsRebase(1000, 1240, 240));
  QVERIFY(AudioEngine::outputPrimeNeedsRebase(1000, 1241, 240));
  QVERIFY(!AudioEngine::outputPrimeNeedsRebase(1000, 760, 240));
  QVERIFY(AudioEngine::outputPrimeNeedsRebase(1000, 759, 240));

  AudioEngine engine;
  QVector<int16_t> primed(
      static_cast<qsizetype>(primeSamples), 0);
  QCOMPARE(
      engine.m_ringBuffer.write(
          primed.constData(), primeSamples),
      primeSamples);
  QVector<int16_t> callbackOutput(periodFrames * 2);
  const int startupCallbacks =
      static_cast<int>(
          (streamLatencyFrames + periodFrames - 1) /
          periodFrames) +
      2;
  for (int callback = 0; callback < startupCallbacks; ++callback) {
    QCOMPARE(
        AudioEngine::rtAudioCallback(
            callbackOutput.data(),
            nullptr,
            periodFrames,
            0.0,
            0,
            &engine),
        0);
  }
  QCOMPARE(engine.m_underrunCount.load(), 0);
  QCOMPARE(engine.m_ringBuffer.available(), size_t(0));
}

void TestAudioMixPolicy::
testPendingOutputRebasePreservesExactSampleAndIsOneShot() {
  AudioEngine engine;
  constexpr int64_t exactSample = 12345;
  engine.m_playing.store(true);
  QVector<int16_t> staleSamples(2048, 1);
  QCOMPARE(
      engine.m_ringBuffer.write(
          staleSamples.constData(),
          static_cast<size_t>(staleSamples.size())),
      static_cast<size_t>(staleSamples.size()));

  engine.seekToTimelineSample(exactSample);
  QCOMPARE(engine.m_timelineSampleCursor, exactSample);
  QCOMPARE(engine.m_audioClockSample.load(), exactSample);
  QCOMPARE(engine.m_ringBufferEndSample.load(), exactSample);
  QCOMPARE(engine.m_authoritativeTransportSample.load(), exactSample);
  QCOMPARE(engine.m_ringBuffer.available(), size_t(0));
  QVERIFY(engine.m_outputStartPending.load());
  QVERIFY(engine.m_outputPrimeCanRebase);

  const uint64_t generationBeforeRebase = engine.m_mixGeneration;
  const int64_t rebasedSample =
      exactSample +
      AudioEngine::kOutputPrimeRebaseDeadbandSamples + 1;
  engine.setAuthoritativeTransportSample(rebasedSample);
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    QVERIFY(engine.rebasePendingOutputToAuthoritativeLocked());
  }
  QCOMPARE(engine.m_mixGeneration, generationBeforeRebase + 1);
  QCOMPARE(engine.m_timelineSampleCursor, rebasedSample);
  QCOMPARE(engine.m_audioClockSample.load(), rebasedSample);
  QCOMPARE(engine.m_ringBufferEndSample.load(), rebasedSample);
  QCOMPARE(engine.m_ringBuffer.available(), size_t(0));
  QCOMPARE(engine.m_outputPrimeRebaseCount.load(), qint64(1));
  QVERIFY(!engine.m_outputPrimeCanRebase);

  engine.setAuthoritativeTransportSample(rebasedSample + 48000);
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    QVERIFY(!engine.rebasePendingOutputToAuthoritativeLocked());
  }
  QCOMPARE(engine.m_mixGeneration, generationBeforeRebase + 1);
}

void TestAudioMixPolicy::testAudioFollowerSnapshotIsCoherent() {
  AudioEngine engine;
  std::atomic<bool> writerFinished{false};
  std::atomic<bool> mismatch{false};
  std::thread writer([&]() {
    for (uint64_t revision = 1; revision <= 10000; ++revision) {
      std::lock_guard<std::mutex> lock(engine.m_stateMutex);
      engine.m_lastOutputStartTimelineSample.store(
          static_cast<int64_t>(revision * 10));
      engine.m_lastOutputStartFeedbackSample.store(
          static_cast<int64_t>(revision * 10 + 1));
      engine.m_outputStartRevision.store(revision);
    }
    writerFinished.store(true);
  });

  while (!writerFinished.load()) {
    const AudioEngine::AudioFollowerSnapshot snapshot =
        engine.audioFollowerSnapshot();
    if (snapshot.outputStartRevision > 0 &&
        (snapshot.outputStartTimelineSample !=
             static_cast<int64_t>(snapshot.outputStartRevision * 10) ||
         snapshot.outputStartFeedbackSample !=
             static_cast<int64_t>(snapshot.outputStartRevision * 10 + 1))) {
      mismatch.store(true);
      break;
    }
  }
  writer.join();
  QVERIFY(!mismatch.load());
}

void TestAudioMixPolicy::
testCapabilitiesRankNativeBackendBeforePortableFallback() {
  const RuntimeCapabilities capabilities = detectRuntimeCapabilities();
  QVERIFY(!capabilities.audioOutputBackends.empty());
  const auto rtaudio = std::find_if(
      capabilities.audioOutputBackends.cbegin(),
      capabilities.audioOutputBackends.cend(),
      [](const AudioOutputBackendCapability& candidate) {
        return candidate.kind == AudioOutputBackendKind::RtAudio;
      });
  QVERIFY(rtaudio != capabilities.audioOutputBackends.cend());
  QVERIFY(rtaudio->compiled);
  QVERIFY(rtaudio->operatingSystemSupported);

#if defined(__linux__)
  QCOMPARE(capabilities.operatingSystem, std::string("linux"));
  QCOMPARE(capabilities.audioOutputBackends.front().kind,
           AudioOutputBackendKind::PipeWire);
#if defined(JCUT_HAVE_PIPEWIRE) && JCUT_HAVE_PIPEWIRE
  QVERIFY(capabilities.audioOutputBackends.front().compiled);
#endif
#endif
}

void TestAudioMixPolicy::testCapabilitiesDescribeVideoDecoderBackends() {
  const RuntimeCapabilities capabilities = detectRuntimeCapabilities();
  QVERIFY(!capabilities.videoDecodeBackends.empty());

  bool unavailableSeen = false;
  for (const jcut::VideoDecodeBackendCapability& backend :
       capabilities.videoDecodeBackends) {
    QVERIFY(backend.deviceType != AV_HWDEVICE_TYPE_NONE);
    QVERIFY(!backend.id.empty());
    if (!backend.available) {
      unavailableSeen = true;
    } else {
      QVERIFY2(!unavailableSeen,
               "available video decoders must rank before unavailable ones");
    }
  }

  const std::vector<AVHWDeviceType> h264Order =
      jcut::preferredVideoDecodeDeviceOrder(AV_CODEC_ID_H264);
  for (AVHWDeviceType type : h264Order) {
    const jcut::VideoDecodeBackendCapability* backend =
        jcut::videoDecodeCapability(type);
    QVERIFY(backend);
    QVERIFY(backend->available);
    QVERIFY(jcut::videoDecodeBackendSupportsCodec(
        *backend, AV_CODEC_ID_H264));
  }
}

void TestAudioMixPolicy::
testSeamlessBackendSeekNeverCallsBlockingDeviceStop() {
  AudioEngine engine;
  auto backend = std::make_unique<BlockingStopAudioBackend>();
  BlockingStopAudioBackend* backendPtr = backend.get();
  QVERIFY(backend->initialize({}));
  engine.m_outputBackend = std::move(backend);
  engine.m_playing.store(true);

  const auto started = std::chrono::steady_clock::now();
  engine.seekToTimelineSample(48000);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  QCOMPARE(backendPtr->stopCalls.load(), 0);
  QVERIFY(elapsed < std::chrono::milliseconds(100));
  QCOMPARE(engine.m_audioClockSample.load(), int64_t{48000});
  QVERIFY(engine.m_outputStartPending.load());
  engine.m_outputBackend->shutdown();
}

void TestAudioMixPolicy::
testRecoveredBackendRebasesToAuthoritativeTransport() {
  AudioEngine engine;
  auto backend = std::make_unique<BlockingStopAudioBackend>();
  BlockingStopAudioBackend* backendPtr = backend.get();
  QVERIFY(backend->initialize({}));
  engine.m_outputBackend = std::move(backend);
  engine.m_lastObservedBackendConnectionRevision.store(1);
  engine.m_timelineSampleCursor = 1000;
  engine.m_audioClockSample.store(1000);
  const std::array<int16_t, 4> stale{{1, 2, 3, 4}};
  QCOMPARE(engine.m_ringBuffer.write(stale.data(), stale.size()),
           stale.size());
  const uint64_t generation = engine.m_mixGeneration;

  backendPtr->revision.store(2);
  engine.setAuthoritativeTransportSample(96000);

  QCOMPARE(engine.m_timelineSampleCursor, int64_t{96000});
  QCOMPARE(engine.m_audioClockSample.load(), int64_t{96000});
  QCOMPARE(engine.m_ringBufferEndSample.load(), int64_t{96000});
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QCOMPARE(engine.m_mixGeneration, generation + 1);
  QCOMPARE(engine.m_lastObservedBackendConnectionRevision.load(),
           uint64_t{2});
  engine.m_outputBackend->shutdown();
}

void TestAudioMixPolicy::
testDetectedBackendDrivesLiveCallbackWhenDeviceIsAvailable() {
  std::atomic<int> callbackCount{0};
  const AudioOutputBackendConfig config{
      48000, 2, 1024, &countBackendCallbacks, &callbackCount};
  AudioOutputBackendSelection selection =
      selectBestAudioOutputBackend(config);
  if (!selection.backend) {
    QSKIP("No live audio output is available in this test environment");
  }

  QVERIFY(!selection.selectedId.empty());
  QVERIFY(!selection.probes.empty());
  QVERIFY(selection.probes.back().available);
  QCOMPARE(selection.probes.back().capability.id, selection.selectedId);
  QVERIFY(selection.backend->isOpen());
  QVERIFY(selection.backend->start());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (callbackCount.load(std::memory_order_relaxed) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  QVERIFY2(callbackCount.load(std::memory_order_relaxed) > 0,
           qPrintable(QStringLiteral("%1 backend produced no callback: %2")
                          .arg(QString::fromStdString(selection.selectedId),
                               QString::fromStdString(
                                   selection.backend->lastError()))));
  QVERIFY(selection.backend->stop(false));
  selection.backend->shutdown();
}

void TestAudioMixPolicy::testStarvedClipDoesNotBlockReadyClip() {
  AudioEngine engine;
  const QString readyPath = QStringLiteral("/tmp/jcut_mix_policy_ready.wav");
  const QString starvedPath =
      QStringLiteral("/tmp/jcut_mix_policy_starved.wav");

  AudioEngine::MixContext context;
  context.clips.push_back(
      makeAudioClip(QStringLiteral("ready"), readyPath, 0, 30));
  context.clips.push_back(
      makeAudioClip(QStringLiteral("starved"), starvedPath, 0, 30));
  context.volume = 1.0;

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    // Ready clip covers the chunk; starved clip's decode window is far away.
    engine.m_audioCache.insert(readyPath, makeCacheEntry(0, 48000, 0.25f));
    engine.m_audioCache.insert(starvedPath,
                               makeCacheEntry(10000000, 1024, 0.25f));
  }

  QVector<float> output(1024 * 2, 0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));

  QCOMPARE(engine.m_lastMixStarvedClipCount.load(), 1);
  QVERIFY(engine.m_mixDegradedChunkCount.load() >= 1);
  QVERIFY(!engine.m_audioPlaybackBlocked.load());
  QVERIFY(!engine.m_pitchPreservingAudioBlocked.load());
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    QCOMPARE(engine.m_lastMixStarvedClipPath, starvedPath);
  }

  // The ready clip is audible: away from clip fades the mixed value is the
  // ready clip's sample, with no contribution from the starved clip.
  QVERIFY(std::abs(output[512 * 2] - 0.25f) < 0.01f);
}

void TestAudioMixPolicy::testOnlyStarvedClipBlocksChunk() {
  AudioEngine engine;
  const QString starvedPath =
      QStringLiteral("/tmp/jcut_mix_policy_only_starved.wav");

  AudioEngine::MixContext context;
  context.clips.push_back(
      makeAudioClip(QStringLiteral("starved"), starvedPath, 0, 30));
  context.volume = 1.0;

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_audioCache.insert(starvedPath,
                               makeCacheEntry(10000000, 1024, 0.25f));
  }

  QVector<float> output(1024 * 2, 0.0f);
  QVERIFY(!engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));
  QVERIFY(engine.m_audioPlaybackBlocked.load());
  QCOMPARE(engine.m_lastMixStarvedClipCount.load(), 0);
}

void TestAudioMixPolicy::testSpeechFilterRangesAreDerivedFromExportRanges() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_mix_policy_speech_filter.wav");

  AudioEngine::MixContext context;
  context.clips.push_back(
      makeAudioClip(QStringLiteral("speech"), path, 0, 100));
  context.exportRanges.push_back(ExportRangeSegment{0, 14});
  context.exportRanges.push_back(ExportRangeSegment{60, 74});
  context.volume = 1.0;

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_audioCache.insert(path, makeCacheEntry(0, 160000, 0.25f));
  }
  engine.m_speechFilterFadeSamples.store(0);

  QVector<float> output(1024 * 2, 0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, frameToSamples(30), 1.0, 1.0));

  for (float sample : std::as_const(output)) {
    QVERIFY2(std::abs(sample) < 0.000001f,
             qPrintable(QStringLiteral("expected speech-filtered gap silence, got %1").arg(sample)));
  }
}

void TestAudioMixPolicy::testSpeechFilterFadeModesShapeBoundaryGain() {
  AudioEngine engine;
  QVector<AudioEngine::SpeechSampleRange> ranges{
      AudioEngine::SpeechSampleRange{0, 1000},
  };

  const auto jump = engine.calculateSpeechRangeBlend(
      25, ranges, 100, AudioEngine::SpeechFilterFadeMode::JumpCut, 1.0, false);
  QCOMPARE(jump.primaryGain, 1.0f);

  const auto linear = engine.calculateSpeechRangeBlend(
      25, ranges, 100, AudioEngine::SpeechFilterFadeMode::Fade, 1.0, false);
  QCOMPARE(linear.primaryGain, 0.25f);

  const auto smooth = engine.calculateSpeechRangeBlend(
      25, ranges, 100, AudioEngine::SpeechFilterFadeMode::SmoothStep, 1.0, false);
  QCOMPARE(smooth.primaryGain, 0.15625f);

  const auto strongerSmooth = engine.calculateSpeechRangeBlend(
      25, ranges, 100, AudioEngine::SpeechFilterFadeMode::SmoothStep, 2.0, false);
  QVERIFY(strongerSmooth.primaryGain < smooth.primaryGain);

  const auto smoother = engine.calculateSpeechRangeBlend(
      25, ranges, 100, AudioEngine::SpeechFilterFadeMode::SmootherStep, 1.0, false);
  QVERIFY(smoother.primaryGain > 0.1035f);
  QVERIFY(smoother.primaryGain < 0.1036f);

  QVector<AudioEngine::SpeechSampleRange> adjacentRanges{
      AudioEngine::SpeechSampleRange{0, 1000},
      AudioEngine::SpeechSampleRange{2000, 3000},
  };
  const auto crossfadeMode = engine.calculateSpeechRangeBlend(
      950, adjacentRanges, 100, AudioEngine::SpeechFilterFadeMode::Crossfade, 1.0, false);
  QVERIFY(crossfadeMode.primaryGain < 1.0f);
  QVERIFY(crossfadeMode.secondaryGain > 0.0f);
  QCOMPARE(crossfadeMode.secondaryTimelineSample, static_cast<int64_t>(2050));
}

void TestAudioMixPolicy::testExportMixerUsesSharedSpeechFilterFadeBlend() {
  const QString path =
      QStringLiteral("/tmp/jcut_export_speech_fade.wav");
  TimelineClip clip =
      makeAudioClip(QStringLiteral("speech-fade"), path, 0, 2);
  clip.fadeSamples = 1;

  render_detail::DecodedAudioClip decoded;
  decoded.samples.resize(3200 * 2);
  for (int sample = 0; sample < 3200; ++sample) {
    const float value = sample < 1500 ? 0.25f : 0.5f;
    decoded.samples[sample * 2] = value;
    decoded.samples[sample * 2 + 1] = value;
  }
  decoded.sourceStartSample = 0;
  decoded.fullyDecoded = true;
  decoded.valid = true;
  const QHash<QString, render_detail::DecodedAudioClip> cache{
      {path, decoded}};
  const QVector<editor::speech::SampleRange> ranges{
      {0, 1000},
      {2000, 3000}};
  const editor::speech::RangeBlend expected =
      editor::speech::rangeBlendAtSample(
          950,
          ranges,
          100,
          editor::speech::FadeMode::Crossfade,
          1.0);

  std::array<float, 2> output{};
  render_detail::mixAudioChunk(
      QVector<TimelineClip>{clip},
      {},
      {},
      cache,
      output.data(),
      1,
      950,
      1.0,
      ranges,
      100,
      editor::speech::FadeMode::Crossfade,
      1.0);
  const float expectedSample =
      0.25f * expected.primaryGain +
      0.5f * expected.secondaryGain;
  QVERIFY(std::abs(output[0] - expectedSample) < 0.00001f);
  QVERIFY(std::abs(output[1] - expectedSample) < 0.00001f);
}

void TestAudioMixPolicy::testSpliceSecondaryTapStopsAtClipEnd() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_mix_policy_splice.wav");

  // Clip covers timeline frames [0, 15) -> samples [0, 24000). The second
  // speech range starts at frame 60, where this clip does not exist, so the
  // crossfade's secondary tap must contribute nothing from this clip.
  AudioEngine::MixContext context;
  context.clips.push_back(
      makeAudioClip(QStringLiteral("splice"), path, 0, 15));
  context.exportRanges.push_back(ExportRangeSegment{0, 14});
  context.exportRanges.push_back(ExportRangeSegment{60, 74});
  context.volume = 1.0;

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_audioCache.insert(path, makeCacheEntry(0, 24000, 0.25f));
  }
  engine.m_speechFilterRangeCrossfadeEnabled.store(true);
  engine.m_speechFilterFadeSamples.store(4800);

  // Mix the tail of the first range, deep inside the crossfade window
  // [19200, 24000). At the last frame the primary gain is ~cos(pi/2)=0 and
  // the secondary gain is ~1; with the secondary tap outside the clip's
  // extent the output must be near silent instead of replaying clip audio.
  const int frames = 1024;
  const int64_t chunkStart = 24000 - frames;
  QVector<float> output(frames * 2, 0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), frames, chunkStart, 1.0, 1.0));
  QCOMPARE(engine.m_lastMixStarvedClipCount.load(), 0);

  const float lastFrameValue = std::abs(output[(frames - 1) * 2]);
  QVERIFY2(lastFrameValue < 0.01f,
           qPrintable(QStringLiteral("expected near-silence at crossfade "
                                     "tail, got %1")
                          .arg(lastFrameValue)));
}

void TestAudioMixPolicy::testAudioDynamicsAreAppliedPerClipBeforeMix() {
  AudioEngine engine;
  const QString pathA = QStringLiteral("/tmp/jcut_mix_policy_dynamics_a.wav");
  const QString pathB = QStringLiteral("/tmp/jcut_mix_policy_dynamics_b.wav");

  AudioEngine::MixContext context;
  TimelineClip clipA = makeAudioClip(QStringLiteral("a"), pathA, 0, 30);
  clipA.audioDynamicsSet = true;
  clipA.audioDynamics.amplifyEnabled = true;
  clipA.audioDynamics.amplifyDb = 6.0206;
  TimelineClip clipB = makeAudioClip(QStringLiteral("b"), pathB, 0, 30);
  context.clips.push_back(clipA);
  context.clips.push_back(clipB);
  context.tracks.resize(1);
  context.volume = 1.0;

  PreviewSurface::AudioDynamicsSettings obsoleteGlobal;
  obsoleteGlobal.amplifyEnabled = true;
  obsoleteGlobal.amplifyDb = 12.0;
  engine.setAudioDynamicsSettings(obsoleteGlobal);

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_audioCache.insert(pathA, makeCacheEntry(0, 48000, 0.1f));
    engine.m_audioCache.insert(pathB, makeCacheEntry(0, 48000, 0.2f));
  }

  QVector<float> output(1024 * 2, 0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));
  QVERIFY(std::abs(output[512 * 2] - 0.4f) < 0.02f);
  QVERIFY(std::abs(output[512 * 2 + 1] - 0.4f) < 0.02f);
}

void TestAudioMixPolicy::testTrackGainMuteAndSoloAffectMix() {
  AudioEngine engine;
  const QString pathA = QStringLiteral("/tmp/jcut_mix_policy_track_a.wav");
  const QString pathB = QStringLiteral("/tmp/jcut_mix_policy_track_b.wav");

  AudioEngine::MixContext context;
  TimelineClip clipA = makeAudioClip(QStringLiteral("a"), pathA, 0, 30);
  clipA.trackIndex = 0;
  TimelineClip clipB = makeAudioClip(QStringLiteral("b"), pathB, 0, 30);
  clipB.trackIndex = 1;
  context.clips.push_back(clipA);
  context.clips.push_back(clipB);
  context.tracks.resize(2);
  context.tracks[0].audioGain = 0.5;
  context.tracks[1].audioGain = 1.0;
  context.volume = 1.0;

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_audioCache.insert(pathA, makeCacheEntry(0, 48000, 0.4f));
    engine.m_audioCache.insert(pathB, makeCacheEntry(0, 48000, 0.2f));
  }

  QVector<float> output(1024 * 2, 0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));
  QVERIFY(std::abs(output[512 * 2] - 0.4f) < 0.01f);

  context.tracks[1].audioMuted = true;
  output.fill(0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));
  QVERIFY(std::abs(output[512 * 2] - 0.2f) < 0.01f);

  context.tracks[1].audioMuted = false;
  context.tracks[1].audioSolo = true;
  output.fill(0.0f);
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0, 1.0));
  QVERIFY(std::abs(output[512 * 2] - 0.2f) < 0.01f);
}

void TestAudioMixPolicy::testExportMixerUsesPreviewClipFadePolicy() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_export_clip_fade.wav");
  TimelineClip clip =
      makeAudioClip(QStringLiteral("export-fade"), path, 0, 2);
  const int64_t clipStartSample = clipTimelineStartSamples(clip);
  const int64_t clipEndSample = clipTimelineEndSamples(clip);

  render_detail::DecodedAudioClip decoded;
  decoded.samples = QVector<float>(
      static_cast<int>(clipEndSample - clipStartSample) * 2, 0.5f);
  decoded.sourceStartSample = 0;
  decoded.fullyDecoded = true;
  decoded.valid = true;
  const QHash<QString, render_detail::DecodedAudioClip> cache{{path, decoded}};

  for (const int configuredFadeSamples : {100, 0}) {
    clip.fadeSamples = configuredFadeSamples;
    const int effectiveFadeSamples =
        editor::audio::effectiveClipFadeSamples(configuredFadeSamples);
    const std::array<int64_t, 5> positions{
        clipStartSample,
        clipStartSample + (effectiveFadeSamples / 2),
        clipStartSample + effectiveFadeSamples,
        clipEndSample - (effectiveFadeSamples / 2),
        clipEndSample - 1};

    for (const int64_t samplePosition : positions) {
      std::array<float, 2> output{};
      render_detail::mixAudioChunk(
          QVector<TimelineClip>{clip}, {}, {}, cache, output.data(), 1,
          samplePosition, 1.0);
      const float previewGain = engine.calculateClipCrossfadeGain(
          samplePosition, clip, clipStartSample, clipEndSample,
          effectiveFadeSamples);
      QVERIFY2(std::abs(output[0] - (0.5f * previewGain)) < 0.00001f,
               "export mix must use the same clip fade gain as preview");
      QVERIFY2(std::abs(output[1] - (0.5f * previewGain)) < 0.00001f,
               "export mix must apply the fade equally to both channels");
    }
  }
}

void TestAudioMixPolicy::testTimelineStateAtFrameRejectsStaleMixerGeneration() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_mix_generation.wav");
  const TimelineClip clip =
      makeAudioClip(QStringLiteral("generation"), path, 0, 120);
  const QVector<TimelineTrack> tracks(1);
  const QVector<TimelineClip> clips{clip};
  const QVector<ExportRangeSegment> ranges{{0, 119}};
  const QVector<RenderSyncMarker> markers;

  uint64_t staleGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    engine.m_playing = true;
    staleGeneration = engine.m_mixGeneration;
  }

  engine.setTimelineStateAtFrame(tracks, clips, ranges, markers, 42);
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QCOMPARE(engine.m_ringBufferEndSample.load(), frameToSamples(42));
  QCOMPARE(engine.scheduledAudioSourcePaths(), QVector<QString>{path});

  const QVector<int16_t> stalePcm(16, 123);
  QVERIFY(!engine.commitMixedChunk(staleGeneration, stalePcm.constData(),
                                   static_cast<size_t>(stalePcm.size()),
                                   frameToSamples(43)));
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QCOMPARE(engine.m_ringBufferEndSample.load(), frameToSamples(42));

  uint64_t currentGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    currentGeneration = engine.m_mixGeneration;
  }
  QVERIFY(engine.commitMixedChunk(currentGeneration, stalePcm.constData(),
                                  static_cast<size_t>(stalePcm.size()),
                                  frameToSamples(43)));
  QCOMPARE(engine.m_ringBuffer.available(),
           static_cast<size_t>(stalePcm.size()));

  engine.seekToTimelineSample(frameToSamples(10));
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QVERIFY(!engine.commitMixedChunk(currentGeneration, stalePcm.constData(),
                                   static_cast<size_t>(stalePcm.size()),
                                   frameToSamples(44)));
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QCOMPARE(engine.m_ringBufferEndSample.load(), frameToSamples(10));
}

void TestAudioMixPolicy::
    testAudioSourceCacheInvalidationVersionsQueuedWork() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_source_generation.wav");
  const TimelineClip clip =
      makeAudioClip(QStringLiteral("source-generation"), path, 0, 120);
  engine.setTimelineStateAtFrame(
      QVector<TimelineTrack>(1), QVector<TimelineClip>{clip},
      QVector<ExportRangeSegment>{{0, 119}}, QVector<RenderSyncMarker>{}, 24);

  uint64_t previousSourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    previousSourceGeneration = engine.m_sourceGeneration;
    engine.m_audioCache.insert(path, makeCacheEntry(0, 48000, 0.25f));
    engine.m_timeStretchAudioCache[path][1000].push_back(
        makeCacheEntry(0, 48000, 0.25f));
    engine.m_timeStretchSidecarEntryCache.insert(
        QStringLiteral("stale-sidecar"), AudioTimeStretchCacheEntry{});
  }
  const std::array<int16_t, 4> stalePcm{{1, 2, 3, 4}};
  QCOMPARE(engine.m_ringBuffer.write(stalePcm.data(), stalePcm.size()),
           stalePcm.size());

  engine.invalidateAudioSourceCaches();

  std::lock_guard<std::mutex> lock(engine.m_stateMutex);
  QCOMPARE(engine.m_sourceGeneration, previousSourceGeneration + 1);
  QVERIFY(engine.m_audioCache.isEmpty());
  QVERIFY(engine.m_timeStretchAudioCache.isEmpty());
  QVERIFY(engine.m_timeStretchSidecarEntryCache.isEmpty());
  QCOMPARE(engine.m_ringBuffer.available(), size_t{0});
  QCOMPARE(engine.m_timelineSampleCursor,
           engine.m_audioClockSample.load(std::memory_order_acquire));
  QVERIFY(!engine.m_pendingDecodePaths.empty());
  for (const AudioEngine::DecodeTask& task : engine.m_pendingDecodePaths) {
    QCOMPARE(task.sourceGeneration, engine.m_sourceGeneration);
  }
}

void TestAudioMixPolicy::
    testStaleTimeStretchStateRejectedAfterSourceInvalidation() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_stale_time_stretch.wav");
  constexpr int speedKey = 1500;

  uint64_t staleSourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    staleSourceGeneration = engine.m_sourceGeneration;
  }
  QCOMPARE(engine.beginTimeStretchJobAttempt(path, speedKey,
                                              staleSourceGeneration),
           1);
  QVERIFY(engine.beginTimeStretchGenerationForSourceGeneration(
      path, speedKey, 48000, staleSourceGeneration));
  QVERIFY(engine.updateTimeStretchGenerationForSourceGeneration(
      path, speedKey, AudioEngine::TimeStretchJobWritingSidecar, 0.98,
      AudioEngine::TimeStretchGenerationWritingSidecar,
      staleSourceGeneration));

  engine.invalidateAudioSourceCaches();

  uint64_t currentSourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    currentSourceGeneration = engine.m_sourceGeneration;
  }
  QCOMPARE(currentSourceGeneration, staleSourceGeneration + 1);

  // These calls model a sidecar progress callback and terminal write result
  // arriving after invalidation. Neither may recreate current-generation
  // jobs or failures.
  QVERIFY(!engine.updateTimeStretchGenerationForSourceGeneration(
      path, speedKey, AudioEngine::TimeStretchJobWritingSidecar, 0.99,
      AudioEngine::TimeStretchGenerationWritingSidecar,
      staleSourceGeneration));
  QVERIFY(!engine.finishTimeStretchGenerationForSourceGeneration(
      path, speedKey, false, 0, QStringLiteral("sidecar_write_failed"),
      QStringLiteral("stale_sidecar_write_result"), staleSourceGeneration));
  QVERIFY(!engine.markTimeStretchJobForSourceGeneration(
      path, speedKey, AudioEngine::TimeStretchJobFailed, 0.0,
      staleSourceGeneration));
  {
    std::lock_guard<std::mutex> lock(engine.m_timeStretchGenerationMutex);
    QVERIFY(engine.m_timeStretchJobs.isEmpty());
    QVERIFY(engine.m_timeStretchFailedJobs.isEmpty());
    QVERIFY(!engine.m_timeStretchGenerationActive.load());
    QCOMPARE(engine.m_timeStretchGenerationSourceGeneration, uint64_t{0});
    QCOMPARE(engine.m_timeStretchGenerationLastEndReason,
             QStringLiteral("source_generation_invalidated"));
  }

  // A stale worker abandoning its telemetry must not clear telemetry owned
  // by work started after the source-generation change.
  QCOMPARE(engine.beginTimeStretchJobAttempt(path, speedKey,
                                              currentSourceGeneration),
           1);
  QVERIFY(engine.beginTimeStretchGenerationForSourceGeneration(
      path, speedKey, 96000, currentSourceGeneration));
  engine.abandonTimeStretchGenerationForSourceGeneration(
      path, speedKey, staleSourceGeneration);
  {
    std::lock_guard<std::mutex> lock(engine.m_timeStretchGenerationMutex);
    QVERIFY(engine.m_timeStretchGenerationActive.load());
    QCOMPARE(engine.m_timeStretchGenerationSourceGeneration,
             currentSourceGeneration);
    QCOMPARE(engine.m_timeStretchGenerationPhase.load(),
             static_cast<int>(AudioEngine::TimeStretchGenerationReadingSidecar));
  }
  QVERIFY(engine.finishTimeStretchGenerationForSourceGeneration(
      path, speedKey, true, 64000,
      QStringLiteral("generated_and_sidecar_committed"), QString(),
      currentSourceGeneration));
  {
    std::lock_guard<std::mutex> lock(engine.m_timeStretchGenerationMutex);
    const QString key = AudioEngine::timeStretchJobKey(path, speedKey);
    QVERIFY(engine.m_timeStretchJobs.contains(key));
    QCOMPARE(engine.m_timeStretchJobs.value(key).state,
             static_cast<int>(AudioEngine::TimeStretchJobComplete));
    QCOMPARE(engine.m_timeStretchJobs.value(key).sourceGeneration,
             currentSourceGeneration);
    QVERIFY(!engine.m_timeStretchFailedJobs.contains(key));
    QVERIFY(!engine.m_timeStretchGenerationActive.load());
  }
}

void TestAudioMixPolicy::
    testWarmupPermanentFailureIsSourceGenerationScoped() {
  AudioEngine engine;
  const QString path = QStringLiteral("/tmp/jcut_warmup_failure.wav");
  const TimelineClip clip =
      makeAudioClip(QStringLiteral("warmup-failure"), path, 0, 120);
  engine.setTimelineStateAtFrame(
      QVector<TimelineTrack>(1), QVector<TimelineClip>{clip},
      QVector<ExportRangeSegment>{{0, 119}}, QVector<RenderSyncMarker>{}, 0);
  const QString sourceKey = engine.clipAudioPathForScheduling(clip);

  uint64_t failedSourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    failedSourceGeneration = engine.m_sourceGeneration;
    QVERIFY(engine.recordDecodeResultLocked(sourceKey, failedSourceGeneration,
                                             false));
  }
  QVERIFY(engine.playbackAudioWarmupPermanentlyFailed(0));

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    QVERIFY(engine.recordDecodeResultLocked(sourceKey, failedSourceGeneration,
                                             true));
  }
  QVERIFY(!engine.playbackAudioWarmupPermanentlyFailed(0));

  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    QVERIFY(engine.recordDecodeResultLocked(sourceKey, failedSourceGeneration,
                                             false));
  }
  engine.invalidateAudioSourceCaches();
  uint64_t currentSourceGeneration = 0;
  {
    std::lock_guard<std::mutex> lock(engine.m_stateMutex);
    currentSourceGeneration = engine.m_sourceGeneration;
    QVERIFY(engine.m_failedDecodePaths.isEmpty());
    QVERIFY(!engine.recordDecodeResultLocked(sourceKey, failedSourceGeneration,
                                              false));
  }
  QVERIFY(!engine.playbackAudioWarmupPermanentlyFailed(0));

  engine.setPlaybackWarpMode(PlaybackAudioWarpMode::TimeStretch);
  engine.setPlaybackRate(1.5);
  const int speedKey = AudioEngine::precomputedTimeStretchSpeedKey(
      1.5, PlaybackAudioWarpMode::TimeStretch);
  QCOMPARE(engine.beginTimeStretchJobAttempt(sourceKey, speedKey,
                                              currentSourceGeneration),
           1);
  QVERIFY(engine.beginTimeStretchGenerationForSourceGeneration(
      sourceKey, speedKey, 48000, currentSourceGeneration));
  QVERIFY(engine.finishTimeStretchGenerationForSourceGeneration(
      sourceKey, speedKey, false, 0,
      QStringLiteral("rubberband_empty_output"),
      QStringLiteral("rubberband_empty_output"), currentSourceGeneration));

  engine.m_timeStretchReadinessState.store(
      AudioEngine::TimeStretchReadinessQueuedPrecompute);
  QVERIFY(!engine.playbackAudioWarmupPermanentlyFailed(0));
  engine.m_timeStretchReadinessState.store(
      AudioEngine::TimeStretchReadinessMissing);
  QVERIFY(engine.playbackAudioWarmupPermanentlyFailed(0));

  engine.invalidateAudioSourceCaches();
  QVERIFY(!engine.playbackAudioWarmupPermanentlyFailed(0));
}

void TestAudioMixPolicy::
    testRingBufferClearWaitsForReaderAndRejectsReadsDuringReset() {
  AudioRingBuffer ring;
  const std::array<int16_t, 4> buffered{{1, 2, 3, 4}};
  QCOMPARE(ring.write(buffered.data(), buffered.size()), buffered.size());

  // Hold the consumer side active so clear() deterministically reaches its
  // rendezvous and waits rather than relying on scheduler timing to overlap
  // a short read operation.
  ring.m_readerActive.store(true, std::memory_order_seq_cst);
  std::atomic<bool> clearDone{false};
  std::thread clearer([&] {
    ring.clear();
    clearDone.store(true, std::memory_order_release);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!ring.m_resetting.load(std::memory_order_seq_cst) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  const bool resetObserved =
      ring.m_resetting.load(std::memory_order_seq_cst);
  const bool completedWhileReaderActive =
      clearDone.load(std::memory_order_acquire);
  int16_t sample = -1;
  const size_t readDuringReset = ring.read(&sample, 1);

  // A write can race a reset, but clear's linearization point must discard
  // every sample published before it completes.
  const std::array<int16_t, 2> writtenDuringReset{{5, 6}};
  const size_t writeDuringReset =
      ring.write(writtenDuringReset.data(), writtenDuringReset.size());

  // Always release and join before making Qt assertions: an early-returning
  // assertion with a joinable std::thread would terminate the test process.
  ring.m_readerActive.store(false, std::memory_order_seq_cst);
  clearer.join();

  QVERIFY(resetObserved);
  QVERIFY(!completedWhileReaderActive);
  QCOMPARE(readDuringReset, size_t{0});
  QCOMPARE(sample, int16_t{-1});
  QCOMPARE(writeDuringReset, writtenDuringReset.size());
  QCOMPARE(ring.available(), size_t{0});
  QCOMPARE(ring.space(), AudioRingBuffer::kCapacity);

  const std::array<int16_t, 3> fresh{{7, 8, 9}};
  std::array<int16_t, 3> output{};
  QCOMPARE(ring.write(fresh.data(), fresh.size()), fresh.size());
  QCOMPARE(ring.read(output.data(), output.size()), output.size());
  QVERIFY(output == fresh);
}

QTEST_MAIN(TestAudioMixPolicy)
#include "test_audio_mix_policy.moc"
