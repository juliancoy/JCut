#include "editor.h"
#include "debug_controls.h"
#include "playback_clock_coordinator.h"
#include "playback_debug.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace editor {

namespace {
bool shouldSkipBlockingAudioWarmup(PlaybackAudioWarpMode runtimeWarpMode,
                                   qreal effectiveWarpRate,
                                   bool needsPitchPreservingAudio)
{
    return runtimeWarpMode == PlaybackAudioWarpMode::TimeStretch &&
           !needsPitchPreservingAudio &&
           qAbs(effectiveWarpRate - 1.0) < 0.0001;
}
}

void EditorWindow::advanceFrame()
{
    if (!m_timeline) return;
    editor::accumulatePlaybackStageMetric(&m_playbackClockStageMetric,
                                  1,
                                  0,
                                  0,
                                  QStringLiteral("tick"),
                                  QStringLiteral("advance_frame"));
    const qint64 tickNowMs = nowMs();
    const bool pitchPreservingAudioRequired = needsPitchPreservingPlaybackAudio();
    const bool audioBlocked =
        pitchPreservingAudioRequired && m_audioEngine && m_audioEngine->playbackAudioBlocked();
    const bool audioReady =
        !pitchPreservingAudioRequired ||
        (m_audioEngine && m_audioEngine->playbackAudioReadyForFrame(m_timeline->currentFrame()));
    if (pitchPreservingAudioRequired && (audioBlocked || !audioReady)) {
        editor::accumulatePlaybackStageMetric(&m_playbackClockStageMetric,
                                      0,
                                      0,
                                      1,
                                      QStringLiteral("source_unavailable"),
                                      audioBlocked
                                          ? QStringLiteral("audio_blocked")
                                          : QStringLiteral("retimed_audio_not_ready"));
        if (debugPlaybackWarnEnabled()) {
            qWarning().noquote()
                << QStringLiteral("[PLAYBACK WARN] transport playback continuing while pitch-preserving audio warms: %1")
                       .arg(audioBlocked
                                ? QStringLiteral("audio_blocked")
                                : QStringLiteral("retimed_audio_not_ready"));
        }
        requestPlaybackAudioWarmup(false);
    }

    editor::accumulatePlaybackStageMetric(&m_playbackClockStageMetric,
                                  0,
                                  1,
                                  0,
                                  QStringLiteral("monotonic_transport"),
                                  QStringLiteral("system_clock_transport"));
    if (m_lastTimelineAdvanceTickMs <= 0) {
        m_lastTimelineAdvanceTickMs = tickNowMs;
    }
    const qint64 elapsedMs = qMax<qint64>(0, tickNowMs - m_lastTimelineAdvanceTickMs);
    m_lastTimelineAdvanceTickMs = tickNowMs;

    const qreal speed = normalizedPlaybackSpeed(m_playbackSpeed);
    const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
    m_timelineAdvanceCarrySamples +=
        elapsedSeconds * static_cast<double>(kAudioSampleRate) * static_cast<double>(speed);
    int64_t deltaSamples = static_cast<int64_t>(std::floor(m_timelineAdvanceCarrySamples));
    m_timelineAdvanceCarrySamples -= static_cast<double>(deltaSamples);
    if (deltaSamples <= 0) {
        deltaSamples = qMax<int64_t>(
            1, static_cast<int64_t>(std::llround(speed * static_cast<qreal>(kSamplesPerFrame))));
        m_timelineAdvanceCarrySamples = 0.0;
    }

    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    const int64_t currentSample = m_transportTimelineSample;
    bool reachedEnd = false;
    const int64_t nextSample = nextPlaybackSample(currentSample, deltaSamples, ranges, &reachedEnd);
    if (reachedEnd && m_playbackLoopEnabled) {
        const int64_t loopStartFrame =
            ranges.isEmpty() ? 0 : qMax<int64_t>(0, ranges.constFirst().startFrame);
        const int64_t loopStartSample = frameToSamples(loopStartFrame);
        m_timelineAdvanceCarrySamples = 0.0;
        m_lastTimelineAdvanceTickMs = tickNowMs;
        if (m_preview) {
            m_preview->preparePlaybackAdvance(loopStartFrame);
        }
        setCurrentPlaybackSample(loopStartSample, true, true);
        return;
    }

    if (m_preview) {
        const int64_t nextFrame =
            qBound<int64_t>(0,
                            static_cast<int64_t>(std::floor(samplesToFramePosition(nextSample))),
                            m_timeline->totalFrames());
        m_preview->preparePlaybackAdvance(nextFrame);
    }
    setCurrentPlaybackSample(nextSample, false, true);
    updateAudioDriftRetime();
    if (reachedEnd) {
        stopPlaybackWithReason(QStringLiteral("range_end"));
    }
}

bool EditorWindow::speechFilterPlaybackEnabled() const
{
    return m_speechFilterEnabled;
}

int64_t EditorWindow::filteredPlaybackSampleForAbsoluteSample(int64_t absoluteSample) const
{
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) return qMax<int64_t>(0, absoluteSample);

    int64_t filteredSample = 0;
    for (const ExportRangeSegment &range : ranges) {
        const int64_t rangeStartSample = frameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive = frameToSamples(range.endFrame + 1);
        if (absoluteSample <= rangeStartSample) return filteredSample;
        if (absoluteSample < rangeEndSampleExclusive) return filteredSample + (absoluteSample - rangeStartSample);
        filteredSample += (rangeEndSampleExclusive - rangeStartSample);
    }
    return filteredSample;
}

int64_t EditorWindow::timelineSampleForFilteredPlaybackSample(int64_t filteredSample) const
{
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) {
        return qMax<int64_t>(0, filteredSample);
    }

    int64_t remaining = qMax<int64_t>(0, filteredSample);
    for (const ExportRangeSegment& range : ranges) {
        const int64_t rangeStartSample = frameToSamples(range.startFrame);
        const int64_t rangeEndSampleExclusive = frameToSamples(range.endFrame + 1);
        const int64_t rangeLength = qMax<int64_t>(0, rangeEndSampleExclusive - rangeStartSample);
        if (remaining < rangeLength) {
            return rangeStartSample + remaining;
        }
        remaining -= rangeLength;
    }

    const ExportRangeSegment& last = ranges.constLast();
    return frameToSamples(last.endFrame + 1);
}

int64_t EditorWindow::timelineSampleForAudioFeedbackSample(int64_t audioFeedbackSample) const
{
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    return editor::projectAudioFeedbackSampleToTimelineSample(
        audioFeedbackSample,
        m_playbackAudioFeedbackAnchorTimelineSample,
        m_playbackAudioFeedbackAnchorFeedbackSample,
        ranges);
}

