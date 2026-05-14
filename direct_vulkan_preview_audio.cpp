#include "direct_vulkan_preview_audio.h"

#include "audio_preview_support.h"
#include "editor_shared.h"
#include "preview_surface.h"
#include "vulkan_audio_tab.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QSet>
#include <QVulkanFunctions>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSpectrumSignalLength = 1 << 15;
constexpr int kSpectrogramTileColumns = 64;

struct HoverSpeakerProfile {
    QString speakerId;
    QString name;
    QString organization;
    QString description;
    QString imagePath;
};

struct HoverSpeakerProfileCacheEntry {
    qint64 mtimeMs = -1;
    QHash<QString, HoverSpeakerProfile> profilesBySpeaker;
};

QHash<QString, HoverSpeakerProfileCacheEntry>& hoverSpeakerProfileCache()
{
    static QHash<QString, HoverSpeakerProfileCacheEntry> cache;
    return cache;
}

QHash<QString, QPixmap>& hoverSpeakerImageCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

QString clippedSummaryFromWords(const QStringList& words)
{
    if (words.isEmpty()) {
        return QStringLiteral("No transcript summary available.");
    }
    QStringList clipped;
    clipped.reserve(qMin(34, words.size()));
    for (int i = 0; i < words.size() && i < 34; ++i) {
        QString token = words.at(i).trimmed();
        token.remove(QRegularExpression(QStringLiteral("^[\\s\\p{Punct}]+|[\\s\\p{Punct}]+$")));
        if (token.isEmpty()) {
            continue;
        }
        clipped.push_back(token);
    }
    if (clipped.isEmpty()) {
        return QStringLiteral("No transcript summary available.");
    }
    QString summary = clipped.join(QLatin1Char(' '));
    if (words.size() > clipped.size()) {
        summary += QStringLiteral("...");
    }
    return summary;
}

QHash<QString, QStringList> wordsBySpeakerFromTranscriptRoot(const QJsonObject& root)
{
    QHash<QString, QStringList> wordsBySpeaker;
    const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& segValue : segments) {
        const QJsonObject segObj = segValue.toObject();
        const QString segmentSpeaker = segObj.value(QStringLiteral("speaker")).toString().trimmed();
        const QJsonArray words = segObj.value(QStringLiteral("words")).toArray();
        for (const QJsonValue& wordValue : words) {
            const QJsonObject wordObj = wordValue.toObject();
            if (wordObj.value(QStringLiteral("skipped")).toBool(false)) {
                continue;
            }
            QString speakerId = wordObj.value(QStringLiteral("speaker")).toString().trimmed();
            if (speakerId.isEmpty()) {
                speakerId = segmentSpeaker;
            }
            const QString token = wordObj.value(QStringLiteral("word")).toString().trimmed();
            if (!speakerId.isEmpty() && !token.isEmpty()) {
                wordsBySpeaker[speakerId].push_back(token);
            }
        }
    }
    return wordsBySpeaker;
}

const HoverSpeakerProfile* hoverSpeakerProfileFor(const QString& transcriptPath, const QString& speakerId)
{
    if (transcriptPath.isEmpty() || speakerId.trimmed().isEmpty()) {
        return nullptr;
    }
    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return nullptr;
    }
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    HoverSpeakerProfileCacheEntry& entry = hoverSpeakerProfileCache()[transcriptPath];
    if (entry.mtimeMs != mtimeMs || entry.profilesBySpeaker.isEmpty()) {
        entry = HoverSpeakerProfileCacheEntry{};
        entry.mtimeMs = mtimeMs;
        QJsonDocument doc;
        if (loadTranscriptJsonCached(transcriptPath, &doc) && doc.isObject()) {
            const QJsonObject root = doc.object();
            const QHash<QString, QStringList> wordsBySpeaker = wordsBySpeakerFromTranscriptRoot(root);
            const QJsonObject profiles = root.value(QStringLiteral("speaker_profiles")).toObject();
            QSet<QString> speakerIds;
            for (auto it = wordsBySpeaker.constBegin(); it != wordsBySpeaker.constEnd(); ++it) {
                speakerIds.insert(it.key());
            }
            for (auto it = profiles.begin(); it != profiles.end(); ++it) {
                speakerIds.insert(it.key());
            }
            for (const QString& id : speakerIds) {
                const QJsonObject profileObj = profiles.value(id).toObject();
                HoverSpeakerProfile profile;
                profile.speakerId = id;
                profile.name = profileObj.value(QStringLiteral("name")).toString(id).trimmed();
                profile.organization = profileObj.value(QStringLiteral("organization")).toString().trimmed();
                QString description = profileObj.value(QStringLiteral("brief_description")).toString().trimmed();
                if (description.isEmpty()) {
                    description = profileObj.value(QStringLiteral("description")).toString().trimmed();
                }
                if (description.isEmpty()) {
                    description = profileObj.value(QStringLiteral("bio")).toString().trimmed();
                }
                if (description.isEmpty()) {
                    description = clippedSummaryFromWords(wordsBySpeaker.value(id));
                }
                profile.description = description;
                QString imagePath = profileObj.value(QStringLiteral("image_path")).toString().trimmed();
                if (imagePath.isEmpty()) {
                    imagePath = profileObj.value(QStringLiteral("avatar_path")).toString().trimmed();
                }
                if (imagePath.isEmpty()) {
                    imagePath = profileObj.value(QStringLiteral("photo_path")).toString().trimmed();
                }
                if (imagePath.isEmpty()) {
                    imagePath = profileObj.value(QStringLiteral("image")).toString().trimmed();
                }
                if (!imagePath.isEmpty() && QDir::isRelativePath(imagePath)) {
                    imagePath = QFileInfo(info.absolutePath(), imagePath).absoluteFilePath();
                }
                profile.imagePath = imagePath;
                entry.profilesBySpeaker.insert(id, profile);
            }
        }
    }
    auto it = entry.profilesBySpeaker.constFind(speakerId);
    if (it == entry.profilesBySpeaker.constEnd()) {
        return nullptr;
    }
    return &(*it);
}

