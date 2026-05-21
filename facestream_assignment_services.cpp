#include "facestream_assignment_services.h"

#include "facestream_artifact_utils.h"
#include "facestream_time_mapping.h"
#include "decoder_context.h"
#include "identity_resolution.h"

#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <limits>
#include <vector>

namespace jcut::facestream_assignment {
namespace {
constexpr auto kTrackingConfidenceKey = "confidence";
constexpr auto kTrackingFrameKey = "frame";
constexpr auto kTrackingXKey = "x";
constexpr auto kTrackingYKey = "y";
constexpr auto kTrackingBoxSizeKey = "box_size";

QRect pixelRectFromNormalizedRect(const QRectF& normalizedRect, const QSize& imageSize)
{
    if (!normalizedRect.isValid() || normalizedRect.isEmpty() ||
        imageSize.width() <= 0 || imageSize.height() <= 0) {
        return {};
    }
    const qreal leftPx = normalizedRect.left() * imageSize.width();
    const qreal topPx = normalizedRect.top() * imageSize.height();
    const qreal rightPx = normalizedRect.right() * imageSize.width();
    const qreal bottomPx = normalizedRect.bottom() * imageSize.height();

    const int left = qBound(0, static_cast<int>(std::floor(leftPx)), imageSize.width() - 1);
    const int top = qBound(0, static_cast<int>(std::floor(topPx)), imageSize.height() - 1);
    const int right = qBound(0, static_cast<int>(std::ceil(rightPx)), imageSize.width());
    const int bottom = qBound(0, static_cast<int>(std::ceil(bottomPx)), imageSize.height());
    const int width = qMax(1, right - left);
    const int height = qMax(1, bottom - top);
    return QRect(left, top, width, height);
}

std::vector<float> averageNormalizedEmbeddings(const QVector<std::vector<float>>& embeddings)
{
    if (embeddings.isEmpty()) {
        return {};
    }
    const int dims = static_cast<int>(embeddings.first().size());
    if (dims <= 0) {
        return {};
    }
    std::vector<float> merged(dims, 0.0f);
    int used = 0;
    for (const std::vector<float>& embedding : embeddings) {
        if (static_cast<int>(embedding.size()) != dims) {
            continue;
        }
        double norm = 0.0;
        for (float value : embedding) {
            norm += static_cast<double>(value) * static_cast<double>(value);
        }
        norm = std::sqrt(norm);
        if (norm <= 0.0) {
            continue;
        }
        for (int i = 0; i < dims; ++i) {
            merged[i] += static_cast<float>(embedding[i] / norm);
        }
        ++used;
    }
    if (used <= 0) {
        return {};
    }
    double mergedNorm = 0.0;
    for (float value : merged) {
        mergedNorm += static_cast<double>(value) * static_cast<double>(value);
    }
    mergedNorm = std::sqrt(mergedNorm);
    if (mergedNorm <= 0.0) {
        return {};
    }
    for (float& value : merged) {
        value = static_cast<float>(value / mergedNorm);
    }
    return merged;
}
} // namespace

QVector<QJsonObject> selectRepresentativeKeyframesForIdentity(
    const QJsonArray& keyframes,
    int maxRepresentativeCrops,
    int minFrameSpacing)
{
    QVector<QJsonObject> rows;
    rows.reserve(keyframes.size());
    for (const QJsonValue& value : keyframes) {
        const QJsonObject keyframe = value.toObject();
        if (!keyframe.isEmpty()) {
            rows.push_back(keyframe);
        }
    }
    if (rows.isEmpty()) {
        return {};
    }

    maxRepresentativeCrops = qMax(1, maxRepresentativeCrops);
    minFrameSpacing = qMax(0, minFrameSpacing);

    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) {
        const double scoreA = a.value(QLatin1StringView(kTrackingConfidenceKey)).toDouble(0.0);
        const double scoreB = b.value(QLatin1StringView(kTrackingConfidenceKey)).toDouble(0.0);
        if (scoreA == scoreB) {
            const int64_t frameA =
                a.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
            const int64_t frameB =
                b.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
            return frameA < frameB;
        }
        return scoreA > scoreB;
    });

    QVector<QJsonObject> selected;
    selected.reserve(qMin(maxRepresentativeCrops, rows.size()));
    for (const QJsonObject& keyframe : rows) {
        const int64_t frame =
            keyframe.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
        bool spacedEnough = true;
        for (const QJsonObject& chosen : selected) {
            const int64_t chosenFrame =
                chosen.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
            if (std::llabs(frame - chosenFrame) < minFrameSpacing) {
                spacedEnough = false;
                break;
            }
        }
        if (!spacedEnough) {
            continue;
        }
        selected.push_back(keyframe);
        if (selected.size() >= maxRepresentativeCrops) {
            break;
        }
    }

    if (selected.isEmpty()) {
        selected.push_back(rows.first());
    }
    if (selected.size() < qMin(maxRepresentativeCrops, rows.size())) {
        for (const QJsonObject& keyframe : rows) {
            bool alreadySelected = false;
            for (const QJsonObject& chosen : selected) {
                if (chosen == keyframe) {
                    alreadySelected = true;
                    break;
                }
            }
            if (alreadySelected) {
                continue;
            }
            selected.push_back(keyframe);
            if (selected.size() >= qMin(maxRepresentativeCrops, rows.size())) {
                break;
            }
        }
    }

    std::sort(selected.begin(), selected.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong() <
               b.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
    });
    return selected;
}

