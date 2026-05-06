#include "opengl_preview.h"
#include "opengl_preview_debug.h"

#include "async_decoder.h"
#include "frame_handle.h"
#include "media_pipeline_shared.h"
#include "memory_budget.h"
#include "playback_frame_pipeline.h"
#include "timeline_cache.h"
#include "debug_controls.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

using namespace editor;

bool PreviewWindow::preparePlaybackAdvance(int64_t targetFrame) {
    return preparePlaybackAdvanceSample(frameToSamples(targetFrame));
}

bool PreviewWindow::preparePlaybackAdvanceSample(int64_t targetSample) {
    if (m_interaction.clips.isEmpty()) return true;

    ensurePipeline();
    if (!m_cache) return false;

    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) || !isSampleWithinClip(clip, targetSample)) {
            continue;
        }
        if (clip.mediaType == ClipMediaType::Title) {
            // Title clips are overlay-only and do not require decoded frame readiness.
            continue;
        }

        const int64_t localFrame = sourceFrameForSample(clip, targetSample);
        const bool usePlaybackPipeline =
            m_interaction.playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool pausedSequenceNeedsExact =
            !m_interaction.playing &&
            clip.sourceKind == MediaSourceKind::ImageSequence &&
            clip.mediaType != ClipMediaType::Image;
        const bool ready = usePlaybackPipeline
                                ? m_playbackPipeline->isFrameBuffered(clip.id, localFrame)
                                : (pausedSequenceNeedsExact
                                      ? m_cache->isFrameCached(clip.id, localFrame)
                                : m_cache->hasDisplayableFrameForPreview(
                                      clip.id,
                                      localFrame,
                                      m_interaction.playing,
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

bool PreviewWindow::warmPlaybackLookahead(int futureFrames, int timeoutMs) {
    if (futureFrames <= 0) {
        return true;
    }

    ensurePipeline();
    if (!m_cache) {
        return false;
    }

    if (m_playbackPipeline) {
        m_playbackPipeline->setPlaybackActive(true);
        m_playbackPipeline->setPlayheadFrame(m_interaction.currentFrame);
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (hasPlaybackLookaheadBuffered(futureFrames)) {
            return true;
        }

        for (int offset = 0; offset <= futureFrames; ++offset) {
            const int64_t targetSample = m_interaction.currentSample + frameToSamples(offset);
            preparePlaybackAdvanceSample(targetSample);
            if (m_playbackPipeline) {
                m_playbackPipeline->requestFramesForSample(
                    targetSample,
                    [this]() {
                        QMetaObject::invokeMethod(this, [this]() { scheduleRepaint(); }, Qt::QueuedConnection);
                    });
            }
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
        QThread::msleep(8);
    }

    return hasPlaybackLookaheadBuffered(futureFrames);
}

bool PreviewWindow::hasPlaybackLookaheadBuffered(int futureFrames) const {
    if (futureFrames < 0 || !m_cache) {
        return true;
    }

    for (int offset = 0; offset <= futureFrames; ++offset) {
        const int64_t samplePosition = m_interaction.currentSample + frameToSamples(offset);
        const qreal framePosition = samplesToFramePosition(samplePosition);
        for (const TimelineClip& clip : m_interaction.clips) {
            if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks) || !isSampleWithinClip(clip, samplePosition)) {
                continue;
            }
            if (clip.mediaType == ClipMediaType::Title) {
                continue;
            }
            if (!editor::clipIsActiveAtTimelineFrame(clip, m_interaction.tracks, framePosition, m_bypassGrading)) {
                continue;
            }
            const int64_t localFrame = sourceFrameForSample(clip, samplePosition);
            const bool isImageSequence = clip.sourceKind == MediaSourceKind::ImageSequence &&
                                         clip.mediaType != ClipMediaType::Image;
            if (isImageSequence) {
                if (!m_playbackPipeline || !m_playbackPipeline->isFrameBuffered(clip.id, localFrame)) {
                    return false;
                }
                continue;
            }

            if (!m_cache->hasDisplayableFrameForPreview(
                    clip.id,
                    localFrame,
                    true,
                    editor::debugPlaybackCacheFallbackEnabled())) {
                return false;
            }
        }
    }
    return true;
}

void PreviewWindow::ensurePipeline() {
    if (m_cache) return;

    playbackTrace(QStringLiteral("PreviewWindow::ensurePipeline.begin"),
                  QStringLiteral("clips=%1 frame=%2").arg(m_interaction.clips.size()).arg(m_interaction.currentFramePosition, 0, 'f', 3));

    m_decoder = std::make_unique<AsyncDecoder>(this);
    m_decoder->initialize();
    if (MemoryBudget* budget = m_decoder->memoryBudget()) {
        budget->setMaxCpuMemory(1536 * 1024 * 1024); // 1.5GB
    }

    m_cache = std::make_unique<TimelineCache>(m_decoder.get(), m_decoder->memoryBudget(), this);
    m_playbackPipeline = std::make_unique<PlaybackFramePipeline>(m_decoder.get(), this);
    connect(m_cache.get(), &TimelineCache::frameLoaded, this,
            [this](const QString&, int64_t, FrameHandle frame) {
                if (frame.isNull()) {
                    return;
                }
                m_lastFrameReadyMs = nowMs();
                scheduleRepaint();
            },
            Qt::QueuedConnection);
    connect(m_playbackPipeline.get(), &PlaybackFramePipeline::frameAvailable, this,
            [this]() {
                m_lastFrameReadyMs = nowMs();
                scheduleRepaint();
            },
            Qt::QueuedConnection);
    m_cache->setMaxMemory(768 * 1024 * 1024);
    m_cache->setLookaheadFrames(36);
    m_cache->setPlaybackSpeed(1.0);
    m_cache->setPlaybackState(m_interaction.playing ? TimelineCache::PlaybackState::Playing
                                        : TimelineCache::PlaybackState::Stopped);
    m_cache->setPlayheadFrame(m_interaction.currentFrame);
    m_playbackPipeline->setPlaybackActive(m_interaction.playing);
    m_playbackPipeline->setPlayheadFrame(m_interaction.currentFrame);
    m_playbackPipeline->setTimelineClips(m_interaction.clips);
    m_playbackPipeline->setRenderSyncMarkers(m_interaction.renderSyncMarkers);
    m_registeredClips.clear();
    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks)) continue;
        m_cache->registerClip(clip);
        m_registeredClips.insert(clip.id);
    }
    m_cache->setRenderSyncMarkers(m_interaction.renderSyncMarkers);
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
    return sourceFrameForClipAtTimelinePosition(clip, samplesToFramePosition(samplePosition), m_interaction.renderSyncMarkers);
}

