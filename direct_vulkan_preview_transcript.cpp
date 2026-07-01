#include "direct_vulkan_preview_transcript.h"

#include "direct_vulkan_preview_config.h"
#include "direct_vulkan_preview_geometry.h"
#include "direct_vulkan_preview_interaction.h"
#include "editor_shared_media.h"
#include "editor_shared_timing.h"
#include "editor_shared_transcript.h"
#include "preview_view_transform.h"
#include "transcript_overlay_cache_key.h"

#include <QStringList>

#include <algorithm>
#include <memory>

namespace jcut::direct_vulkan_preview {
namespace {

PreparedTranscriptOverlay buildTranscriptOverlay(const PreviewInteractionState* state,
                                                 const PreviewViewTransform& viewTransform,
                                                 const TimelineClip& effectiveClip,
                                                 int64_t samplePosition,
                                                 const VulkanPreviewClipFrameStatus* status,
                                                 TranscriptOverlayCollectionStats* stats,
                                                 QString* skipReason)
{
    PreparedTranscriptOverlay prepared;
    if (!state) {
        if (skipReason) *skipReason = QStringLiteral("invalid_state");
        return prepared;
    }
    const QString transcriptPath = activeTranscriptPathForClip(effectiveClip);
    if (transcriptPath.isEmpty()) {
        if (skipReason) *skipReason = QStringLiteral("missing_transcript_source");
        return prepared;
    }
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    const bool usePresentedFrame =
        status && status->hasFrame && status->presentedSourceFrame >= 0;
    const int64_t sourceFrame =
        usePresentedFrame
            ? transcriptFrameForClipSourceFrame(effectiveClip, status->presentedSourceFrame)
            : transcriptFrameForClipAtTimelineSample(effectiveClip,
                                                     samplePosition,
                                                     state->renderSyncMarkers);
    const TranscriptOverlayTiming timing{
        state->transcriptPrependMs, state->transcriptPostpendMs, state->transcriptOffsetMs};
    const TranscriptOverlayLayout layout =
        transcriptOverlayLayoutAtSourceFrame(effectiveClip, sections, sourceFrame, timing);
    if (layout.lines.isEmpty()) {
        if (skipReason) *skipReason = QStringLiteral("empty_layout");
        return prepared;
    }

    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        effectiveClip,
        state->outputSize,
        transcriptPath,
        sections,
        sourceFrame);
    const QPointF center = viewTransform.outputToScreen(outputRect.center());
    const QPointF previewScale = viewTransform.outputScale();
    const QRectF bounds(center.x() - ((outputRect.width() * previewScale.x()) * 0.5),
                        center.y() - ((outputRect.height() * previewScale.y()) * 0.5),
                        outputRect.width() * previewScale.x(),
                        outputRect.height() * previewScale.y());
    const QRectF localTextBounds =
        QRectF(0.0, 0.0, outputRect.width(), outputRect.height()).adjusted(18.0, 14.0, -18.0, -14.0);
    if (outputRect.width() <= 0.0 ||
        outputRect.height() <= 0.0 ||
        bounds.width() <= 0.0 ||
        bounds.height() <= 0.0 ||
        localTextBounds.width() <= 0.0 ||
        localTextBounds.height() <= 0.0 ||
        effectiveClip.transcriptOverlay.fontPointSize <= 0.0) {
        if (skipReason) *skipReason = QStringLiteral("invalid_geometry");
        return prepared;
    }

    QString speakerTitle;
    if (effectiveClip.transcriptOverlay.showSpeakerTitle) {
        speakerTitle = transcriptSpeakerTitleForSourceFrame(
            transcriptPath,
            sections,
            sourceFrame,
            timing).trimmed();
    }

    prepared.clip = effectiveClip;
    prepared.layout = layout;
    prepared.outputRect = outputRect;
    prepared.bounds = bounds;
    prepared.speakerTitle = speakerTitle;
    prepared.ready = true;
    if (stats) {
        stats->lastPreparedClipId = effectiveClip.id;
        stats->lastPreparedTranscriptPath = transcriptPath;
        stats->lastPreparedTimingSource =
            usePresentedFrame ? QStringLiteral("presented_media_source_frame")
                              : QStringLiteral("transport_timeline_sample");
        stats->lastPreparedTimelineSample = samplePosition;
        stats->lastPreparedTranscriptFrame = sourceFrame;
        stats->lastPreparedPresentedMediaSourceFrame =
            usePresentedFrame ? status->presentedSourceFrame : -1;
    }
    return prepared;
}

} // namespace