CropExtractionResult extractRepresentativeCrops(
    const CropExtractionRequest& request,
    const std::function<bool(int, const QString&)>& progress)
{
    CropExtractionResult result;

    editor::DecoderContext decoder(request.mediaPath);
    if (!decoder.initialize()) {
        result.errorMessage = QStringLiteral("Could not decode media for FaceStream comparison crops.");
        return result;
    }

    int streamIndex = 0;
    for (const QJsonValue& streamValue : request.streams) {
        if ((streamIndex % 20) == 0 &&
            !progress(streamIndex,
                      QStringLiteral("Extracting representative face crops: %1 / %2")
                          .arg(streamIndex)
                          .arg(request.streams.size()))) {
            result.canceled = true;
            return result;
        }
        ++streamIndex;

        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString(),
                &frameDomain)) {
            continue;
        }
        int64_t streamFrameMin = std::numeric_limits<int64_t>::max();
        int64_t streamFrameMax = -1;
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframe = keyframeValue.toObject();
            const int64_t frame =
                keyframe.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong();
            if (frame >= 0) {
                streamFrameMin = qMin<int64_t>(streamFrameMin, frame);
                streamFrameMax = qMax<int64_t>(streamFrameMax, frame);
            }
        }
        const QVector<QJsonObject> selectedKeyframes =
            selectRepresentativeKeyframesForIdentity(keyframes, 3, 24);
        int sampleIndex = 0;
        for (const QJsonObject& keyframe : selectedKeyframes) {
            const double keyframeScore =
                keyframe.value(QLatin1StringView(kTrackingConfidenceKey)).toDouble(0.0);
            const int64_t keyframeFrame = qMax<int64_t>(
                0, keyframe.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong());
            const int64_t sourceFrame = mapFacestreamFrameToSourceFrame(
                request.clip, keyframeFrame, frameDomain, request.renderSyncMarkers);
            int64_t timelineFrame = keyframeFrame;
            if (frameDomain == FacestreamFrameDomain::ClipTimeline30Fps) {
                timelineFrame = request.clip.startFrame + keyframeFrame;
            } else if (frameDomain == FacestreamFrameDomain::SourceAbsolute) {
                const int64_t localSourceFrame = qMax<int64_t>(0, sourceFrame - request.clip.sourceInFrame);
                timelineFrame = request.clip.startFrame + localSourceFrame;
            }
            const editor::FrameHandle decoded = decoder.decodeFrame(sourceFrame);
            const QImage image = decoded.hasCpuImage() ? decoded.cpuImage() : QImage();
            if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
                ++sampleIndex;
                continue;
            }

            const qreal xNorm = qBound<qreal>(
                0.0, keyframe.value(QLatin1StringView(kTrackingXKey)).toDouble(0.5), 1.0);
            const qreal yNorm = qBound<qreal>(
                0.0, keyframe.value(QLatin1StringView(kTrackingYKey)).toDouble(0.85), 1.0);
            const qreal boxNorm = qBound<qreal>(
                0.01, keyframe.value(QLatin1StringView(kTrackingBoxSizeKey)).toDouble(0.2), 1.0);
            const QRectF normRect = normalizedCenterBoxRect(
                xNorm, yNorm, boxNorm, QSizeF(image.width(), image.height()));
            const QRect cropRect = pixelRectFromNormalizedRect(normRect, image.size());
            if (!cropRect.isValid() || cropRect.isEmpty()) {
                ++sampleIndex;
                continue;
            }
            const QString cropPath = QDir(request.cropsDir).absoluteFilePath(
                QStringLiteral("%1_track_%2_f%3_s%4.png")
                    .arg(request.videoStem)
                    .arg(trackId, 3, 10, QLatin1Char('0'))
                    .arg(keyframeFrame, 8, 10, QLatin1Char('0'))
                    .arg(sampleIndex, 2, 10, QLatin1Char('0')));
            const QImage crop = image.copy(cropRect)
                                    .scaled(160, 160, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            if (crop.isNull() || !crop.save(cropPath)) {
                ++sampleIndex;
                continue;
            }

            facefind::Candidate candidate;
            candidate.frame = timelineFrame;
            candidate.x = xNorm;
            candidate.y = yNorm;
            candidate.box = boxNorm;
            candidate.score = qBound<qreal>(0.0, keyframeScore, 1.0);
            candidate.trackId = trackId;
            candidate.cropPath = cropPath;
            result.candidates.push_back(candidate);

            QJsonObject row;
            row[QStringLiteral("frame")] = static_cast<qint64>(candidate.frame);
            row[QStringLiteral("source_frame")] = static_cast<qint64>(sourceFrame);
            row[QStringLiteral("x")] = candidate.x;
            row[QStringLiteral("y")] = candidate.y;
            row[QStringLiteral("box")] = candidate.box;
            row[QStringLiteral("score")] = candidate.score;
            row[QStringLiteral("track_id")] = candidate.trackId;
            row[QStringLiteral("crop_path")] = candidate.cropPath;
            row[QStringLiteral("sample_index")] = sampleIndex;
            row[QStringLiteral("selected_crop_count")] = selectedKeyframes.size();
            result.candidateRows.push_back(row);
            ++sampleIndex;
        }
    }

    if (!progress(request.streams.size(), QStringLiteral("Writing FaceStream crop candidates..."))) {
        result.canceled = true;
        return result;
    }

    result.ok = true;
    return result;
}

