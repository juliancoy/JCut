#include "render_internal.h"
#include "decoder_context.h"
#include "cpu_overlay_render_backend.h"

#include <QFileInfo>
#include <QPainter>

#include <algorithm>

namespace render_detail {

namespace {

QString transcriptSectionsCacheKeyForClip(const TimelineClip& clip)
{
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QFileInfo info(transcriptPath);
    const qint64 mtimeMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
    return clip.filePath + QLatin1Char('|') + transcriptPath + QLatin1Char('|') +
           QString::number(mtimeMs);
}

} // namespace

QVector<TimelineClip> sortedTranscriptOverlayClips(const QVector<TimelineClip>& clips,
                                                   const QVector<TimelineTrack>& tracks) {
    QVector<TimelineClip> overlayClips;
    for (const TimelineClip& clip : clips) {
        const bool visual = clipVisualPlaybackEnabled(clip, tracks);
        const bool transcriptOverlay =
            (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) &&
            clip.transcriptOverlay.enabled &&
            trackVisualModeForClip(clip, tracks) != TrackVisualMode::Hidden;
        if (visual || transcriptOverlay) {
            overlayClips.push_back(clip);
        }
    }
    std::sort(overlayClips.begin(), overlayClips.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex == b.trackIndex) {
            return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
        }
        return a.trackIndex > b.trackIndex;
    });
    return overlayClips;
}

void enqueueRenderSequenceLookahead(const RenderRequest& request,
                                    int64_t timelineFrame,
                                    const QVector<TimelineClip>& orderedClips,
                                    editor::AsyncDecoder* asyncDecoder,
                                    const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache) {
    if (!asyncDecoder || !editor::debugLeadPrefetchEnabled()) {
        return;
    }

    const int lookaheadFrames = qMax(editor::debugLeadPrefetchCount(),
                                     editor::debugPlaybackWindowAhead());
    QVector<editor::SequencePrefetchClip> sequenceClips;
    sequenceClips.reserve(orderedClips.size());
    for (const TimelineClip& clip : orderedClips) {
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, playbackMediaPathForClip(clip)});
    }
    const QVector<int64_t> futureTimelineFrames =
        editor::collectLookaheadTimelineFrames(timelineFrame,
                                               lookaheadFrames,
                                               1,
                                               {});
    for (int offset = 0; offset < futureTimelineFrames.size(); ++offset) {
        const int64_t futureTimelineFrame = futureTimelineFrames[offset];
        const int priority = qMax(8, 128 - (offset * 4));
        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(futureTimelineFrame),
                                                                   request.renderSyncMarkers,
                                                                   false,
                                                                   priority);
        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            const RenderAsyncFrameKey key{prefetch.decodePath, prefetch.sourceFrame};
            if (asyncFrameCache.contains(key)) {
                continue;
            }
            asyncDecoder->requestFrame(prefetch.decodePath,
                                       prefetch.sourceFrame,
                                       prefetch.priority,
                                       30000,
                                       editor::DecodeRequestKind::Prefetch,
                                       {});
        }
    }
}

