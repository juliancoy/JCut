#include "direct_vulkan_preview_interaction.h"

#include "audio_preview_support.h"
#include "direct_vulkan_preview_config.h"
#include "preview_speaker_profiles.h"
#include "preview_view_transform.h"
#include "titles.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace jcut::direct_vulkan_preview {

namespace {

void emitThrottledInteractionStatus(const std::function<void(const QString&)>& statusCallback,
                                    const QString& message,
                                    qint64 intervalMs = 1500)
{
    if (!statusCallback) {
        return;
    }
    static QHash<QString, qint64> lastEmitByMessage;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = lastEmitByMessage.value(message, 0);
    if (now - last < intervalMs) {
        return;
    }
    lastEmitByMessage.insert(message, now);
    statusCallback(message);
}

} // namespace

bool applyVideoPreviewWheelZoom(PreviewInteractionState* state,
                               const QRectF& surfaceRect,
                               const QPointF& surfacePosition,
                               int deltaY)
{
    if (!state || deltaY == 0 || surfaceRect.isEmpty()) {
        return false;
    }
    const PreviewZoomResult zoom = PreviewViewTransform::zoomForWheel(
        surfaceRect,
        state->outputSize,
        vulkanPreviewCanvasMarginPx(),
        state->previewZoom,
        state->previewPanOffset,
        surfacePosition,
        deltaY);
    if (!zoom.changed) {
        return true;
    }
    state->previewZoom = zoom.zoom;
    state->previewPanOffset = zoom.panOffset;
    return true;
}

bool applyAudioPreviewWheelZoom(PreviewInteractionState* state,
                                const QRectF& surfaceRect,
                                const QPointF& surfacePosition,
                                int deltaY)
{
    if (!state || deltaY == 0 || surfaceRect.isEmpty()) {
        return false;
    }
    const qreal oldZoom = qBound<qreal>(1.0, state->previewZoom, 100000.0);
    const qreal factor = deltaY > 0 ? 1.18 : (1.0 / 1.18);
    const qreal newZoom = qBound<qreal>(1.0, oldZoom * factor, 100000.0);
    if (qFuzzyCompare(oldZoom, newZoom)) {
        return true;
    }
    const qreal oldVisible = qBound<qreal>(0.00001, 1.0 / oldZoom, 1.0);
    const qreal newVisible = qBound<qreal>(0.00001, 1.0 / newZoom, 1.0);
    const qreal focus = qBound<qreal>(
        0.0,
        (surfacePosition.x() - surfaceRect.left()) / qMax<qreal>(1.0, surfaceRect.width()),
        1.0);
    const qreal oldStart = qBound<qreal>(0.0, state->previewPanOffset.x(), qMax<qreal>(0.0, 1.0 - oldVisible));
    const qreal focusNorm = oldStart + (focus * oldVisible);
    const qreal newStart = qBound<qreal>(0.0, focusNorm - (focus * newVisible), qMax<qreal>(0.0, 1.0 - newVisible));
    state->previewZoom = newZoom;
    state->previewPanOffset.setX(newStart);
    return true;
}

