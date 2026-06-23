#include "audio_preview_support.h"

#include "editor_shared_timing.h"
#include "waveform_service.h"

QString audioPreviewDynamicsCacheKey(const PreviewSurface::AudioDynamicsSettings& settings)
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15|%16")
        .arg(settings.amplifyEnabled ? 1 : 0)
        .arg(settings.amplifyDb, 0, 'f', 2)
        .arg(settings.normalizeEnabled ? 1 : 0)
        .arg(settings.normalizeTargetDb, 0, 'f', 2)
        .arg(settings.selectiveNormalizeEnabled ? 1 : 0)
        .arg(settings.selectiveNormalizeMinSegmentSeconds, 0, 'f', 2)
        .arg(settings.selectiveNormalizePeakDb, 0, 'f', 2)
        .arg(settings.selectiveNormalizePasses)
        .arg(settings.peakReductionEnabled ? 1 : 0)
        .arg(settings.peakThresholdDb, 0, 'f', 2)
        .arg(settings.limiterEnabled ? 1 : 0)
        .arg(settings.limiterThresholdDb, 0, 'f', 2)
        .arg(settings.compressorEnabled ? 1 : 0)
        .arg(QStringLiteral("%1|%2")
                 .arg(settings.compressorThresholdDb, 0, 'f', 2)
                 .arg(settings.compressorRatio, 0, 'f', 2))
        .arg(settings.softClipEnabled ? 1 : 0)
        .arg(settings.waveformPreviewPostProcessing ? 1 : 0);
}

int64_t resolvedAudioPreviewClipSamples(const TimelineClip& clip)
{
    const int64_t fallbackSamples = clipTimelineDurationSamples(clip);

    QString mediaPath = playbackAudioPathForClip(clip);
    if (mediaPath.isEmpty()) {
        mediaPath = interactivePreviewMediaPathForClip(clip);
    }
    if (mediaPath.isEmpty()) {
        return fallbackSamples;
    }

    int64_t totalDecodedSamples = 0;
    if (!editor::WaveformService::instance().queryTotalSamples(mediaPath, &totalDecodedSamples)) {
        return fallbackSamples;
    }
    const int64_t sourceStartSample = qMax<int64_t>(0, clipSourceInSamples(clip));
    const int64_t availableSourceSamples = qMax<int64_t>(0, totalDecodedSamples - sourceStartSample);
    if (availableSourceSamples <= 0) {
        return fallbackSamples;
    }

    const int64_t metadataSourceSamples = qMax<int64_t>(
        1,
        sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(1, clip.sourceDurationFrames))));
    const int64_t resolvedSourceSamples = qMin(availableSourceSamples, metadataSourceSamples);
    const qreal playbackRate = qBound<qreal>(0.001, clip.playbackRate, 1000.0);
    const int64_t timelineSamples = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::llround(static_cast<qreal>(resolvedSourceSamples) / playbackRate)));
    return timelineSamples;
}

AudioPreviewViewport resolveAudioPreviewViewport(const TimelineClip& clip,
                                                int rowCount,
                                                qreal previewZoom,
                                                qreal previewPanNorm,
                                                int64_t currentSample)
{
    AudioPreviewViewport viewport;
    const int safeRows = qMax(1, rowCount);
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipSamples = resolvedAudioPreviewClipSamples(clip);
    const qreal minVisibleBySamples = qBound<qreal>(
        0.00001,
        (100.0 * static_cast<qreal>(safeRows)) / static_cast<qreal>(clipSamples),
        1.0);
    const qreal maxAudioZoom = qBound<qreal>(
        20.0,
        1.0 / qMax<qreal>(0.00001, minVisibleBySamples),
        100000.0);
    viewport.zoom = qBound<qreal>(1.0, previewZoom, maxAudioZoom);
    viewport.visibleFraction = qBound<qreal>(minVisibleBySamples, 1.0 / viewport.zoom, 1.0);
    const qreal maxStart = qMax<qreal>(0.0, 1.0 - viewport.visibleFraction);

    viewport.playheadVisible =
        currentSample >= clipStartSample && currentSample < (clipStartSample + clipSamples);
    if (viewport.playheadVisible) {
        viewport.playheadClipNorm = qBound<qreal>(
            0.0,
            static_cast<qreal>(qMax<int64_t>(0, currentSample - clipStartSample)) /
                static_cast<qreal>(clipSamples),
            1.0);
        const qreal centeredStart = viewport.playheadClipNorm - (viewport.visibleFraction * 0.5);
        viewport.startNorm = qBound<qreal>(0.0, centeredStart, maxStart);
    } else {
        viewport.startNorm = qBound<qreal>(0.0, previewPanNorm, maxStart);
    }

    viewport.endNorm = qBound<qreal>(viewport.startNorm, viewport.startNorm + viewport.visibleFraction, 1.0);
    if (viewport.playheadVisible) {
        viewport.playheadVisibleNorm = qBound<qreal>(
            0.0,
            (viewport.playheadClipNorm - viewport.startNorm) /
                qMax<qreal>(0.00001, viewport.visibleFraction),
            1.0);
    }
    return viewport;
}