ClusterResult clusterTrackIdentityEvidence(
    const QVector<TrackIdentityEvidence>& trackEvidence,
    double autoClusterThreshold,
    double reviewThreshold)
{
    ClusterResult result;
    result.autoClusterThreshold = autoClusterThreshold;
    result.reviewThreshold = reviewThreshold;
    result.embeddingReady = true;
    result.ok = true;

    QVector<TrackIdentityEvidence> evidence = trackEvidence;
    evidence.erase(
        std::remove_if(
            evidence.begin(),
            evidence.end(),
            [](const TrackIdentityEvidence& row) {
                return row.trackId < 0 || row.cropSamples.isEmpty();
            }),
        evidence.end());
    std::sort(evidence.begin(), evidence.end(), [](const TrackIdentityEvidence& a, const TrackIdentityEvidence& b) {
        return a.trackId < b.trackId;
    });
    const int n = evidence.size();

    QVector<int> parent(n);
    for (int i = 0; i < n; ++i) {
        parent[i] = i;
    }
    std::function<int(int)> findRoot = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = findRoot(a);
        const int rb = findRoot(b);
        if (ra != rb) {
            parent[rb] = ra;
        }
    };

    for (int i = 0; i < n; ++i) {
        if (evidence.at(i).hasEmbedding &&
            !evidence.at(i).embedding.empty()) {
            ++result.embeddedTrackCount;
        }
        for (int j = i + 1; j < n; ++j) {
            if (!evidence.at(i).hasEmbedding ||
                !evidence.at(j).hasEmbedding ||
                evidence.at(i).embedding.empty() ||
                evidence.at(j).embedding.empty()) {
                continue;
            }
            const double cosine =
                jcut::identity_resolution::cosineSimilarity(
                    evidence.at(i).embedding,
                    evidence.at(j).embedding);
            QString decision = QStringLiteral("different");
            if (cosine >= result.autoClusterThreshold) {
                unite(i, j);
                decision = QStringLiteral("auto_cluster");
                ++result.autoClusterPairCount;
            } else if (cosine >= result.reviewThreshold) {
                decision = QStringLiteral("review");
                ++result.reviewPairCount;
            }
            QJsonObject pairRow;
            pairRow[QStringLiteral("track_a")] = evidence.at(i).trackId;
            pairRow[QStringLiteral("track_b")] = evidence.at(j).trackId;
            pairRow[QStringLiteral("cosine")] = cosine;
            pairRow[QStringLiteral("decision")] = decision;
            pairRow[QStringLiteral("embedded_crop_count_a")] = evidence.at(i).cropSamples.size();
            pairRow[QStringLiteral("embedded_crop_count_b")] = evidence.at(j).cropSamples.size();
            result.clusterDiagnosticsRows.push_back(pairRow);
        }
    }

    QHash<int, QVector<int>> membersByRoot;
    for (int i = 0; i < n; ++i) {
        membersByRoot[findRoot(i)].push_back(i);
    }
    QVector<int> roots = membersByRoot.keys().toVector();
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        const auto& ma = membersByRoot[a];
        const auto& mb = membersByRoot[b];
        const int ta = ma.isEmpty() ? -1 : evidence.at(ma.first()).trackId;
        const int tb = mb.isEmpty() ? -1 : evidence.at(mb.first()).trackId;
        return ta < tb;
    });

    int clusterIndex = 1;
    for (int rootIndex : roots) {
        QVector<int> memberIndexes = membersByRoot.value(rootIndex);
        if (memberIndexes.isEmpty()) {
            continue;
        }
        std::sort(memberIndexes.begin(), memberIndexes.end(), [&](int a, int b) {
            return evidence.at(a).trackId < evidence.at(b).trackId;
        });
        int representativeIndex = memberIndexes.first();
        for (int idx : memberIndexes) {
            const QVector<facefind::Candidate>& candidatesA = evidence.at(idx).cropSamples;
            const QVector<facefind::Candidate>& candidatesB = evidence.at(representativeIndex).cropSamples;
            const qreal scoreA = candidatesA.isEmpty() ? 0.0 : candidatesA.first().score;
            const qreal scoreB = candidatesB.isEmpty() ? 0.0 : candidatesB.first().score;
            if (scoreA > scoreB) {
                representativeIndex = idx;
            }
        }

        double confidence = 1.0;
        int pairCount = 0;
        double pairSum = 0.0;
        for (int i = 0; i < memberIndexes.size(); ++i) {
            for (int j = i + 1; j < memberIndexes.size(); ++j) {
                const int a = memberIndexes.at(i);
                const int b = memberIndexes.at(j);
                if (!evidence.at(a).hasEmbedding || !evidence.at(b).hasEmbedding) {
                    continue;
                }
                pairSum += jcut::identity_resolution::cosineSimilarity(
                    evidence.at(a).embedding,
                    evidence.at(b).embedding);
                ++pairCount;
            }
        }
        if (pairCount > 0) {
            confidence = qBound(0.0, pairSum / static_cast<double>(pairCount), 1.0);
        }

        QString status = memberIndexes.size() > 1
            ? QStringLiteral("auto_clustered")
            : QStringLiteral("singleton");
        bool anyEmbedding = false;
        for (int idx : memberIndexes) {
            anyEmbedding = anyEmbedding || evidence.at(idx).hasEmbedding;
        }
        if (!result.embeddingReady || !anyEmbedding) {
            status = QStringLiteral("singleton_no_embedding");
            confidence = 1.0;
        }

        const QString clusterId = QStringLiteral("person_%1").arg(clusterIndex++, 3, 10, QLatin1Char('0'));
        QVector<int> memberTrackIds;
        QJsonArray memberTracksJson;
        for (int idx : memberIndexes) {
            const int trackId = evidence.at(idx).trackId;
            if (trackId >= 0) {
                memberTrackIds.push_back(trackId);
                memberTracksJson.push_back(trackId);
            }
        }

        QVector<facefind::Candidate> representativeSamples = evidence.at(representativeIndex).cropSamples;
        std::sort(representativeSamples.begin(), representativeSamples.end(), [](const facefind::Candidate& a,
                                                                                const facefind::Candidate& b) {
            return a.score > b.score;
        });
        facefind::Candidate representative =
            representativeSamples.isEmpty() ? facefind::Candidate{} : representativeSamples.first();
        representative.trackId = evidence.at(representativeIndex).trackId;
        representative.clusterId = clusterId;
        representative.clusterTrackIds = memberTrackIds;
        representative.clusterConfidence = confidence;
        representative.clusterStatus = status;
        result.clusterCandidates.push_back(representative);

        QJsonObject clusterRow;
        clusterRow[QStringLiteral("cluster_id")] = clusterId;
        clusterRow[QStringLiteral("track_ids")] = memberTracksJson;
        clusterRow[QStringLiteral("representative_track_id")] = representative.trackId;
        clusterRow[QStringLiteral("representative_crop")] = representative.cropPath;
        clusterRow[QStringLiteral("representative_crop_count")] = representativeSamples.size();
        clusterRow[QStringLiteral("embedded_track_count")] = memberIndexes.size();
        clusterRow[QStringLiteral("confidence")] = confidence;
        clusterRow[QStringLiteral("status")] = status;
        result.clusterRows.push_back(clusterRow);
    }

    return result;
}