QPixmap hoverSpeakerImage(const HoverSpeakerProfile& profile, int edgePx)
{
    const int safeSize = qBound(28, edgePx, 192);
    if (profile.imagePath.trimmed().isEmpty()) {
        return QPixmap();
    }
    const QString cacheKey = QStringLiteral("%1|%2").arg(profile.imagePath).arg(safeSize);
    auto& cache = hoverSpeakerImageCache();
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    QPixmap pix(profile.imagePath);
    if (!pix.isNull() && (pix.width() != safeSize || pix.height() != safeSize)) {
        pix = pix.scaled(safeSize, safeSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    cache.insert(cacheKey, pix);
    return pix;
}

QPixmap fallbackSpeakerAvatar(const QString& speakerId, const QString& displayName, int edgePx)
{
    const int size = qBound(28, edgePx, 192);
    QPixmap avatar(size, size);
    avatar.fill(Qt::transparent);
    QPainter p(&avatar);
    p.setRenderHint(QPainter::Antialiasing, true);
    const uint hueHash = qHash(speakerId.trimmed().isEmpty() ? displayName : speakerId);
    QColor base = QColor::fromHsv(static_cast<int>(hueHash % 360), 140, 165);
    p.setPen(Qt::NoPen);
    p.setBrush(base);
    p.drawEllipse(QRect(0, 0, size, size));
    p.setPen(QPen(QColor(255, 255, 255, 210), 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(1.0, 1.0, size - 2.0, size - 2.0));

    QString initials = displayName.trimmed();
    if (initials.isEmpty()) {
        initials = speakerId.trimmed();
    }
    if (initials.isEmpty()) {
        initials = QStringLiteral("?");
    } else {
        const QStringList parts = initials.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            initials = parts.at(0).left(1) + parts.at(1).left(1);
        } else {
            initials = parts.first().left(2);
        }
    }
    p.setPen(QColor(245, 250, 255));
    QFont f = p.font();
    f.setBold(true);
    f.setPointSize(qMax(10, size / 3));
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, initials.toUpper());
    return avatar;
}

QList<TimelineClip> activeAudioClipsForState(const PreviewInteractionState* state)
{
    QList<TimelineClip> active;
    if (!state) {
        return active;
    }
    for (const TimelineClip& clip : state->clips) {
        if (!(clip.mediaType == ClipMediaType::Audio || clip.hasAudio)) {
            continue;
        }
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
        if (state->currentSample >= clipStartSample && state->currentSample < clipEndSample) {
            active.push_back(clip);
        }
    }
    if (active.isEmpty() && !state->selectedClipId.isEmpty()) {
        for (const TimelineClip& clip : state->clips) {
            if (clip.id == state->selectedClipId &&
                (clip.mediaType == ClipMediaType::Audio || clip.hasAudio)) {
                active.push_back(clip);
                break;
            }
        }
    }
    std::sort(active.begin(), active.end(), [](const TimelineClip& a, const TimelineClip& b) {
        if (a.trackIndex != b.trackIndex) {
            return a.trackIndex < b.trackIndex;
        }
        return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
    });
    return active;
}

QString speakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame)
{
    for (const TranscriptSection& section : sections) {
        if (sourceFrame < section.startFrame) {
            return QString();
        }
        if (sourceFrame > section.endFrame) {
            continue;
        }
        int bestIndex = -1;
        for (int i = 0; i < section.words.size(); ++i) {
            const TranscriptWord& word = section.words.at(i);
            if (sourceFrame >= word.startFrame && sourceFrame <= word.endFrame) {
                bestIndex = i;
                break;
            }
            if (sourceFrame > word.endFrame) {
                bestIndex = i;
            }
        }
        if (bestIndex < 0 && !section.words.isEmpty()) {
            bestIndex = 0;
        }
        if (bestIndex >= 0 && bestIndex < section.words.size()) {
            return section.words.at(bestIndex).speaker.trimmed();
        }
        return QString();
    }
    return QString();
}

QColor speakerColor(const QString& speakerId, int alpha)
{
    const uint hueHash = qHash(speakerId);
    QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 170, 185);
    color.setAlpha(qBound(0, alpha, 255));
    return color;
}

void fillShortUnknownSpeakerGaps(QVector<int>* speakerIndexByBin, int maxGapBins)
{
    if (!speakerIndexByBin || speakerIndexByBin->isEmpty() || maxGapBins <= 0) {
        return;
    }
    QVector<int>& bins = *speakerIndexByBin;
    const int count = bins.size();
    int i = 0;
    while (i < count) {
        if (bins[i] >= 0) {
            ++i;
            continue;
        }
        const int start = i;
        while (i < count && bins[i] < 0) {
            ++i;
        }
        const int end = i;
        const int gap = end - start;
        if (gap <= 0 || gap > maxGapBins || start == 0 || end >= count) {
            continue;
        }
        const int left = bins[start - 1];
        const int right = bins[end];
        if (left < 0 || right < 0 || left != right) {
            continue;
        }
        for (int k = start; k < end; ++k) {
            bins[k] = left;
        }
    }
}

struct DecodedAudioCacheEntry {
    QVector<float> samples;
    int sampleRate = 0;
};

QHash<QString, DecodedAudioCacheEntry>& decodedAudioCache()
{
    static QHash<QString, DecodedAudioCacheEntry> cache;
    return cache;
}