bool PreviewWindow::isFrameTooStaleForPlayback(const TimelineClip& clip,
                                               int64_t localFrame,
                                               const FrameHandle& frame) const {
    if (!m_interaction.playing || frame.isNull()) {
        return false;
    }
    if (clip.mediaType == ClipMediaType::Image || clip.durationFrames <= 1) {
        return false;
    }
    const int64_t frameNumber = frame.frameNumber();
    if (frameNumber < 0) {
        return false;
    }
    constexpr int64_t kMaxPlaybackStaleFrameDelta = 4;
    return frameNumber + kMaxPlaybackStaleFrameDelta < localFrame;
}

void PreviewWindow::requestFramesForCurrentPosition() {
    static constexpr int kMaxVisibleBacklog = 4;
    playbackTrace(QStringLiteral("PreviewWindow::requestFramesForCurrentPosition"),
                  QStringLiteral("frame=%1 playing=%2 activeClips=%3")
                      .arg(m_interaction.currentFramePosition, 0, 'f', 3)
                      .arg(m_interaction.playing)
                      .arg(m_interaction.clips.size()));
    QVector<const TimelineClip*> activeClips;
    activeClips.reserve(m_interaction.clips.size());
    for (const TimelineClip& clip : m_interaction.clips) {
        if (!clipVisualPlaybackEnabled(clip, m_interaction.tracks)) {
            continue;
        }
        if (isSampleWithinClip(clip, m_interaction.currentSample)) {
            if (!editor::clipIsActiveAtTimelineFrame(clip, m_interaction.tracks, m_interaction.currentFramePosition, m_bypassGrading)) {
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
        const int64_t localFrame = sourceFrameForSample(*clip, m_interaction.currentSample);
        const bool isImageSequence = clip->sourceKind == MediaSourceKind::ImageSequence &&
                                     clip->mediaType != ClipMediaType::Image;
        const bool usePlaybackPipeline = m_interaction.playing && isImageSequence;

        const bool pausedNeedsExact = !m_interaction.playing;
        const bool cached = usePlaybackPipeline
                                ? m_playbackPipeline->isFrameBuffered(clip->id, localFrame)
                                : (pausedNeedsExact
                                      ? m_cache->isFrameCached(clip->id, localFrame)
                                : m_cache->hasDisplayableFrameForPreview(
                                      clip->id,
                                      localFrame,
                                      m_interaction.playing,
                                      true));
        const bool pending = usePlaybackPipeline
                                 ? m_playbackPipeline->pendingVisibleRequestCount() >= kMaxVisibleBacklog
                                 : m_cache->isVisibleRequestPending(clip->id, localFrame);
        const bool forceRetry = !usePlaybackPipeline &&
                                m_cache->shouldForceVisibleRequestRetry(clip->id, localFrame, 250);
        if (!cached && (!pending || forceRetry)) {
            m_lastFrameRequestMs = nowMs();
            if (usePlaybackPipeline) {
                m_playbackPipeline->requestFramesForSample(
                    m_interaction.currentSample,
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
