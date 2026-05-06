#include "opengl_preview.h"
#include "gl_frame_texture_shared.h"
#include "transcript_engine.h"

#include <cmath>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace {
bool clipSupportsTranscript(const TimelineClip& clip) {
    return clip.mediaType == ClipMediaType::Audio || clip.hasAudio;
}

QString activeSpeakerAtSourceFrame(const QVector<TranscriptSection>& sections, int64_t sourceFrame) {
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
        invalidateTranscriptSpeakerProfileCache();
        for (auto it = m_transcriptTextureCache.begin(); it != m_transcriptTextureCache.end(); ++it) {
            editor::destroyGlTextureEntry(&it.value());
        }
        m_transcriptTextureCache.clear();
    } else {
        m_transcriptSectionsCache.remove(clipFilePath);
        m_speakerTrackPointsCache.remove(activeTranscriptPathForClipFile(clipFilePath));
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
    const QString key = clip.filePath;
    auto it = m_transcriptSectionsCache.find(key);
    if (it == m_transcriptSectionsCache.end()) {
        it = m_transcriptSectionsCache.insert(key, loadTranscriptSections(activeTranscriptPathForClipFile(clip.filePath)));
    }
    return it.value();
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
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    auto it = m_speakerTrackPointsCache.find(transcriptPath);
    if (it != m_speakerTrackPointsCache.end() && it->mtimeMs == mtimeMs) {
        return it->points;
    }

    SpeakerTrackPointCacheEntry entry;
    entry.mtimeMs = mtimeMs;
    QFile file(transcriptPath);
    if (!file.open(QIODevice::ReadOnly)) {
        it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
        return it->points;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
        return it->points;
    }

    const QJsonObject transcriptRoot = doc.object();
    const QJsonObject profiles = transcriptRoot.value(QStringLiteral("speaker_profiles")).toObject();
    for (auto profileIt = profiles.constBegin(); profileIt != profiles.constEnd(); ++profileIt) {
        const QString speakerId = profileIt.key().trimmed();
        if (speakerId.isEmpty()) {
            continue;
        }
        const QJsonObject profile = profileIt.value().toObject();
        QJsonObject tracking = profile.value(QStringLiteral("framing")).toObject();
        if (tracking.isEmpty()) {
            tracking = profile.value(QStringLiteral("tracking")).toObject();
        }
        const QJsonArray keyframes = tracking.value(QStringLiteral("keyframes")).toArray();
        for (const QJsonValue& value : keyframes) {
            const QJsonObject obj = value.toObject();
            if (obj.isEmpty()) {
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
            p.x = rawX;
            p.y = rawY;
            const qreal boxSize = obj.value(QStringLiteral("box_size")).toDouble(-1.0);
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
            } else {
                if (p.boxSizeNorm > 0.0) {
                    const qreal side = p.boxSizeNorm;
                    const qreal half = side * 0.5;
                    p.hasBox = true;
                    p.boxLeft = qBound<qreal>(0.0, rawX - half, 1.0);
                    p.boxTop = qBound<qreal>(0.0, rawY - half, 1.0);
                    p.boxRight = qBound<qreal>(0.0, rawX + half, 1.0);
                    p.boxBottom = qBound<qreal>(0.0, rawY + half, 1.0);
                }
            }
            p.speakerId = speakerId;
            entry.points.push_back(p);
        }
    }

    // Add identity-agnostic continuity FaceStream tracks so post users can compare detector outputs.
    editor::TranscriptEngine engine;
    QJsonObject artifactRoot;
    if (engine.loadBoxstreamArtifact(transcriptPath, &artifactRoot)) {
        const QJsonObject continuityByClip =
            artifactRoot.value(QStringLiteral("continuity_boxstreams_by_clip")).toObject();
        const QJsonObject continuityRoot = continuityByClip.value(clip.id).toObject();
        const QJsonArray streams = continuityRoot.value(QStringLiteral("streams")).toArray();
        const QString sourceFilter = m_boxstreamOverlaySource.trimmed().toLower();
        for (const QJsonValue& streamValue : streams) {
            const QJsonObject streamObj = streamValue.toObject();
            const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
            const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
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
                p.x = rawX;
                p.y = rawY;
                p.speakerId = streamId.isEmpty() ? QStringLiteral("track") : streamId;
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
    it = m_speakerTrackPointsCache.insert(transcriptPath, entry);
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
                .arg(prettyTrackerLabel(m_boxstreamOverlaySource));
            const QRectF legendRect(12.0, 12.0, 300.0, 24.0);
            const bool dockerSource = m_boxstreamOverlaySource.trimmed().toLower().startsWith(QStringLiteral("docker_"));
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
        const int64_t sourceEnd = sourceStart + qMax<int64_t>(0, clip.durationFrames - 1);
        const int64_t currentSourceFrame =
            transcriptFrameForClipAtTimelineSample(clip, m_interaction.currentSample, m_interaction.renderSyncMarkers);
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
            if (p.frame < sourceStart || p.frame > sourceEnd) {
                continue;
            }
            const uint hueHash = qHash(p.speakerId);
            const QColor color = QColor::fromHsv(static_cast<int>(hueHash % 360), 180, 245, 190);
            const QPointF screenPoint = mapNormalizedClipPointToScreen(info, QPointF(p.x, p.y));
            const int64_t distance = std::llabs(p.frame - currentSourceFrame);
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
            if (!m_showSpeakerTrackBoxes || !candidate.point.hasBox) {
                continue;
            }
            qreal leftNorm = candidate.point.boxLeft;
            qreal topNorm = candidate.point.boxTop;
            qreal rightNorm = candidate.point.boxRight;
            qreal bottomNorm = candidate.point.boxBottom;
            if (candidate.point.boxSizeNorm > 0.0 &&
                info.clipPixelSize.width() > 1.0 &&
                info.clipPixelSize.height() > 1.0) {
                const qreal minSidePx = qMin(info.clipPixelSize.width(), info.clipPixelSize.height());
                const qreal sidePx = candidate.point.boxSizeNorm * minSidePx;
                const qreal halfXNorm = 0.5 * (sidePx / info.clipPixelSize.width());
                const qreal halfYNorm = 0.5 * (sidePx / info.clipPixelSize.height());
                leftNorm = qBound<qreal>(0.0, candidate.point.x - halfXNorm, 1.0);
                topNorm = qBound<qreal>(0.0, candidate.point.y - halfYNorm, 1.0);
                rightNorm = qBound<qreal>(0.0, candidate.point.x + halfXNorm, 1.0);
                bottomNorm = qBound<qreal>(0.0, candidate.point.y + halfYNorm, 1.0);
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
            const QColor boxStroke(candidate.color.red(), candidate.color.green(), candidate.color.blue(), 235);
            const QColor boxFill(candidate.color.red(), candidate.color.green(), candidate.color.blue(), 42);
            painter->setPen(QPen(boxStroke, 2.0));
            painter->setBrush(boxFill);
            painter->drawPath(boxPath);
        }
    }

    painter->restore();
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
    const QImage image = renderTranscriptOverlayImage(
        clip,
        localBounds,
        localTextBounds,
        clip.transcriptOverlay.fontPointSize,
        shadowHtml,
        textHtml);
    painter->drawImage(bounds, image);
    painter->restore();

    if (m_interaction.transcriptOverlayInteractionEnabled) {
        PreviewOverlayInfo info;
        info.kind = PreviewOverlayKind::TranscriptOverlay;
        info.bounds = bounds;
        constexpr qreal kHandleSize = 12.0;
        info.rightHandle = QRectF(bounds.right() - kHandleSize, bounds.center().y() - kHandleSize, kHandleSize, kHandleSize * 2.0);
        info.bottomHandle = QRectF(bounds.center().x() - kHandleSize, bounds.bottom() - kHandleSize, kHandleSize * 2.0, kHandleSize);
        info.cornerHandle = QRectF(bounds.right() - kHandleSize * 1.5, bounds.bottom() - kHandleSize * 1.5, kHandleSize * 1.5, kHandleSize * 1.5);
        m_overlayModel.overlays.insert(clip.id, info);
        m_overlayModel.paintOrder.push_back(clip.id);
    }
}