QVector<ExportRangeSegment> EditorWindow::effectivePlaybackRanges() const
{
    if (!m_timeline) return {};
    const QString signature = playbackRangeCacheSignature(false);
    if (m_effectivePlaybackRangesCacheSignature == signature) {
        return m_effectivePlaybackRangesCache;
    }

    QVector<ExportRangeSegment> ranges = m_timeline->exportRanges();
    if (speechFilterPlaybackEnabled()) {
        ranges = m_transcriptEngine.transcriptWordExportRanges(ranges,
                                                               m_timeline->clips(),
                                                               m_timeline->renderSyncMarkers(),
                                                               m_transcriptPrependMs,
                                                               m_transcriptPostpendMs);
    }
    if (ranges.isEmpty()) {
        return ranges;
    }

    std::sort(ranges.begin(), ranges.end(), [](const ExportRangeSegment& a, const ExportRangeSegment& b) {
        if (a.startFrame != b.startFrame) {
            return a.startFrame < b.startFrame;
        }
        return a.endFrame < b.endFrame;
    });

    QVector<ExportRangeSegment> normalized;
    normalized.reserve(ranges.size());
    for (const ExportRangeSegment& range : ranges) {
        if (range.endFrame < range.startFrame) {
            continue;
        }
        if (normalized.isEmpty()) {
            normalized.push_back(range);
            continue;
        }
        ExportRangeSegment& last = normalized.last();
        if (range.startFrame <= (last.endFrame + 1)) {
            last.endFrame = qMax(last.endFrame, range.endFrame);
        } else {
            normalized.push_back(range);
        }
    }
    m_effectivePlaybackRangesCacheSignature = signature;
    m_effectivePlaybackRangesCache = normalized;
    return m_effectivePlaybackRangesCache;
}

QVector<ExportRangeSegment> EditorWindow::applySpeechFilterToExportRanges(
    const QVector<ExportRangeSegment>& ranges) const
{
    if (!m_timeline || ranges.isEmpty() || !speechFilterPlaybackEnabled()) {
        return ranges;
    }

    return m_transcriptEngine.transcriptWordExportRanges(ranges,
                                                        m_timeline->clips(),
                                                        m_timeline->renderSyncMarkers(),
                                                        m_transcriptPrependMs,
                                                        m_transcriptPostpendMs);
}

QVector<ExportRangeSegment> EditorWindow::effectiveTranscriptNormalizeRanges() const
{
    if (!m_timeline) {
        return {};
    }
    const int neighborWordRadius = speechFilterPlaybackEnabled() ? 10 : 0;
    const QString signature = playbackRangeCacheSignature(true, neighborWordRadius);
    if (m_effectiveTranscriptNormalizeRangesCacheSignature == signature) {
        return m_effectiveTranscriptNormalizeRangesCache;
    }

    m_effectiveTranscriptNormalizeRangesCache = m_transcriptEngine.transcriptWordExportRangesDiscrete(
        m_timeline->exportRanges(),
        m_timeline->clips(),
        m_timeline->renderSyncMarkers(),
        m_transcriptPrependMs,
        m_transcriptPostpendMs,
        neighborWordRadius);
    m_effectiveTranscriptNormalizeRangesCacheSignature = signature;
    return m_effectiveTranscriptNormalizeRangesCache;
}

QString EditorWindow::playbackRangeCacheSignature(bool discrete, int neighborWordRadius) const
{
    if (!m_timeline) {
        return QStringLiteral("no_timeline");
    }
    QString signature;
    signature.reserve(512);
    signature += discrete ? QStringLiteral("discrete|") : QStringLiteral("effective|");
    signature += QStringLiteral("speech=%1|pre=%2|post=%3|radius=%4|")
                     .arg(speechFilterPlaybackEnabled() ? 1 : 0)
                     .arg(m_transcriptPrependMs)
                     .arg(m_transcriptPostpendMs)
                     .arg(qBound(0, neighborWordRadius, 10));
    for (const ExportRangeSegment& range : m_timeline->exportRanges()) {
        signature += QStringLiteral("range:%1-%2|").arg(range.startFrame).arg(range.endFrame);
    }
    for (const RenderSyncMarker& marker : m_timeline->renderSyncMarkers()) {
        signature += QStringLiteral("marker:%1:%2:%3:%4|")
                         .arg(marker.clipId)
                         .arg(marker.frame)
                         .arg(static_cast<int>(marker.action))
                         .arg(marker.count);
    }
    for (const TimelineClip& clip : m_timeline->clips()) {
        const QFileInfo transcriptInfo(m_transcriptEngine.transcriptPathForClip(clip));
        signature += QStringLiteral("clip:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10|")
                         .arg(clip.id)
                         .arg(clip.startFrame)
                         .arg(clip.startSubframeSamples)
                         .arg(clip.durationFrames)
                         .arg(clip.sourceInFrame)
                         .arg(clip.sourceInSubframeSamples)
                         .arg(clip.sourceDurationFrames)
                         .arg(clip.playbackRate, 0, 'g', 12)
                         .arg(clip.filePath)
                         .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : -1);
    }
    return signature;
}

void EditorWindow::invalidatePlaybackRangeCaches()
{
    m_effectivePlaybackRangesCacheSignature.clear();
    m_effectivePlaybackRangesCache.clear();
    m_effectiveTranscriptNormalizeRangesCacheSignature.clear();
    m_effectiveTranscriptNormalizeRangesCache.clear();
}

void EditorWindow::scheduleTranscriptNormalizeRangeRefresh(int delayMs)
{
    if (!m_previewAudioDynamics.transcriptNormalizeEnabled) {
        ++m_transcriptNormalizeRefreshGeneration;
        m_transcriptNormalizeRefreshTimer.stop();
        if (m_audioEngine) {
            m_audioEngine->setTranscriptNormalizeRanges({});
        }
        return;
    }
    ++m_transcriptNormalizeRefreshGeneration;
    const int clampedDelayMs = qMax(0, delayMs);
    if (m_transcriptNormalizeRefreshWatcher.isRunning()) {
        if (clampedDelayMs > 0) {
            m_transcriptNormalizeRefreshTimer.start(clampedDelayMs);
        }
        return;
    }
    if (clampedDelayMs <= 0) {
        startTranscriptNormalizeRangeRefresh();
        return;
    }
    m_transcriptNormalizeRefreshTimer.start(clampedDelayMs);
}