bool audioSeekSampleAtSurfacePosition(const PreviewInteractionState& state,
                                      const QRectF& surfaceRect,
                                      const QPointF& surfacePosition,
                                      int64_t* sampleOut)
{
    if (!sampleOut || state.viewMode != PreviewSurface::ViewMode::Audio) {
        return false;
    }
    const QRectF safeRect = surfaceRect.adjusted(18.0, 18.0, -18.0, -18.0);
    const QRectF panel = safeRect.adjusted(12.0, 12.0, -12.0, -12.0);
    const QRectF waveRect = panel.adjusted(24.0, 118.0, -24.0, -36.0);
    const qreal rulerGutterWidth = qBound<qreal>(32.0, waveRect.width() * 0.12, 56.0);
    const QRectF graphRect(waveRect.left() + rulerGutterWidth,
                           waveRect.top(),
                           qMax<qreal>(1.0, waveRect.width() - rulerGutterWidth),
                           waveRect.height());
    if (!graphRect.contains(surfacePosition)) {
        return false;
    }

    const TimelineClip* clip = nullptr;
    for (const TimelineClip& candidate : state.clips) {
        const int64_t clipStartSample = clipTimelineStartSamples(candidate);
        const int64_t clipEndSample = clipStartSample + frameToSamples(candidate.durationFrames);
        const bool withinClip = state.currentSample >= clipStartSample && state.currentSample < clipEndSample;
        const bool includeForAudioView =
            clipAudioPlaybackEnabled(candidate) &&
            (candidate.id == state.selectedClipId || withinClip);
        const bool includeAsFallback = clipIsAudioOnly(candidate) && withinClip;
        if (includeForAudioView || includeAsFallback) {
            clip = &candidate;
            break;
        }
    }
    if (!clip) {
        return false;
    }

    const int rowCount = qBound(2, static_cast<int>(waveRect.height()) / 88, 6);
    const int64_t clipStartSample = clipTimelineStartSamples(*clip);
    const int64_t clipSamples = resolvedAudioPreviewClipSamples(*clip);
    const AudioPreviewViewport viewport = resolveAudioPreviewViewport(
        *clip, rowCount, state.previewZoom, state.previewPanOffset.x(), state.currentSample);
    const qreal localX = qBound<qreal>(0.0,
                                       (surfacePosition.x() - graphRect.left()) / qMax<qreal>(1.0, graphRect.width()),
                                       1.0);
    const qreal localY = qBound<qreal>(0.0,
                                       (surfacePosition.y() - graphRect.top()) / qMax<qreal>(1.0, graphRect.height()),
                                       0.99999);
    const int row = qBound(0, static_cast<int>(std::floor(localY * rowCount)), rowCount - 1);
    const qreal clickedVisibleNorm = qBound<qreal>(
        0.0,
        (static_cast<qreal>(row) + localX) / static_cast<qreal>(qMax(1, rowCount)),
        1.0);
    qreal targetClipNorm = viewport.startNorm + (clickedVisibleNorm * viewport.visibleFraction);
    if (viewport.playheadVisible) {
        const qreal deltaVisibleNorm = clickedVisibleNorm - viewport.playheadVisibleNorm;
        const int64_t deltaSamples = static_cast<int64_t>(
            std::llround(deltaVisibleNorm * viewport.visibleFraction * static_cast<qreal>(clipSamples - 1)));
        *sampleOut = qBound<int64_t>(
            clipStartSample,
            state.currentSample + deltaSamples,
            clipStartSample + clipSamples - 1);
        return true;
    }
    const int64_t targetOffset = static_cast<int64_t>(
        std::llround(targetClipNorm * static_cast<qreal>(clipSamples - 1)));
    *sampleOut = qBound<int64_t>(clipStartSample, clipStartSample + targetOffset, clipStartSample + clipSamples - 1);
    return true;
}

bool clipSupportsTranscriptOverlay(const TimelineClip& clip)
{
    return (clip.mediaType == ClipMediaType::Audio || clip.hasAudio) && clip.transcriptOverlay.enabled;
}

const TimelineClip* clipForId(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            return &clip;
        }
    }
    return nullptr;
}

TimelineClip clipWithTransientTranscriptOverride(const PreviewInteractionState* state, const TimelineClip& clip)
{
    if (!state ||
        !state->transient.transcriptOverrideActive ||
        state->transient.transcriptOverrideClipId != clip.id) {
        return clip;
    }

    TimelineClip effective = clip;
    effective.transcriptOverlay.translationX = state->transient.transcriptTranslationOverride.x();
    effective.transcriptOverlay.translationY = state->transient.transcriptTranslationOverride.y();
    effective.transcriptOverlay.useManualPlacement = true;
    if (state->transient.transcriptSizeOverride.width() > 0.0) {
        effective.transcriptOverlay.boxWidth = state->transient.transcriptSizeOverride.width();
    }
    if (state->transient.transcriptSizeOverride.height() > 0.0) {
        effective.transcriptOverlay.boxHeight = state->transient.transcriptSizeOverride.height();
    }
    return effective;
}

