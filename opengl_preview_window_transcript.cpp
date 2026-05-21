#include "opengl_preview.h"
#include "facestream_artifact_utils.h"
#include "facestream_runtime.h"
#include "facestream_time_mapping.h"
#include "gl_frame_texture_shared.h"
#include "preview_view_transform.h"
#include "transcript_engine.h"

#include <cmath>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace {
bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
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

}

bool PreviewWindow::clipShowsTranscriptOverlay(const TimelineClip& clip) const {
    return clipSupportsTranscript(clip) && clip.transcriptOverlay.enabled;
}

void PreviewWindow::invalidateTranscriptOverlayCache(const QString& clipFilePath) {
    if (clipFilePath.isEmpty()) {
        m_transcriptSectionsCache.clear();
        m_speakerTrackPointsCache.clear();
        m_rawDetectionPointsCache.clear();
        invalidateTranscriptSpeakerProfileCache();
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    } else {
        m_transcriptSectionsCache.remove(clipFilePath);
        const QString transcriptPath = activeTranscriptPathForClipFile(clipFilePath);
        if (!transcriptPath.isEmpty()) {
            const QString keyPrefix = transcriptPath + QLatin1Char('|');
            for (auto it = m_speakerTrackPointsCache.begin(); it != m_speakerTrackPointsCache.end();) {
                if (it.key() == transcriptPath || it.key().startsWith(keyPrefix)) {
                    it = m_speakerTrackPointsCache.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = m_rawDetectionPointsCache.begin(); it != m_rawDetectionPointsCache.end();) {
                if (it.key() == transcriptPath || it.key().startsWith(keyPrefix)) {
                    it = m_rawDetectionPointsCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
        invalidateTranscriptSpeakerProfileCache(activeTranscriptPathForClipFile(clipFilePath));
        // Textures are content/style keyed; clear all to avoid keeping stale overlay textures.
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    }
    scheduleRepaint();
}

const QVector<TranscriptSection>& PreviewWindow::transcriptSectionsForClip(const TimelineClip& clip) const {
    static const QVector<TranscriptSection> kEmpty;
    const QString key = clip.filePath;
    auto it = m_transcriptSectionsCache.find(key);
    if (it == m_transcriptSectionsCache.end()) {
        it = m_transcriptSectionsCache.insert(
            key, loadTranscriptRuntimeDocument(activeTranscriptPathForClipFile(clip.filePath)));
    }
    return it.value() ? it.value()->sections : kEmpty;
}

const QVector<PreviewWindow::SpeakerTrackPoint>& PreviewWindow::speakerTrackPointsForClip(const TimelineClip& clip) const {
    static const QVector<SpeakerTrackPoint> kEmpty;
    if (!clipSupportsTranscript(clip) || clip.filePath.isEmpty()) {
        return kEmpty;
    }
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    if (transcriptPath.isEmpty()) {
        return kEmpty;
    }

    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return kEmpty;
    }
    const qint64 transcriptMtimeMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 artifactMtimeMs = facestreamArtifactRevisionMsForTranscript(transcriptPath);
    const QString trackPointsCacheKey = QStringLiteral("%1|%2")
        .arg(transcriptPath, clip.id.trimmed());
    auto it = m_speakerTrackPointsCache.find(trackPointsCacheKey);
    if (it != m_speakerTrackPointsCache.end() &&
        it->transcriptMtimeMs == transcriptMtimeMs &&
        it->artifactMtimeMs == artifactMtimeMs) {
        return it->points;
    }

    SpeakerTrackPointCacheEntry entry;
    entry.transcriptMtimeMs = transcriptMtimeMs;
    entry.artifactMtimeMs = artifactMtimeMs;
    QJsonDocument doc;
    if (!loadTranscriptJsonCached(transcriptPath, &doc) || !doc.isObject()) {
        it = m_speakerTrackPointsCache.insert(trackPointsCacheKey, entry);
        return it->points;
    }

    // Canonical runtime source: continuity FaceStream tracks from artifact sidecar.
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(transcriptPath, &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
        const QJsonArray streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            doc.object());
        FacestreamFrameDomain explicitFrameDomain = FacestreamFrameDomain::SourceRelative;
        const bool hasExplicitFrameDomain = continuityPayloadFrameDomain(
            continuityRoot,
            QStringLiteral("streams_frame_domain"),
            &explicitFrameDomain);
        if (!hasExplicitFrameDomain) {
            it = m_speakerTrackPointsCache.insert(trackPointsCacheKey, entry);
            return it->points;
        }
        const QString sourceFilter = m_facestreamOverlaySource.trimmed().toLower();
        for (const QJsonValue& streamValue : streams) {
            const QJsonObject streamObj = streamValue.toObject();
            const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
            const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
            const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
            int64_t streamFrameMin = std::numeric_limits<int64_t>::max();
            int64_t streamFrameMax = -1;
            for (const QJsonValue& keyValue : keyframes) {
                const QJsonObject obj = keyValue.toObject();
                if (obj.isEmpty()) {
                    continue;
                }
                const int64_t frame = obj.value(QStringLiteral("frame")).toVariant().toLongLong();
                if (frame < 0) {
                    continue;
                }
                streamFrameMin = qMin<int64_t>(streamFrameMin, frame);
                streamFrameMax = qMax<int64_t>(streamFrameMax, frame);
            }
            const FacestreamFrameDomain streamFrameDomain = explicitFrameDomain;
            for (const QJsonValue& keyValue : keyframes) {
                const QJsonObject obj = keyValue.toObject();
                if (obj.isEmpty()) {
                    continue;
                }
                const QString source = obj.value(QStringLiteral("source")).toString().trimmed().toLower();
                if (!sourceFilter.isEmpty() &&
                    sourceFilter != QStringLiteral("all") &&
                    source != sourceFilter) {
                    continue;
                }
                const int64_t frame = obj.value(QStringLiteral("frame")).toVariant().toLongLong();
                const qreal rawX = obj.value(QStringLiteral("x")).toDouble(-1.0);
                const qreal rawY = obj.value(QStringLiteral("y")).toDouble(-1.0);
                if (frame < 0 || rawX < 0.0 || rawX > 1.0 || rawY < 0.0 || rawY > 1.0) {
                    continue;
                }
                SpeakerTrackPoint p;
                p.frame = frame;
                p.frameDomain = streamFrameDomain;
                p.x = rawX;
                p.y = rawY;
                p.speakerId = streamId.isEmpty() ? QStringLiteral("track") : streamId;
                p.streamId = streamId;
                p.trackId = trackId;
                const qreal boxSize = obj.value(QStringLiteral("box_size")).toDouble(
                    obj.value(QStringLiteral("box")).toDouble(-1.0));
                if (boxSize > 0.0) {
                    p.boxSizeNorm = qBound<qreal>(0.01, boxSize, 1.0);
                }
                const qreal boxLeft = obj.value(QStringLiteral("box_left")).toDouble(-1.0);
                const qreal boxTop = obj.value(QStringLiteral("box_top")).toDouble(-1.0);
                const qreal boxRight = obj.value(QStringLiteral("box_right")).toDouble(-1.0);
                const qreal boxBottom = obj.value(QStringLiteral("box_bottom")).toDouble(-1.0);
                if (boxLeft >= 0.0 && boxTop >= 0.0 && boxRight > boxLeft && boxBottom > boxTop &&
                    boxRight <= 1.0 && boxBottom <= 1.0) {
                    p.hasBox = true;
                    p.boxLeft = boxLeft;
                    p.boxTop = boxTop;
                    p.boxRight = boxRight;
                    p.boxBottom = boxBottom;
                } else if (p.boxSizeNorm > 0.0) {
                    const qreal half = p.boxSizeNorm * 0.5;
                    p.hasBox = true;
                    p.boxLeft = qBound<qreal>(0.0, rawX - half, 1.0);
                    p.boxTop = qBound<qreal>(0.0, rawY - half, 1.0);
                    p.boxRight = qBound<qreal>(0.0, rawX + half, 1.0);
                    p.boxBottom = qBound<qreal>(0.0, rawY + half, 1.0);
                }
                entry.points.push_back(p);
            }
        }
    }

    std::sort(entry.points.begin(), entry.points.end(), [](const SpeakerTrackPoint& a, const SpeakerTrackPoint& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        return a.speakerId < b.speakerId;
    });
    it = m_speakerTrackPointsCache.insert(trackPointsCacheKey, entry);
    return it->points;
}

const QVector<PreviewWindow::SpeakerTrackPoint>& PreviewWindow::rawDetectionPointsForClip(const TimelineClip& clip) const {
    static const QVector<SpeakerTrackPoint> kEmpty;
    if (!clipSupportsTranscript(clip) || clip.filePath.isEmpty()) {
        return kEmpty;
    }
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    if (transcriptPath.isEmpty()) {
        return kEmpty;
    }

    const QFileInfo info(transcriptPath);
    if (!info.exists() || !info.isFile()) {
        return kEmpty;
    }
    const qint64 transcriptMtimeMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 artifactMtimeMs = facestreamArtifactRevisionMsForTranscript(transcriptPath);
    const QString cacheKey = QStringLiteral("%1|raw|%2").arg(transcriptPath, clip.id.trimmed());
    auto it = m_rawDetectionPointsCache.find(cacheKey);
    if (it != m_rawDetectionPointsCache.end() &&
        it->transcriptMtimeMs == transcriptMtimeMs &&
        it->artifactMtimeMs == artifactMtimeMs) {
        return it->points;
    }

    SpeakerTrackPointCacheEntry entry;
    entry.transcriptMtimeMs = transcriptMtimeMs;
    entry.artifactMtimeMs = artifactMtimeMs;
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadFacestreamArtifact(transcriptPath, &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
        const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
        int64_t minFrame = std::numeric_limits<int64_t>::max();
        int64_t maxFrame = -1;
        for (const QJsonValue& frameValue : rawFrames) {
            const QJsonObject frameObj = frameValue.toObject();
            const int64_t frame = frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            if (frame < 0) {
                continue;
            }
            minFrame = qMin(minFrame, frame);
            maxFrame = qMax(maxFrame, frame);
        }
        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!continuityPayloadFrameDomain(
                continuityRoot,
                QStringLiteral("raw_frames_frame_domain"),
                &frameDomain)) {
            it = m_rawDetectionPointsCache.insert(cacheKey, entry);
            return it->points;
        }
        for (const QJsonValue& frameValue : rawFrames) {
            const QJsonObject frameObj = frameValue.toObject();
            const int64_t frame = frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            if (frame < 0) {
                continue;
            }
            const QJsonArray detections = frameObj.value(QStringLiteral("detections")).toArray();
            for (const QJsonValue& detValue : detections) {
                const QJsonObject detObj = detValue.toObject();
                SpeakerTrackPoint p;
                p.frame = frame;
                p.frameDomain = frameDomain;
                const qreal frameWidth = qMax<qreal>(1.0, frameObj.value(QStringLiteral("frame_width")).toDouble(
                    detObj.value(QStringLiteral("frame_width")).toDouble(0.0)));
                const qreal frameHeight = qMax<qreal>(1.0, frameObj.value(QStringLiteral("frame_height")).toDouble(
                    detObj.value(QStringLiteral("frame_height")).toDouble(0.0)));
                p.x = qBound<qreal>(
                    0.0,
                    detObj.value(QStringLiteral("x_norm")).toDouble(
                        detObj.value(QStringLiteral("x")).toDouble(0.0) / frameWidth),
                    1.0);
                p.y = qBound<qreal>(
                    0.0,
                    detObj.value(QStringLiteral("y_norm")).toDouble(
                        detObj.value(QStringLiteral("y")).toDouble(0.0) / frameHeight),
                    1.0);
                const qreal width = qBound<qreal>(
                    0.0,
                    detObj.value(QStringLiteral("w_norm")).toDouble(
                        detObj.value(QStringLiteral("w")).toDouble(0.0) / frameWidth),
                    1.0);
                const qreal height = qBound<qreal>(
                    0.0,
                    detObj.value(QStringLiteral("h_norm")).toDouble(
                        detObj.value(QStringLiteral("h")).toDouble(0.0) / frameHeight),
                    1.0);
                if (width <= 0.0 || height <= 0.0) {
                    continue;
                }
                p.hasBox = true;
                p.boxLeft = p.x;
                p.boxTop = p.y;
                p.boxRight = qBound<qreal>(0.0, p.x + width, 1.0);
                p.boxBottom = qBound<qreal>(0.0, p.y + height, 1.0);
                p.boxSizeNorm = qMax(width, height);
                p.speakerId = QStringLiteral("raw_detection");
                p.streamId = QStringLiteral("raw_detection");
                entry.points.push_back(p);
            }
        }
    }
    it = m_rawDetectionPointsCache.insert(cacheKey, entry);
    return it->points;
}

void PreviewWindow::drawSpeakerTrackPointsOverlay(QPainter* painter, const QList<TimelineClip>& activeClips) {
    if (!painter || activeClips.isEmpty()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto prettyTrackerLabel = [](const QString& sourceRaw) {
        const QString source = sourceRaw.trimmed().toLower();
        if (source.isEmpty() || source == QStringLiteral("all")) return QStringLiteral("All Trackers");
        if (source == QStringLiteral("opencv_haar_v1")) return QStringLiteral("OpenCV Haar (Balanced)");
        if (source == QStringLiteral("opencv_haar_smallfaces_v1")) return QStringLiteral("OpenCV Haar (Small Faces)");
        if (source == QStringLiteral("opencv_haar_precision_v1")) return QStringLiteral("OpenCV Haar (Precision)");
        if (source == QStringLiteral("opencv_lbp_smallfaces_v1")) return QStringLiteral("OpenCV LBP (Small Faces)");
        if (source == QStringLiteral("opencv_dnn_auto_v1")) return QStringLiteral("OpenCV DNN Auto");
        if (source == QStringLiteral("opencv_python_compatible_v1")) return QStringLiteral("OpenCV Python-Compatible");
        if (source == QStringLiteral("python_legacy_v1")) return QStringLiteral("Legacy Python Haar");
        if (source == QStringLiteral("local_insightface_hybrid_v1")) return QStringLiteral("Local Production Hybrid");
        if (source == QStringLiteral("docker_dnn_v1")) return QStringLiteral("Docker DNN Res10");
        if (source == QStringLiteral("docker_insightface_retinaface_v1")) return QStringLiteral("Docker InsightFace RetinaFace");
        if (source == QStringLiteral("docker_insightface_hybrid_v1")) return QStringLiteral("Docker Production Hybrid");
        if (source == QStringLiteral("docker_yolov8_face_v1")) return QStringLiteral("Docker YOLOv8 Face");
        if (source == QStringLiteral("docker_mtcnn_v1")) return QStringLiteral("Docker MTCNN");
        if (source == QStringLiteral("sam3_face_v1")) return QStringLiteral("SAM3 Face");
        if (source == QStringLiteral("opencv_contrib_csrt_v1")) return QStringLiteral("OpenCV Contrib CSRT");
        if (source == QStringLiteral("opencv_contrib_kcf_v1")) return QStringLiteral("OpenCV Contrib KCF");
        return sourceRaw;
    };

    bool drewLegend = false;
    for (const TimelineClip& clip : activeClips) {
        if (!clipSupportsTranscript(clip)) {
            continue;
        }
        const PreviewOverlayInfo info = m_overlayModel.overlays.value(clip.id);
        if (!info.bounds.isValid()) {
            continue;
        }
        const QVector<SpeakerTrackPoint>& points = speakerTrackPointsForClip(clip);
        if (points.isEmpty()) {
            continue;
        }
        if (!drewLegend && (m_showSpeakerTrackBoxes || m_showSpeakerTrackPoints)) {
            const QString legendText = QStringLiteral("Tracker: %1")
                .arg(prettyTrackerLabel(m_facestreamOverlaySource));
            const QRectF legendRect(12.0, 12.0, 300.0, 24.0);
            const bool dockerSource = m_facestreamOverlaySource.trimmed().toLower().startsWith(QStringLiteral("docker_"));
            const QColor chipBg = dockerSource ? QColor(QStringLiteral("#dff1ff")) : QColor(0, 0, 0, 165);
            const QColor chipFg = dockerSource ? QColor(QStringLiteral("#16384f")) : QColor(235, 245, 255, 240);
            painter->setPen(Qt::NoPen);
            painter->setBrush(chipBg);
            painter->drawRoundedRect(legendRect, 6.0, 6.0);
            painter->setPen(chipFg);
            painter->drawText(legendRect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, legendText);
            drewLegend = true;
        }
        const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
        const int64_t sourceEnd =
            sourceStart + qMax<int64_t>(0, clip.sourceDurationFrames - 1);
        const int64_t currentSourceFrame =
            sourceFrameForSample(clip, m_interaction.currentSample);
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        struct OverlayCandidate {
            SpeakerTrackPoint point;
            QPointF screenPoint;
            QColor color;
            int64_t distance = std::numeric_limits<int64_t>::max();
        };
        QVector<OverlayCandidate> candidates;
        candidates.reserve(points.size());

        for (const SpeakerTrackPoint& p : points) {
            const int64_t pointSourceFrame = mapFacestreamFrameToSourceFrame(
                clip, p.frame, p.frameDomain, m_interaction.renderSyncMarkers);
            if (pointSourceFrame < sourceStart || pointSourceFrame > sourceEnd) {
                continue;
            }
            const uint hueHash = qHash(p.speakerId);
            const QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 180, 245, 190);
            const QPointF screenPoint = mapNormalizedClipPointToScreen(info, QPointF(p.x, p.y));
            const int64_t distance = std::llabs(pointSourceFrame - currentSourceFrame);
            if (m_showSpeakerTrackPoints) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(color);
                painter->drawEllipse(screenPoint, 2.7, 2.7);
            }
            OverlayCandidate candidate;
            candidate.point = p;
            candidate.screenPoint = screenPoint;
            candidate.color = color;
            candidate.distance = distance;
            candidates.push_back(candidate);
            if (distance < nearestDistance) {
                nearestDistance = distance;
            }
        }

        if (candidates.isEmpty()) {
            continue;
        }

        for (const OverlayCandidate& candidate : candidates) {
            if (candidate.distance != nearestDistance) {
                continue;
            }
            if (m_showSpeakerTrackPoints) {
                painter->setPen(QPen(QColor(255, 255, 255, 235), 1.2));
                painter->setBrush(candidate.color);
                painter->drawEllipse(candidate.screenPoint, 4.4, 4.4);
            }
            if ((!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled) ||
                !candidate.point.hasBox) {
                continue;
            }
            qreal leftNorm = candidate.point.boxLeft;
            qreal topNorm = candidate.point.boxTop;
            qreal rightNorm = candidate.point.boxRight;
            qreal bottomNorm = candidate.point.boxBottom;
            if (candidate.point.boxSizeNorm > 0.0 &&
                info.clipPixelSize.width() > 1.0 &&
                info.clipPixelSize.height() > 1.0) {
                const QRectF centerBoxNorm = normalizedCenterBoxRect(
                    candidate.point.x,
                    candidate.point.y,
                    candidate.point.boxSizeNorm,
                    info.clipPixelSize);
                if (centerBoxNorm.isValid() && !centerBoxNorm.isEmpty()) {
                    leftNorm = centerBoxNorm.left();
                    topNorm = centerBoxNorm.top();
                    rightNorm = centerBoxNorm.right();
                    bottomNorm = centerBoxNorm.bottom();
                }
            }
            const QPointF p1 = mapNormalizedClipPointToScreen(info, QPointF(leftNorm, topNorm));
            const QPointF p2 = mapNormalizedClipPointToScreen(info, QPointF(rightNorm, topNorm));
            const QPointF p3 = mapNormalizedClipPointToScreen(info, QPointF(rightNorm, bottomNorm));
            const QPointF p4 = mapNormalizedClipPointToScreen(info, QPointF(leftNorm, bottomNorm));
            QPainterPath boxPath;
            boxPath.moveTo(p1);
            boxPath.lineTo(p2);
            boxPath.lineTo(p3);
            boxPath.lineTo(p4);
            boxPath.closeSubpath();
            const bool hovered =
                m_interaction.faceStreamAssignmentInteractionEnabled &&
                candidate.point.trackId >= 0 &&
                candidate.point.trackId == m_interaction.transient.hoveredFaceStreamTrackId &&
                candidate.point.streamId == m_interaction.transient.hoveredFaceStreamId &&
                clip.id == m_interaction.transient.hoveredFaceStreamClipId;
            const QColor boxStroke = hovered
                ? QColor(QStringLiteral("#ffd54a"))
                : QColor(candidate.color.red(), candidate.color.green(), candidate.color.blue(), 235);
            const QColor boxFill = hovered
                ? QColor(255, 213, 74, 54)
                : QColor(candidate.color.red(), candidate.color.green(), candidate.color.blue(), 42);
            painter->setPen(QPen(boxStroke, 2.0));
            painter->setBrush(boxFill);
            painter->drawPath(boxPath);
        }
    }

    painter->restore();
}