void prewarmRenderSequenceSegment(const RenderRequest& request,
                                  int64_t segmentStartFrame,
                                  int64_t segmentEndFrame,
                                  const QVector<TimelineClip>& orderedClips,
                                  editor::AsyncDecoder* asyncDecoder,
                                  const QHash<RenderAsyncFrameKey, editor::FrameHandle>& asyncFrameCache) {
    if (!asyncDecoder || !editor::debugLeadPrefetchEnabled()) {
        return;
    }

    const int prewarmFrames = qMax(editor::debugPlaybackWindowAhead() * 2,
                                   editor::debugLeadPrefetchCount() * 4);
    const int lookaheadFrames = static_cast<int>(qMin<int64_t>(segmentEndFrame - segmentStartFrame + 1,
                                                               qMax<int64_t>(1, prewarmFrames)));
    QVector<editor::SequencePrefetchClip> sequenceClips;
    sequenceClips.reserve(orderedClips.size());
    for (const TimelineClip& clip : orderedClips) {
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, playbackMediaPathForClip(clip)});
    }
    const QVector<int64_t> prewarmTimelineFrames =
        editor::collectLookaheadTimelineFrames(segmentStartFrame - 1,
                                               lookaheadFrames,
                                               1,
                                               {});
    for (int64_t prewarmTimelineFrame : prewarmTimelineFrames) {
        const QVector<editor::SequencePrefetchRequest> requests =
            editor::collectSequencePrefetchRequestsAtTimelineFrame(sequenceClips,
                                                                   static_cast<qreal>(prewarmTimelineFrame),
                                                                   request.renderSyncMarkers,
                                                                   false,
                                                                   192);
        for (const editor::SequencePrefetchRequest& prefetch : requests) {
            const RenderAsyncFrameKey key{prefetch.decodePath, prefetch.sourceFrame};
            if (asyncFrameCache.contains(key)) {
                continue;
            }
            asyncDecoder->requestFrame(prefetch.decodePath,
                                       prefetch.sourceFrame,
                                       prefetch.priority,
                                       30000,
                                       editor::DecodeRequestKind::Prefetch,
                                       {});
        }
    }
}

editor::FrameHandle decodeRenderFrame(const QString& path,
                                      int64_t frameNumber,
                                      QHash<QString, editor::DecoderContext*>& decoders,
                                      editor::AsyncDecoder* asyncDecoder,
                                      QHash<RenderAsyncFrameKey, editor::FrameHandle>* asyncFrameCache) {
    if (isImageSequencePath(path) && asyncDecoder && asyncFrameCache) {
        const RenderAsyncFrameKey cacheKey{path, frameNumber};
        auto cachedIt = asyncFrameCache->find(cacheKey);
        if (cachedIt != asyncFrameCache->end()) {
            return cachedIt.value();
        }

        editor::FrameHandle result;
        bool completed = false;
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            completed = true;
            loop.quit();
        });

        asyncDecoder->requestFrame(path,
                                   frameNumber,
                                   255,
                                   30000,
                                   editor::DecodeRequestKind::Visible,
                                   [&](editor::FrameHandle frame) {
                                       result = frame;
                                       if (!result.isNull()) {
                                           asyncFrameCache->insert(cacheKey, result);
                                       }
                                       completed = true;
                                       loop.quit();
                                   });
        for (int64_t prefetchFrame = frameNumber + 1; prefetchFrame <= frameNumber + 6; ++prefetchFrame) {
            const RenderAsyncFrameKey prefetchKey{path, prefetchFrame};
            if (!asyncFrameCache->contains(prefetchKey)) {
                asyncDecoder->requestFrame(path,
                                           prefetchFrame,
                                           64,
                                           30000,
                                           editor::DecodeRequestKind::Prefetch,
                                           {});
            }
        }

        timeoutTimer.start(30000);
        while (!completed) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }

        if (!result.isNull()) {
            asyncFrameCache->insert(cacheKey, result);
        }
        return result;
    }

    auto it = decoders.find(path);
    if (it == decoders.end()) {
        editor::DecoderContext* ctx = new editor::DecoderContext(path);
        if (!ctx->initialize()) {
            delete ctx;
            return editor::FrameHandle();
        }
        it = decoders.insert(path, ctx);
    }
    return it.value()->decodeFrame(frameNumber);
}

QString avErrToString(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return QString::fromUtf8(errbuf);
}

QRect fitRect(const QSize& source, const QSize& bounds) {
    const QRectF rect = fitRectF(source, bounds);
    return QRect(qRound(rect.x()), qRound(rect.y()), qRound(rect.width()), qRound(rect.height()));
}

QRectF fitRectF(const QSize& source, const QSize& bounds) {
    return previewFitRectToBoundsF(source, QRectF(QPointF(0.0, 0.0), QSizeF(bounds)));
}

QRect coverRect(const QSize& source, const QSize& bounds)
{
    if (!source.isValid() || source.isEmpty() || !bounds.isValid() || bounds.isEmpty()) {
        return QRect(QPoint(0, 0), bounds);
    }

    const qreal scale = qMax(static_cast<qreal>(bounds.width()) / source.width(),
                             static_cast<qreal>(bounds.height()) / source.height());
    const int width = qMax(1, qRound(source.width() * scale));
    const int height = qMax(1, qRound(source.height() * scale));
    return QRect((bounds.width() - width) / 2,
                 (bounds.height() - height) / 2,
                 width,
                 height);
}

