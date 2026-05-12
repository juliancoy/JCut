#include "facestream_assignment_services.h"

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
} // namespace

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
        const FacestreamFrameDomain frameDomain =
            inferFacestreamFrameDomain(request.clip, streamFrameMin, streamFrameMax);

        QJsonObject bestKeyframe;
        double bestScore = -1.0;
        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframe = keyframeValue.toObject();
            const double score = keyframe.value(QLatin1StringView(kTrackingConfidenceKey)).toDouble(0.0);
            if (score > bestScore) {
                bestScore = score;
                bestKeyframe = keyframe;
            }
        }
        if (bestKeyframe.isEmpty()) {
            bestKeyframe = keyframes.at(keyframes.size() / 2).toObject();
            bestScore = bestKeyframe.value(QLatin1StringView(kTrackingConfidenceKey)).toDouble(0.0);
        }

        const int64_t keyframeFrame = qMax<int64_t>(
            0, bestKeyframe.value(QLatin1StringView(kTrackingFrameKey)).toVariant().toLongLong());
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
            continue;
        }

        const qreal xNorm = qBound<qreal>(
            0.0, bestKeyframe.value(QLatin1StringView(kTrackingXKey)).toDouble(0.5), 1.0);
        const qreal yNorm = qBound<qreal>(
            0.0, bestKeyframe.value(QLatin1StringView(kTrackingYKey)).toDouble(0.85), 1.0);
        const qreal boxNorm = qBound<qreal>(
            0.01, bestKeyframe.value(QLatin1StringView(kTrackingBoxSizeKey)).toDouble(0.2), 1.0);
        // Keep crop extraction aligned with the preview box rendering center-box path.
        const QRectF normRect = normalizedCenterBoxRect(
            xNorm, yNorm, boxNorm, QSizeF(image.width(), image.height()));
        const QRect cropRect = pixelRectFromNormalizedRect(normRect, image.size());
        if (!cropRect.isValid() || cropRect.isEmpty()) {
            continue;
        }
        const QString cropPath = QDir(request.cropsDir).absoluteFilePath(
            QStringLiteral("%1_track_%2.png").arg(request.videoStem).arg(trackId, 3, 10, QLatin1Char('0')));
        const QImage crop = image.copy(cropRect)
                                .scaled(160, 160, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        if (crop.isNull() || !crop.save(cropPath)) {
            continue;
        }

        facefind::Candidate candidate;
        candidate.frame = timelineFrame;
        candidate.x = xNorm;
        candidate.y = yNorm;
        candidate.box = boxNorm;
        candidate.score = qBound<qreal>(0.0, bestScore, 1.0);
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
        result.candidateRows.push_back(row);
    }

    if (!progress(request.streams.size(), QStringLiteral("Writing FaceStream crop candidates..."))) {
        result.canceled = true;
        return result;
    }

    result.ok = true;
    return result;
}

ClusterResult clusterFaceTracks(
    const ClusterRequest& request,
    const std::function<bool(int, const QString&)>& progress)
{
    ClusterResult result;
    const int n = request.trackCandidates.size();

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

    QVector<std::vector<float>> embeddings(n);
    QVector<bool> hasEmbedding(n, false);
    jcut::identity_resolution::ArcFaceNcnnEmbedder embedder;
    result.embeddingReady = embedder.initialize(request.arcfaceParamPath, request.arcfaceBinPath, &result.embeddingError);

    if (result.embeddingReady) {
        for (int i = 0; i < n; ++i) {
            if ((i % 25) == 0 &&
                !progress(1,
                          QStringLiteral("Embedding representative crops: %1 / %2")
                              .arg(i)
                              .arg(n))) {
                result.cancelStageMessage = QStringLiteral("User canceled FaceStream identity embedding.");
                return result;
            }
            if (request.trackCandidates.at(i).cropPath.trimmed().isEmpty()) {
                continue;
            }
            std::vector<float> embedding;
            if (embedder.embedFaceCrop(request.trackCandidates.at(i).cropPath, &embedding)) {
                embeddings[i] = std::move(embedding);
                hasEmbedding[i] = true;
                ++result.embeddedTrackCount;
            }
        }

        for (int i = 0; i < n; ++i) {
            if ((i % 10) == 0 &&
                !progress(2,
                          QStringLiteral("Clustering FaceStream tracks: row %1 / %2")
                              .arg(i)
                              .arg(n))) {
                result.cancelStageMessage = QStringLiteral("User canceled FaceStream identity clustering.");
                return result;
            }
            for (int j = i + 1; j < n; ++j) {
                if (!hasEmbedding[i] || !hasEmbedding[j]) {
                    continue;
                }
                const double cosine =
                    jcut::identity_resolution::cosineSimilarity(embeddings.at(i), embeddings.at(j));
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
                pairRow[QStringLiteral("track_a")] = request.trackCandidates.at(i).trackId;
                pairRow[QStringLiteral("track_b")] = request.trackCandidates.at(j).trackId;
                pairRow[QStringLiteral("cosine")] = cosine;
                pairRow[QStringLiteral("decision")] = decision;
                result.clusterDiagnosticsRows.push_back(pairRow);
            }
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
        const int ta = ma.isEmpty() ? -1 : request.trackCandidates.at(ma.first()).trackId;
        const int tb = mb.isEmpty() ? -1 : request.trackCandidates.at(mb.first()).trackId;
        return ta < tb;
    });

    int clusterIndex = 1;
    for (int rootIndex : roots) {
        QVector<int> memberIndexes = membersByRoot.value(rootIndex);
        if (memberIndexes.isEmpty()) {
            continue;
        }
        std::sort(memberIndexes.begin(), memberIndexes.end(), [&](int a, int b) {
            return request.trackCandidates.at(a).trackId < request.trackCandidates.at(b).trackId;
        });
        int representativeIndex = memberIndexes.first();
        for (int idx : memberIndexes) {
            if (request.trackCandidates.at(idx).score > request.trackCandidates.at(representativeIndex).score) {
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
                if (!hasEmbedding[a] || !hasEmbedding[b]) {
                    continue;
                }
                pairSum += jcut::identity_resolution::cosineSimilarity(embeddings.at(a), embeddings.at(b));
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
            anyEmbedding = anyEmbedding || hasEmbedding[idx];
        }
        if (!result.embeddingReady || !anyEmbedding) {
            status = QStringLiteral("singleton_no_embedding");
            confidence = 1.0;
        }

        const QString clusterId = QStringLiteral("person_%1").arg(clusterIndex++, 3, 10, QLatin1Char('0'));
        QVector<int> memberTrackIds;
        QJsonArray memberTracksJson;
        for (int idx : memberIndexes) {
            const int trackId = request.trackCandidates.at(idx).trackId;
            if (trackId >= 0) {
                memberTrackIds.push_back(trackId);
                memberTracksJson.push_back(trackId);
            }
        }

        facefind::Candidate representative = request.trackCandidates.at(representativeIndex);
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
        clusterRow[QStringLiteral("confidence")] = confidence;
        clusterRow[QStringLiteral("status")] = status;
        result.clusterRows.push_back(clusterRow);
    }

    result.ok = true;
    return result;
}

} // namespace jcut::facestream_assignment