bool decodeMonoSamples(const QString& mediaPath, int targetSampleRate, DecodedAudioCacheEntry* out)
{
    if (!out || mediaPath.isEmpty()) {
        return false;
    }
    const QString key = mediaPath + QLatin1Char('|') + QString::number(targetSampleRate);
    auto it = decodedAudioCache().constFind(key);
    if (it != decodedAudioCache().constEnd()) {
        *out = it.value();
        return !out->samples.isEmpty();
    }

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, mediaPath.toUtf8().constData(), nullptr, nullptr) < 0 || !formatCtx) {
        return false;
    }
    auto closeFormat = [&]() { avformat_close_input(&formatCtx); };
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        closeFormat();
        return false;
    }
    const int streamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        closeFormat();
        return false;
    }
    AVStream* stream = formatCtx->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codecCtx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!codecCtx ||
        avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0 ||
        avcodec_open2(codecCtx, codec, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        closeFormat();
        return false;
    }
    const AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr,
                            &outLayout,
                            AV_SAMPLE_FMT_FLT,
                            targetSampleRate,
                            &codecCtx->ch_layout,
                            codecCtx->sample_fmt,
                            codecCtx->sample_rate,
                            0,
                            nullptr) < 0 ||
        !swr ||
        swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&codecCtx);
        closeFormat();
        return false;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        closeFormat();
        return false;
    }

    QVector<float> samples;
    auto receive = [&]() {
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
            const int outSamples = swr_get_out_samples(swr, frame->nb_samples);
            if (outSamples > 0) {
                std::vector<float> mono(static_cast<size_t>(outSamples), 0.0f);
                uint8_t* outData[1] = {reinterpret_cast<uint8_t*>(mono.data())};
                const int converted = swr_convert(swr,
                                                  outData,
                                                  outSamples,
                                                  const_cast<const uint8_t**>(frame->extended_data),
                                                  frame->nb_samples);
                for (int i = 0; i < converted; ++i) {
                    samples.push_back(qBound(-1.0f, mono[static_cast<size_t>(i)], 1.0f));
                }
            }
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index == streamIndex && avcodec_send_packet(codecCtx, packet) == 0) {
            receive();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(codecCtx, nullptr);
    receive();

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    closeFormat();

    DecodedAudioCacheEntry entry;
    entry.samples = std::move(samples);
    entry.sampleRate = targetSampleRate;
    decodedAudioCache().insert(key, entry);
    *out = entry;
    return !out->samples.isEmpty();
}

double spectrumWindowWeight(int mode, int index, int length)
{
    if (length <= 1) {
        return 1.0;
    }
    constexpr double kTwoPi = 2.0 * M_PI;
    const double phase = kTwoPi * static_cast<double>(index) / static_cast<double>(length - 1);
    switch (mode) {
    case 1: return 0.5 - 0.5 * std::cos(phase);
    case 2: return 0.54 - 0.46 * std::cos(phase);
    case 3: return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
    case 4: return 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) - 0.01168 * std::cos(3.0 * phase);
    default: return 1.0;
    }
}

double spectrumNormalizationScale(int normalizationMode, int windowMode, int length)
{
    const int safeLength = std::max(1, length);
    double weightSum = 0.0;
    double weightEnergy = 0.0;
    for (int i = 0; i < safeLength; ++i) {
        const double weight = spectrumWindowWeight(windowMode, i, safeLength);
        weightSum += weight;
        weightEnergy += weight * weight;
    }
    switch (normalizationMode) {
    case 1: return weightSum > 0.0 ? 1.0 / weightSum : 1.0;
    case 2: return weightEnergy > 0.0 ? 1.0 / std::sqrt(weightEnergy) : 1.0;
    default: return 1.0;
    }
}

int spectrumFftLength(const std::vector<int>& windowLengths)
{
    int maxWindow = 2;
    for (int length : windowLengths) {
        maxWindow = std::max(maxWindow, length);
    }
    int fftLength = 1;
    while (fftLength * 2 <= maxWindow && fftLength * 2 <= kSpectrumSignalLength) {
        fftLength *= 2;
    }
    return std::max(2, fftLength);
}

struct SpectrumGpuInput {
    std::vector<float> signal;
    std::vector<float> freqs;
    std::vector<float> norms;
    std::vector<int> windowLengths;
    int validSamples = 0;
    int fftLength = 2;
};

int64_t timelineSamplesToDecodedSamples(int64_t timelineSamples, int decodedSampleRate)
{
    if (decodedSampleRate <= 0) {
        return 0;
    }
    const long double scale =
        static_cast<long double>(decodedSampleRate) / static_cast<long double>(kAudioSampleRate);
    const long double scaled = static_cast<long double>(qMax<int64_t>(0, timelineSamples)) * scale;
    return qMax<int64_t>(0, static_cast<int64_t>(std::llround(scaled)));
}

bool fillSpectrumSignalWindow(const DecodedAudioCacheEntry& decoded,
                              const TimelineClip& clip,
                              int64_t currentTimelineSample,
                              std::vector<float>* signalOut,
                              int* validSamplesOut)
{
    if (!signalOut || !validSamplesOut) {
        return false;
    }
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t localSample = qMax<int64_t>(0, currentTimelineSample - clipStartSample);
    const int64_t clipTimelineSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
    const qreal localNorm = qBound<qreal>(
        0.0,
        static_cast<qreal>(localSample) / static_cast<qreal>(clipTimelineSamples),
        1.0);
    const int64_t sourceSample = timelineSamplesToDecodedSamples(
        clipSourceInSamples(clip) +
            static_cast<int64_t>(std::llround(localNorm * static_cast<qreal>(
                qMax<int64_t>(1, sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(1, clip.durationFrames)))) - 1))),
        decoded.sampleRate);
    signalOut->assign(static_cast<size_t>(kSpectrumSignalLength), 0.0f);
    const int64_t start = sourceSample - kSpectrumSignalLength + 1;
    int validSamples = 0;
    for (int i = 0; i < kSpectrumSignalLength; ++i) {
        const int64_t idx = start + i;
        if (idx >= 0 && idx < decoded.samples.size()) {
            (*signalOut)[static_cast<size_t>(i)] = decoded.samples[static_cast<int>(idx)];
            validSamples = i + 1;
        }
    }
    *validSamplesOut = std::max(1, validSamples);
    return true;
}