void PreviewWindow::drawRawDetectionOverlay(QPainter* painter, const QList<TimelineClip>& activeClips) {
    if (!painter || activeClips.isEmpty()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (const TimelineClip& clip : activeClips) {
        if (!clipSupportsTranscript(clip)) {
            continue;
        }
        const PreviewOverlayInfo info = m_overlayModel.overlays.value(clip.id);
        if (!info.bounds.isValid()) {
            continue;
        }
        const QVector<SpeakerTrackPoint>& points = rawDetectionPointsForClip(clip);
        if (points.isEmpty()) {
            continue;
        }
        const int64_t currentSourceFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        for (const SpeakerTrackPoint& point : points) {
            const int64_t pointSourceFrame = mapFacestreamFrameToSourceFrame(
                clip, point.frame, point.frameDomain, m_interaction.renderSyncMarkers);
            if (pointSourceFrame != currentSourceFrame || !point.hasBox) {
                continue;
            }
            const QPointF p1 = mapNormalizedClipPointToScreen(info, QPointF(point.boxLeft, point.boxTop));
            const QPointF p2 = mapNormalizedClipPointToScreen(info, QPointF(point.boxRight, point.boxTop));
            const QPointF p3 = mapNormalizedClipPointToScreen(info, QPointF(point.boxRight, point.boxBottom));
            const QPointF p4 = mapNormalizedClipPointToScreen(info, QPointF(point.boxLeft, point.boxBottom));
            QPainterPath boxPath;
            boxPath.moveTo(p1);
            boxPath.lineTo(p2);
            boxPath.lineTo(p3);
            boxPath.lineTo(p4);
            boxPath.closeSubpath();
            painter->setPen(QPen(QColor(QStringLiteral("#4ade80")), 1.5));
            painter->setBrush(QColor(74, 222, 128, 32));
            painter->drawPath(boxPath);
        }
    }
    painter->restore();
}

bool PreviewWindow::dispatchFaceStreamBoxAtPosition(const QPointF& position)
{
    if (!faceStreamBoxRequested ||
        (!m_showSpeakerTrackBoxes && !m_interaction.faceStreamAssignmentInteractionEnabled)) {
        return false;
    }
    const QList<TimelineClip> activeClips = getActiveClips();
    for (auto clipIt = activeClips.crbegin(); clipIt != activeClips.crend(); ++clipIt) {
        const TimelineClip& clip = *clipIt;
        if (!clipSupportsTranscript(clip)) {
            continue;
        }
        const PreviewOverlayInfo info = m_overlayModel.overlays.value(clip.id);
        if (!info.bounds.isValid()) {
            continue;
        }
        const QVector<SpeakerTrackPoint>& points = speakerTrackPointsForClip(clip);
        if (points.isEmpty()) {
            continue;
        }

        const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
        const int64_t sourceEnd =
            sourceStart + qMax<int64_t>(0, clip.sourceDurationFrames - 1);
        const int64_t currentSourceFrame =
            sourceFrameForSample(clip, m_interaction.currentSample);

        struct HitCandidate {
            SpeakerTrackPoint point;
            QRectF boxNorm;
            int64_t distance = std::numeric_limits<int64_t>::max();
        };
        QVector<HitCandidate> candidates;
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        for (SpeakerTrackPoint p : points) {
            const int64_t pointSourceFrame = mapFacestreamFrameToSourceFrame(
                clip, p.frame, p.frameDomain, m_interaction.renderSyncMarkers);
            if (pointSourceFrame < sourceStart || pointSourceFrame > sourceEnd || !p.hasBox) {
                continue;
            }
            p.sourceFrame = pointSourceFrame;
            qreal leftNorm = p.boxLeft;
            qreal topNorm = p.boxTop;
            qreal rightNorm = p.boxRight;
            qreal bottomNorm = p.boxBottom;
            if (p.boxSizeNorm > 0.0 &&
                info.clipPixelSize.width() > 1.0 &&
                info.clipPixelSize.height() > 1.0) {
                const QRectF centerBoxNorm = normalizedCenterBoxRect(
                    p.x,
                    p.y,
                    p.boxSizeNorm,
                    info.clipPixelSize);
                if (centerBoxNorm.isValid() && !centerBoxNorm.isEmpty()) {
                    leftNorm = centerBoxNorm.left();
                    topNorm = centerBoxNorm.top();
                    rightNorm = centerBoxNorm.right();
                    bottomNorm = centerBoxNorm.bottom();
                }
            }
            const QRectF boxNorm(QPointF(leftNorm, topNorm), QPointF(rightNorm, bottomNorm));
            if (!boxNorm.isValid() || boxNorm.isEmpty()) {
                continue;
            }
            const int64_t distance = std::llabs(pointSourceFrame - currentSourceFrame);
            nearestDistance = qMin(nearestDistance, distance);
            candidates.push_back(HitCandidate{p, boxNorm, distance});
        }

        for (const HitCandidate& candidate : std::as_const(candidates)) {
            if (candidate.distance != nearestDistance) {
                continue;
            }
            const QPointF p1 = mapNormalizedClipPointToScreen(info, candidate.boxNorm.topLeft());
            const QPointF p2 = mapNormalizedClipPointToScreen(info, candidate.boxNorm.topRight());
            const QPointF p3 = mapNormalizedClipPointToScreen(info, candidate.boxNorm.bottomRight());
            const QPointF p4 = mapNormalizedClipPointToScreen(info, candidate.boxNorm.bottomLeft());
            QPainterPath boxPath;
            boxPath.moveTo(p1);
            boxPath.lineTo(p2);
            boxPath.lineTo(p3);
            boxPath.lineTo(p4);
            boxPath.closeSubpath();
            if (!boxPath.contains(position)) {
                continue;
            }
            const QPointF center = candidate.boxNorm.center();
            const qreal side =
                qBound<qreal>(0.01, qMax(candidate.boxNorm.width(), candidate.boxNorm.height()), 1.0);
            faceStreamBoxRequested(clip.id,
                                   candidate.point.trackId,
                                   candidate.point.streamId,
                                   candidate.point.sourceFrame,
                                   center.x(),
                                   center.y(),
                                   side);
            return true;
        }
    }
    return false;
}

bool PreviewWindow::updateHoveredFaceStreamBox(const QPointF& position)
{
    if (!m_interaction.faceStreamAssignmentInteractionEnabled) {
        return false;
    }

    const QList<TimelineClip> activeClips = getActiveClips();
    QString hoveredClipId;
    QString hoveredStreamId;
    int hoveredTrackId = -1;
    for (auto clipIt = activeClips.crbegin(); clipIt != activeClips.crend(); ++clipIt) {
        const TimelineClip& clip = *clipIt;
        if (!clipSupportsTranscript(clip)) {
            continue;
        }
        const PreviewOverlayInfo info = m_overlayModel.overlays.value(clip.id);
        if (!info.bounds.isValid()) {
            continue;
        }
        const QVector<SpeakerTrackPoint>& points = speakerTrackPointsForClip(clip);
        if (points.isEmpty()) {
            continue;
        }

        const int64_t sourceStart = qMax<int64_t>(0, clip.sourceInFrame);
        const int64_t sourceEnd =
            sourceStart + qMax<int64_t>(0, clip.sourceDurationFrames - 1);
        const int64_t currentSourceFrame = sourceFrameForSample(clip, m_interaction.currentSample);
        int64_t nearestDistance = std::numeric_limits<int64_t>::max();
        for (SpeakerTrackPoint p : points) {
            const int64_t pointSourceFrame = mapFacestreamFrameToSourceFrame(
                clip, p.frame, p.frameDomain, m_interaction.renderSyncMarkers);
            if (pointSourceFrame < sourceStart || pointSourceFrame > sourceEnd || !p.hasBox) {
                continue;
            }
            qreal leftNorm = p.boxLeft;
            qreal topNorm = p.boxTop;
            qreal rightNorm = p.boxRight;
            qreal bottomNorm = p.boxBottom;
            if (p.boxSizeNorm > 0.0 &&
                info.clipPixelSize.width() > 1.0 &&
                info.clipPixelSize.height() > 1.0) {
                const QRectF centerBoxNorm = normalizedCenterBoxRect(
                    p.x,
                    p.y,
                    p.boxSizeNorm,
                    info.clipPixelSize);
                if (centerBoxNorm.isValid() && !centerBoxNorm.isEmpty()) {
                    leftNorm = centerBoxNorm.left();
                    topNorm = centerBoxNorm.top();
                    rightNorm = centerBoxNorm.right();
                    bottomNorm = centerBoxNorm.bottom();
                }
            }
            QPainterPath boxPath;
            boxPath.moveTo(mapNormalizedClipPointToScreen(info, QPointF(leftNorm, topNorm)));
            boxPath.lineTo(mapNormalizedClipPointToScreen(info, QPointF(rightNorm, topNorm)));
            boxPath.lineTo(mapNormalizedClipPointToScreen(info, QPointF(rightNorm, bottomNorm)));
            boxPath.lineTo(mapNormalizedClipPointToScreen(info, QPointF(leftNorm, bottomNorm)));
            boxPath.closeSubpath();
            if (!boxPath.contains(position)) {
                continue;
            }
            const int64_t distance = std::llabs(pointSourceFrame - currentSourceFrame);
            if (distance > nearestDistance) {
                continue;
            }
            nearestDistance = distance;
            hoveredClipId = clip.id;
            hoveredStreamId = p.streamId;
            hoveredTrackId = p.trackId;
        }
        if (hoveredTrackId >= 0) {
            break;
        }
    }

    PreviewInteractionTransientState& transient = m_interaction.transient;
    const bool changed =
        transient.hoveredFaceStreamTrackId != hoveredTrackId ||
        transient.hoveredFaceStreamClipId != hoveredClipId ||
        transient.hoveredFaceStreamId != hoveredStreamId;
    if (changed) {
        transient.hoveredFaceStreamTrackId = hoveredTrackId;
        transient.hoveredFaceStreamClipId = hoveredClipId;
        transient.hoveredFaceStreamId = hoveredStreamId;
        scheduleRepaint();
    }
    return hoveredTrackId >= 0;
}

void PreviewWindow::drawSpeakerFramingTargetOverlay(QPainter* painter,
                                                    const QList<TimelineClip>& activeClips,
                                                    const QRect& compositeRect) {
    if (!painter || !compositeRect.isValid() || activeClips.isEmpty()) {
        return;
    }

    const TimelineClip* selectedClip = nullptr;
    const QString selectedId = m_interaction.selectedClipId.trimmed();
    if (!selectedId.isEmpty()) {
        for (const TimelineClip& clip : activeClips) {
            if (clip.id == selectedId) {
                selectedClip = &clip;
                break;
            }
        }
    }
    // Fallback: if selection is stale, still show target box for the first
    // active clip that has a valid target box configured.
    if (!selectedClip) {
        for (const TimelineClip& clip : activeClips) {
            if (qBound<qreal>(-1.0, clip.speakerFramingTargetBoxNorm, 1.0) > 0.0) {
                selectedClip = &clip;
                break;
            }
        }
    }
    if (!selectedClip) {
        return;
    }

    const qreal targetXNorm = qBound<qreal>(0.0, selectedClip->speakerFramingTargetXNorm, 1.0);
    const qreal targetYNorm = qBound<qreal>(0.0, selectedClip->speakerFramingTargetYNorm, 1.0);
    const qreal targetBoxNorm = qBound<qreal>(-1.0, selectedClip->speakerFramingTargetBoxNorm, 1.0);
    if (targetBoxNorm <= 0.0) {
        return;
    }

    const qreal centerX = compositeRect.left() + (targetXNorm * compositeRect.width());
    const qreal centerY = compositeRect.top() + (targetYNorm * compositeRect.height());
    const qreal targetSideScreenPx = qBound<qreal>(
        2.0,
        targetBoxNorm * qMax<qreal>(1.0, qMin<qreal>(compositeRect.width(), compositeRect.height())),
        4096.0);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QColor accentColor(255, 226, 74, 235);
    painter->setPen(QPen(accentColor, 2.0, Qt::DashLine));
    painter->setBrush(QColor(255, 226, 74, 28));

    const qreal halfSide = targetSideScreenPx * 0.5;
    const QRectF box(centerX - halfSide, centerY - halfSide, targetSideScreenPx, targetSideScreenPx);
    painter->drawRect(box);

    painter->setPen(QPen(accentColor, 1.8));
    painter->drawLine(QPointF(centerX - 9.0, centerY), QPointF(centerX + 9.0, centerY));
    painter->drawLine(QPointF(centerX, centerY - 9.0), QPointF(centerX, centerY + 9.0));

    const QRectF badgeRect(centerX + 12.0, centerY - 40.0, 260.0, 34.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(17, 19, 22, 190));
    painter->drawRoundedRect(badgeRect, 6.0, 6.0);
    painter->setPen(QColor(255, 238, 153, 245));
    painter->drawText(badgeRect.adjusted(8.0, 0.0, -8.0, 0.0),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral("FaceBox  X:%1 Y:%2 S:%3")
                          .arg(QString::number(targetXNorm, 'f', 2))
                          .arg(QString::number(targetYNorm, 'f', 2))
                          .arg(QString::number(targetBoxNorm, 'f', 2)));

    painter->restore();
}

TranscriptOverlayLayout PreviewWindow::transcriptOverlayLayoutForClip(const TimelineClip& clip) const {
    if (!clipShowsTranscriptOverlay(clip)) return {};
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    if (sections.isEmpty()) return {};
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_interaction.currentSample, m_interaction.renderSyncMarkers);
    return transcriptOverlayLayoutAtSourceFrame(clip, sections, sourceFrame);
}

