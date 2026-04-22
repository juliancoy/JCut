#include "render_internal.h"
#include "decoder_context.h"

namespace render_detail {

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
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, clip.filePath});
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
        sequenceClips.push_back(editor::SequencePrefetchClip{clip, clip.filePath});
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
    if (source.isEmpty() || bounds.isEmpty()) {
        return QRect(QPoint(0, 0), bounds);
    }

    QSize scaled = source;
    scaled.scale(bounds, Qt::KeepAspectRatio);
    const QPoint topLeft((bounds.width() - scaled.width()) / 2,
                         (bounds.height() - scaled.height()) / 2);
    return QRect(topLeft, scaled);
}

TranscriptOverlayLayout transcriptOverlayLayoutForFrame(const TimelineClip& clip,
                                                        int64_t timelineFrame,
                                                        const QVector<RenderSyncMarker>& markers,
                                                        QHash<QString, QVector<TranscriptSection>>& transcriptCache) {
    if (!((clip.mediaType == ClipMediaType::Audio || clip.hasAudio) && clip.transcriptOverlay.enabled)) {
        return {};
    }
    const QString cacheKey = clip.filePath;
    if (!transcriptCache.contains(cacheKey)) {
        transcriptCache.insert(cacheKey, loadTranscriptSections(transcriptWorkingPathForClipFile(clip.filePath)));
    }
    const QVector<TranscriptSection>& sections = transcriptCache.value(cacheKey);
    if (sections.isEmpty()) {
        return {};
    }
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip,
                                                                     static_cast<qreal>(timelineFrame),
                                                                     markers);
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) {
            return {};
        }
        if (sourceFrame <= section.endFrame) {
            const qreal estimatedLineHeight = qMax<qreal>(12.0, clip.transcriptOverlay.fontPointSize * 1.35);
            const qreal usableHeight = qMax<qreal>(estimatedLineHeight, clip.transcriptOverlay.boxHeight - 28.0);
            const int fittedLines = qMax(1, static_cast<int>(std::floor(usableHeight / estimatedLineHeight)));
            const qreal estimatedCharWidth = qMax<qreal>(6.0, clip.transcriptOverlay.fontPointSize * 0.62);
            const qreal usableWidth = qMax<qreal>(estimatedCharWidth, clip.transcriptOverlay.boxWidth - 36.0);
            const int fittedChars = qMax(1, static_cast<int>(std::floor(usableWidth / estimatedCharWidth)));
            return layoutTranscriptSection(section,
                                           sourceFrame,
                                           qMax(1, qMin(clip.transcriptOverlay.maxCharsPerLine, fittedChars)),
                                           qMax(1, qMin(clip.transcriptOverlay.maxLines, fittedLines)),
                                           clip.transcriptOverlay.autoScroll);
        }
    }
    return {};
}

void renderTranscriptOverlays(QImage* canvas,
                              const RenderRequest& request,
                              int64_t timelineFrame,
                              const QVector<TimelineClip>& orderedClips,
                              QHash<QString, QVector<TranscriptSection>>& transcriptCache) {
    if (!canvas || canvas->isNull()) {
        return;
    }

    QPainter painter(canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const TimelineClip& clip : orderedClips) {
        if (timelineFrame < clip.startFrame || timelineFrame >= clip.startFrame + clip.durationFrames) {
            continue;
        }
        const TranscriptOverlayLayout overlayLayout =
            transcriptOverlayLayoutForFrame(clip, timelineFrame, request.renderSyncMarkers, transcriptCache);
        if (overlayLayout.lines.isEmpty()) {
            continue;
        }

        const QRectF bounds((request.outputSize.width() / 2.0) + clip.transcriptOverlay.translationX -
                                (clip.transcriptOverlay.boxWidth / 2.0),
                            (request.outputSize.height() / 2.0) + clip.transcriptOverlay.translationY -
                                (clip.transcriptOverlay.boxHeight / 2.0),
                            clip.transcriptOverlay.boxWidth,
                            clip.transcriptOverlay.boxHeight);
        if (clip.transcriptOverlay.showBackground) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 120));
            painter.drawRoundedRect(bounds, 14.0, 14.0);
        }
        QFont font(clip.transcriptOverlay.fontFamily);
        font.setPixelSize(clip.transcriptOverlay.fontPointSize);
        font.setBold(clip.transcriptOverlay.bold);
        font.setItalic(clip.transcriptOverlay.italic);
        const QRectF textBounds = bounds.adjusted(18.0, 14.0, -18.0, -14.0);
        const QColor highlightFillColor(QStringLiteral("#fff2a8"));
        const QColor highlightTextColor(QStringLiteral("#181818"));
        QTextDocument shadowDoc;
        shadowDoc.setDefaultFont(font);
        shadowDoc.setDocumentMargin(0.0);
        shadowDoc.setTextWidth(textBounds.width());
        shadowDoc.setHtml(transcriptOverlayHtml(overlayLayout,
                                                QColor(0, 0, 0, 200),
                                                QColor(0, 0, 0, 200),
                                                QColor(0, 0, 0, 0)));
        QTextDocument textDoc;
        textDoc.setDefaultFont(font);
        textDoc.setDocumentMargin(0.0);
        textDoc.setTextWidth(textBounds.width());
        textDoc.setHtml(transcriptOverlayHtml(overlayLayout,
                                              clip.transcriptOverlay.textColor,
                                              highlightTextColor,
                                              highlightFillColor));
        const qreal widthScale = textDoc.size().width() > textBounds.width()
            ? textBounds.width() / textDoc.size().width()
            : 1.0;
        const qreal heightScale = textDoc.size().height() > textBounds.height()
            ? textBounds.height() / textDoc.size().height()
            : 1.0;
        const qreal docScale = qMin(widthScale, heightScale);
        const qreal scaledDocHeight = textDoc.size().height() * docScale;
        const qreal textY = textBounds.top() + qMax<qreal>(0.0, (textBounds.height() - scaledDocHeight) / 2.0);
        painter.save();
        painter.translate(textBounds.left() + 5.0, textY + 5.0);
        if (docScale < 0.999) {
            painter.scale(docScale, docScale);
        }
        shadowDoc.drawContents(&painter);
        if (docScale < 0.999) {
            painter.scale(1.0 / docScale, 1.0 / docScale);
        }
        painter.translate(-5.0, -5.0);
        if (docScale < 0.999) {
            painter.scale(docScale, docScale);
        }
        textDoc.drawContents(&painter);
        painter.restore();
    }
}


} // namespace render_detail