bool buildSpectrumGpuInput(const DecodedAudioCacheEntry& decoded,
                           const TimelineClip& clip,
                           int64_t currentTimelineSample,
                           const PreviewSurface::LoiaconoSpectrumSettings& settings,
                           SpectrumGpuInput* out)
{
    if (!out) {
        return false;
    }
    const int binCount = qBound(32, settings.bins, 2400);
    const double sampleRate = std::max(1, decoded.sampleRate);
    const double referenceA = 440.0;
    const double freqMin = qBound(20, settings.freqMin, 2000);
    const double freqMax = qBound(500, settings.freqMax, 12000);
    const double midiMinExact = 69.0 + 12.0 * std::log2(freqMin / referenceA);
    const double midiMaxExact = 69.0 + 12.0 * std::log2(freqMax / referenceA);
    const double semitoneSpan = std::max(1e-6, midiMaxExact - midiMinExact);
    const int steps = std::max(1, binCount - 1);
    const int binsPerSemitone = std::max(1, static_cast<int>(std::lround(steps / semitoneSpan)));
    const double midiStep = 1.0 / static_cast<double>(binsPerSemitone);
    const double totalMidiSpan = static_cast<double>(steps) * midiStep;
    const double midiCenter = 0.5 * (midiMinExact + midiMaxExact);
    const double midiStart = std::round((midiCenter - 0.5 * totalMidiSpan) / midiStep) * midiStep;
    const double midiEnd = midiStart + totalMidiSpan;
    const double freqMinGrid = referenceA * std::pow(2.0, (midiStart - 69.0) / 12.0);
    const double freqMaxGrid = referenceA * std::pow(2.0, (midiEnd - 69.0) / 12.0);
    const double logMin = std::log(freqMinGrid);
    const double logMax = std::log(freqMaxGrid);
    const double logStep = binCount > 1 ? (logMax - logMin) / static_cast<double>(binCount - 1) : 0.0;
    const double maxFreqNorm = std::exp(logMax) / sampleRate;
    const double safeMaxFreqNorm = std::max(maxFreqNorm, 1.0 / sampleRate);
    const double baseWindow = static_cast<double>(qBound(2, settings.multiple, 240)) / safeMaxFreqNorm;
    double windowLengthExponent = 1.0;
    switch (settings.windowLengthMode) {
    case 0: windowLengthExponent = 0.0; break;
    case 1: windowLengthExponent = 0.5; break;
    default: windowLengthExponent = 1.0; break;
    }

    out->signal.assign(static_cast<size_t>(kSpectrumSignalLength), 0.0f);
    out->freqs.resize(static_cast<size_t>(binCount));
    out->norms.resize(static_cast<size_t>(binCount));
    out->windowLengths.resize(static_cast<size_t>(binCount));
    int validSamples = 0;
    fillSpectrumSignalWindow(decoded, clip, currentTimelineSample, &out->signal, &validSamples);

    for (int i = 0; i < binCount; ++i) {
        const double freqHz = std::exp(logMin + static_cast<double>(i) * logStep);
        const double freqNorm = freqHz / sampleRate;
        out->freqs[static_cast<size_t>(i)] = static_cast<float>(freqNorm);
        const double freqRatio = std::clamp(
            safeMaxFreqNorm / std::max(freqNorm, 1.0 / sampleRate),
            1.0,
            static_cast<double>(kSpectrumSignalLength));
        int windowLength = static_cast<int>(std::lround(baseWindow * std::pow(freqRatio, windowLengthExponent)));
        windowLength = std::max(windowLength, 2);
        windowLength = std::min(windowLength, kSpectrumSignalLength);
        out->windowLengths[static_cast<size_t>(i)] = windowLength;
        out->norms[static_cast<size_t>(i)] = static_cast<float>(
            spectrumNormalizationScale(settings.normalizationMode, settings.temporalWeightingMode, windowLength));
    }
    out->validSamples = std::max(1, validSamples);
    out->fftLength = spectrumFftLength(out->windowLengths);
    return true;
}

bool loiaconoSpectrumGpuInputForClip(const TimelineClip& clip,
                                     int64_t currentTimelineSample,
                                     const PreviewSurface::LoiaconoSpectrumSettings& settings,
                                     SpectrumGpuInput* out)
{
    QString mediaPath = playbackAudioPathForClip(clip);
    if (mediaPath.isEmpty()) {
        mediaPath = interactivePreviewMediaPathForClip(clip);
    }
    DecodedAudioCacheEntry decoded;
    if (!decodeMonoSamples(mediaPath, qBound(8000, settings.sampleRate, 192000), &decoded)) {
        return false;
    }
    return buildSpectrumGpuInput(decoded, clip, currentTimelineSample, settings, out);
}
} // namespace