bool shouldDrawBlurredFillBackground(const QSize& source, const QSize& output)
{
    if (!source.isValid() || source.isEmpty() || !output.isValid() || output.isEmpty()) {
        return false;
    }
    const QRect fitted = fitRect(source, output);
    return fitted.width() < output.width() || fitted.height() < output.height();
}

QImage buildBlurredFillBackground(const QImage& source, const QSize& outputSize)
{
    if (source.isNull() || !outputSize.isValid() || outputSize.isEmpty()) {
        return QImage();
    }

    QImage sourceArgb = source;
    if (sourceArgb.format() != QImage::Format_ARGB32 &&
        sourceArgb.format() != QImage::Format_ARGB32_Premultiplied) {
        sourceArgb = source.convertToFormat(QImage::Format_ARGB32);
    }
    if (sourceArgb.isNull()) {
        return QImage();
    }

    QImage background(outputSize, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::black);
    {
        QPainter painter(&background);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRectF(coverRect(sourceArgb.size(), outputSize)), sourceArgb);
    }

    const QSize blurSize(qMax(24, outputSize.width() / 18),
                         qMax(24, outputSize.height() / 18));
    QImage blurred = background.scaled(blurSize,
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation)
                         .scaled(outputSize,
                                 Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
    if (blurred.format() != QImage::Format_ARGB32_Premultiplied) {
        blurred = blurred.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    QPainter painter(&blurred);
    painter.fillRect(blurred.rect(), QColor(0, 0, 0, 72));
    return blurred;
}

QString transcriptSpeakerTitleHtml(const QString& title, const QColor& color) {
    const QString safeTitle = title.trimmed().toHtmlEscaped();
    if (safeTitle.isEmpty()) {
        return QString();
    }
    return QStringLiteral(
               "<div style=\"text-align:center;"
               " font-weight:700;"
               " letter-spacing:0.02em;"
               " font-size:0.62em;"
               " margin:0 0 0.30em 0;"
               " color:%1;\">%2</div>")
        .arg(color.name(QColor::HexRgb), safeTitle);
}

TranscriptOverlayLayout transcriptOverlayLayoutForFrame(const TimelineClip& clip,
                                                        int64_t timelineFrame,
                                                        const QVector<RenderSyncMarker>& markers,
                                                        QHash<QString, QVector<TranscriptSection>>& transcriptCache) {
    if (!((clip.mediaType == ClipMediaType::Audio || clip.hasAudio) && clip.transcriptOverlay.enabled)) {
        return {};
    }
    const QString cacheKey = transcriptSectionsCacheKeyForClip(clip);
    if (!transcriptCache.contains(cacheKey)) {
        const QString clipCachePrefix = clip.filePath + QLatin1Char('|');
        for (auto it = transcriptCache.begin(); it != transcriptCache.end();) {
            if (it.key().startsWith(clipCachePrefix)) {
                it = transcriptCache.erase(it);
            } else {
                ++it;
            }
        }
        transcriptCache.insert(cacheKey, loadTranscriptSections(activeTranscriptPathForClipFile(clip.filePath)));
    }
    const QVector<TranscriptSection>& sections = transcriptCache.value(cacheKey);
    if (sections.isEmpty()) {
        return {};
    }
    const int64_t sourceFrame = transcriptFrameForClipAtTimelineSample(
        clip, frameToSamples(timelineFrame), markers);
    return transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame);
}

OverlayImage renderTranscriptOverlay(const QSize& imageSize,
                                     const RenderRequest& request,
                                     int64_t timelineFrame,
                                     const QVector<TimelineClip>& orderedClips,
                                     QHash<QString, QVector<TranscriptSection>>& transcriptCache)
{
    return overlayRenderBackend().renderTranscriptOverlay(
        imageSize, request, timelineFrame, orderedClips, transcriptCache);
}

} // namespace render_detail