bool syncAudioPreviewPanToPlayhead(PreviewInteractionState* state,
                                   int rowCount)
{
    if (!state ||
        !state->playing ||
        state->viewMode != PreviewSurface::ViewMode::Audio) {
        return false;
    }

    const TimelineClip* audioClip = nullptr;
    for (const TimelineClip& clip : state->clips) {
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipTimelineEndSamples(clip);
        const bool withinClip =
            state->currentSample >= clipStartSample && state->currentSample < clipEndSample;
        const bool includeForAudioView =
            clipAudioPlaybackEnabled(clip) &&
            (clip.id == state->selectedClipId || withinClip);
        const bool includeAsFallback = clipIsAudioOnly(clip) && withinClip;
        if (includeForAudioView || includeAsFallback) {
            audioClip = &clip;
            break;
        }
    }
    if (!audioClip) {
        return false;
    }

    const AudioPreviewViewport viewport = resolveAudioPreviewViewport(
        *audioClip, rowCount, state->previewZoom, state->previewPanOffset.x(), state->currentSample);
    if (!viewport.playheadVisible) {
        return false;
    }
    if (qAbs(viewport.startNorm - state->previewPanOffset.x()) < 0.000001) {
        return false;
    }
    state->previewPanOffset.setX(viewport.startNorm);
    return true;
}

bool queryAudioWaveformEnvelopeForClip(const TimelineClip& clip,
                                       const PreviewSurface::AudioDynamicsSettings& settings,
                                       int binCount,
                                       qreal rangeStartNorm,
                                       qreal rangeEndNorm,
                                       const QVector<RenderSyncMarker>& markers,
                                       QVector<qreal>* minOut,
                                       QVector<qreal>* maxOut)
{
    if (!minOut || !maxOut) {
        return false;
    }
    const int safeBins = qBound(64, binCount, 8192);
    minOut->fill(0.0, safeBins);
    maxOut->fill(0.0, safeBins);

    QString mediaPath = playbackAudioPathForClip(clip);
    if (mediaPath.isEmpty()) {
        mediaPath = interactivePreviewMediaPathForClip(clip);
    }
    if (mediaPath.isEmpty()) {
        return false;
    }

    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipTimelineDurationSamples = resolvedAudioPreviewClipSamples(clip);
    const qreal startNorm = qBound<qreal>(0.0, rangeStartNorm, 1.0);
    const qreal endNorm = qBound<qreal>(startNorm, rangeEndNorm, 1.0);
    const int64_t visibleStartOffset = static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(clipTimelineDurationSamples)));
    const int64_t visibleEndOffset = static_cast<int64_t>(
        std::ceil(endNorm * static_cast<qreal>(clipTimelineDurationSamples)));
    const int64_t visibleStartTimelineSample =
        clipStartSample + qBound<int64_t>(0, visibleStartOffset, clipTimelineDurationSamples - 1);
    const int64_t visibleEndTimelineSample =
        clipStartSample +
        qBound<int64_t>(visibleStartOffset + 1, visibleEndOffset, clipTimelineDurationSamples);
    const int64_t visibleStartSample =
        sourceSampleForClipAtTimelineSample(clip, visibleStartTimelineSample, markers);
    const int64_t visibleEndSampleExclusive =
        sourceSampleForClipAtTimelineSample(clip, qMax<int64_t>(visibleStartTimelineSample + 1,
                                                                visibleEndTimelineSample - 1),
                                            markers) + 1;
    const int64_t queryStartSample = qMin(visibleStartSample, visibleEndSampleExclusive - 1);
    const int64_t queryEndSample = qMax(visibleStartSample + 1, visibleEndSampleExclusive);
    const QString variantKey = settings.waveformPreviewPostProcessing
        ? audioPreviewDynamicsCacheKey(settings)
        : QStringLiteral("disk");

    const editor::WaveformService::WaveformProcessSettings processSettings{
        settings.amplifyEnabled,
        static_cast<float>(settings.amplifyDb),
        settings.normalizeEnabled,
        static_cast<float>(settings.normalizeTargetDb),
        settings.selectiveNormalizeEnabled,
        static_cast<float>(settings.selectiveNormalizeMinSegmentSeconds),
        static_cast<float>(settings.selectiveNormalizePeakDb),
        settings.selectiveNormalizePasses,
        settings.peakReductionEnabled,
        static_cast<float>(settings.peakThresholdDb),
        settings.limiterEnabled,
        static_cast<float>(settings.limiterThresholdDb),
        settings.compressorEnabled,
        static_cast<float>(settings.compressorThresholdDb),
        static_cast<float>(settings.compressorRatio),
        settings.softClipEnabled};

    QVector<float> minValues;
    QVector<float> maxValues;
    if (!editor::WaveformService::instance().queryEnvelope(mediaPath,
                                                           queryStartSample,
                                                           queryEndSample,
                                                           safeBins,
                                                           &minValues,
                                                           &maxValues,
                                                           variantKey,
                                                           settings.waveformPreviewPostProcessing
                                                               ? &processSettings
                                                               : nullptr) ||
        minValues.size() != safeBins ||
        maxValues.size() != safeBins) {
        return false;
    }

    for (int i = 0; i < safeBins; ++i) {
        const qreal minV = qBound<qreal>(-1.0, static_cast<qreal>(minValues[i]), 1.0);
        const qreal maxV = qBound<qreal>(-1.0, static_cast<qreal>(maxValues[i]), 1.0);
        (*minOut)[i] = qMin(minV, maxV);
        (*maxOut)[i] = qMax(minV, maxV);
    }
    return true;
}
