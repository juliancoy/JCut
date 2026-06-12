#include "../audio_engine.h"
#include "../audio_mix_readiness.h"

#include <QtTest/QtTest>

// Multi-stream mix readiness policy (TIME.md "Multi-Stream Audio Readiness"):
// a starved clip degrades per-clip instead of stalling ready clips, and the
// chunk blocks only when no active clip can contribute.
class TestAudioMixPolicy : public QObject {
  Q_OBJECT

private slots:
  void testPolicyHelpers();
  void testStarvedClipDoesNotBlockReadyClip();
  void testOnlyStarvedClipBlocksChunk();
  void testSpliceSecondaryTapStopsAtClipEnd();

private:
  static TimelineClip makeAudioClip(const QString &id, const QString &path,
                                    int64_t startFrame,
                                    int64_t durationFrames);
  static AudioEngine::AudioClipCacheEntry
  makeCacheEntry(int64_t sourceStartSample, int64_t frameCount, float value);
};

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
  QVERIFY(engine.mixChunk(context, output.data(), 1024, 0, 1.0));

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
  QVERIFY(!engine.mixChunk(context, output.data(), 1024, 0, 1.0));
  QVERIFY(engine.m_audioPlaybackBlocked.load());
  QCOMPARE(engine.m_lastMixStarvedClipCount.load(), 0);
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
  QVERIFY(engine.mixChunk(context, output.data(), frames, chunkStart, 1.0));
  QCOMPARE(engine.m_lastMixStarvedClipCount.load(), 0);

  const float lastFrameValue = std::abs(output[(frames - 1) * 2]);
  QVERIFY2(lastFrameValue < 0.01f,
           qPrintable(QStringLiteral("expected near-silence at crossfade "
                                     "tail, got %1")
                          .arg(lastFrameValue)));
}

QTEST_MAIN(TestAudioMixPolicy)
#include "test_audio_mix_policy.moc"