bool renderDirectVulkanAudioFrame(const DirectVulkanAudioRenderContext& context,
                                  bool* waitingForWaveformOut)
{
    if (waitingForWaveformOut) {
        *waitingForWaveformOut = false;
    }
    const PreviewInteractionState* state = context.state;
    if (!state ||
        state->viewMode != PreviewSurface::ViewMode::Audio ||
        !context.deviceFunctions ||
        !context.audioTab ||
        !context.audioTab->isReady() ||
        context.commandBuffer == VK_NULL_HANDLE) {
        return false;
    }

    const QList<TimelineClip> activeAudioClips = activeAudioClipsForState(state);
    if (activeAudioClips.isEmpty()) {
        return true;
    }

    const TimelineClip& clip = activeAudioClips.constFirst();
    const QRectF safeRect = QRectF(QPointF(18.0, 18.0), QSizeF(context.swapchainSize)).adjusted(0.0, 0.0, -36.0, -36.0);
    const QRectF panel = safeRect.adjusted(12.0, 12.0, -12.0, -12.0);
    const QRectF waveRect = panel.adjusted(24.0, 118.0, -24.0, -36.0);
    const qreal rulerGutterWidth = qBound<qreal>(32.0, waveRect.width() * 0.12, 56.0);
    const QRectF graphRect(waveRect.left() + rulerGutterWidth,
                           waveRect.top(),
                           qMax<qreal>(1.0, waveRect.width() - rulerGutterWidth),
                           waveRect.height());
    const int rowCount = qBound(2, static_cast<int>(waveRect.height()) / 88, 6);
    const int binsPerRow = qMax(256, static_cast<int>(graphRect.width()));
    const int totalDrawBins = qMax(96, qMin(8192, rowCount * binsPerRow));
    const int64_t clipSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
    const qreal minVisibleBySamples = qBound<qreal>(
        0.00001,
        (100.0 * static_cast<qreal>(rowCount)) / static_cast<qreal>(clipSamples),
        1.0);
    const qreal maxAudioZoom = qBound<qreal>(20.0, 1.0 / qMax<qreal>(0.00001, minVisibleBySamples), 100000.0);
    const qreal zoom = qBound<qreal>(1.0, state->previewZoom, maxAudioZoom);
    const qreal visibleFraction = qBound<qreal>(minVisibleBySamples, 1.0 / zoom, 1.0);
    const qreal maxStart = qMax<qreal>(0.0, 1.0 - visibleFraction);
    const qreal startNorm = qBound<qreal>(0.0, state->previewPanOffset.x(), maxStart);
    const qreal endNorm = qBound<qreal>(startNorm, startNorm + visibleFraction, 1.0);
    const int64_t sourceStartFrame = qMax<int64_t>(0, clip.sourceInFrame);
    const int64_t sourceSpanFrames = qMax<int64_t>(1, clip.durationFrames);
    const int64_t visibleSourceStart = sourceStartFrame + static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceSpanFrames)));
    const int64_t visibleSourceSpan = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::ceil(visibleFraction * static_cast<qreal>(sourceSpanFrames))));
    const int64_t clipStartSample = clipTimelineStartSamples(clip);
    const int64_t clipEndSample = clipStartSample + clipSamples;
    const bool playheadVisible = state->currentSample >= clipStartSample && state->currentSample < clipEndSample;
    const qreal currentTimelineNorm = qBound<qreal>(
        0.0,
        static_cast<qreal>(qMax<int64_t>(0, state->currentSample - clipStartSample)) /
            static_cast<qreal>(clipSamples),
        1.0);
    const int64_t sourceStartSample = qMax<int64_t>(0, clipSourceInSamples(clip));
    const int64_t sourceDurationSamples =
        qMax<int64_t>(1, sourceFramesToSamples(clip, static_cast<qreal>(qMax<int64_t>(1, clip.durationFrames))));
    const int64_t currentSourceSample = sourceStartSample + static_cast<int64_t>(
        std::llround(currentTimelineNorm * static_cast<qreal>(sourceDurationSamples - 1)));
    const int64_t visibleSourceStartSample = sourceStartSample + static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceDurationSamples)));
    const int64_t visibleSourceSpanSamples = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::ceil(visibleFraction * static_cast<qreal>(sourceDurationSamples))));
    const qreal playheadTimelineNorm = qBound<qreal>(
        0.0,
        static_cast<qreal>(currentSourceSample - visibleSourceStartSample) /
            static_cast<qreal>(visibleSourceSpanSamples),
        1.0);
    const int playheadWrappedBin = qBound(
        0,
        static_cast<int>(std::floor(playheadTimelineNorm * static_cast<qreal>(qMax(1, totalDrawBins - 1)))),
        qMax(0, totalDrawBins - 1));
    const int playheadRowIndex = qBound(0, playheadWrappedBin / qMax(1, binsPerRow), qMax(0, rowCount - 1));
    const int playheadBinInRow = qBound(0, playheadWrappedBin - playheadRowIndex * qMax(1, binsPerRow), qMax(0, binsPerRow - 1));
    const qreal playheadNorm = (static_cast<qreal>(playheadBinInRow) + 0.5) /
        static_cast<qreal>(qMax(1, binsPerRow));
    QVector<qreal> waveformMin(totalDrawBins, 0.0);
    QVector<qreal> waveformMax(totalDrawBins, 0.0);
    const bool spectrumMode =
        state->audioVisualizationMode == PreviewSurface::AudioVisualizationMode::Spectrum;
    const int displaySpectrumColumns = qMax(96, qMin(8192, rowCount * binsPerRow));
    const int spectrumOversampleFactor = qMax(
        1,
        qMin(4, context.audioTab->spectrogramHistoryColumns() / qMax(1, displaySpectrumColumns)));
    const int spectrumColumns = spectrumMode
        ? qMax(displaySpectrumColumns,
               qMin(context.audioTab->spectrogramHistoryColumns(),
                    displaySpectrumColumns * spectrumOversampleFactor))
        : displaySpectrumColumns;
    const int speakerTintBins = spectrumMode ? displaySpectrumColumns : totalDrawBins;
    std::vector<float> speakerTint(static_cast<size_t>(speakerTintBins) * 4u, 0.0f);
    bool waveformReady = false;
    int drawBins = totalDrawBins;
    PreviewSurface::AudioDynamicsSettings rawWaveformSettings;
    rawWaveformSettings.waveformPreviewPostProcessing = false;

    if (spectrumMode) {
        QString mediaPath = playbackAudioPathForClip(clip);
        if (mediaPath.isEmpty()) {
            mediaPath = interactivePreviewMediaPathForClip(clip);
        }
        DecodedAudioCacheEntry decoded;
        waveformReady = decodeMonoSamples(
            mediaPath, qBound(8000, state->loiaconoSpectrumSettings.sampleRate, 192000), &decoded);
        if (waveformReady) {
            SpectrumGpuInput spectrumInput;
            waveformReady = buildSpectrumGpuInput(
                decoded, clip, state->currentSample, state->loiaconoSpectrumSettings, &spectrumInput);
            if (waveformReady) {
                drawBins = qMin(8192, static_cast<int>(spectrumInput.freqs.size()));
                waveformReady = context.audioTab->uploadSpectrumConfig(spectrumInput.freqs,
                                                                       spectrumInput.norms,
                                                                       spectrumInput.windowLengths,
                                                                       spectrumInput.fftLength);
            }
            if (waveformReady) {
                const int historyColumns = spectrumColumns;
                const QString signatureKey = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15")
                    .arg(clip.id)
                    .arg(visibleSourceStart)
                    .arg(visibleSourceSpan)
                    .arg(drawBins)
                    .arg(state->loiaconoSpectrumSettings.multiple)
                    .arg(state->loiaconoSpectrumSettings.bins)
                    .arg(state->loiaconoSpectrumSettings.freqMin)
                    .arg(state->loiaconoSpectrumSettings.freqMax)
                    .arg(state->loiaconoSpectrumSettings.sampleRate)
                    .arg(state->loiaconoSpectrumSettings.gain, 0, 'f', 3)
                    .arg(state->loiaconoSpectrumSettings.gamma, 0, 'f', 3)
                    .arg(state->loiaconoSpectrumSettings.floor, 0, 'f', 3)
                    .arg(state->loiaconoSpectrumSettings.leakiness, 0, 'f', 5)
                    .arg(state->loiaconoSpectrumSettings.temporalWeightingMode)
                    .arg(state->loiaconoSpectrumSettings.normalizationMode);
                const quint64 signature = static_cast<quint64>(qHash(signatureKey));
                const bool rebuildVisibleSpan =
                    context.audioTab->spectrogramSignature() != signature;
                if (rebuildVisibleSpan) {
                    context.audioTab->resetSpectrogramHistory();
                    context.audioTab->setSpectrogramSignature(signature);
                    const int64_t clipStartSample = clipTimelineStartSamples(clip);
                    const int64_t visibleLocalStartSample = qMax<int64_t>(
                        0, static_cast<int64_t>(std::floor(startNorm * static_cast<qreal>(clipSamples))));
                    const int64_t visibleLocalSpanSampleCount = qMax<int64_t>(
                        1,
                        static_cast<int64_t>(std::ceil(visibleFraction * static_cast<qreal>(clipSamples))));
                    std::vector<float> tileSignals;
                    std::vector<uint32_t> tileValidSamples;
                    tileSignals.resize(static_cast<size_t>(kSpectrogramTileColumns) *
                                       static_cast<size_t>(kSpectrumSignalLength));
                    tileValidSamples.resize(static_cast<size_t>(kSpectrogramTileColumns));
                    std::vector<float> columnSignal;
                    for (int tileStart = 0; tileStart < historyColumns; tileStart += kSpectrogramTileColumns) {
                        const int tileColumns = qMin(kSpectrogramTileColumns, historyColumns - tileStart);
                        std::fill(tileSignals.begin(), tileSignals.end(), 0.0f);
                        std::fill(tileValidSamples.begin(), tileValidSamples.end(), 0u);
                        for (int tileColumn = 0; tileColumn < tileColumns; ++tileColumn) {
                            const int column = tileStart + tileColumn;
                            const qreal t = historyColumns <= 1
                                ? 0.5
                                : static_cast<qreal>(column) / static_cast<qreal>(historyColumns - 1);
                            const int64_t timelineSample = clipStartSample +
                                visibleLocalStartSample +
                                static_cast<int64_t>(std::llround(t * static_cast<qreal>(visibleLocalSpanSampleCount - 1)));
                            int validSamples = 0;
                            if (!fillSpectrumSignalWindow(
                                    decoded, clip, timelineSample, &columnSignal, &validSamples)) {
                                waveformReady = false;
                                break;
                            }
                            const size_t signalOffset = static_cast<size_t>(tileColumn) *
                                static_cast<size_t>(kSpectrumSignalLength);
                            std::copy(columnSignal.begin(),
                                      columnSignal.end(),
                                      tileSignals.begin() + static_cast<std::ptrdiff_t>(signalOffset));
                            tileValidSamples[static_cast<size_t>(tileColumn)] =
                                static_cast<uint32_t>(validSamples);
                        }
                        if (!waveformReady) {
                            break;
                        }
                        waveformReady = context.audioTab->uploadSpectrumTileSignals(
                            tileSignals, tileValidSamples, tileColumns);
                        if (!waveformReady) {
                            break;
                        }
                        context.audioTab->processSpectrumTile(
                            context.commandBuffer,
                            drawBins,
                            tileColumns,
                            tileStart,
                            state->loiaconoSpectrumSettings);
                    }
                    if (waveformReady) {
                        context.audioTab->normalizeSpectrogramHistory(
                            context.commandBuffer,
                            drawBins,
                            historyColumns,
                            state->loiaconoSpectrumSettings);
                    }
                }
            }
        }
    } else {
        waveformReady = queryAudioWaveformEnvelopeForClip(
            clip,
            rawWaveformSettings,
            totalDrawBins,
            startNorm,
            endNorm,
            &waveformMin,
            &waveformMax);
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    if (!transcriptPath.isEmpty()) {
        const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
            loadTranscriptRuntimeDocument(transcriptPath);
        const QVector<TranscriptSection>& sections =
            runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
        if (!sections.isEmpty()) {
            QStringList speakerIds;
            QHash<QString, int> speakerToIndex;
            QVector<int> speakerIndexByBin(speakerTintBins, -1);
            for (int i = 0; i < speakerTintBins; ++i) {
                const qreal t = (static_cast<qreal>(i) + 0.5) / static_cast<qreal>(speakerTintBins);
                const int64_t sourceFrame = visibleSourceStart + static_cast<int64_t>(
                    std::floor(t * static_cast<qreal>(visibleSourceSpan)));
                const QString speakerId = speakerAtSourceFrame(sections, sourceFrame);
                if (speakerId.isEmpty()) {
                    continue;
                }
                int idx = speakerToIndex.value(speakerId, -1);
                if (idx < 0) {
                    idx = speakerIds.size();
                    speakerIds.push_back(speakerId);
                    speakerToIndex.insert(speakerId, idx);
                }
                speakerIndexByBin[i] = idx;
            }
            const int maxGapBins = qMax(1, speakerTintBins / 72);
            fillShortUnknownSpeakerGaps(&speakerIndexByBin, maxGapBins);
            for (int i = 0; i < speakerTintBins; ++i) {
                const int speakerIdx = speakerIndexByBin.value(i, -1);
                if (speakerIdx < 0 || speakerIdx >= speakerIds.size()) {
                    continue;
                }
                const QColor tint = speakerColor(speakerIds.at(speakerIdx), 30);
                const size_t baseIndex = static_cast<size_t>(i) * 4u;
                speakerTint[baseIndex + 0u] = static_cast<float>(tint.redF());
                speakerTint[baseIndex + 1u] = static_cast<float>(tint.greenF());
                speakerTint[baseIndex + 2u] = static_cast<float>(tint.blueF());
                speakerTint[baseIndex + 3u] = static_cast<float>(tint.alphaF());
            }
        }
    }

    if (waitingForWaveformOut) {
        *waitingForWaveformOut = !waveformReady;
    }
    if (waveformReady) {
        context.audioTab->uploadSpeakerTint(speakerTint, speakerTintBins);
        if (!spectrumMode) {
            context.audioTab->uploadWaveform(waveformMin, waveformMax, drawBins);
            const PreviewSurface::AudioDynamicsSettings gpuDynamics =
                state->audioDynamics.waveformPreviewPostProcessing
                    ? state->audioDynamics
                    : PreviewSurface::AudioDynamicsSettings{};
            context.audioTab->processWaveform(context.commandBuffer, drawBins, gpuDynamics);
        }
    }
    const qreal selectivePeakLinear = std::pow(
        10.0, qBound<qreal>(-36.0, state->audioDynamics.selectiveNormalizePeakDb, 0.0) / 20.0);
    context.audioTab->draw(context.commandBuffer,
                           context.swapchainSize,
                           panel,
                           graphRect,
                           rowCount,
                           binsPerRow,
                           drawBins,
                           zoom,
                           spectrumMode ? true : state->audioWaveformVisible,
                           state->audioDynamics.selectiveNormalizeEnabled &&
                               state->audioDynamics.selectiveNormalizeOverlayVisible &&
                               !spectrumMode,
                           selectivePeakLinear,
                           playheadVisible,
                           playheadNorm,
                           playheadRowIndex,
                           spectrumMode,
                           waveformReady);
    return true;
}