TimelineClip::TransformKeyframe transformWithTransientOverride(const PreviewInteractionState* state,
                                                               const QString& clipId,
                                                               const TimelineClip::TransformKeyframe& fallback)
{
    if (state &&
        state->transient.transformOverrideActive &&
        state->transient.transformOverrideClipId == clipId) {
        return state->transient.transformOverride;
    }
    return fallback;
}

void clearVulkanDragOverrides(PreviewInteractionState* state)
{
    if (!state) {
        return;
    }
    state->transient.transformOverrideActive = false;
    state->transient.transformOverrideClipId.clear();
    state->transient.transformOverride = TimelineClip::TransformKeyframe();
    state->transient.transcriptOverrideActive = false;
    state->transient.transcriptOverrideClipId.clear();
    state->transient.transcriptTranslationOverride = QPointF();
    state->transient.transcriptSizeOverride = QSizeF();
}

QRectF transcriptOverlayBoundsForClip(const PreviewInteractionState* state,
                                      const TimelineClip& clip,
                                      const PreviewViewTransform& viewTransform,
                                      bool requireInteraction)
{
    const TimelineClip effectiveClip = clipWithTransientTranscriptOverride(state, clip);
    if (!state || !clipSupportsTranscriptOverlay(effectiveClip)) {
        return QRectF();
    }
    if (requireInteraction && !state->transcriptOverlayInteractionEnabled) {
        return QRectF();
    }
    const QSize safeOutputSize = state->outputSize.isValid() ? state->outputSize : QSize(1080, 1920);
    const QString transcriptPath = activeTranscriptPathForClip(effectiveClip);
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(effectiveClip, state->currentSample, state->renderSyncMarkers);
    const TranscriptOverlayLayout layout = transcriptOverlayLayoutAtSourceFrame(
        effectiveClip,
        sections,
        sourceFrame,
        TranscriptOverlayTiming{
            state->transcriptPrependMs, state->transcriptPostpendMs, state->transcriptOffsetMs});
    if (layout.lines.isEmpty()) {
        return QRectF();
    }

    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        effectiveClip, safeOutputSize, transcriptPath, sections, sourceFrame);
    if (outputRect.width() <= 0.0 || outputRect.height() <= 0.0) {
        return QRectF();
    }

    const QPointF center = viewTransform.outputToScreen(outputRect.center());
    const QPointF previewScale = viewTransform.outputScale();
    const QSizeF size(outputRect.width() * qMax<qreal>(0.0001, previewScale.x()),
                      outputRect.height() * qMax<qreal>(0.0001, previewScale.y()));
    if (size.width() <= 0.0 || size.height() <= 0.0) {
        return QRectF();
    }
    return QRectF(center.x() - (size.width() * 0.5),
                  center.y() - (size.height() * 0.5),
                  size.width(),
                  size.height());
}