PreparedTranscriptOverlayMap collectPreparedTranscriptOverlays(const PreviewInteractionState* state,
                                                               const QSize& swapSize,
                                                               TranscriptOverlayCollectionStats* stats)
{
    PreparedTranscriptOverlayMap overlays;
    if (!state || !swapSize.isValid()) {
        if (stats) {
            stats->candidateCount = 0;
            stats->preparedCount = 0;
            stats->lastSkipReason = QStringLiteral("invalid_state");
        }
        return overlays;
    }
    TranscriptOverlayCollectionStats localStats;

    const QRectF fullSwapRect(QPointF(0, 0), QSizeF(swapSize));
    const PreviewViewTransform viewTransform(fullSwapRect,
                                             state->outputSize,
                                             vulkanPreviewCanvasMarginPx(),
                                             state->previewZoom,
                                             state->previewPanOffset);

    for (const TimelineClip& clip : state->clips) {
        const VulkanPreviewClipFrameStatus* status = frameStatusForClip(state, clip.id);
        const bool statusDrawable = status && status->active && !status->drawSuppressed;
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipTimelineEndSamples(clip);
        const bool supportsTranscriptOverlay = clipSupportsTranscriptOverlay(clip);
        const bool timelineInRange =
            state->currentSample >= clipStartSample &&
            state->currentSample < clipEndSample;
        const bool trackVisible = trackVisualModeForClip(clip, state->tracks) != TrackVisualMode::Hidden;
        const bool timelineTranscriptActive =
            supportsTranscriptOverlay && timelineInRange && trackVisible;
        if (!supportsTranscriptOverlay) {
            continue;
        }
        if (!statusDrawable && !timelineTranscriptActive) {
            localStats.lastSkipReason = !trackVisible ? QStringLiteral("track_hidden") : QStringLiteral("inactive");
            continue;
        }
        ++localStats.candidateCount;

        const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
        if (!clipSupportsTranscriptOverlay(effectiveClip)) {
            localStats.lastSkipReason = QStringLiteral("unsupported_after_override");
            continue;
        }
        QString skipReason;
        const PreparedTranscriptOverlay prepared =
            buildTranscriptOverlay(state,
                                   viewTransform,
                                   effectiveClip,
                                   state->currentSample,
                                   statusDrawable ? status : nullptr,
                                   &localStats,
                                   &skipReason);
        if (prepared.ready) {
            overlays.insert(clip.id, prepared);
            ++localStats.preparedCount;
        } else if (!skipReason.isEmpty()) {
            localStats.lastSkipReason = skipReason;
        }
    }
    if (stats) {
        *stats = localStats;
    }
    return overlays;
}

QString transcriptOverlayTextPrepMaterial(const PreparedTranscriptOverlayMap& overlays,
                                          const QSize& outputSize)
{
    QString material =
        QStringLiteral("out=%1x%2|").arg(outputSize.width()).arg(outputSize.height());
    QStringList clipIds = overlays.keys();
    std::sort(clipIds.begin(), clipIds.end());
    for (const QString& clipId : clipIds) {
        const PreparedTranscriptOverlay transcript = overlays.value(clipId);
        if (!transcript.ready) {
            continue;
        }
        material += QStringLiteral("t:%1:%2,%3,%4,%5:%6:")
                        .arg(clipId)
                        .arg(transcript.outputRect.x(), 0, 'f', 2)
                        .arg(transcript.outputRect.y(), 0, 'f', 2)
                        .arg(transcript.outputRect.width(), 0, 'f', 2)
                        .arg(transcript.outputRect.height(), 0, 'f', 2)
                        .arg(transcript.speakerTitle);
        material += transcriptOverlayStyleCacheMaterial(transcript.clip);
        material += QLatin1Char(':');
        for (const TranscriptOverlayLine& line : transcript.layout.lines) {
            material += line.words.join(QLatin1Char(' '));
            material += QLatin1Char('#');
            material += QString::number(line.activeWord);
            material += QLatin1Char(';');
        }
        material += QLatin1Char('|');
    }
    return material;
}

} // namespace jcut::direct_vulkan_preview
