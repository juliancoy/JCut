#include "audio_preview_support.h"

#include "waveform_service.h"

QString audioPreviewDynamicsCacheKey(const PreviewSurface::AudioDynamicsSettings& settings)
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15")
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
        .arg(settings.waveformPreviewPostProcessing ? 1 : 0);
}

bool queryAudioWaveformEnvelopeForClip(const TimelineClip& clip,
                                       const PreviewSurface::AudioDynamicsSettings& settings,
                                       int binCount,
                                       qreal rangeStartNorm,
                                       qreal rangeEndNorm,
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

    const int64_t sourceStartSample = qMax<int64_t>(0, clipSourceInSamples(clip));
    const int64_t sourceDurationSamples =
        qMax<int64_t>(1, sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(1, clip.durationFrames))));
    const qreal startNorm = qBound<qreal>(0.0, rangeStartNorm, 1.0);
    const qreal endNorm = qBound<qreal>(startNorm, rangeEndNorm, 1.0);
    const int64_t visibleStartOffset = static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceDurationSamples)));
    const int64_t visibleEndOffset = static_cast<int64_t>(
        std::ceil(endNorm * static_cast<qreal>(sourceDurationSamples)));
    const int64_t visibleStartSample = sourceStartSample + qBound<int64_t>(0, visibleStartOffset, sourceDurationSamples);
    const int64_t visibleEndSample = sourceStartSample + qBound<int64_t>(
        visibleStartOffset + 1,
        visibleEndOffset,
        sourceDurationSamples);
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
        static_cast<float>(settings.compressorRatio)};

    QVector<float> minValues;
    QVector<float> maxValues;
    if (!editor::WaveformService::instance().queryEnvelope(mediaPath,
                                                           visibleStartSample,
                                                           visibleEndSample,
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