VulkanInteractionOverlayInfos collectVulkanInteractionInfos(const PreviewInteractionState* state,
                                                          const QRectF& surfaceRect)
{
    VulkanInteractionOverlayInfos infos;
    if (!state) {
        return infos;
    }
    if (state->vulkanFrameStatuses.isEmpty()) {
        return infos;
    }
    if (surfaceRect.isEmpty()) {
        return infos;
    }
    const PreviewViewTransform viewTransform(
        surfaceRect,
        state->outputSize,
        vulkanPreviewCanvasMarginPx(),
        state->previewZoom,
        state->previewPanOffset);
    const QPointF previewScale = viewTransform.outputScale();
    const QHash<QString, QSize> sourceSizes = [&state]() {
        QHash<QString, QSize> sizes;
        for (const TimelineClip& clip : state->clips) {
            if (!clip.id.isEmpty()) {
                sizes.insert(clip.id, clip.sourceFrameSize);
            }
        }
        return sizes;
    }();

    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (!status.active || status.drawSuppressed) {
            continue;
        }
        const TimelineClip* clip = clipForId(state, status.clipId);
        const QRectF fitted = viewTransform.fittedClipRect(
            sourceSizes.value(status.clipId),
            status.frameSize);
        const TimelineClip::TransformKeyframe transform =
            transformWithTransientOverride(state, status.clipId, status.transform);
        const PreviewClipGeometry geometry = PreviewViewTransform::clipGeometry(
            fitted,
            previewScale,
            QPointF(transform.translationX, transform.translationY),
            transform.rotation,
            QPointF(transform.scaleX, transform.scaleY));
        const QRectF overlayBounds = [&]() {
            if (!clip) {
                return geometry.bounds;
            }
            if (clipSupportsTranscriptOverlay(*clip) && state->transcriptOverlayInteractionEnabled) {
                const QRectF candidate = transcriptOverlayBoundsForClip(state, *clip, viewTransform);
                if (candidate.isValid() && candidate.width() > 1.0 && candidate.height() > 1.0) {
                    return candidate;
                }
            }
            return geometry.bounds;
        }();
        const PreviewResizeHandles handles = PreviewViewTransform::resizeHandlesForBounds(overlayBounds);
        const bool transcriptBoundsValid = !overlayBounds.isEmpty() &&
            overlayBounds != geometry.bounds &&
            (clip && clipSupportsTranscriptOverlay(*clip) && state->transcriptOverlayInteractionEnabled);
        infos.push_back(VulkanInteractionOverlayInfo{
            status.clipId,
            overlayBounds,
            handles.right,
            handles.bottom,
            handles.corner,
            transcriptBoundsValid
                    ? PreviewOverlayKind::TranscriptOverlay
                    : PreviewOverlayKind::VisualClip,
            geometry.clipToScreen,
            geometry.localRect,
            transform,
            geometry.clipPixelSize,
            surfaceRect});
    }
    return infos;
}

QString clipIdAtPositionForVulkan(const VulkanInteractionOverlayInfos& infos, const QPointF& position)
{
    for (int i = infos.size() - 1; i >= 0; --i) {
        const VulkanInteractionOverlayInfo& info = infos.at(i);
        if (info.bounds.contains(position)) {
            return info.clipId;
        }
    }
    return QString();
}

QPointF mapScreenPointToNormalizedClipForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& screenPoint)
{
    if (info.clipPixelSize.width() > 1.0 &&
        info.clipPixelSize.height() > 1.0 &&
        !info.clipToScreen.isIdentity()) {
        bool invertible = false;
        const QTransform inverse = info.clipToScreen.inverted(&invertible);
        if (invertible) {
            const QPointF localPoint = inverse.map(screenPoint);
            const QRectF localRect(-info.clipPixelSize.width() / 2.0,
                                   -info.clipPixelSize.height() / 2.0,
                                   info.clipPixelSize.width(),
                                   info.clipPixelSize.height());
            return QPointF(
                qBound<qreal>(0.0, (localPoint.x() - localRect.left()) / qMax<qreal>(1.0, localRect.width()), 1.0),
                qBound<qreal>(0.0, (localPoint.y() - localRect.top()) / qMax<qreal>(1.0, localRect.height()), 1.0));
        }
    }

    return QPointF(
        qBound<qreal>(0.0, (screenPoint.x() - info.bounds.left()) / qMax<qreal>(1.0, info.bounds.width()), 1.0),
        qBound<qreal>(0.0, (screenPoint.y() - info.bounds.top()) / qMax<qreal>(1.0, info.bounds.height()), 1.0));
}

QPointF mapNormalizedClipPointToScreenForVulkan(const VulkanInteractionOverlayInfo& info,
                                                const QPointF& normalizedPoint)
{
    const qreal x = qBound<qreal>(0.0, normalizedPoint.x(), 1.0);
    const qreal y = qBound<qreal>(0.0, normalizedPoint.y(), 1.0);
    if (info.clipPixelSize.width() > 1.0 &&
        info.clipPixelSize.height() > 1.0 &&
        !info.clipToScreen.isIdentity()) {
        const QRectF localRect(-info.clipPixelSize.width() / 2.0,
                               -info.clipPixelSize.height() / 2.0,
                               info.clipPixelSize.width(),
                               info.clipPixelSize.height());
        const QPointF localPoint =
            PreviewViewTransform::localPointForNormalizedPoint(QPointF(x, y), localRect);
        return info.clipToScreen.map(localPoint);
    }
    return QPointF(info.bounds.left() + (x * info.bounds.width()),
                   info.bounds.top() + (y * info.bounds.height()));
}