void EditorWindow::startTranscriptNormalizeRangeRefresh()
{
    m_transcriptNormalizeRefreshTimer.stop();
    if (m_transcriptNormalizeRefreshWatcher.isRunning()) {
        return;
    }
    if (!m_previewAudioDynamics.transcriptNormalizeEnabled) {
        m_appliedTranscriptNormalizeRefreshGeneration = m_transcriptNormalizeRefreshGeneration;
        if (m_audioEngine) {
            m_audioEngine->setTranscriptNormalizeRanges({});
        }
        return;
    }
    if (!m_timeline) {
        m_appliedTranscriptNormalizeRefreshGeneration = m_transcriptNormalizeRefreshGeneration;
        if (m_audioEngine) {
            m_audioEngine->setTranscriptNormalizeRanges({});
        }
        return;
    }

    const qint64 generation = m_transcriptNormalizeRefreshGeneration;
    const QVector<ExportRangeSegment> baseRanges = m_timeline->exportRanges();
    const QVector<TimelineClip> clips = m_timeline->clips();
    const QVector<RenderSyncMarker> markers = m_timeline->renderSyncMarkers();
    const int transcriptPrependMs = m_transcriptPrependMs;
    const int transcriptPostpendMs = m_transcriptPostpendMs;
    const int neighborWordRadius = speechFilterPlaybackEnabled() ? 10 : 0;
    const QString normalizeSignature = playbackRangeCacheSignature(true, neighborWordRadius);
    if (m_effectiveTranscriptNormalizeRangesCacheSignature == normalizeSignature) {
        m_appliedTranscriptNormalizeRefreshGeneration = generation;
        if (m_audioEngine) {
            m_audioEngine->setTranscriptNormalizeRanges(m_effectiveTranscriptNormalizeRangesCache);
        }
        return;
    }

    m_transcriptNormalizeRefreshWatcher.setProperty("generation", generation);
    m_transcriptNormalizeRefreshWatcher.setProperty("signature", normalizeSignature);
    m_transcriptNormalizeRefreshWatcher.setFuture(QtConcurrent::run(
        [baseRanges, clips, markers, transcriptPrependMs, transcriptPostpendMs, neighborWordRadius]() {
            TranscriptEngine engine;
            return engine.transcriptWordExportRangesDiscrete(
                baseRanges,
                clips,
                markers,
                transcriptPrependMs,
                transcriptPostpendMs,
                neighborWordRadius);
        }));
}

int64_t EditorWindow::nextPlaybackFrame(int64_t currentFrame) const
{
    if (!m_timeline) return 0;

    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) {
        const int64_t nextFrame = currentFrame + 1;
        return qMin<int64_t>(m_timeline->totalFrames(), nextFrame);
    }

    for (const ExportRangeSegment &range : ranges) {
        if (currentFrame < range.startFrame) return range.startFrame;
        if (currentFrame >= range.startFrame && currentFrame < range.endFrame) return currentFrame + 1;
    }
    return qMin<int64_t>(m_timeline->totalFrames(), ranges.constLast().endFrame);
}

int64_t EditorWindow::nextPlaybackSample(int64_t currentSample,
                                         int64_t deltaSamples,
                                         const QVector<ExportRangeSegment>& ranges,
                                         bool* reachedEnd) const
{
    if (reachedEnd) {
        *reachedEnd = false;
    }
    if (!m_timeline) {
        return qMax<int64_t>(0, currentSample + qMax<int64_t>(0, deltaSamples));
    }
    const int64_t totalSamples = frameToSamples(m_timeline->totalFrames());
    if (deltaSamples <= 0) {
        return qBound<int64_t>(0, currentSample, totalSamples);
    }
    if (ranges.isEmpty()) {
        const int64_t next = qBound<int64_t>(0, currentSample + deltaSamples, totalSamples);
        if (reachedEnd && next >= totalSamples) {
            *reachedEnd = true;
        }
        return next;
    }

    auto rangeStartSample = [](const ExportRangeSegment& range) {
        return frameToSamples(range.startFrame);
    };
    auto rangeEndSampleExclusive = [](const ExportRangeSegment& range) {
        return frameToSamples(range.endFrame + 1);
    };

    int64_t sample = qBound<int64_t>(0, currentSample, totalSamples);
    int64_t remaining = deltaSamples;
    const int64_t lastRangeEndExclusive = rangeEndSampleExclusive(ranges.constLast());
    int guard = 0;
    while (remaining > 0 && guard++ < 4096) {
        int activeIndex = -1;
        int nextIndex = -1;
        for (int i = 0; i < ranges.size(); ++i) {
            const int64_t start = rangeStartSample(ranges.at(i));
            const int64_t endExclusive = rangeEndSampleExclusive(ranges.at(i));
            if (sample < start) {
                nextIndex = i;
                break;
            }
            if (sample >= start && sample < endExclusive) {
                activeIndex = i;
                break;
            }
        }

        if (activeIndex < 0) {
            if (nextIndex < 0) {
                sample = qBound<int64_t>(0, lastRangeEndExclusive - 1, totalSamples);
                if (reachedEnd) {
                    *reachedEnd = true;
                }
                break;
            }
            sample = rangeStartSample(ranges.at(nextIndex));
            continue;
        }

        const int64_t endExclusive = rangeEndSampleExclusive(ranges.at(activeIndex));
        const int64_t available = qMax<int64_t>(0, endExclusive - sample);
        if (remaining < available) {
            sample += remaining;
            remaining = 0;
            break;
        }
        remaining -= available;
        const int nextRangeIndex = activeIndex + 1;
        if (nextRangeIndex >= ranges.size()) {
            sample = qBound<int64_t>(0, endExclusive - 1, totalSamples);
            if (reachedEnd) {
                *reachedEnd = true;
            }
            break;
        }
        sample = rangeStartSample(ranges.at(nextRangeIndex));
    }

    return qBound<int64_t>(0, sample, totalSamples);
}

int64_t EditorWindow::stepForwardFrame(int64_t currentFrame) const
{
    if (!m_timeline) return 0;
    const int64_t totalFrames = m_timeline->totalFrames();
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) {
        return qMin<int64_t>(totalFrames, currentFrame + 1);
    }

    for (const ExportRangeSegment& range : ranges) {
        if (currentFrame < range.startFrame) {
            return range.startFrame;
        }
        if (currentFrame >= range.startFrame && currentFrame < range.endFrame) {
            return currentFrame + 1;
        }
    }
    return totalFrames;
}

int64_t EditorWindow::stepBackwardFrame(int64_t currentFrame) const
{
    if (!m_timeline) return 0;
    const QVector<ExportRangeSegment> ranges = effectivePlaybackRanges();
    if (ranges.isEmpty()) {
        return qMax<int64_t>(0, currentFrame - 1);
    }

    for (int i = ranges.size() - 1; i >= 0; --i) {
        const ExportRangeSegment& range = ranges.at(i);
        if (currentFrame > range.endFrame) {
            return range.endFrame;
        }
        if (currentFrame > range.startFrame && currentFrame <= range.endFrame) {
            return currentFrame - 1;
        }
        if (currentFrame == range.startFrame) {
            if (i > 0) {
                return ranges.at(i - 1).endFrame;
            }
            return 0;
        }
    }
    return 0;
}

QString EditorWindow::clipLabelForId(const QString &clipId) const
{
    if (!m_timeline) return clipId;
    for (const TimelineClip &clip : m_timeline->clips()) {
        if (clip.id == clipId) return clip.label;
    }
    return clipId;
}

QColor EditorWindow::clipColorForId(const QString &clipId) const
{
    if (!m_timeline) return QColor(QStringLiteral("#24303c"));
    for (const TimelineClip &clip : m_timeline->clips()) {
        if (clip.id == clipId) return clip.color;
    }
    return QColor(QStringLiteral("#24303c"));
}

void EditorWindow::refreshProfileInspector()
{
    if (m_profileTab) {
        m_profileTab->refresh();
    }
}

void EditorWindow::runDecodeBenchmarkFromProfile()
{
    if (m_profileTab) {
        m_profileTab->runDecodeBenchmark();
    }
}