QRectF PreviewWindow::transcriptOverlayRectForTarget(const TimelineClip& clip, const QRect& targetRect) const {
    const QPointF previewScale = previewCanvasScale(targetRect);
    const QSize outputSize = m_interaction.outputSize.isValid() ? m_interaction.outputSize : QSize(1080, 1920);
    const qreal outputWidth = qMax<qreal>(1.0, static_cast<qreal>(outputSize.width()));
    const qreal outputHeight = qMax<qreal>(1.0, static_cast<qreal>(outputSize.height()));
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_interaction.currentSample, m_interaction.renderSyncMarkers);
    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, sourceFrame);
    const QSizeF size(outputRect.width() * previewScale.x(),
                      outputRect.height() * previewScale.y());
    const QPointF outputTranslation(outputRect.center().x() - (outputWidth / 2.0),
                                    outputRect.center().y() - (outputHeight / 2.0));
    const QPointF center(targetRect.center().x() + (outputTranslation.x() * previewScale.x()),
                         targetRect.center().y() + (outputTranslation.y() * previewScale.y()));
    return QRectF(center.x() - (size.width() / 2.0),
                  center.y() - (size.height() / 2.0),
                  size.width(),
                  size.height());
}

QSizeF PreviewWindow::transcriptOverlaySizeForSelectedClip() const {
    const PreviewOverlayInfo info = m_overlayModel.overlays.value(m_interaction.selectedClipId);
    const QRect compositeRect = scaledCanvasRect(previewCanvasBaseRect());
    const QPointF previewScale = previewCanvasScale(compositeRect);
    if (previewScale.x() == 0.0 || previewScale.y() == 0.0) {
        return {};
    }
    return QSizeF(info.bounds.width() / previewScale.x(),
                  info.bounds.height() / previewScale.y());
}