ClusterResult clusterFaceTracks(
    const ClusterRequest& request,
    const std::function<bool(int, const QString&)>& progress)
{
    QHash<int, QVector<facefind::Candidate>> candidatesByTrackId;
    for (const facefind::Candidate& candidate : request.trackCandidates) {
        if (candidate.trackId < 0) {
            continue;
        }
        candidatesByTrackId[candidate.trackId].push_back(candidate);
    }
    QVector<int> trackIds = candidatesByTrackId.keys().toVector();
    std::sort(trackIds.begin(), trackIds.end());

    ClusterResult result;
    result.autoClusterThreshold = 0.70;
    result.reviewThreshold = 0.55;

    jcut::identity_resolution::ArcFaceNcnnEmbedder embedder;
    result.embeddingReady = embedder.initialize(request.arcfaceParamPath, request.arcfaceBinPath, &result.embeddingError);

    QVector<TrackIdentityEvidence> evidence;
    evidence.reserve(trackIds.size());
    if (result.embeddingReady) {
        for (int i = 0; i < trackIds.size(); ++i) {
            if ((i % 25) == 0 &&
                !progress(1,
                          QStringLiteral("Embedding FaceStream track crops: %1 / %2")
                              .arg(i)
                              .arg(trackIds.size()))) {
                result.cancelStageMessage = QStringLiteral("User canceled FaceStream identity embedding.");
                return result;
            }
            TrackIdentityEvidence row;
            row.trackId = trackIds.at(i);
            row.cropSamples = candidatesByTrackId.value(row.trackId);
            QVector<std::vector<float>> cropEmbeddings;
            for (const facefind::Candidate& candidate : row.cropSamples) {
                if (candidate.cropPath.trimmed().isEmpty()) {
                    continue;
                }
                std::vector<float> embedding;
                if (embedder.embedFaceCrop(candidate.cropPath, &embedding)) {
                    cropEmbeddings.push_back(std::move(embedding));
                }
            }
            row.embedding = averageNormalizedEmbeddings(cropEmbeddings);
            row.hasEmbedding = !row.embedding.empty();
            evidence.push_back(row);
        }
    } else {
        for (int trackId : trackIds) {
            TrackIdentityEvidence row;
            row.trackId = trackId;
            row.cropSamples = candidatesByTrackId.value(trackId);
            evidence.push_back(row);
        }
    }

    ClusterResult clustered =
        clusterTrackIdentityEvidence(evidence, result.autoClusterThreshold, result.reviewThreshold);
    clustered.embeddingReady = result.embeddingReady;
    clustered.embeddingError = result.embeddingError;
    if (!result.embeddingReady) {
        for (QJsonValueRef value : clustered.clusterRows) {
            QJsonObject row = value.toObject();
            row[QStringLiteral("status")] = QStringLiteral("singleton_no_embedding");
            row[QStringLiteral("confidence")] = 1.0;
            value = row;
        }
        for (facefind::Candidate& candidate : clustered.clusterCandidates) {
            candidate.clusterStatus = QStringLiteral("singleton_no_embedding");
            candidate.clusterConfidence = 1.0;
        }
    }
    return clustered;
}