QPainterPath faceDetectionScreenPathForVulkan(const VulkanInteractionOverlayInfo& info,
                                              const QRectF& normalizedBox,
                                              const QRectF& surfaceRect)
{
    QPainterPath boxPath;
    boxPath.moveTo(mapNormalizedClipPointToScreenForVulkan(info, normalizedBox.topLeft()));
    boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, normalizedBox.topRight()));
    boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, normalizedBox.bottomRight()));
    boxPath.lineTo(mapNormalizedClipPointToScreenForVulkan(info, normalizedBox.bottomLeft()));
    boxPath.closeSubpath();

    if (surfaceRect.isEmpty() || boxPath.boundingRect().intersects(surfaceRect)) {
        return boxPath;
    }

    constexpr qreal kOffscreenMarkerSizePx = 26.0;
    constexpr qreal kOffscreenMarkerInsetPx = 8.0;
    const QPointF offscreenCenter =
        mapNormalizedClipPointToScreenForVulkan(info, normalizedBox.center());
    const QRectF markerBounds(
        qBound<qreal>(surfaceRect.left() + kOffscreenMarkerInsetPx,
                      offscreenCenter.x(),
                      surfaceRect.right() - kOffscreenMarkerInsetPx) - (kOffscreenMarkerSizePx * 0.5),
        qBound<qreal>(surfaceRect.top() + kOffscreenMarkerInsetPx,
                      offscreenCenter.y(),
                      surfaceRect.bottom() - kOffscreenMarkerInsetPx) - (kOffscreenMarkerSizePx * 0.5),
        kOffscreenMarkerSizePx,
        kOffscreenMarkerSizePx);
    QPainterPath markerPath;
    markerPath.addRect(markerBounds);
    return markerPath;
}