void updateDirectVulkanAudioOverlay(const PreviewInteractionState* state,
                                    const DirectVulkanAudioOverlayWidgets& widgets)
{
    if (!widgets.host || !widgets.infoPanel || !widgets.hoverCard || !state) {
        return;
    }
    const bool audioMode = state->viewMode == PreviewSurface::ViewMode::Audio;
    const QList<TimelineClip> activeAudioClips = activeAudioClipsForState(state);
    if (!audioMode || activeAudioClips.isEmpty()) {
        widgets.infoPanel->hide();
        widgets.hoverCard->hide();
        return;
    }

    const TimelineClip& clip = activeAudioClips.constFirst();
    const QRect hostRect = widgets.host->rect();
    const QRect safeRect = hostRect.adjusted(18, 18, -18, -18);
    const QRect panel = safeRect.adjusted(12, 12, -12, -12);
    const QRect waveRect = panel.adjusted(24, 118, -24, -36);
    const qreal rulerGutterWidth = qBound<qreal>(32.0, waveRect.width() * 0.12, 56.0);
    const QRectF graphRect(static_cast<qreal>(waveRect.left()) + rulerGutterWidth,
                           static_cast<qreal>(waveRect.top()),
                           qMax<qreal>(1.0, static_cast<qreal>(waveRect.width()) - rulerGutterWidth),
                           static_cast<qreal>(waveRect.height()));
    const int rowCount = qBound(2, waveRect.height() / 88, 6);
    const int binsPerRow = qMax(256, static_cast<int>(graphRect.width()));
    const bool spectrumMode =
        state->audioVisualizationMode == PreviewSurface::AudioVisualizationMode::Spectrum;
    const int totalDrawBins = qMax(96, qMin(8192, rowCount * binsPerRow));
    const int64_t clipSamples = qMax<int64_t>(1, frameToSamples(qMax<int64_t>(1, clip.durationFrames)));
    const qreal minVisibleBySamples = qBound<qreal>(
        0.00001,
        (100.0 * static_cast<qreal>(rowCount)) / static_cast<qreal>(clipSamples),
        1.0);
    const qreal maxAudioZoom = qBound<qreal>(20.0, 1.0 / qMax<qreal>(0.00001, minVisibleBySamples), 100000.0);
    const qreal zoom = qBound<qreal>(1.0, state->previewZoom, maxAudioZoom);
    const qreal visibleFraction = qBound<qreal>(minVisibleBySamples, 1.0 / zoom, 1.0);
    const qreal maxStart = qMax<qreal>(0.0, 1.0 - visibleFraction);
    const qreal startNorm = qBound<qreal>(0.0, state->previewPanOffset.x(), maxStart);
    const qreal endNorm = qBound<qreal>(startNorm, startNorm + visibleFraction, 1.0);

    const QString waveformSource = state->audioDynamics.waveformPreviewPostProcessing
        ? QStringLiteral("preview post-processing")
        : QStringLiteral("on-disk decoded audio");
    widgets.titleLabel->setText(spectrumMode ? QStringLiteral("Audio Spectrum") : QStringLiteral("Audio Monitor"));
    widgets.summaryLabel->setText(
        QStringLiteral("Selected audio clip: %1\nTransport audio: %2\nWaveform source: %3")
            .arg(clip.label)
            .arg(state->playing ? QStringLiteral("live") : QStringLiteral("paused"))
            .arg(waveformSource));
    const QString selectiveLegend =
        (state->audioDynamics.selectiveNormalizeEnabled &&
         state->audioDynamics.selectiveNormalizeOverlayVisible)
            ? QStringLiteral(" | Selective overlay: green bands = corrected segments")
            : QString();
    widgets.footerLabel->setText(
        QStringLiteral("%1 | Zoom %2%%%3")
            .arg(spectrumMode
                     ? QStringLiteral("Speaker timeline tint and transport-linked background")
                     : QStringLiteral("Waveform: mono envelope, absolute full-scale display (1.0 = 0 dBFS), wrapped rows with speaker timeline tint"))
            .arg(QString::number(zoom * 100.0, 'f', 0))
            .arg(selectiveLegend));
    widgets.infoPanel->setGeometry(panel.left() + 8,
                                   panel.top() + 8,
                                   qMax(180, qMin(430, panel.width() - 32)),
                                   112);
    widgets.infoPanel->show();
    widgets.infoPanel->raise();

    if (!state->audioSpeakerHoverModalEnabled || !graphRect.contains(state->transient.lastMousePos)) {
        widgets.hoverCard->hide();
        return;
    }

    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
        loadTranscriptRuntimeDocument(transcriptPath);
    const QVector<TranscriptSection>& sections =
        runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
    if (sections.isEmpty()) {
        widgets.hoverCard->hide();
        return;
    }

    QStringList speakerIds;
    QHash<QString, int> speakerToIndex;
    QVector<int> speakerIndexByBin(totalDrawBins, -1);
    const int64_t sourceStartFrame = qMax<int64_t>(0, clip.sourceInFrame);
    const int64_t sourceSpanFrames = qMax<int64_t>(1, clip.durationFrames);
    const int64_t visibleSourceStart = sourceStartFrame + static_cast<int64_t>(
        std::floor(startNorm * static_cast<qreal>(sourceSpanFrames)));
    const int64_t visibleSourceSpan = qMax<int64_t>(
        1,
        static_cast<int64_t>(std::ceil(visibleFraction * static_cast<qreal>(sourceSpanFrames))));
    for (int i = 0; i < totalDrawBins; ++i) {
        const qreal t = (static_cast<qreal>(i) + 0.5) / static_cast<qreal>(totalDrawBins);
        const int64_t sourceFrame = visibleSourceStart + static_cast<int64_t>(
            std::floor(t * static_cast<qreal>(visibleSourceSpan)));
        const QString speakerId = speakerAtSourceFrame(sections, sourceFrame);
        if (speakerId.isEmpty()) {
            continue;
        }
        int idx = speakerToIndex.value(speakerId, -1);
        if (idx < 0) {
            idx = speakerIds.size();
            speakerIds.push_back(speakerId);
            speakerToIndex.insert(speakerId, idx);
        }
        speakerIndexByBin[i] = idx;
    }
    fillShortUnknownSpeakerGaps(&speakerIndexByBin, qMax(1, binsPerRow / 72));

    const qreal localX = qBound<qreal>(
        0.0,
        (state->transient.lastMousePos.x() - graphRect.left()) / qMax<qreal>(1.0, graphRect.width()),
        1.0);
    int hoverBin = 0;
    if (spectrumMode) {
        const qreal localY = qBound<qreal>(
            0.0,
            (state->transient.lastMousePos.y() - graphRect.top()) / qMax<qreal>(1.0, graphRect.height()),
            0.99999);
        const int row = qBound(0, static_cast<int>(std::floor(localY * rowCount)), rowCount - 1);
        const int rowStartBin = row * binsPerRow;
        const int rowEndBin = qMin(totalDrawBins, rowStartBin + binsPerRow);
        const int rowBinCount = qMax(1, rowEndBin - rowStartBin);
        const int hoverBinInRow = qBound(
            0,
            static_cast<int>(std::round(localX * static_cast<qreal>(qMax(1, rowBinCount - 1)))),
            rowBinCount - 1);
        hoverBin = qBound(0, rowStartBin + hoverBinInRow, totalDrawBins - 1);
    } else {
        const qreal localY = qBound<qreal>(
            0.0,
            (state->transient.lastMousePos.y() - graphRect.top()) / qMax<qreal>(1.0, graphRect.height()),
            0.99999);
        const int row = qBound(0, static_cast<int>(std::floor(localY * rowCount)), rowCount - 1);
        const int rowStartBin = row * binsPerRow;
        const int rowEndBin = qMin(totalDrawBins, rowStartBin + binsPerRow);
        const int rowBinCount = qMax(1, rowEndBin - rowStartBin);
        const int hoverBinInRow = qBound(
            0,
            static_cast<int>(std::round(localX * static_cast<qreal>(qMax(1, rowBinCount - 1)))),
            rowBinCount - 1);
        hoverBin = qBound(0, rowStartBin + hoverBinInRow, totalDrawBins - 1);
    }
    const int speakerIdx = speakerIndexByBin.value(hoverBin, -1);
    if (speakerIdx < 0 || speakerIdx >= speakerIds.size()) {
        widgets.hoverCard->hide();
        return;
    }

    QVector<qreal> waveformMin(totalDrawBins, 0.0);
    QVector<qreal> waveformMax(totalDrawBins, 0.0);
    PreviewSurface::AudioDynamicsSettings rawWaveformSettings;
    rawWaveformSettings.waveformPreviewPostProcessing = false;
    (void)queryAudioWaveformEnvelopeForClip(
        clip, rawWaveformSettings, totalDrawBins, startNorm, endNorm, &waveformMin, &waveformMax);
    const qreal hoverAmplitude = qMax(qAbs(waveformMin.value(hoverBin)), qAbs(waveformMax.value(hoverBin)));
    const int64_t hoverSourceFrame = visibleSourceStart + static_cast<int64_t>(
        std::floor(((static_cast<qreal>(hoverBin) + 0.5) / static_cast<qreal>(totalDrawBins)) *
                   static_cast<qreal>(visibleSourceSpan)));
    const qreal hoverTimelinePercent = qBound<qreal>(
        0.0,
        ((static_cast<qreal>(hoverBin) + 0.5) / static_cast<qreal>(totalDrawBins)) * 100.0,
        100.0);
    const QString hoverSpeakerId = speakerIds.at(speakerIdx);
    const HoverSpeakerProfile* profile = hoverSpeakerProfileFor(transcriptPath, hoverSpeakerId);
    const QString hoverName =
        profile && !profile->name.trimmed().isEmpty() ? profile->name.trimmed() : hoverSpeakerId;
    const QString hoverOrg = profile ? profile->organization.trimmed() : QString();
    const QString hoverDesc =
        profile && !profile->description.trimmed().isEmpty()
            ? profile->description.trimmed()
            : QStringLiteral("No speaker description available.");
    QPixmap profileImage = profile ? hoverSpeakerImage(*profile, 72) : QPixmap();
    if (profileImage.isNull()) {
        profileImage = fallbackSpeakerAvatar(hoverSpeakerId, hoverName, 72);
    }

    widgets.hoverAvatarLabel->setPixmap(profileImage);
    widgets.hoverNameLabel->setText(hoverName);
    widgets.hoverOrgLabel->setText(hoverOrg.isEmpty() ? QStringLiteral("Independent") : hoverOrg);
    widgets.hoverMetaLabel->setText(
        QStringLiteral("ID %1  •  Frame %2  •  Amp %3  •  Pos %4%")
            .arg(hoverSpeakerId)
            .arg(hoverSourceFrame)
            .arg(QString::number(hoverAmplitude, 'f', 3))
            .arg(QString::number(hoverTimelinePercent, 'f', 1)));
    widgets.hoverDescLabel->setText(hoverDesc);
    const int hoverWidth = qMin(396, qMax(240, panel.width() - 48));
    const int hoverLeft = qMax(panel.left() + 12, panel.right() - hoverWidth - 34);
    widgets.hoverCard->setGeometry(hoverLeft, panel.top() + 22, hoverWidth, 176);
    widgets.hoverCard->show();
    widgets.hoverCard->raise();
}