bool EditorWindow::profileBenchmarkClip(TimelineClip *out) const
{
    if (!out) return false;
    if (!m_timeline) return false;
    const TimelineClip *selected = m_timeline->selectedClip();
    if (selected && clipHasVisuals(*selected)) {
        *out = *selected;
        return true;
    }
    const QVector<TimelineClip> clips = m_timeline->clips();
    for (const TimelineClip &clip : clips) {
        if (clipHasVisuals(clip)) {
            *out = clip;
            return true;
        }
    }
    return false;
}

QStringList EditorWindow::availableHardwareDeviceTypes() const
{
    QStringList types;
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE;
         type = av_hwdevice_iterate_types(type)) {
        if (const char *name = av_hwdevice_get_type_name(type)) {
            types.push_back(QString::fromLatin1(name));
        }
    }
    return types;
}

void EditorWindow::setCurrentPlaybackSample(int64_t samplePosition, bool syncAudio, bool duringPlayback)
{
    QElapsedTimer seekUpdateTimer;
    seekUpdateTimer.start();
    editor::accumulatePlaybackStageMetric(&m_playbackSampleApplyStageMetric,
                                  1,
                                  0,
                                  0,
                                  QStringLiteral("started"),
                                  duringPlayback
                                      ? QStringLiteral("during_playback")
                                      : QStringLiteral("seek_or_idle"));
    const qint64 tickNowMs = nowMs();
    const int64_t boundedSample = qBound<int64_t>(0, samplePosition, frameToSamples(m_timeline->totalFrames()));
    const qreal framePosition = samplesToFramePosition(boundedSample);
    const int64_t bounded = qBound<int64_t>(0, static_cast<int64_t>(std::floor(framePosition)), m_timeline->totalFrames());
    if (!duringPlayback &&
        boundedSample == m_transportTimelineSample &&
        m_timeline &&
        bounded == m_timeline->currentFrame()) {
        editor::accumulatePlaybackStageMetric(&m_playbackSampleApplyStageMetric,
                                      1,
                                      0,
                                      0,
                                      QStringLiteral("skipped"),
                                      QStringLiteral("same_idle_sample"));
        return;
    }
    
    playbackTrace(QStringLiteral("EditorWindow::setCurrentFrame"),
                  QStringLiteral("requestedSample=%1 boundedSample=%2 frame=%3")
                      .arg(samplePosition).arg(boundedSample).arg(framePosition, 0, 'f', 3));
    
    if (!m_timeline || bounded != m_timeline->currentFrame()) {
        m_lastPlayheadAdvanceMs.store(tickNowMs);
    }
    
    m_transportTimelineSample = boundedSample;
    m_filteredPlaybackSample = filteredPlaybackSampleForAbsoluteSample(boundedSample);
    if (!duringPlayback || syncAudio) {
        m_playbackAudioFeedbackAnchorTimelineSample = boundedSample;
        m_playbackAudioFeedbackAnchorFeedbackSample = boundedSample;
    }
    m_fastCurrentFrame.store(bounded);
    
    if (syncAudio && m_audioEngine && m_audioEngine->audioClockAvailable()) {
        m_audioEngine->seek(bounded);
        const int64_t audioAnchorSample = frameToSamples(bounded);
        m_playbackAudioFeedbackAnchorTimelineSample = audioAnchorSample;
        m_playbackAudioFeedbackAnchorFeedbackSample = audioAnchorSample;
    }
    
    m_timeline->setCurrentFrame(bounded);
    m_preview->setCurrentPlaybackSample(boundedSample);
    if (!m_startupReadinessVideoSampleApplied.exchange(true)) {
        startupReadinessMark(QStringLiteral("video.playback_sample_applied"),
                             QJsonObject{{QStringLiteral("frame"), static_cast<qint64>(bounded)},
                                         {QStringLiteral("sample"), static_cast<qint64>(boundedSample)}});
    }
    
    m_ignoreSeekSignal = true;
    m_seekSlider->setValue(static_cast<int>(qMin<int64_t>(bounded, INT_MAX)));
    m_ignoreSeekSignal = false;
    
    m_timecodeLabel->setText(frameToTimecode(bounded));
    
    updateTransportLabels();
    if (duringPlayback) {
        if ((tickNowMs - m_lastPlaybackUiSyncMs) >= m_playbackUiSyncMinIntervalMs) {
            syncTranscriptTableToPlayhead();
            syncKeyframeTableToPlayhead();
            syncGradingTableToPlayhead();
            m_titlesTab->syncTableToPlayhead();
            if (m_inspectorPane && m_inspectorPane->tabs()) {
                const int inspectorIndex = m_inspectorPane->tabs()->currentIndex();
                if (inspectorIndex >= 0 &&
                    m_inspectorPane->tabs()->tabText(inspectorIndex).compare(QStringLiteral("Audio"),
                                                                            Qt::CaseInsensitive) == 0) {
                    refreshAudioInspectorViews();
                }
            }
            m_lastPlaybackUiSyncMs = tickNowMs;
        }
    } else {
        scheduleDeferredInspectorRefresh();
        syncTranscriptTableToPlayhead();
        syncKeyframeTableToPlayhead();
        syncGradingTableToPlayhead();
        m_titlesTab->syncTableToPlayhead();
        if (m_speakersTab) {
            m_speakersTab->syncCurrentSpeakerSentenceToPlayhead(/*duringPlayback=*/false);
        }
        m_lastPlaybackUiSyncMs = tickNowMs;
    }
    if (!duringPlayback || (tickNowMs - m_lastPlaybackStateSaveMs) >= m_playbackStateSaveMinIntervalMs) {
        scheduleSaveState();
        m_lastPlaybackStateSaveMs = tickNowMs;
    }

    const qint64 elapsedMs = seekUpdateTimer.elapsed();
    m_lastSetCurrentPlaybackSampleDurationMs.store(elapsedMs);
    qint64 maxDuration = m_maxSetCurrentPlaybackSampleDurationMs.load();
    while (elapsedMs > maxDuration &&
           !m_maxSetCurrentPlaybackSampleDurationMs.compare_exchange_weak(maxDuration, elapsedMs)) {
    }
    if (elapsedMs >= m_slowSeekWarnThresholdMs) {
        m_setCurrentPlaybackSampleSlowCount.fetch_add(1);
        if (debugPlaybackWarnEnabled()) {
            qWarning().noquote()
                << QStringLiteral("[PLAYBACK WARN] slow setCurrentPlaybackSample: %1 ms | frame=%2 duringPlayback=%3 syncAudio=%4")
                       .arg(elapsedMs)
                       .arg(bounded)
                       .arg(duringPlayback)
                       .arg(syncAudio);
        }
    } else if (debugPlaybackVerboseEnabled()) {
        playbackTrace(QStringLiteral("EditorWindow::setCurrentPlaybackSample.complete"),
                      QStringLiteral("elapsed_ms=%1 frame=%2 duringPlayback=%3 syncAudio=%4")
                          .arg(elapsedMs)
                          .arg(bounded)
                          .arg(duringPlayback)
                          .arg(syncAudio));
    }
    editor::accumulatePlaybackStageMetric(&m_playbackSampleApplyStageMetric,
                                  0,
                                  1,
                                  0,
                                  elapsedMs >= m_slowSeekWarnThresholdMs
                                      ? QStringLiteral("slow")
                                      : QStringLiteral("applied"),
                                  QStringLiteral("elapsed_ms=%1").arg(elapsedMs));
}

