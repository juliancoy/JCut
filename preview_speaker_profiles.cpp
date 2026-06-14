#include "preview_speaker_profiles.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

struct HoverSpeakerProfileCacheEntry {
    qint64 mtimeMs = -1;
    QHash<QString, HoverSpeakerProfile> profilesBySpeaker;
};

struct CurrentSpeakerRangeCacheEntry {
    QString clipId;
    QString transcriptPath;
    QString speakerId;
    int64_t startFrame = -1;
    int64_t endFrame = -1;
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

const VulkanPreviewClipFrameStatus* presentedFrameStatusForClip(const PreviewInteractionState* state,
                                                                const QString& clipId)
{
    if (!state) {
        return nullptr;
    }
    for (const VulkanPreviewClipFrameStatus& status : state->vulkanFrameStatuses) {
        if (status.clipId == clipId && status.active && status.hasFrame &&
            status.presentedSourceFrame >= 0) {
            return &status;
        }
    }
    return nullptr;
}

int64_t transcriptFrameForClipAtPreviewState(const TimelineClip& clip,
                                             const PreviewInteractionState* state)
{
    if (const VulkanPreviewClipFrameStatus* status =
            presentedFrameStatusForClip(state, clip.id)) {
        return transcriptFrameForClipSourceFrame(clip, status->presentedSourceFrame);
    }
    return state
        ? transcriptFrameForClipAtTimelineSample(clip, state->currentSample, state->renderSyncMarkers)
        : 0;
}

int64_t mediaSourceFrameForClipAtPreviewState(const TimelineClip& clip,
                                              const PreviewInteractionState* state)
{
    if (const VulkanPreviewClipFrameStatus* status =
            presentedFrameStatusForClip(state, clip.id)) {
        return status->presentedSourceFrame;
    }
    return state
        ? sourceFrameForClipAtTimelineSample(clip, state->currentSample, state->renderSyncMarkers)
        : 0;
}

QMutex& currentSpeakerRangeCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

CurrentSpeakerRangeCacheEntry& currentSpeakerRangeCache()
{
    static CurrentSpeakerRangeCacheEntry cache;
    return cache;
}

CurrentSpeakerLabel currentSpeakerLabelFromSpeakerId(const QString& transcriptPath,
                                                     const QString& speakerId)
{
    if (speakerId.trimmed().isEmpty()) {
        return {};
    }
    const HoverSpeakerProfile* profile = hoverSpeakerProfileFor(transcriptPath, speakerId);
    CurrentSpeakerLabel label;
    label.speakerId = speakerId;
    label.name = profile && !profile->name.trimmed().isEmpty()
        ? profile->name.trimmed()
        : speakerId;
    label.organization = profile ? profile->organization.trimmed() : QString();
    return label;
}

bool cachedCurrentSpeakerId(const QString& clipId,
                            const QString& transcriptPath,
                            int64_t sourceFrame,
                            QString* speakerIdOut)
{
    QMutexLocker locker(&currentSpeakerRangeCacheMutex());
    const CurrentSpeakerRangeCacheEntry& cache = currentSpeakerRangeCache();
    if (cache.clipId == clipId &&
        cache.transcriptPath == transcriptPath &&
        sourceFrame >= cache.startFrame &&
        sourceFrame <= cache.endFrame &&
        !cache.speakerId.trimmed().isEmpty()) {
        if (speakerIdOut) {
            *speakerIdOut = cache.speakerId;
        }
        return true;
    }
    return false;
}

void rememberCurrentSpeakerIdRange(const QString& clipId,
                                   const QString& transcriptPath,
                                   const QString& speakerId,
                                   const ExportRangeSegment& range)
{
    if (speakerId.trimmed().isEmpty() || range.startFrame < 0 || range.endFrame < range.startFrame) {
        return;
    }
    QMutexLocker locker(&currentSpeakerRangeCacheMutex());
    CurrentSpeakerRangeCacheEntry& cache = currentSpeakerRangeCache();
    cache.clipId = clipId;
    cache.transcriptPath = transcriptPath;
    cache.speakerId = speakerId;
    cache.startFrame = range.startFrame;
    cache.endFrame = range.endFrame;
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

} // namespace

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

CurrentSpeakerLabel currentSpeakerLabelForState(const PreviewInteractionState* state)
{
    if (!state) {
        return {};
    }

    QList<TimelineClip> candidates;
    for (const TimelineClip& clip : state->clips) {
        const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
        if (transcriptPath.isEmpty()) {
            continue;
        }
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
        if (state->currentSample >= clipStartSample && state->currentSample < clipEndSample) {
            candidates.push_back(clip);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [state](const TimelineClip& a, const TimelineClip& b) {
        const bool aSelected = !state->selectedClipId.isEmpty() && a.id == state->selectedClipId;
        const bool bSelected = !state->selectedClipId.isEmpty() && b.id == state->selectedClipId;
        if (aSelected != bSelected) {
            return aSelected;
        }
        if (a.trackIndex != b.trackIndex) {
            return a.trackIndex < b.trackIndex;
        }
        return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
    });

    for (const TimelineClip& clip : candidates) {
        const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
        const int64_t sourceFrame = transcriptFrameForClipAtPreviewState(clip, state);
        QString speakerId;
        if (cachedCurrentSpeakerId(clip.id, transcriptPath, sourceFrame, &speakerId)) {
            return currentSpeakerLabelFromSpeakerId(transcriptPath, speakerId);
        }
        const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
            loadTranscriptRuntimeDocument(transcriptPath);
        const QVector<TranscriptSection>& sections =
            runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
        if (sections.isEmpty()) {
            continue;
        }
        ExportRangeSegment activeRange{-1, -1};
        speakerId = transcriptOverlaySpeakerAtSourceFrame(
            sections,
            sourceFrame,
            &activeRange,
            TranscriptOverlayTiming{state->transcriptPrependMs, state->transcriptPostpendMs});
        if (speakerId.isEmpty()) {
            continue;
        }
        rememberCurrentSpeakerIdRange(clip.id, transcriptPath, speakerId, activeRange);
        return currentSpeakerLabelFromSpeakerId(transcriptPath, speakerId);
    }

    return {};
}

QJsonObject currentSpeakerLabelDebugForState(const PreviewInteractionState* state)
{
    QJsonObject debug;
    if (!state) {
        debug.insert(QStringLiteral("status"), QStringLiteral("missing_state"));
        return debug;
    }

    debug.insert(QStringLiteral("show_name"), state->showCurrentSpeakerName);
    debug.insert(QStringLiteral("show_organization"), state->showCurrentSpeakerOrganization);
    debug.insert(QStringLiteral("current_frame"), static_cast<qint64>(state->currentFrame));
    debug.insert(QStringLiteral("current_sample"), static_cast<qint64>(state->currentSample));
    debug.insert(QStringLiteral("clip_count"), state->clips.size());
    debug.insert(QStringLiteral("selected_clip_id"), state->selectedClipId);

    if (!state->showCurrentSpeakerName && !state->showCurrentSpeakerOrganization) {
        debug.insert(QStringLiteral("status"), QStringLiteral("overlay_disabled"));
        return debug;
    }

    QJsonArray skippedClips;
    QList<TimelineClip> candidates;
    for (const TimelineClip& clip : state->clips) {
        const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
        if (transcriptPath.isEmpty()) {
            continue;
        }
        const int64_t clipStartSample = clipTimelineStartSamples(clip);
        const int64_t clipEndSample = clipStartSample + frameToSamples(clip.durationFrames);
        if (state->currentSample >= clipStartSample && state->currentSample < clipEndSample) {
            candidates.push_back(clip);
        } else if (skippedClips.size() < 8) {
            skippedClips.push_back(QJsonObject{
                {QStringLiteral("clip_id"), clip.id},
                {QStringLiteral("file_path"), clip.filePath},
                {QStringLiteral("transcript_path"), transcriptPath},
                {QStringLiteral("clip_start_sample"), static_cast<qint64>(clipStartSample)},
                {QStringLiteral("clip_end_sample"), static_cast<qint64>(clipEndSample)}
            });
        }
    }
    debug.insert(QStringLiteral("candidate_clip_count"), candidates.size());
    if (!skippedClips.isEmpty()) {
        debug.insert(QStringLiteral("inactive_transcript_clips"), skippedClips);
    }

    std::sort(candidates.begin(), candidates.end(), [state](const TimelineClip& a, const TimelineClip& b) {
        const bool aSelected = !state->selectedClipId.isEmpty() && a.id == state->selectedClipId;
        const bool bSelected = !state->selectedClipId.isEmpty() && b.id == state->selectedClipId;
        if (aSelected != bSelected) {
            return aSelected;
        }
        if (a.trackIndex != b.trackIndex) {
            return a.trackIndex < b.trackIndex;
        }
        return clipTimelineStartSamples(a) < clipTimelineStartSamples(b);
    });

    if (candidates.isEmpty()) {
        debug.insert(QStringLiteral("status"), QStringLiteral("no_active_transcript_clip"));
        return debug;
    }

    QJsonArray candidateDebug;
    for (const TimelineClip& clip : candidates) {
        const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
        QJsonObject clipDebug{
            {QStringLiteral("clip_id"), clip.id},
            {QStringLiteral("file_path"), clip.filePath},
            {QStringLiteral("transcript_path"), transcriptPath},
            {QStringLiteral("selected"), !state->selectedClipId.isEmpty() && clip.id == state->selectedClipId}
        };

        const std::shared_ptr<const TranscriptRuntimeDocument> runtimeDocument =
            loadTranscriptRuntimeDocument(transcriptPath);
        const QVector<TranscriptSection>& sections =
            runtimeDocument ? runtimeDocument->sections : QVector<TranscriptSection>{};
        clipDebug.insert(QStringLiteral("section_count"), sections.size());
        if (sections.isEmpty()) {
            clipDebug.insert(QStringLiteral("status"), QStringLiteral("transcript_has_no_sections"));
            candidateDebug.push_back(clipDebug);
            continue;
        }

        const int64_t transcriptFrame = transcriptFrameForClipAtPreviewState(clip, state);
        const int64_t mediaSourceFrame = mediaSourceFrameForClipAtPreviewState(clip, state);
        const QString speakerId = transcriptOverlaySpeakerAtSourceFrame(
            sections,
            transcriptFrame,
            nullptr,
            TranscriptOverlayTiming{state->transcriptPrependMs, state->transcriptPostpendMs});
        clipDebug.insert(QStringLiteral("source_frame"), static_cast<qint64>(transcriptFrame));
        clipDebug.insert(QStringLiteral("transcript_frame"), static_cast<qint64>(transcriptFrame));
        clipDebug.insert(QStringLiteral("media_source_frame"), static_cast<qint64>(mediaSourceFrame));
        clipDebug.insert(QStringLiteral("speaker_timing_source"),
                         presentedFrameStatusForClip(state, clip.id)
                             ? QStringLiteral("presented_frame_transcript_timing")
                             : QStringLiteral("transport_sample_transcript_timing"));
        clipDebug.insert(QStringLiteral("speaker_id"), speakerId);
        if (speakerId.isEmpty()) {
            clipDebug.insert(QStringLiteral("status"), QStringLiteral("no_speaker_at_source_frame"));
            candidateDebug.push_back(clipDebug);
            continue;
        }

        const HoverSpeakerProfile* profile = hoverSpeakerProfileFor(transcriptPath, speakerId);
        const QString name = profile && !profile->name.trimmed().isEmpty()
            ? profile->name.trimmed()
            : speakerId;
        const QString organization = profile ? profile->organization.trimmed() : QString();
        clipDebug.insert(QStringLiteral("status"), QStringLiteral("resolved"));
        clipDebug.insert(QStringLiteral("profile_found"), profile != nullptr);
        clipDebug.insert(QStringLiteral("name"), name);
        clipDebug.insert(QStringLiteral("organization"), organization);
        clipDebug.insert(QStringLiteral("organization_present"), !organization.isEmpty());
        candidateDebug.push_back(clipDebug);

        debug.insert(QStringLiteral("status"), QStringLiteral("resolved"));
        debug.insert(QStringLiteral("resolved_clip"), clipDebug);
        debug.insert(QStringLiteral("candidate_clips"), candidateDebug);
        return debug;
    }

    debug.insert(QStringLiteral("status"), QStringLiteral("no_resolved_speaker"));
    debug.insert(QStringLiteral("candidate_clips"), candidateDebug);
    return debug;
}

render_detail::SpeakerLabelOverlaySpec currentSpeakerLabelOverlaySpecForState(const PreviewInteractionState* state)
{
    render_detail::SpeakerLabelOverlaySpec spec;
    if (!state) {
        return spec;
    }
    spec.showName = state->showCurrentSpeakerName;
    spec.showOrganization = state->showCurrentSpeakerOrganization;
    spec.nameTextScale = qBound<qreal>(0.25, state->currentSpeakerNameTextScale, 3.0);
    spec.organizationTextScale = qBound<qreal>(0.25, state->currentSpeakerOrganizationTextScale, 3.0);
    spec.nameVerticalPosition = qBound<qreal>(0.0, state->currentSpeakerNameVerticalPosition, 1.0);
    spec.organizationVerticalPosition =
        qBound<qreal>(0.0, state->currentSpeakerOrganizationVerticalPosition, 1.0);
    spec.nameColor = state->currentSpeakerNameColor;
    spec.organizationColor = state->currentSpeakerOrganizationColor;
    spec.backgroundColor = state->currentSpeakerBackgroundColor;
    spec.borderColor = state->currentSpeakerBorderColor;
    spec.backgroundCornerRadius =
        qBound<qreal>(0.0, state->currentSpeakerBackgroundCornerRadius, 128.0);
    spec.borderWidth = qBound<qreal>(0.0, state->currentSpeakerBorderWidth, 16.0);
    spec.showShadow = state->currentSpeakerShadowEnabled;
    spec.shadowColor = state->currentSpeakerShadowColor;
    if (!spec.showName && !spec.showOrganization) {
        return spec;
    }
    const CurrentSpeakerLabel label = currentSpeakerLabelForState(state);
    spec.name = label.name.trimmed();
    spec.organization = label.organization.trimmed();
    return spec;
}

QString speakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame)
{
    const auto firstPossibleSection = std::lower_bound(
        sections.constBegin(),
        sections.constEnd(),
        sourceFrame,
        [](const TranscriptSection& section, int64_t frame) {
            return section.endFrame < frame;
        });
    for (auto it = firstPossibleSection; it != sections.constEnd(); ++it) {
        const TranscriptSection& section = *it;
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