SeedTrackMatchResult matchFaceTracksToSeed(
    const SeedTrackMatchRequest& request,
    const std::function<bool(int, const QString&)>& progress)
{
    SeedTrackMatchResult result;
    result.autoMatchThreshold = request.autoMatchThreshold;
    result.reviewThreshold = request.reviewThreshold;

    QHash<int, QVector<facefind::Candidate>> candidatesByTrackId;
    for (const facefind::Candidate& candidate : request.trackCandidates) {
        if (candidate.trackId < 0) {
            continue;
        }
        candidatesByTrackId[candidate.trackId].push_back(candidate);
    }
    if (request.seedTrackId < 0 || !candidatesByTrackId.contains(request.seedTrackId)) {
        result.embeddingError = QStringLiteral("Selected seed track was not available for identity matching.");
        return result;
    }

    QVector<int> trackIds = candidatesByTrackId.keys().toVector();
    std::sort(trackIds.begin(), trackIds.end());

    jcut::identity_resolution::ArcFaceNcnnEmbedder embedder;
    result.embeddingReady =
        embedder.initialize(request.arcfaceParamPath, request.arcfaceBinPath, &result.embeddingError);

    QVector<TrackIdentityEvidence> evidence;
    evidence.reserve(trackIds.size());
    if (result.embeddingReady) {
        for (int i = 0; i < trackIds.size(); ++i) {
            if ((i % 25) == 0 &&
                !progress(1,
                          QStringLiteral("Embedding FaceStream track crops: %1 / %2")
                              .arg(i)
                              .arg(trackIds.size()))) {
                result.cancelStageMessage = QStringLiteral("User canceled seeded FaceStream identity matching.");
                return result;
            }
            TrackIdentityEvidence row;
            row.trackId = trackIds.at(i);
            row.cropSamples = candidatesByTrackId.value(row.trackId);
            QVector<std::vector<float>> cropEmbeddings;
            for (const facefind::Candidate& candidate : row.cropSamples) {
                if (candidate.cropPath.trimmed().isEmpty()) {
                    continue;
                }
                std::vector<float> embedding;
                if (embedder.embedFaceCrop(candidate.cropPath, &embedding)) {
                    cropEmbeddings.push_back(std::move(embedding));
                }
            }
            row.embedding = averageNormalizedEmbeddings(cropEmbeddings);
            row.hasEmbedding = !row.embedding.empty();
            if (row.hasEmbedding) {
                ++result.embeddedTrackCount;
            }
            evidence.push_back(row);
        }
    } else {
        for (int trackId : trackIds) {
            TrackIdentityEvidence row;
            row.trackId = trackId;
            row.cropSamples = candidatesByTrackId.value(trackId);
            evidence.push_back(row);
        }
    }

    int seedIndex = -1;
    for (int i = 0; i < evidence.size(); ++i) {
        if (evidence.at(i).trackId == request.seedTrackId) {
            seedIndex = i;
            break;
        }
    }
    if (seedIndex < 0) {
        result.embeddingError = QStringLiteral("Selected seed track was not available for identity matching.");
        return result;
    }
    if (!result.embeddingReady) {
        return result;
    }
    const TrackIdentityEvidence& seedEvidence = evidence.at(seedIndex);
    if (!seedEvidence.hasEmbedding || seedEvidence.embedding.empty()) {
        result.embeddingError = QStringLiteral("Could not compute an identity embedding for the selected seed track.");
        return result;
    }

    result.ok = true;
    for (const TrackIdentityEvidence& row : std::as_const(evidence)) {
        SeedTrackMatch match;
        match.trackId = row.trackId;
        match.cropSamples = row.cropSamples;
        match.embedding = row.embedding;
        match.hasEmbedding = row.hasEmbedding;
        QVector<facefind::Candidate> representativeSamples = row.cropSamples;
        std::sort(representativeSamples.begin(), representativeSamples.end(), [](const facefind::Candidate& a,
                                                                                const facefind::Candidate& b) {
            return a.score > b.score;
        });
        match.representativeCandidate =
            representativeSamples.isEmpty() ? facefind::Candidate{} : representativeSamples.first();
        match.representativeCandidate.trackId = row.trackId;
        if (row.trackId == request.seedTrackId) {
            match.cosine = 1.0;
            match.decision = QStringLiteral("seed");
        } else if (!row.hasEmbedding || row.embedding.empty()) {
            match.decision = QStringLiteral("no_embedding");
        } else {
            match.cosine =
                jcut::identity_resolution::cosineSimilarity(seedEvidence.embedding, row.embedding);
            if (match.cosine >= result.autoMatchThreshold) {
                match.decision = QStringLiteral("auto_match");
            } else if (match.cosine >= result.reviewThreshold) {
                match.decision = QStringLiteral("review");
            } else {
                match.decision = QStringLiteral("different");
            }
        }
        QJsonObject rowJson;
        rowJson[QStringLiteral("seed_track_id")] = request.seedTrackId;
        rowJson[QStringLiteral("track_id")] = match.trackId;
        rowJson[QStringLiteral("cosine")] = match.cosine;
        rowJson[QStringLiteral("decision")] = match.decision;
        rowJson[QStringLiteral("has_embedding")] = match.hasEmbedding;
        rowJson[QStringLiteral("crop_count")] = match.cropSamples.size();
        rowJson[QStringLiteral("representative_crop")] = match.representativeCandidate.cropPath;
        result.matchRows.push_back(rowJson);
        result.matches.push_back(match);
    }

    std::sort(result.matches.begin(), result.matches.end(), [seedTrackId = request.seedTrackId](const SeedTrackMatch& a,
                                                                                                 const SeedTrackMatch& b) {
        auto rank = [seedTrackId](const SeedTrackMatch& match) {
            if (match.trackId == seedTrackId) {
                return 0;
            }
            if (match.decision == QLatin1StringView("auto_match")) {
                return 1;
            }
            if (match.decision == QLatin1StringView("review")) {
                return 2;
            }
            if (match.decision == QLatin1StringView("no_embedding")) {
                return 3;
            }
            return 4;
        };
        const int rankA = rank(a);
        const int rankB = rank(b);
        if (rankA != rankB) {
            return rankA < rankB;
        }
        if (std::abs(a.cosine - b.cosine) > 0.0001) {
            return a.cosine > b.cosine;
        }
        return a.trackId < b.trackId;
    });
    return result;
}