bool dispatchFaceDetectionsBoxAtPosition(const PreviewInteractionState* state,
                                     const VulkanInteractionOverlayInfos& infos,
                                     const QPointF& surfacePosition,
                                     const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
                                     const std::function<void(const QString&)>& statusCallback)
{
    QElapsedTimer clickTimer;
    clickTimer.start();
    if (!state || !callback) {
        emitThrottledInteractionStatus(
            statusCallback,
            QStringLiteral("Face box click ignored: FaceDetections click callback is not installed."));
        return false;
    }
    if (state->facedetectionsOverlays.isEmpty()) {
        emitThrottledInteractionStatus(
            statusCallback,
            QStringLiteral("Face box click ignored: no FaceDetections boxes are available at this frame."));
        return false;
    }
    const VulkanPreviewFacestreamOverlay* nearestOverlay = nullptr;
    qreal nearestDistanceSq = std::numeric_limits<qreal>::max();
    int testedOverlays = 0;
    int mappedOverlays = 0;
    for (int overlayIndex = state->facedetectionsOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facedetectionsOverlays.at(overlayIndex);
        ++testedOverlays;
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            continue;
        }
        VulkanInteractionOverlayInfo info;
        if (!lookupVulkanInteractionInfo(infos, overlay.clipId, &info)) {
            continue;
        }
        ++mappedOverlays;
        const QPainterPath boxPath = faceDetectionScreenPathForVulkan(
            info,
            overlay.boxNorm,
            info.surfaceRect);
        QPainterPath hitPath = boxPath;
        const QRectF hitBounds = boxPath.boundingRect();
        constexpr qreal kMinOverlayHitSizePx = 18.0;
        constexpr qreal kNearestOverlayRadiusPx = 28.0;
        if (hitBounds.width() < kMinOverlayHitSizePx || hitBounds.height() < kMinOverlayHitSizePx) {
            const QPointF center = hitBounds.center();
            const QRectF expandedRect(center.x() - (kMinOverlayHitSizePx * 0.5),
                                      center.y() - (kMinOverlayHitSizePx * 0.5),
                                      kMinOverlayHitSizePx,
                                      kMinOverlayHitSizePx);
            hitPath.addRect(expandedRect);
        }
        if (!hitPath.contains(surfacePosition)) {
            const QPointF center = hitBounds.center();
            const qreal dx = surfacePosition.x() - center.x();
            const qreal dy = surfacePosition.y() - center.y();
            const qreal distanceSq = dx * dx + dy * dy;
            if (distanceSq <= (kNearestOverlayRadiusPx * kNearestOverlayRadiusPx) &&
                distanceSq < nearestDistanceSq) {
                nearestDistanceSq = distanceSq;
                nearestOverlay = &overlay;
            }
            continue;
        }
        const QPointF center = overlay.boxNorm.center();
        const qreal boxSideNorm =
            qBound<qreal>(0.01, qMax(overlay.boxNorm.width(), overlay.boxNorm.height()), 1.0);
        const QString clickedClipId = overlay.clipId;
        const QString clickedStreamId = overlay.streamId;
        const int clickedTrackId = overlay.trackId;
        const int64_t clickedSourceFrame = overlay.sourceFrame;
        qInfo().noquote()
            << QStringLiteral("Face box click dispatch: clip=%1 track=%2 stream=%3 source_frame=%4 hit_test_ms=%5 overlays=%6 mapped=%7")
                   .arg(clickedClipId)
                   .arg(clickedTrackId)
                   .arg(clickedStreamId.isEmpty() ? QStringLiteral("<empty>") : clickedStreamId)
                   .arg(clickedSourceFrame)
                   .arg(clickTimer.elapsed())
                   .arg(testedOverlays)
                   .arg(mappedOverlays);
        callback(clickedClipId,
                 clickedTrackId,
                 clickedStreamId,
                 clickedSourceFrame,
                 center.x(),
                 center.y(),
                 boxSideNorm);
        return true;
    }
    if (nearestOverlay) {
        const QPointF center = nearestOverlay->boxNorm.center();
        const qreal boxSideNorm =
            qBound<qreal>(0.01, qMax(nearestOverlay->boxNorm.width(), nearestOverlay->boxNorm.height()), 1.0);
        const QString clickedClipId = nearestOverlay->clipId;
        const QString clickedStreamId = nearestOverlay->streamId;
        const int clickedTrackId = nearestOverlay->trackId;
        const int64_t clickedSourceFrame = nearestOverlay->sourceFrame;
        qInfo().noquote()
            << QStringLiteral("Face box click dispatch nearest: clip=%1 track=%2 stream=%3 source_frame=%4 hit_test_ms=%5 overlays=%6 mapped=%7")
                   .arg(clickedClipId)
                   .arg(clickedTrackId)
                   .arg(clickedStreamId.isEmpty() ? QStringLiteral("<empty>") : clickedStreamId)
                   .arg(clickedSourceFrame)
                   .arg(clickTimer.elapsed())
                   .arg(testedOverlays)
                   .arg(mappedOverlays);
        callback(clickedClipId,
                 clickedTrackId,
                 clickedStreamId,
                 clickedSourceFrame,
                 center.x(),
                 center.y(),
                 boxSideNorm);
        return true;
    }
    emitThrottledInteractionStatus(
        statusCallback,
        QStringLiteral("Face box click ignored: no FaceDetections box at the clicked location."));
    return false;
}

bool dispatchFaceDetectionsFocusClearAtPosition(
    const PreviewInteractionState* state,
    const VulkanInteractionOverlayInfos& infos,
    const QPointF& surfacePosition,
    const std::function<void(const QString&, int, const QString&, int64_t, qreal, qreal, qreal)>& callback,
    const std::function<void(const QString&)>& statusCallback)
{
    return dispatchFaceDetectionsBoxAtPosition(
        state,
        infos,
        surfacePosition,
        callback,
        statusCallback);
}