void EditorWindow::setCurrentFrame(int64_t frame, bool syncAudio)
{
    setCurrentPlaybackSample(frameToSamples(frame), syncAudio);
}

EditorWindow::PlaybackRuntimeConfig EditorWindow::playbackRuntimeConfig() const
{
    PlaybackRuntimeConfig config;
    config.speed = m_playbackSpeed;
    config.clockSource = m_playbackClockSource;
    config.audioWarpMode = m_playbackAudioWarpMode;
    config.loopEnabled = m_playbackLoopEnabled;
    return config;
}

void EditorWindow::applyPlaybackRuntimeConfig(const PlaybackRuntimeConfig& requestedConfig)
{
    const qreal normalizedSpeed = normalizedPlaybackSpeed(requestedConfig.speed);
    const PlaybackAudioWarpMode normalizedWarpMode =
        normalizedPlaybackAudioWarpMode(normalizedSpeed, requestedConfig.audioWarpMode);
    const PlaybackClockSource normalizedClockSource = PlaybackClockSource::Auto;
    const bool normalizedLoopEnabled = requestedConfig.loopEnabled;

    const bool speedChanged = qAbs(m_playbackSpeed - normalizedSpeed) >= 0.0001;
    const bool clockChanged = m_playbackClockSource != normalizedClockSource;
    const bool warpChanged = m_playbackAudioWarpMode != normalizedWarpMode;
    const bool loopChanged = m_playbackLoopEnabled != normalizedLoopEnabled;
    if (!speedChanged && !clockChanged && !warpChanged && !loopChanged) {
        return;
    }

    m_playbackSpeed = normalizedSpeed;
    m_playbackClockSource = normalizedClockSource;
    m_playbackAudioWarpMode = normalizedWarpMode;
    m_playbackLoopEnabled = normalizedLoopEnabled;
    if (m_preview) {
        m_preview->setPlaybackSpeed(m_playbackSpeed);
    }

    if (m_playbackSpeedCombo) {
        const int speedIndex = m_playbackSpeedCombo->findData(normalizedSpeed);
        if (speedIndex >= 0 && speedIndex != m_playbackSpeedCombo->currentIndex()) {
            QSignalBlocker blocker(m_playbackSpeedCombo);
            m_playbackSpeedCombo->setCurrentIndex(speedIndex);
        }
    }
    if (m_playbackClockSourceCombo) {
        const int index =
            m_playbackClockSourceCombo->findData(playbackClockSourceToString(normalizedClockSource));
        if (index >= 0 && m_playbackClockSourceCombo->currentIndex() != index) {
            QSignalBlocker blocker(m_playbackClockSourceCombo);
            m_playbackClockSourceCombo->setCurrentIndex(index);
        }
    }
    if (m_playbackAudioWarpModeCombo) {
        const int index =
            m_playbackAudioWarpModeCombo->findData(playbackAudioWarpModeToString(normalizedWarpMode));
        if (index >= 0 && m_playbackAudioWarpModeCombo->currentIndex() != index) {
            QSignalBlocker blocker(m_playbackAudioWarpModeCombo);
            m_playbackAudioWarpModeCombo->setCurrentIndex(index);
        }
    }

    const bool activePlaybackReconfigured =
        playbackActive() && (speedChanged || clockChanged || warpChanged);
    if (activePlaybackReconfigured && m_timeline) {
        m_timelineAdvanceCarrySamples = 0.0;
        m_lastTimelineAdvanceTickMs = nowMs();
        m_playbackAudioFeedbackAnchorTimelineSample = m_transportTimelineSample;
        m_playbackAudioFeedbackAnchorFeedbackSample = m_transportTimelineSample;
    }

    if (m_audioEngine) {
        m_audioEngine->setPlaybackWarpMode(normalizedWarpMode);
        m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        m_audioEngine->setPlaybackDriftRetimeRate(m_audioDriftRetimeMultiplier);
        m_audioEngine->setTranscriptNormalizeEnabled(
            m_previewAudioDynamics.transcriptNormalizeEnabled);
        m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
    }
    updatePlaybackTimerInterval();
    reconcileActivePlaybackAudioState(activePlaybackReconfigured);
    updateTransportLabels();
    if (!m_loadingState) {
        scheduleSaveState();
    }
}

void EditorWindow::setPlaybackSpeed(qreal speed)
{
    const bool wasPlaying = playbackActive();
    PlaybackRuntimeConfig config = playbackRuntimeConfig();
    config.speed = speed;
    applyPlaybackRuntimeConfig(config);
    if (qAbs(normalizedPlaybackSpeed(speed) - 1.0) >= 0.0001 &&
        normalizedPlaybackAudioWarpMode(normalizedPlaybackSpeed(speed), config.audioWarpMode) ==
            PlaybackAudioWarpMode::TimeStretch) {
        requestPlaybackAudioWarmup(wasPlaying);
    } else if (m_playbackAudioWarmupPending) {
        ++m_playbackAudioWarmupRequestId;
        m_playbackAudioWarmupPending = false;
        m_retimingAudioForPlayback = false;
        m_startPlaybackAfterAudioWarmup = false;
        updateTransportLabels();
    }
}

void EditorWindow::setPlaybackClockSource(PlaybackClockSource source)
{
    PlaybackRuntimeConfig config = playbackRuntimeConfig();
    config.clockSource = source;
    applyPlaybackRuntimeConfig(config);
}

void EditorWindow::setPlaybackAudioWarpMode(PlaybackAudioWarpMode mode)
{
    const bool wasPlaying = playbackActive();
    PlaybackRuntimeConfig config = playbackRuntimeConfig();
    config.audioWarpMode = mode;
    applyPlaybackRuntimeConfig(config);
    if (qAbs(m_playbackSpeed - 1.0) >= 0.0001 &&
        normalizedPlaybackAudioWarpMode(m_playbackSpeed, mode) == PlaybackAudioWarpMode::TimeStretch) {
        requestPlaybackAudioWarmup(wasPlaying);
    } else if (m_playbackAudioWarmupPending) {
        ++m_playbackAudioWarmupRequestId;
        m_playbackAudioWarmupPending = false;
        m_retimingAudioForPlayback = false;
        m_startPlaybackAfterAudioWarmup = false;
        updateTransportLabels();
    }
}

qreal EditorWindow::effectiveAudioWarpRate() const
{
    return effectivePlaybackAudioWarpRate(m_playbackSpeed, m_playbackAudioWarpMode);
}