AssignmentResolutionResult resolveTrackIdentityAssignments(
    const QJsonArray& assignmentTableRows,
    const QVector<facefind::Candidate>& trackCandidates,
    const QString& timestampUtc)
{
    AssignmentResolutionResult result;
    QHash<int, facefind::Candidate> candidateByTrackId;
    for (const facefind::Candidate& candidate : trackCandidates) {
        if (candidate.trackId < 0) {
            continue;
        }
        const auto existing = candidateByTrackId.constFind(candidate.trackId);
        if (existing == candidateByTrackId.constEnd() ||
            candidate.score > existing->score) {
            candidateByTrackId.insert(candidate.trackId, candidate);
        }
    }

    for (const QJsonValue& value : assignmentTableRows) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("decision")).toString() != QStringLiteral("accepted")) {
            continue;
        }
        const QString resolvedSpeaker =
            row.value(QStringLiteral("resolved_speaker_id")).toString().trimmed();
        if (resolvedSpeaker.isEmpty()) {
            continue;
        }
        const QString manualOverride =
            row.value(QStringLiteral("manual_override")).toString().trimmed();
        QJsonArray trackIds = row.value(QStringLiteral("track_ids")).toArray();
        if (trackIds.isEmpty()) {
            const int fallbackTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
            if (fallbackTrackId >= 0) {
                trackIds.push_back(fallbackTrackId);
            }
        }

        for (const QJsonValue& trackValue : trackIds) {
            const int trackId = trackValue.toInt(-1);
            if (trackId < 0) {
                continue;
            }

            QJsonObject overrideRow;
            overrideRow[QStringLiteral("track_id")] = trackId;
            overrideRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
            overrideRow[QStringLiteral("identity_id")] = resolvedSpeaker;
            overrideRow[QStringLiteral("source")] =
                manualOverride.isEmpty() ? QStringLiteral("auto_selected")
                                         : QStringLiteral("human_override");
            overrideRow[QStringLiteral("manual_override")] = !manualOverride.isEmpty();
            result.overrides.push_back(overrideRow);

            QJsonObject auditRow;
            auditRow[QStringLiteral("timestamp_utc")] = timestampUtc;
            auditRow[QStringLiteral("action")] = QStringLiteral("track_identity_set");
            auditRow[QStringLiteral("track_id")] = trackId;
            auditRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
            auditRow[QStringLiteral("identity_id")] = resolvedSpeaker;
            auditRow[QStringLiteral("source")] = overrideRow.value(QStringLiteral("source")).toString();
            result.auditLog.push_back(auditRow);

            QJsonObject resolvedRow;
            resolvedRow[QStringLiteral("track_id")] = trackId;
            resolvedRow[QStringLiteral("identity_id")] = resolvedSpeaker;
            resolvedRow[QStringLiteral("cluster_id")] = row.value(QStringLiteral("cluster_id")).toString();
            resolvedRow[QStringLiteral("resolution_source")] =
                overrideRow.value(QStringLiteral("source")).toString();
            result.resolvedMap.push_back(resolvedRow);

            const auto trackIt = candidateByTrackId.constFind(trackId);
            if (trackIt != candidateByTrackId.constEnd()) {
                result.assignmentsBySpeaker[resolvedSpeaker].push_back(trackIt.value());
            }
        }
    }

    return result;
}

} // namespace jcut::facestream_assignment
