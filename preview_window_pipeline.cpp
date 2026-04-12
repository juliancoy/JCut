#include "preview.h"
#include "preview_debug.h"

#include "async_decoder.h"
#include "frame_handle.h"
#include "media_pipeline_shared.h"
#include "memory_budget.h"
#include "playback_frame_pipeline.h"
#include "timeline_cache.h"
#include "debug_controls.h"

using namespace editor;

bool PreviewWindow::preparePlaybackAdvance(int64_t targetFrame) {
    return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
}

bool PreviewWindow::preparePlaybackAdvanceSample(int64_t targetSample) {
    if (m_clips.isEmpty()) return true;

    ensurePipeline();
    if (!m_cache) return false;

    for (const TimelineClip& clip : m_clips) {
        if (!clipVisualPlaybackEnabled(clip, m_tracks) || !isSampleWithinClip(clip, targetSample)) {
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, targetSample);
        const bool usePlaybackPipeline =
            m_playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool pausedSequenceNeedsExact =
            !m_playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool ready = usePlaybackPipeline
                                ? m_playbackPipeline->isFrameBuffered(clip.id, localFrame)
                                : (pausedSequenceNeedsExact
                                      ? m_cache->isFrameCached(clip.id, localFrame)
                                : m_cache->hasDisplayableFrameForPreview(
                                      clip.id,
                                      localFrame,
                                      m_playing,
                                      editor::debugPlaybackCacheFallbackEnabled()));
        if (ready) continue;

        if (usePlaybackPipeline) {
            m_playbackPipeline->requestFramesForSample(targetSample,
                [this]() { QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection); });
        } else {
            m_cache->requestFrame(clip.id, localFrame,
                                  [this](FrameHandle frame) {
                                      Q_UNUSED(frame)
                                      QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection);
                                  });
        }
    }

    return true;
}

void PreviewWindow::ensurePipeline() {
    if (m_cache) return;

    playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.begin"),
                  QStringLiteral("clips=%1 frame=%2").arg(m_clips.size()).arg(m_currentFramePosition, 0, 'f', 3));

    m_decoder = std::make_unique<AsyncDecoder>(this);
    m_decoder->initialize();
    if (MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(1536 * 1024 * 1024); // 1.5GB
    }

    m_cache = std::make_unique<TimelineCache>(m_decoder.get(), m_decoder->memoryBudget(), this);
    m_playbackPipeline = std::make_unique<PlaybackFramePipeline>(m_decoder.get(), this);
    m_cache->setMaxMemory(768 * 1024 * 1024);
    m_cache->setLookaheadFrames(36);
    m_cache->setPlaybackSpeed(1.0);
    m_cache->setPlaybackState(m_playing ? TimelineCache::PlaybackState::Playing
                                        : TimelineCache::PlaybackState::Stopped);
    m_cache->setPlayheadFrame(m_currentFrame);
    m_playbackPipeline->setPlaybackActive(m_playing);
    m_playbackPipeline->setPlayheadFrame(m_currentFrame);
    m_playbackPipeline->setTimelineClips(m_clips);
    m_playbackPipeline->setRenderSyncMarkers(m_renderSyncMarkers);
    m_registeredClips.clear();
    for (const TimelineClip& clip : m_clips) {
        if (!clipVisualPlaybackEnabled(clip, m_tracks)) continue;
        m_cache->registerClip(clip);
        m_registeredClips.insert(clip.id);
    }
    m_cache->setRenderSyncMarkers(m_renderSyncMarkers);
    m_cache->startPrefetching();
    playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.end"),
                  QStringLiteral("workers=%1").arg(m_decoder ? m_decoder->workerCount() : 0));
}

bool PreviewWindow::isSampleWithinClip(const TimelineClip& clip, int64_t samplePosition) const {
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
    return samplePosition >= clipStartSample && samplePosition < clipEndSample;
}

int64_t PreviewWindow::sourceSampleForPlaybackSample(const TimelineClip& clip, int64_t samplePosition) const {
    return qMax<int64_t>(0, clipSourceInSamples(clip) + (samplePosition - clipTimelineStartSamples(clip)));
}

int64_t PreviewWindow::sourceFrameForSample(const TimelineClip& clip, int64_t samplePosition) const {
    return sourceFrameForClipAtTimelinePosition(clip, samplesToFramePosition(samplePosition), m_renderSyncMarkers);
}

void PreviewWindow::requestFramesForCurrentPosition() {
    static constexpr int kMaxVisibleBacklog = 4;
    playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                  QStringLiteral("frame=%1 playing=%2 activeClips=%3")
                      .arg(m_currentFramePosition, 0, 'f', 3)
                      .arg(m_playing)
                      .arg(m_clips.size()));
    QVector<const TimelineClip*> activeClips;
    activeClips.reserve(m_clips.size());
    for (const TimelineClip& clip : m_clips) {
        if (!clipVisualPlaybackEnabled(clip, m_tracks)) {
            continue;
        }
        if (isSampleWithinClip(clip, m_currentSample)) {
            if (!editor::clipIsActiveAtTimelineFrame(clip, m_tracks, m_currentFramePosition, m_bypassGrading)) {
                continue;
            }
            activeClips.push_back(&clip);
        }
    }

    if (activeClips.isEmpty()) {
        return;
    }

    ensurePipeline();
    if (!m_cache) {
        return;
    }

    for (const TimelineClip* clip : activeClips) {
        if (clip->mediaType == ClipMediaType::Title) {
            continue;
        }
        const int64_t localFrame = sourceFrameForSample(*clip, m_currentSample);
        const bool isImageSequence = clip->sourceKind == MediaSourceKind::ImageSequence &&
                                     clip->mediaType != ClipMediaType::Image;
        const bool usePlaybackPipeline = m_playing && isImageSequence;

        const bool pausedSequenceNeedsExact = !m_playing && isImageSequence;
        const bool cached = usePlaybackPipeline
                                ? m_playbackPipeline->isFrameBuffered(clip->id, localFrame)
                                : (pausedSequenceNeedsExact
                                      ? m_cache->isFrameCached(clip->id, localFrame)
                                : m_cache->hasDisplayableFrameForPreview(
                                      clip->id,
                                      localFrame,
                                      false,
                                      true));
        const bool pending = usePlaybackPipeline
                                 ? m_playbackPipeline->pendingVisibleRequestCount() >= kMaxVisibleBacklog
                                 : m_cache->isVisibleRequestPending(clip->id, localFrame);
        if (!cached && !pending) {
            m_lastFrameRequestMs = nowMs();
            if (usePlaybackPipeline) {
                m_playbackPipeline->requestFramesForSample(
                    m_currentSample,
                    [this]() {
                        QMetaObject::invokeMethod(this, [this]() {
                            scheduleRepaint();
                        }, Qt::QueuedConnection);
                    });
            } else {
                m_cache->requestFrame(clip->id, localFrame,
                    [this](FrameHandle frame) {
                        Q_UNUSED(frame)
                        QMetaObject::invokeMethod(this, [this]() {
                            scheduleRepaint();
                        }, Qt::QueuedConnection);
                    });
            }
        }
    }
}