void EditorWindow::updateAudioDriftRetime(bool reset)
{
    constexpr int64_t kAudioDriftHardResyncSamples = 24000; // fifteen 30 fps frames at 48 kHz.
    constexpr int64_t kAudioDriftHardResyncCooldownMs = 2000;
    if (!m_audioEngine) {
        m_audioDriftRetimeMultiplier = 1.0;
        return;
    }

    qreal nextMultiplier = 1.0;
    if (!reset &&
        playbackActive() &&
        m_audioEngine->playbackStarted() &&
        m_audioEngine->audioClockAvailable() &&
        m_audioEngine->hasPlayableAudio()) {
        const int64_t audioSample = timelineSampleForAudioFeedbackSample(
            qMax<int64_t>(0, m_audioEngine->playbackClockSample()));
        const int64_t driftSamples = m_transportTimelineSample - audioSample;
        const int64_t tickNowMs = nowMs();
        if (qAbs(driftSamples) > kAudioDriftHardResyncSamples &&
            tickNowMs - m_lastAudioDriftHardResyncMs >= kAudioDriftHardResyncCooldownMs) {
            const int64_t currentFrame = qMax<int64_t>(
                0,
                static_cast<int64_t>(std::floor(samplesToFramePosition(m_transportTimelineSample))));
            m_lastAudioDriftHardResyncMs = tickNowMs;
            m_audioDriftRetimeMultiplier = 1.0;
            m_audioEngine->setPlaybackDriftRetimeRate(m_audioDriftRetimeMultiplier);
            m_audioEngine->seek(currentFrame);
            const int64_t anchorSample = frameToSamples(currentFrame);
            m_playbackAudioFeedbackAnchorTimelineSample = anchorSample;
            m_playbackAudioFeedbackAnchorFeedbackSample = anchorSample;
            return;
        }
        nextMultiplier = editor::evaluatePlaybackDriftRetimeMultiplier(
            editor::PlaybackDriftRetimeInput{
                true,
                driftSamples,
                m_audioDriftRetimeMultiplier,
            });
    } else {
        nextMultiplier = editor::evaluatePlaybackDriftRetimeMultiplier(
            editor::PlaybackDriftRetimeInput{
                false,
                0,
                m_audioDriftRetimeMultiplier,
            });
    }

    if (qAbs(nextMultiplier - m_audioDriftRetimeMultiplier) >= 0.0001) {
        m_audioDriftRetimeMultiplier = nextMultiplier;
        m_audioEngine->setPlaybackDriftRetimeRate(m_audioDriftRetimeMultiplier);
    }
}

bool EditorWindow::needsPitchPreservingPlaybackAudio() const
{
    return m_audioEngine &&
           pitchPreservingPlaybackRequiresAudioGate(m_playbackAudioWarpMode,
                                                   m_playbackSpeed,
                                                   m_audioEngine->hasPlayableAudio());
}

void EditorWindow::reconcileActivePlaybackAudioState(bool alignRunningAudioToPlayhead)
{
    if (!playbackActive() || !m_audioEngine || !m_timeline) {
        return;
    }
    const PlaybackAudioWarpMode runtimeWarpMode =
        normalizedPlaybackAudioWarpMode(m_playbackSpeed, m_playbackAudioWarpMode);
    const bool canRunAudioAtRequestedSpeed =
        qAbs(m_playbackSpeed - 1.0) < 0.0001 ||
        runtimeWarpMode != PlaybackAudioWarpMode::Disabled;
    const bool shouldRunAudio =
        m_audioEngine->hasPlayableAudio() && canRunAudioAtRequestedSpeed;
    const bool audioStarted = m_audioEngine->playbackStarted();
    if (shouldRunAudio) {
        m_audioEngine->setPlaybackWarpMode(runtimeWarpMode);
        m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
        m_audioEngine->setPlaybackDriftRetimeRate(m_audioDriftRetimeMultiplier);
        m_audioEngine->setTranscriptNormalizeEnabled(
            m_previewAudioDynamics.transcriptNormalizeEnabled);
        m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        if (audioStarted && alignRunningAudioToPlayhead) {
            const int64_t currentFrame = m_timeline->currentFrame();
            const bool needsPitchPreservingAudio = needsPitchPreservingPlaybackAudio();
            if (needsPitchPreservingAudio &&
                !m_audioEngine->playbackAudioReadyForFrame(currentFrame)) {
                m_audioEngine->stop();
                updateAudioDriftRetime(true);
                requestPlaybackAudioWarmup(false);
                return;
            }
            if (!needsPitchPreservingAudio &&
                !shouldSkipBlockingAudioWarmup(runtimeWarpMode,
                                               effectiveAudioWarpRate(),
                                               needsPitchPreservingAudio) &&
                !m_audioEngine->warmPlaybackAudio(
                    currentFrame,
                    qMax(5000, m_playbackStartLookaheadTimeoutMs))) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] continuing system-clock transport playback without warmed audio at frame %1")
                           .arg(currentFrame);
            }
            m_audioEngine->seek(currentFrame);
            const int64_t anchorSample = frameToSamples(currentFrame);
            m_playbackAudioFeedbackAnchorTimelineSample = anchorSample;
            m_playbackAudioFeedbackAnchorFeedbackSample = anchorSample;
        } else if (!audioStarted) {
            const bool needsPitchPreservingAudio = needsPitchPreservingPlaybackAudio();
            if (needsPitchPreservingAudio &&
                !m_audioEngine->playbackAudioReadyForFrame(m_timeline->currentFrame())) {
                requestPlaybackAudioWarmup(false);
                return;
            }
            if (!needsPitchPreservingAudio &&
                !shouldSkipBlockingAudioWarmup(runtimeWarpMode,
                                               effectiveAudioWarpRate(),
                                               needsPitchPreservingAudio) &&
                !m_audioEngine->warmPlaybackAudio(
                    m_timeline->currentFrame(),
                    qMax(5000, m_playbackStartLookaheadTimeoutMs))) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] continuing system-clock transport playback without warmed audio at frame %1")
                           .arg(m_timeline->currentFrame());
            }
            m_audioEngine->start(m_timeline->currentFrame());
            const int64_t anchorSample = frameToSamples(m_timeline->currentFrame());
            m_playbackAudioFeedbackAnchorTimelineSample = anchorSample;
            m_playbackAudioFeedbackAnchorFeedbackSample = anchorSample;
        }
    } else if (audioStarted) {
        updateAudioDriftRetime(true);
        m_audioEngine->stop();
    }
}