bool updateHoveredFaceDetectionsBox(const PreviewInteractionState* state,
                                const VulkanInteractionOverlayInfos& infos,
                                const QPointF& surfacePosition)
{
    if (!state) {
        return false;
    }
    QString hoveredClipId;
    QString hoveredStreamId;
    int hoveredTrackId = -1;
    QString nearestClipId;
    QString nearestStreamId;
    int nearestTrackId = -1;
    qreal nearestDistanceSq = std::numeric_limits<qreal>::max();
    for (int overlayIndex = state->facedetectionsOverlays.size() - 1; overlayIndex >= 0; --overlayIndex) {
        const VulkanPreviewFacestreamOverlay& overlay = state->facedetectionsOverlays.at(overlayIndex);
        if (!overlay.boxNorm.isValid() || overlay.boxNorm.isEmpty()) {
            continue;
        }
        VulkanInteractionOverlayInfo info;
        if (!lookupVulkanInteractionInfo(infos, overlay.clipId, &info)) {
            continue;
        }
        const QPainterPath boxPath = faceDetectionScreenPathForVulkan(
            info,
            overlay.boxNorm,
            info.surfaceRect);
        QPainterPath hitPath = boxPath;
        const QRectF hitBounds = boxPath.boundingRect();
        constexpr qreal kMinOverlayHitSizePx = 18.0;
        constexpr qreal kNearestOverlayRadiusPx = 28.0;
        if (hitBounds.width() < kMinOverlayHitSizePx || hitBounds.height() < kMinOverlayHitSizePx) {
            const QPointF center = hitBounds.center();
            const QRectF expandedRect(center.x() - (kMinOverlayHitSizePx * 0.5),
                                      center.y() - (kMinOverlayHitSizePx * 0.5),
                                      kMinOverlayHitSizePx,
                                      kMinOverlayHitSizePx);
            hitPath.addRect(expandedRect);
        }
        if (!hitPath.contains(surfacePosition)) {
            const QPointF center = hitBounds.center();
            const qreal dx = surfacePosition.x() - center.x();
            const qreal dy = surfacePosition.y() - center.y();
            const qreal distanceSq = dx * dx + dy * dy;
            if (distanceSq <= (kNearestOverlayRadiusPx * kNearestOverlayRadiusPx) &&
                distanceSq < nearestDistanceSq) {
                nearestDistanceSq = distanceSq;
                nearestClipId = overlay.clipId;
                nearestStreamId = overlay.streamId;
                nearestTrackId = overlay.trackId;
            }
            continue;
        }
        hoveredClipId = overlay.clipId;
        hoveredStreamId = overlay.streamId;
        hoveredTrackId = overlay.trackId;
        break;
    }
    if (hoveredTrackId < 0 && nearestTrackId >= 0) {
        hoveredClipId = nearestClipId;
        hoveredStreamId = nearestStreamId;
        hoveredTrackId = nearestTrackId;
    }

    PreviewInteractionTransientState& transient =
        const_cast<PreviewInteractionState*>(state)->transient;
    const bool changed =
        transient.hoveredFaceDetectionsTrackId != hoveredTrackId ||
        transient.hoveredFaceDetectionsClipId != hoveredClipId ||
        transient.hoveredFaceDetectionsId != hoveredStreamId;
    if (changed) {
        transient.hoveredFaceDetectionsTrackId = hoveredTrackId;
        transient.hoveredFaceDetectionsClipId = hoveredClipId;
        transient.hoveredFaceDetectionsId = hoveredStreamId;
    }
    return hoveredTrackId >= 0;
}

bool clipIdIsTitleForVulkan(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state || clipId.isEmpty()) {
        return false;
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            return clip.mediaType == ClipMediaType::Title;
        }
    }
    return false;
}

