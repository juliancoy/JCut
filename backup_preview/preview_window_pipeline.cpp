#include "preview.h"
#include "preview_debug.h"

#include "async_decoder.h"
#include "frame_handle.h"
#include "memory_budget.h"
#include "playback_frame_pipeline.h"
#include "timeline_cache.h"

using namespace editor;

bool PreviewWindow::preparePlaybackAdvance(int64_t targetFrame) {
    return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
}

bool PreviewWindow::preparePlaybackAdvanceSample(int64_t targetSample) {
    if (m_clips.isEmpty()) return true;

    ensurePipeline();
    if (!m_cache) return false;

    for (const TimelineClip& clip : m_clips) {
        if (!clipHasVisuals(clip) || !clip.videoEnabled || !isSampleWithinClip(clip, targetSample)) {
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, targetSample);
        const bool usePlaybackPipeline = false;
        const bool ready = usePlaybackPipeline ? m_playbackPipeline->isFrameBuffered(clip.id, localFrame)
                                               : m_cache->isFrameCached(clip.id, localFrame);
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
        budget->setMaxCpuMemory(768 * 1024 * 1024);
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
        if (!clipHasVisuals(clip) || !clip.videoEnabled) continue;
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
    // Best-effort split point. The uploaded source truncates this method before completion,
    // so this file intentionally preserves the method boundary and leaves the body for the user
    // to paste from the original full source.
    // TODO: paste full body of PreviewWindow::requestFramesForCurrentPosition from the original file.
}