void EditorWindow::requestPlaybackAudioWarmup(bool startWhenReady)
{
    if (!m_audioEngine || !m_timeline) {
        if (startWhenReady) {
            setPlaybackActive(true);
        }
        return;
    }

    if (!needsPitchPreservingPlaybackAudio()) {
        if (startWhenReady) {
            setPlaybackActive(true);
        }
        return;
    }

    const int64_t frame = m_timeline->currentFrame();
    if (m_audioEngine->playbackAudioReadyForFrame(frame)) {
        m_playbackAudioWarmupPending = false;
        m_retimingAudioForPlayback = false;
        updateTransportLabels();
        updatePlaybackStatusOverlay();
        if (startWhenReady) {
            setPlaybackActive(true);
        }
        return;
    }

    const bool sidecarMissing = m_audioEngine->playbackAudioNeedsRetimingForFrame(frame);
    m_playbackAudioWarmupPending = true;
    m_retimingAudioForPlayback = sidecarMissing;
    m_startPlaybackAfterAudioWarmup = startWhenReady;
    const int requestId = ++m_playbackAudioWarmupRequestId;
    updateTransportLabels();
    updatePlaybackStatusOverlay();

    AudioEngine* audioEngine = m_audioEngine.get();
    const int timeoutMs = sidecarMissing
        ? 30 * 60 * 1000
        : qMax(5000, m_playbackStartLookaheadTimeoutMs);
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, requestId, frame]() {
                const bool ready = watcher->result();
                watcher->deleteLater();
                if (requestId != m_playbackAudioWarmupRequestId) {
                    return;
                }

                const bool shouldStart = m_startPlaybackAfterAudioWarmup;
                m_playbackAudioWarmupPending = false;
                m_retimingAudioForPlayback = false;
                m_startPlaybackAfterAudioWarmup = false;
                updateTransportLabels();
                updatePlaybackStatusOverlay();

                if (!ready) {
                    m_lastPlaybackStopReason = QStringLiteral("audio_not_ready");
                    qWarning().noquote()
                        << QStringLiteral("[PLAYBACK WARN] startup gated: waiting for re-timed audio at frame %1 timed out")
                               .arg(frame);
                    updateTransportLabels();
                    updatePlaybackStatusOverlay();
                    return;
                }

                if (shouldStart) {
                    setPlaybackActive(true);
                } else if (playbackActive()) {
                    reconcileActivePlaybackAudioState(true);
                }
            });
    watcher->setFuture(QtConcurrent::run([audioEngine, frame, timeoutMs]() {
        return audioEngine->warmPlaybackAudio(frame, timeoutMs);
    }));
}

void EditorWindow::updatePlaybackStatusOverlay()
{
    if (!m_preview) {
        return;
    }

    QString text;
    qreal progress = -1.0;
    if (needsPitchPreservingPlaybackAudio()) {
        const int percent = qRound(m_playbackSpeed * 100.0);
        const bool audioReady =
            m_audioEngine && m_timeline &&
            m_audioEngine->playbackAudioReadyForFrame(m_timeline->currentFrame());
        const bool timeStretchGenerationActive =
            m_audioEngine && m_audioEngine->timeStretchGenerationActive();
        if (m_retimingAudioForPlayback || timeStretchGenerationActive) {
            progress = m_audioEngine ? m_audioEngine->timeStretchGenerationProgress() : -1.0;
            const int generationPercent = progress >= 0.0 ? qRound(progress * 100.0) : 0;
            text = progress >= 0.0
                ? QStringLiteral("Audio being generated: Rubber Band tempo shift (%1%) %2%")
                      .arg(percent)
                      .arg(generationPercent)
                : QStringLiteral("Audio being generated: Rubber Band tempo shift (%1%)").arg(percent);
        } else if (m_playbackAudioWarmupPending) {
            text = QStringLiteral("Loading re-timed audio (%1%)").arg(percent);
        } else if (!audioReady && m_lastPlaybackStopReason == QStringLiteral("audio_not_ready")) {
            text = QStringLiteral("Playback waiting: loading precomputed %1% audio").arg(percent);
        }
    }
    if (text.isEmpty()) {
        if (m_playbackVideoWarmupPending) {
            text = QStringLiteral("Playback waiting: buffering video frames");
        } else if (m_lastPlaybackStopReason == QStringLiteral("video_not_ready")) {
            text = QStringLiteral("Playback waiting: video frames are still buffering");
        }
    }
    m_preview->setPlaybackStatusOverlayText(text);
    m_preview->setPlaybackStatusOverlayProgress(progress);
}

void EditorWindow::updatePlaybackTimerInterval()
{
    const qreal speed = qBound<qreal>(0.1, m_playbackSpeed, 3.0);
    if (!m_timeline) {
        const int intervalMs = qMax(1, qRound(1000.0 / (kTimelineFps * speed)));
        m_playbackTimer.setInterval(intervalMs);
        return;
    }

    // Determine effective FPS for playback timer. Audio can remain the clock
    // source, but video scheduling still needs to tick at the active clip cadence.
    qreal effectiveFps = kTimelineFps;
    for (const TimelineClip& clip : m_timeline->clips()) {
        const int64_t currentFrame = m_timeline->currentFrame();
        if (currentFrame >= clip.startFrame && currentFrame < clip.startFrame + clip.durationFrames) {
            const qreal clipFps = effectiveFpsForClip(clip);
            if (clipFps > 0) {
                effectiveFps = qMax(effectiveFps, clipFps);
                break;
            }
        }
    }

    const int intervalMs = qMax(1, qRound(1000.0 / (effectiveFps * speed)));
    m_playbackTimer.setInterval(intervalMs);
}