void PreviewWindow::drawTranscriptOverlay(QPainter* painter, const TimelineClip& clip, const QRect& targetRect) {
    const TranscriptOverlayLayout overlayLayout = transcriptOverlayLayoutForClip(clip);
    if (overlayLayout.lines.isEmpty()) return;
    const QSize outputSize = m_interaction.outputSize.isValid() ? m_interaction.outputSize : QSize(1080, 1920);
    const QString transcriptPath = activeTranscriptPathForClipFile(clip.filePath);
    const QVector<TranscriptSection>& sections = transcriptSectionsForClip(clip);
    const int64_t sourceFrame =
        transcriptFrameForClipAtTimelineSample(clip, m_interaction.currentSample, m_interaction.renderSyncMarkers);
    const QRectF outputRect = transcriptOverlayRectInOutputSpace(
        clip, outputSize, transcriptPath, sections, sourceFrame);
    if (outputRect.width() <= 0.0 || outputRect.height() <= 0.0) return;
    const QRectF bounds = transcriptOverlayRectForTarget(clip, targetRect);
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) return;
    const QRectF localBounds(0.0, 0.0, outputRect.width(), outputRect.height());
    const QRectF localTextBounds = localBounds.adjusted(18.0, 14.0, -18.0, -14.0);
    if (localTextBounds.width() <= 0.0 || localTextBounds.height() <= 0.0) return;
    const QColor highlightFillColor(QStringLiteral("#fff2a8"));
    const QColor highlightTextColor(QStringLiteral("#181818"));
    QString titleShadowHtml;
    QString titleTextHtml;
    if (clip.transcriptOverlay.showSpeakerTitle) {
        const QString titleText = transcriptSpeakerTitleForSourceFrame(transcriptPath, sections, sourceFrame);
        if (clip.transcriptOverlay.showShadow) {
            titleShadowHtml = transcriptSpeakerTitleHtml(titleText, QColor(0, 0, 0, 200));
        }
        titleTextHtml = transcriptSpeakerTitleHtml(titleText, clip.transcriptOverlay.textColor);
    }
    const QString shadowHtml = clip.transcriptOverlay.showShadow
        ? (titleShadowHtml + transcriptOverlayHtml(
            overlayLayout, QColor(0, 0, 0, 200), QColor(0, 0, 0, 200), QColor(0, 0, 0, 0)))
        : QString();
    const QString textHtml = titleTextHtml + transcriptOverlayHtml(
        overlayLayout, clip.transcriptOverlay.textColor, highlightTextColor, highlightFillColor);
    if (textHtml.isEmpty()) return;

    painter->save();
    const render_detail::OverlayImage image = renderTranscriptOverlay(
        clip,
        localBounds,
        localTextBounds,
        clip.transcriptOverlay.fontPointSize,
        shadowHtml,
        textHtml);
    painter->drawImage(bounds, image.asQImageView());
    painter->restore();

    if (m_interaction.transcriptOverlayInteractionEnabled) {
        PreviewOverlayInfo info;
        info.kind = PreviewOverlayKind::TranscriptOverlay;
        info.bounds = bounds;
        const PreviewResizeHandles handles = PreviewViewTransform::resizeHandlesForBounds(bounds);
        info.rightHandle = handles.right;
        info.bottomHandle = handles.bottom;
        info.cornerHandle = handles.corner;
        m_overlayModel.overlays.insert(clip.id, info);
        m_overlayModel.paintOrder.push_back(clip.id);
    }
}