bool lookupVulkanInteractionInfo(const VulkanInteractionOverlayInfos& infos,
                                const QString& clipId,
                                VulkanInteractionOverlayInfo* outInfo)
{
    if (!outInfo) {
        return false;
    }
    for (const VulkanInteractionOverlayInfo& info : infos) {
        if (info.clipId == clipId) {
            *outInfo = info;
            return true;
        }
    }
    return false;
}

TimelineClip::TransformKeyframe currentTransformForVulkanClip(const PreviewInteractionState* state, const QString& clipId)
{
    if (!state) {
        return TimelineClip::TransformKeyframe();
    }
    for (const TimelineClip& clip : state->clips) {
        if (clip.id == clipId) {
            if (clip.mediaType == ClipMediaType::Title) {
                const int64_t localFrame = qMax<int64_t>(0,
                    static_cast<int64_t>(state->currentFramePosition) - clip.startFrame);
                const EvaluatedTitle evaluated = evaluateTitleAtLocalFrame(clip, localFrame);
                TimelineClip::TransformKeyframe keyframe;
                keyframe.frame = qBound<int64_t>(0, localFrame, qMax<int64_t>(0, clip.durationFrames - 1));
                keyframe.translationX = evaluated.x;
                keyframe.translationY = evaluated.y;
                keyframe.scaleX = 1.0;
                keyframe.scaleY = 1.0;
                return transformWithTransientOverride(state, clipId, keyframe);
            }
            if (clip.sourceTransformLocked && !clip.linkedSourceClipId.trimmed().isEmpty()) {
                return transformWithTransientOverride(
                    state,
                    clipId,
                    evaluateClipRenderTransformWithSourceLockAtPosition(
                        clip,
                        state->clips,
                        state->currentFramePosition,
                        state->renderSyncMarkers,
                        state->playbackTiming,
                        state->outputSize));
            }
            const VulkanPreviewFacestreamOverlay* selectedFaceOverlay = nullptr;
            for (const VulkanPreviewFacestreamOverlay& overlay : state->facedetectionsOverlays) {
                if (overlay.clipId != clip.id ||
                    overlay.trackId < 0 ||
                    !state->selectedSpeakerAssignedFaceTrackIds.contains(overlay.trackId) ||
                    !overlay.boxNorm.isValid() ||
                    overlay.boxNorm.isEmpty()) {
                    continue;
                }
                if (!selectedFaceOverlay ||
                    overlay.confidence > selectedFaceOverlay->confidence ||
                    (qFuzzyCompare(overlay.confidence, selectedFaceOverlay->confidence) &&
                     overlay.boxNorm.height() > selectedFaceOverlay->boxNorm.height())) {
                    selectedFaceOverlay = &overlay;
                }
            }
            if (selectedFaceOverlay) {
                const QPointF faceCenter(selectedFaceOverlay->boxNorm.center());
                const qreal faceBoxSize = qMax<qreal>(
                    selectedFaceOverlay->boxNorm.height(),
                    selectedFaceOverlay->boxNorm.width());
                TimelineClip::TransformKeyframe base;
                base.frame = qMax<int64_t>(
                    0,
                    static_cast<int64_t>(std::floor(state->currentFramePosition)) - clip.startFrame);
                base.translationX = clip.baseTranslationX;
                base.translationY = clip.baseTranslationY;
                base.rotation = clip.baseRotation;
                base.scaleX = sanitizeScaleValue(clip.baseScaleX);
                base.scaleY = sanitizeScaleValue(clip.baseScaleY);
                return transformWithTransientOverride(
                    state,
                    clipId,
                    composeClipTransforms(
                        base,
                        evaluateClipSpeakerFramingForFaceBoxAtPosition(
                            clip,
                            state->currentFramePosition,
                            faceCenter,
                            faceBoxSize,
                            0.0,
                            state->outputSize)));
            }
            return transformWithTransientOverride(state, clipId, evaluateClipRenderTransformWithSourceLockAtPosition(
                clip,
                state->clips,
                state->currentFramePosition,
                state->renderSyncMarkers,
                state->playbackTiming,
                state->outputSize));
        }
    }
    return TimelineClip::TransformKeyframe();
}


} // namespace jcut::direct_vulkan_preview