void EditorWindow::setPlaybackActive(bool playing)
{
    if (playing == playbackActive()) {
        updateTransportLabels();
        return;
    }

    QElapsedTimer transitionTimer;
    transitionTimer.start();
    qint64 lastPhaseMs = 0;
    const auto logPlaybackTransitionPhase = [&](const QString& phase) {
        if (!debugPlaybackWarnEnabled()) {
            return;
        }
        const qint64 elapsedMs = transitionTimer.elapsed();
        const qint64 deltaMs = elapsedMs - lastPhaseMs;
        lastPhaseMs = elapsedMs;
        if (deltaMs >= 25 || elapsedMs >= m_slowSeekWarnThresholdMs) {
            qWarning().noquote()
                << QStringLiteral("[PLAYBACK WARN] %1 transition phase=%2 delta_ms=%3 elapsed_ms=%4")
                       .arg(playing ? QStringLiteral("start") : QStringLiteral("pause"))
                       .arg(phase)
                       .arg(deltaMs)
                       .arg(elapsedMs);
        }
    };

    if (playing) {
        m_lastPlaybackStopReason = QStringLiteral("none");
        m_audioDriftRetimeMultiplier = 1.0;
        m_timelineAdvanceCarrySamples = 0.0;
        m_lastTimelineAdvanceTickMs = nowMs();
        const auto ranges = effectivePlaybackRanges();
        int64_t playbackStartSample = m_transportTimelineSample;
        if (!ranges.isEmpty()) {
            bool reachedEnd = false;
            playbackStartSample = playableSampleAtOrAfter(playbackStartSample, ranges, &reachedEnd);
            if (reachedEnd && m_playbackLoopEnabled) {
                playbackStartSample = frameToSamples(ranges.constFirst().startFrame);
            }
        }
        playbackStartSample =
            qBound<int64_t>(0, playbackStartSample, frameToSamples(m_timeline->totalFrames()));
        const int64_t playbackStartFrame = qBound<int64_t>(
            0,
            static_cast<int64_t>(std::floor(samplesToFramePosition(playbackStartSample))),
            m_timeline->totalFrames());
        m_transportTimelineSample = playbackStartSample;
        m_playbackSessionStartWallMs = m_lastTimelineAdvanceTickMs;
        m_playbackSessionStartTimelineSample = playbackStartSample;
        m_filteredPlaybackSample = filteredPlaybackSampleForAbsoluteSample(playbackStartSample);
        m_playbackAudioFeedbackAnchorTimelineSample = playbackStartSample;
        m_playbackAudioFeedbackAnchorFeedbackSample = playbackStartSample;
        if (m_timeline->currentFrame() != playbackStartFrame) {
            m_timeline->setCurrentFrame(playbackStartFrame);
            m_fastCurrentFrame.store(playbackStartFrame);
        }
        if (m_preview) {
            m_preview->setCurrentPlaybackSample(playbackStartSample);
        }
        if (m_preview) {
            m_playbackVideoWarmupPending = true;
            updateTransportLabels();
            updatePlaybackStatusOverlay();
            const bool videoLookaheadReady =
                m_preview->warmPlaybackLookahead(m_playbackStartLookaheadFrames,
                                                 m_playbackStartLookaheadTimeoutMs);
            m_playbackVideoWarmupPending = false;
            if (!videoLookaheadReady) {
                m_lastPlaybackStopReason = QStringLiteral("video_not_ready");
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] startup gated: waiting for %1 buffered future frames timed out")
                           .arg(m_playbackStartLookaheadFrames);
                updateTransportLabels();
                updatePlaybackStatusOverlay();
                return;
            }
            updatePlaybackStatusOverlay();
        }

        if (m_audioEngine) {
            m_audioEngine->setExportRanges(ranges);
            m_audioEngine->setTranscriptNormalizeRanges(
                m_previewAudioDynamics.transcriptNormalizeEnabled
                    ? effectiveTranscriptNormalizeRanges()
                    : QVector<ExportRangeSegment>{});
            m_audioEngine->setSpeechFilterFadeSamples(m_speechFilterFadeSamples);
            m_audioEngine->setSpeechFilterRangeCrossfadeEnabled(m_speechFilterRangeCrossfade);
            m_audioEngine->setPlaybackWarpMode(
                normalizedPlaybackAudioWarpMode(m_playbackSpeed, m_playbackAudioWarpMode));
            m_audioEngine->setPlaybackRate(effectiveAudioWarpRate());
            m_audioEngine->setPlaybackDriftRetimeRate(m_audioDriftRetimeMultiplier);
            m_audioEngine->setTranscriptNormalizeEnabled(
                m_previewAudioDynamics.transcriptNormalizeEnabled);
            m_audioEngine->setAudioDynamicsSettings(m_previewAudioDynamics);
        }
        const PlaybackAudioWarpMode runtimeWarpMode =
            normalizedPlaybackAudioWarpMode(m_playbackSpeed, m_playbackAudioWarpMode);
        const bool canRunAudioAtRequestedSpeed =
            qAbs(m_playbackSpeed - 1.0) < 0.0001 ||
            runtimeWarpMode != PlaybackAudioWarpMode::Disabled;
        if (m_audioEngine && m_audioEngine->hasPlayableAudio() && canRunAudioAtRequestedSpeed) {
            const bool needsPitchPreservingAudio = needsPitchPreservingPlaybackAudio();
            bool audioReadyToStart = true;
            if (needsPitchPreservingAudio &&
                !m_audioEngine->playbackAudioReadyForFrame(playbackStartFrame)) {
                audioReadyToStart = false;
                requestPlaybackAudioWarmup(false);
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] continuing transport playback while re-timed audio warms at frame %1")
                           .arg(playbackStartFrame);
            }
            if (audioReadyToStart && !needsPitchPreservingAudio &&
                !shouldSkipBlockingAudioWarmup(runtimeWarpMode,
                                               effectiveAudioWarpRate(),
                                               needsPitchPreservingAudio) &&
                !m_audioEngine->warmPlaybackAudio(
                    playbackStartFrame,
                    qMax(5000, m_playbackStartLookaheadTimeoutMs))) {
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] startup gated: waiting for audio at frame %1 timed out")
                           .arg(playbackStartFrame);
                qWarning().noquote()
                    << QStringLiteral("[PLAYBACK WARN] continuing system-clock transport playback without warmed audio at frame %1")
                           .arg(playbackStartFrame);
            }
            if (audioReadyToStart) {
                m_audioEngine->start(playbackStartFrame);
                const int64_t anchorSample = frameToSamples(playbackStartFrame);
                m_playbackAudioFeedbackAnchorTimelineSample = anchorSample;
                m_playbackAudioFeedbackAnchorFeedbackSample = anchorSample;
                if (!m_startupReadinessAudioStarted.exchange(true)) {
                    startupReadinessMark(QStringLiteral("audio.start.invoked"),
                                         QJsonObject{{QStringLiteral("frame"),
                                                      static_cast<qint64>(playbackStartFrame)},
                                                     {QStringLiteral("warp_mode"),
                                                      playbackAudioWarpModeToString(runtimeWarpMode)},
                                                     {QStringLiteral("speed"), m_playbackSpeed}});
                }
            }
        }
        if (m_preview) {
            m_preview->setExportRanges(ranges);
            m_preview->setPlaybackState(true);
        }
        m_fastPlaybackActive.store(true);
        advanceFrame();
        if (!m_startupReadinessFirstPlaybackTick.exchange(true)) {
            startupReadinessMark(QStringLiteral("playback.first_tick"));
        }
        m_playbackTimer.start();
    } else {
        m_timelineAdvanceCarrySamples = 0.0;
        m_lastTimelineAdvanceTickMs = 0;
        logPlaybackTransitionPhase(QStringLiteral("state_reset"));
        if (m_audioEngine) {
            updateAudioDriftRetime(true);
            m_audioEngine->stop();
        }
        logPlaybackTransitionPhase(QStringLiteral("audio_stop"));
        m_playbackTimer.stop();
        m_fastPlaybackActive.store(false);
        m_preview->setPlaybackState(false);
        logPlaybackTransitionPhase(QStringLiteral("preview_stop"));
        if (m_speakersTab) {
            m_speakersTab->flushDeferredPlaybackRefreshes();
        }
        logPlaybackTransitionPhase(QStringLiteral("speaker_deferred_refresh_queued"));
    }
    updateTransportLabels();
    logPlaybackTransitionPhase(QStringLiteral("transport_labels"));
    if (playing) {
        refreshCurrentInspectorTab();
    } else {
        scheduleDeferredInspectorRefresh(75);
    }
    logPlaybackTransitionPhase(playing ? QStringLiteral("inspector_refresh")
                                       : QStringLiteral("inspector_refresh_queued"));
    scheduleSaveState();
    logPlaybackTransitionPhase(QStringLiteral("save_queued"));
}

void EditorWindow::stopPlaybackWithReason(const QString& reason)
{
    m_lastPlaybackStopReason = reason.isEmpty() ? QStringLiteral("unknown") : reason;
    setPlaybackActive(false);
}

void EditorWindow::togglePlayback()
{
    if (playbackActive()) {
        stopPlaybackWithReason(QStringLiteral("manual_pause"));
    } else {
        setPlaybackActive(true);
    }
}

void EditorWindow::onRestartDecodersRequested()
{
    qDebug() << "Restart All Decoders requested";
    // TODO: Implement actual decoder restart
    // For now, just log and refresh the preview
    if (m_preview) {
        m_preview->asWidget()->update();
    }
}

bool EditorWindow::playbackActive() const
{
    return m_fastPlaybackActive.load();
}

} // namespace editor
