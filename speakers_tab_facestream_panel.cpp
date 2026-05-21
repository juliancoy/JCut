#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "decoder_context.h"
#include "facestream_artifact_utils.h"
#include "facestream_runtime.h"
#include "facestream_time_mapping.h"
#include "transcript_engine.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

QPixmap SpeakersTab::faceStreamPreviewAvatar(const TimelineClip& clip,
                                             const QString& speakerId,
                                             const QJsonObject& keyframeObj,
                                             int size) const
{
    return faceStreamPreviewAvatarWithDecoder(
        clip, speakerId, keyframeObj, size, nullptr, nullptr, resolvedSourceFps(clip));
}

QPixmap SpeakersTab::faceStreamPreviewAvatarWithDecoder(const TimelineClip& clip,
                                                        const QString& speakerId,
                                                        const QJsonObject& keyframeObj,
                                                        int size,
                                                        editor::DecoderContext* decoderCtx,
                                                        QHash<int64_t, QImage>* frameImageCache,
                                                        qreal sourceFps) const
{
    if (keyframeObj.isEmpty()) {
        return placeholderSpeakerAvatar(speakerId);
    }

    const int avatarSize = qMax(24, size);
    const int64_t sourceFrame30 = qMax<int64_t>(
        0, keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong());
    const qreal locX = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("x")).toDouble(0.5), 1.0);
    const qreal locY = qBound<qreal>(0.0, keyframeObj.value(QStringLiteral("y")).toDouble(0.5), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0,
        keyframeObj.value(QStringLiteral("box_size")).toDouble(
            keyframeObj.value(QStringLiteral("box")).toDouble(-1.0)),
        1.0);
    const qreal boxLeft = keyframeObj.value(QStringLiteral("box_left")).toDouble(-1.0);
    const qreal boxTop = keyframeObj.value(QStringLiteral("box_top")).toDouble(-1.0);
    const qreal boxRight = keyframeObj.value(QStringLiteral("box_right")).toDouble(-1.0);
    const qreal boxBottom = keyframeObj.value(QStringLiteral("box_bottom")).toDouble(-1.0);
    const QString cacheKey = QStringLiteral("facestream|%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(m_transcriptSession.transcriptPath())
        .arg(clip.id)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxSizeNorm) * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxLeft) * 1000.0)))
        .arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal effectiveSourceFps = sourceFps > 0.0 ? sourceFps : resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * effectiveSourceFps)));
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    QImage image;
    if (frameImageCache) {
        const auto cachedFrame = frameImageCache->constFind(decodeFrame);
        if (cachedFrame != frameImageCache->cend()) {
            image = cachedFrame.value();
        }
    }
    if (image.isNull()) {
        std::unique_ptr<editor::DecoderContext> localDecoder;
        editor::DecoderContext* activeDecoder = decoderCtx;
        if (!activeDecoder) {
            const QString mediaPath = interactivePreviewMediaPathForClip(clip);
            localDecoder = std::make_unique<editor::DecoderContext>(mediaPath);
            if (!localDecoder->initialize()) {
                activeDecoder = nullptr;
            } else {
                activeDecoder = localDecoder.get();
            }
        }
        if (activeDecoder) {
            const editor::FrameHandle frame = activeDecoder->decodeFrame(decodeFrame);
            image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
            if (frameImageCache && !image.isNull()) {
                frameImageCache->insert(decodeFrame, image);
            }
        }
    }
    if (!image.isNull() && image.width() > 0 && image.height() > 0) {
        const int width = image.width();
        const int height = image.height();
        const int minSide = qMax(1, qMin(width, height));
        QRect cropRect;
        if (boxLeft >= 0.0 && boxTop >= 0.0 && boxRight > boxLeft && boxBottom > boxTop &&
            boxRight <= 1.0 && boxBottom <= 1.0) {
            const int left = qBound(0, static_cast<int>(std::floor(boxLeft * width)), qMax(0, width - 1));
            const int top = qBound(0, static_cast<int>(std::floor(boxTop * height)), qMax(0, height - 1));
            const int right = qBound(left + 1, static_cast<int>(std::ceil(boxRight * width)), width);
            const int bottom = qBound(top + 1, static_cast<int>(std::ceil(boxBottom * height)), height);
            cropRect = QRect(left, top, right - left, bottom - top);
        }
        if (!cropRect.isValid() || cropRect.isEmpty()) {
            int side = qMax(40, minSide / 3);
            if (boxSizeNorm > 0.0) {
                side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
            }
            const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
            const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
            const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
            const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
            cropRect = QRect(left, top, qMin(side, width - left), qMin(side, height - top));
        }
        QImage crop = image.copy(cropRect)
                          .scaled(avatarSize, avatarSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!crop.isNull()) {
            QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
            rounded.fill(Qt::transparent);
            QPainter painter(&rounded);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            path.addRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
            painter.setClipPath(path);
            painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
            painter.setClipping(false);
            painter.setPen(QPen(QColor(QStringLiteral("#f4d35e")), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0), 8.0, 8.0);
            avatar = QPixmap::fromImage(rounded);
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QVector<QPixmap> SpeakersTab::assignedFaceStreamPreviewPixmaps(const TimelineClip& clip,
                                                               const QString& speakerId) const
{
    QVector<QPixmap> pixmaps;
    if (!m_transcriptSession.hasObjectDocument() || speakerId.isEmpty()) {
        return pixmaps;
    }

    const QJsonObject profiles =
        m_transcriptSession.rootObject().value(QString(kTranscriptSpeakerProfilesKey)).toObject();
    const QJsonArray faceRefs = speakerFaceRefs(profiles.value(speakerId).toObject());
    for (const QJsonValue& faceRefValue : faceRefs) {
        const QJsonObject previewKeyframe = previewKeyframeFromSpeakerFaceRef(faceRefValue.toObject());
        if (!previewKeyframe.isEmpty()) {
            pixmaps.push_back(faceStreamPreviewAvatar(clip, speakerId, previewKeyframe));
        }
    }
    if (!pixmaps.isEmpty()) {
        return pixmaps;
    }

    const QJsonArray streams = continuityStreamsForClip(clip);
    const QVector<int> assignedTrackIdList =
        resolvedAssignedTrackIdsForSpeaker(clip, streams, speakerId);
    QSet<int> assignedTrackIds;
    for (int trackId : assignedTrackIdList) {
        assignedTrackIds.insert(trackId);
    }
    if (assignedTrackIds.isEmpty()) {
        return pixmaps;
    }
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        if (!assignedTrackIds.contains(trackId)) {
            continue;
        }
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (keyframes.isEmpty()) {
            continue;
        }
        const QJsonObject keyframeObj = keyframes.first().toObject();
        if (keyframeObj.isEmpty()) {
            continue;
        }
        pixmaps.push_back(faceStreamPreviewAvatar(clip, speakerId, keyframeObj));
    }
    return pixmaps;
}

QJsonArray SpeakersTab::continuityStreamsForClip(const TimelineClip& clip) const
{
    const QString cacheKey = m_transcriptSession.transcriptPath() + QLatin1Char('\n') + clip.id.trimmed();
    const auto cached = m_continuityStreamsCache.constFind(cacheKey);
    if (cached != m_continuityStreamsCache.cend()) {
        return cached.value();
    }

    QJsonArray streams;
    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    if (transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
        const QJsonObject continuityRoot = continuityRootForClip(artifactRoot, clip.id);
        streams = jcut::facestream::continuityStreamsForRoot(
            continuityRoot,
            m_transcriptSession.rootObject());
        if (!streams.isEmpty()) {
            m_continuityStreamsCache.insert(cacheKey, streams);
            return streams;
        }
    }
    const QJsonObject root = m_transcriptSession.rootObject();
    const QJsonObject speakerFlow = root.value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject continuityRoot = clipFlow.value(QStringLiteral("continuity_facestreams")).toObject();
    streams = continuityRoot.value(QStringLiteral("streams")).toArray();
    m_continuityStreamsCache.insert(cacheKey, streams);
    return streams;
}

QJsonObject SpeakersTab::resolveFaceStreamAssignmentRow(const TimelineClip& clip,
                                                        const QJsonArray& streams,
                                                        const QJsonObject& row) const
{
    const QString identityId = row.value(QStringLiteral("identity_id")).toString().trimmed();
    if (identityId.isEmpty() || streams.isEmpty()) {
        return {};
    }

    const int storedTrackId = row.value(QStringLiteral("track_id")).toInt(-1);
    const QString storedStreamId = row.value(QStringLiteral("stream_id")).toString().trimmed();
    const bool hasAnchor =
        row.contains(QString(kSpeakerFlowAnchorSourceFrameKey)) &&
        row.contains(QString(kSpeakerFlowAnchorXKey)) &&
        row.contains(QString(kSpeakerFlowAnchorYKey)) &&
        row.contains(QString(kSpeakerFlowAnchorBoxSizeKey));
    const int64_t anchorSourceFrame =
        row.value(QString(kSpeakerFlowAnchorSourceFrameKey)).toVariant().toLongLong();
    const qreal anchorX = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorXKey)).toDouble(0.5), 1.0);
    const qreal anchorY = qBound<qreal>(0.0, row.value(QString(kSpeakerFlowAnchorYKey)).toDouble(0.5), 1.0);
    const qreal anchorBox = qBound<qreal>(0.01, row.value(QString(kSpeakerFlowAnchorBoxSizeKey)).toDouble(0.2), 1.0);
    const QVector<RenderSyncMarker> renderSyncMarkers =
        m_speakerDeps.getRenderSyncMarkers ? m_speakerDeps.getRenderSyncMarkers() : QVector<RenderSyncMarker>{};

    QJsonObject bestResolved;
    double bestScore = std::numeric_limits<double>::max();
    for (const QJsonValue& streamValue : streams) {
        const QJsonObject streamObj = streamValue.toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        if (trackId < 0 || keyframes.isEmpty()) {
            continue;
        }

        if (!hasAnchor) {
            if ((storedTrackId >= 0 && trackId == storedTrackId) ||
                (!storedStreamId.isEmpty() && streamId == storedStreamId)) {
                QJsonObject resolved = row;
                resolved[QStringLiteral("track_id")] = trackId;
                resolved[QStringLiteral("stream_id")] = streamId;
                return resolved;
            }
            continue;
        }

        FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
        if (!parseFacestreamFrameDomainString(
                streamObj.value(QStringLiteral("frame_domain")).toString(),
                &frameDomain)) {
            continue;
        }

        for (const QJsonValue& keyframeValue : keyframes) {
            const QJsonObject keyframeObj = keyframeValue.toObject();
            const int64_t frame =
                keyframeObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            const int64_t sourceFrame =
                mapFacestreamFrameToSourceFrame(clip, frame, frameDomain, renderSyncMarkers);
            const qreal x =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
            const qreal y =
                qBound<qreal>(0.0, keyframeObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.5), 1.0);
            const qreal box =
                qBound<qreal>(0.01, keyframeObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(0.2), 1.0);
            const qreal posDist = std::hypot(x - anchorX, y - anchorY);
            const qreal boxDist = std::abs(box - anchorBox);
            const qreal frameDist =
                static_cast<qreal>(std::llabs(sourceFrame - anchorSourceFrame));
            const double score = (frameDist / 12.0) + (posDist * 6.0) + (boxDist * 3.0);
            if (score < bestScore) {
                bestScore = score;
                bestResolved = row;
                bestResolved[QStringLiteral("track_id")] = trackId;
                bestResolved[QStringLiteral("stream_id")] = streamId;
            }
        }
    }

    if (!bestResolved.isEmpty()) {
        return bestResolved;
    }

    if (storedTrackId >= 0 || !storedStreamId.isEmpty()) {
        QJsonObject fallback = row;
        return fallback;
    }
    return {};
}

QHash<int, QString> SpeakersTab::resolvedIdentityByTrackId(const TimelineClip& clip,
                                                           const QJsonArray& streams) const
{
    QHash<int, QString> identityByTrackId;
    const QJsonObject speakerFlow = m_transcriptSession.rootObject().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipFlow.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject resolved = resolveFaceStreamAssignmentRow(clip, streams, value.toObject());
        const int trackId = resolved.value(QStringLiteral("track_id")).toInt(-1);
        const QString identityId = resolved.value(QStringLiteral("identity_id")).toString().trimmed();
        if (trackId >= 0 && !identityId.isEmpty()) {
            identityByTrackId.insert(trackId, identityId);
        }
    }
    return identityByTrackId;
}

QVector<int> SpeakersTab::resolvedAssignedTrackIdsForSpeaker(const TimelineClip& clip,
                                                             const QJsonArray& streams,
                                                             const QString& speakerId) const
{
    QVector<int> trackIds;
    const QJsonObject speakerFlow = m_transcriptSession.rootObject().value(QStringLiteral("speaker_flow")).toObject();
    const QJsonObject clipsRoot = speakerFlow.value(QStringLiteral("clips")).toObject();
    const QJsonObject clipFlow = clipsRoot.value(clip.id.trimmed()).toObject();
    const QJsonObject resolvedCurrent = clipFlow.value(QStringLiteral("resolved_current")).toObject();
    const QJsonArray resolvedMap = resolvedCurrent.value(QStringLiteral("track_identity_map")).toArray();
    for (const QJsonValue& value : resolvedMap) {
        const QJsonObject resolved = resolveFaceStreamAssignmentRow(clip, streams, value.toObject());
        if (resolved.value(QStringLiteral("identity_id")).toString().trimmed() != speakerId) {
            continue;
        }
        const int trackId = resolved.value(QStringLiteral("track_id")).toInt(-1);
        if (trackId >= 0 && !trackIds.contains(trackId)) {
            trackIds.push_back(trackId);
        }
    }
    return trackIds;
}

QString SpeakersTab::assignedFaceStreamPreviewTooltipHtml(const TimelineClip& clip,
                                                          const QString& speakerId) const
{
    const QFileInfo transcriptInfo(m_transcriptSession.transcriptPath());
    const qint64 artifactRevisionMs = facestreamArtifactRevisionMsForTranscript(m_transcriptSession.transcriptPath());
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4")
        .arg(clip.id)
        .arg(speakerId)
        .arg(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0)
        .arg(artifactRevisionMs);
    const auto cached = m_avatarHoverTooltipHtmlCache.constFind(cacheKey);
    if (cached != m_avatarHoverTooltipHtmlCache.cend()) {
        return cached.value();
    }

    const QVector<QPixmap> previews = assignedFaceStreamPreviewPixmaps(clip, speakerId);
    if (previews.isEmpty()) {
        return QString();
    }

    QString html = QStringLiteral("<div style='white-space:nowrap;'>");
    int count = 0;
    for (const QPixmap& pixmap : previews) {
        if (pixmap.isNull()) {
            continue;
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        pixmap.save(&buffer, "PNG");
        html += QStringLiteral("<img width='72' height='72' style='margin:2px;border:1px solid #f4d35e;border-radius:6px;' src='data:image/png;base64,%1' />")
                    .arg(QString::fromLatin1(bytes.toBase64()));
        ++count;
        if (count >= 12) {
            break;
        }
    }
    html += QStringLiteral("</div>");
    if (count == 0) {
        return QString();
    }
    m_avatarHoverTooltipHtmlCache.insert(cacheKey, html);
    return html;
}

void SpeakersTab::showSpeakerAvatarHoverPreview(const QString& speakerId, const QPoint& globalPos)
{
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip || speakerId.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    const QString html = assignedFaceStreamPreviewTooltipHtml(*clip, speakerId);
    if (html.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    QToolTip::showText(globalPos, html, m_widgets.speakersTable);
}

void SpeakersTab::hideSpeakerAvatarHoverPreview()
{
    QToolTip::hideText();
}

void SpeakersTab::requestRefreshFaceStreamPathsPanel()
{
    if (!m_faceStreamPanelRefreshTimer) {
        refreshFaceStreamPathsPanel();
        return;
    }
    m_faceStreamPanelRefreshQueued = true;
    m_faceStreamPanelRefreshTimer->start();
}

void SpeakersTab::refreshFaceStreamPathsPanel()
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    if (!m_widgets.speakerFaceStreamTable || m_refreshingFaceStreamPathsPanel) {
        m_lastFaceStreamPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxFaceStreamPanelRefreshDurationMs =
            qMax(m_maxFaceStreamPanelRefreshDurationMs, m_lastFaceStreamPanelRefreshDurationMs);
        return;
    }
    m_refreshingFaceStreamPathsPanel = true;
    struct RefreshGuard {
        bool& flag;
        qint64* lastDurationMs = nullptr;
        qint64* maxDurationMs = nullptr;
        QElapsedTimer* timer = nullptr;
        ~RefreshGuard() {
            flag = false;
            if (lastDurationMs && maxDurationMs && timer) {
                *lastDurationMs = timer->elapsed();
                *maxDurationMs = qMax(*maxDurationMs, *lastDurationMs);
            }
        }
    } guard{m_refreshingFaceStreamPathsPanel,
            &m_lastFaceStreamPanelRefreshDurationMs,
            &m_maxFaceStreamPanelRefreshDurationMs,
            &refreshTimer};

    QSignalBlocker tableBlocker(m_widgets.speakerFaceStreamTable);
    QSignalBlocker selectionBlocker(
        m_widgets.speakerFaceStreamTable->selectionModel());
    m_widgets.speakerFaceStreamTable->clearContents();
    m_widgets.speakerFaceStreamTable->setRowCount(0);
    if (m_widgets.speakerFaceStreamTable->columnCount() >= 5) {
        m_widgets.speakerFaceStreamTable->setHorizontalHeaderLabels(
            QStringList{
                QStringLiteral("Stream"),
                QStringLiteral("Track"),
                QStringLiteral("Assignment"),
                QStringLiteral("Range"),
                QStringLiteral("Source")
            });
    }
    m_faceStreamPanelRows = QJsonArray();
    if (m_widgets.speakerFaceStreamDetailsEdit) {
        m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
            QStringLiteral("Select a FaceStream path row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }
    if (m_widgets.speakerRawDetectionTable) {
        QSignalBlocker rawTableBlocker(m_widgets.speakerRawDetectionTable);
        m_widgets.speakerRawDetectionTable->clearContents();
        m_widgets.speakerRawDetectionTable->setRowCount(0);
    }
    if (!m_transcriptSession.hasObjectDocument()) {
        return;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return;
    }
    const QFileInfo transcriptInfo(m_transcriptSession.transcriptPath());
    const qint64 artifactRevisionMs = facestreamArtifactRevisionMsForTranscript(m_transcriptSession.transcriptPath());
    const QString refreshSignature =
        clip->id + QLatin1Char('|') +
        m_transcriptSession.transcriptPath() + QLatin1Char('|') +
        QString::number(transcriptInfo.exists() ? transcriptInfo.lastModified().toMSecsSinceEpoch() : 0) +
        QLatin1Char('|') +
        QString::number(artifactRevisionMs);
    if (refreshSignature == m_faceStreamPanelRefreshSignature) {
        return;
    }

    editor::TranscriptEngine transcriptEngine;
    QJsonObject artifactRoot;
    QJsonObject continuityRoot;
    if (transcriptEngine.loadFacestreamArtifact(m_transcriptSession.transcriptPath(), &artifactRoot)) {
        continuityRoot = continuityRootForClip(artifactRoot, clip->id);
    }
    if (m_widgets.speakerDetectionsAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerDetectionsAvailableCheckBox);
        m_widgets.speakerDetectionsAvailableCheckBox->setChecked(
            !continuityRoot.value(QStringLiteral("raw_frames")).toArray().isEmpty());
    }
    if (m_widgets.speakerTracksAvailableCheckBox) {
        QSignalBlocker block(m_widgets.speakerTracksAvailableCheckBox);
        m_widgets.speakerTracksAvailableCheckBox->setChecked(
            !continuityRoot.value(QStringLiteral("raw_tracks")).toArray().isEmpty());
    }

    const QJsonArray streams = continuityStreamsForClip(*clip);
    m_faceStreamPanelRefreshSignature = refreshSignature;
    const QHash<int, QString> identityByTrackId = resolvedIdentityByTrackId(*clip, streams);

    struct StreamRow {
        QJsonObject streamObj;
        int trackId = -1;
        QString identityId;
        int64_t minFrame = -1;
        int64_t maxFrame = -1;
        QString sourceTag;
        int keyframeCount = 0;
    };
    QVector<StreamRow> panelRows;
    panelRows.reserve(streams.size());
    for (int row = 0; row < streams.size(); ++row) {
        const QJsonObject streamObj = streams.at(row).toObject();
        const int trackId = streamObj.value(QStringLiteral("track_id")).toInt(-1);
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        int64_t minFrame = std::numeric_limits<int64_t>::max();
        int64_t maxFrame = std::numeric_limits<int64_t>::min();
        QString sourceTag;
        for (const QJsonValue& value : keyframes) {
            const QJsonObject keyframe = value.toObject();
            const int64_t frame = keyframe.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong();
            minFrame = qMin(minFrame, frame);
            maxFrame = qMax(maxFrame, frame);
            if (sourceTag.isEmpty()) {
                sourceTag = keyframe.value(QString(kTranscriptSpeakerTrackingSourceKey)).toString().trimmed();
            }
        }
        StreamRow panelRow;
        panelRow.streamObj = streamObj;
        panelRow.trackId = trackId;
        panelRow.identityId = identityByTrackId.value(trackId).trimmed();
        panelRow.minFrame = keyframes.isEmpty() ? -1 : minFrame;
        panelRow.maxFrame = keyframes.isEmpty() ? -1 : maxFrame;
        panelRow.sourceTag = sourceTag;
        panelRow.keyframeCount = keyframes.size();
        panelRows.push_back(panelRow);
    }
    std::sort(panelRows.begin(), panelRows.end(), [](const StreamRow& a, const StreamRow& b) {
        const bool aAssigned = !a.identityId.isEmpty();
        const bool bAssigned = !b.identityId.isEmpty();
        if (aAssigned != bAssigned) {
            return aAssigned && !bAssigned;
        }
        if (aAssigned && bAssigned && a.identityId != b.identityId) {
            return a.identityId.localeAwareCompare(b.identityId) < 0;
        }
        return a.trackId < b.trackId;
    });

    int assignedCount = 0;
    int unassignedCount = 0;
    for (const StreamRow& row : std::as_const(panelRows)) {
        if (row.identityId.isEmpty()) {
            ++unassignedCount;
        } else {
            ++assignedCount;
        }
        m_faceStreamPanelRows.push_back(row.streamObj);
    }

    m_widgets.speakerFaceStreamTable->setRowCount(panelRows.size());
    for (int row = 0; row < panelRows.size(); ++row) {
        const StreamRow& panelRow = panelRows.at(row);
        const QJsonObject streamObj = panelRow.streamObj;
        const QString streamId = streamObj.value(QStringLiteral("stream_id")).toString().trimmed();
        const QJsonArray keyframes = streamObj.value(QStringLiteral("keyframes")).toArray();
        const QString rangeText = keyframes.isEmpty()
            ? QStringLiteral("-")
            : QStringLiteral("%1..%2").arg(panelRow.minFrame).arg(panelRow.maxFrame);
        auto* streamItem = new QTableWidgetItem(streamId.isEmpty() ? QStringLiteral("—") : streamId);
        streamItem->setData(Qt::UserRole + 1, row);
        const qlonglong seekFrame = keyframes.isEmpty()
            ? static_cast<qlonglong>(-1)
            : static_cast<qlonglong>(panelRow.minFrame);
        streamItem->setData(Qt::UserRole + 2, QVariant(seekFrame));
        auto* trackItem = new QTableWidgetItem(panelRow.trackId >= 0 ? QString::number(panelRow.trackId) : QStringLiteral("—"));
        const bool assigned = !panelRow.identityId.isEmpty();
        auto* countItem = new QTableWidgetItem(
            assigned ? panelRow.identityId : QStringLiteral("Unassigned"));
        auto* rangeItem = new QTableWidgetItem(rangeText);
        auto* sourceItem = new QTableWidgetItem(
            panelRow.sourceTag.isEmpty() ? QStringLiteral("continuity_track_v1") : panelRow.sourceTag);
        countItem->setToolTip(
            assigned
                ? QStringLiteral("Assigned to speaker identity: %1").arg(panelRow.identityId)
                : QStringLiteral("No speaker identity assignment yet."));
        rangeItem->setToolTip(QStringLiteral("Keyframes: %1").arg(panelRow.keyframeCount));
        if (!assigned) {
            const QColor bg(QStringLiteral("#3a2a2a"));
            streamItem->setBackground(bg);
            trackItem->setBackground(bg);
            countItem->setBackground(bg);
            rangeItem->setBackground(bg);
            sourceItem->setBackground(bg);
        }
        m_widgets.speakerFaceStreamTable->setItem(row, 0, streamItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 1, trackItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 2, countItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 3, rangeItem);
        m_widgets.speakerFaceStreamTable->setItem(row, 4, sourceItem);
    }
    if (streams.isEmpty() && m_widgets.speakerFaceStreamDetailsEdit) {
        m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
            QStringLiteral("No FaceStream paths found for this clip. Run JCut DNN FaceStream Generator first."));
    } else if (!streams.isEmpty()) {
        if (m_widgets.speakerFaceStreamDetailsEdit) {
            m_widgets.speakerFaceStreamDetailsEdit->setPlainText(
                QStringLiteral("Track assignment summary\n\nAssigned: %1\nUnassigned: %2\nTotal: %3\n\nSelect a row to inspect full JSON.")
                    .arg(assignedCount)
                    .arg(unassignedCount)
                    .arg(panelRows.size()));
        }
        m_widgets.speakerFaceStreamTable->setCurrentCell(0, 0);
    }

    refreshRawDetectionsPanel(continuityRoot);
}

void SpeakersTab::refreshRawDetectionsPanel(const QJsonObject& continuityRoot)
{
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    auto finalizeRefreshTiming = [this, &refreshTimer]() {
        m_lastRawDetectionsPanelRefreshDurationMs = refreshTimer.elapsed();
        m_maxRawDetectionsPanelRefreshDurationMs =
            qMax(m_maxRawDetectionsPanelRefreshDurationMs, m_lastRawDetectionsPanelRefreshDurationMs);
    };
    if (!m_widgets.speakerRawDetectionTable) {
        finalizeRefreshTiming();
        return;
    }

    QSignalBlocker tableBlocker(m_widgets.speakerRawDetectionTable);
    m_widgets.speakerRawDetectionTable->clearContents();
    m_widgets.speakerRawDetectionTable->setRowCount(0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Select a detection row to inspect full JSON."));
    }

    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        finalizeRefreshTiming();
        return;
    }

    const QJsonArray rawFrames = continuityRoot.value(QStringLiteral("raw_frames")).toArray();
    if (rawFrames.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections were imported for this clip."));
        }
        finalizeRefreshTiming();
        return;
    }

    FacestreamFrameDomain frameDomain = FacestreamFrameDomain::SourceRelative;
    if (!continuityPayloadFrameDomain(
            continuityRoot,
            QStringLiteral("raw_frames_frame_domain"),
            &frameDomain)) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("Raw detections are present, but frame-domain metadata is missing. "
                               "This artifact does not satisfy the current contract."));
        }
        finalizeRefreshTiming();
        return;
    }
    const int64_t timelineFrame =
        m_deps.getCurrentTimelineFrame ? m_deps.getCurrentTimelineFrame() : clip->startFrame;
    const QVector<RenderSyncMarker> markers =
        m_speakerDeps.getRenderSyncMarkers
            ? m_speakerDeps.getRenderSyncMarkers()
            : QVector<RenderSyncMarker>{};
    const int64_t absoluteSourceFrame =
        sourceFrameForClipAtTimelinePosition(*clip, static_cast<qreal>(timelineFrame), markers);
    const int64_t localTimelineFrame = qMax<int64_t>(0, timelineFrame - clip->startFrame);
    const int64_t localSourceFrame =
        qMax<int64_t>(0, absoluteSourceFrame - qMax<int64_t>(0, clip->sourceInFrame));
    const int64_t lookupFrame = facestreamLookupFrameForDomain(
        frameDomain, localTimelineFrame, localSourceFrame, absoluteSourceFrame);
    const QString frameLabel =
        frameDomain == FacestreamFrameDomain::ClipTimeline30Fps
            ? QStringLiteral("clip frame")
            : (frameDomain == FacestreamFrameDomain::SourceRelative
                   ? QStringLiteral("local source frame")
                   : QStringLiteral("source frame"));

    QJsonArray detectionsForFrame;
    for (const QJsonValue& frameValue : continuityRoot.value(QStringLiteral("raw_frames")).toArray()) {
        const QJsonObject frameObj = frameValue.toObject();
        const int64_t frameNumber =
            frameObj.value(QStringLiteral("frame")).toVariant().toLongLong();
        if (frameNumber != lookupFrame) {
            continue;
        }
        detectionsForFrame = frameObj.value(QStringLiteral("detections")).toArray();
        break;
    }

    if (detectionsForFrame.isEmpty()) {
        if (m_widgets.speakerRawDetectionDetailsEdit) {
            m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
                QStringLiteral("No raw detections at %1 %2.")
                    .arg(frameLabel)
                    .arg(lookupFrame));
        }
        finalizeRefreshTiming();
        return;
    }

    m_widgets.speakerRawDetectionTable->setRowCount(detectionsForFrame.size());
    for (int row = 0; row < detectionsForFrame.size(); ++row) {
        const QJsonObject detectionObj = detectionsForFrame.at(row).toObject();
        auto* indexItem = new QTableWidgetItem(QString::number(row + 1));
        indexItem->setData(
            Qt::UserRole + 1,
            QString::fromUtf8(QJsonDocument(detectionObj).toJson(QJsonDocument::Indented)));
        auto* confItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("confidence")).toDouble(
                                detectionObj.value(QStringLiteral("score")).toDouble(0.0)),
                            'f',
                            3));
        auto* xItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("x")).toDouble(0.0), 'f', 3));
        auto* yItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("y")).toDouble(0.0), 'f', 3));
        auto* wItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("w")).toDouble(0.0), 'f', 3));
        auto* hItem = new QTableWidgetItem(
            QString::number(detectionObj.value(QStringLiteral("h")).toDouble(0.0), 'f', 3));
        m_widgets.speakerRawDetectionTable->setItem(row, 0, indexItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 1, confItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 2, xItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 3, yItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 4, wItem);
        m_widgets.speakerRawDetectionTable->setItem(row, 5, hItem);
    }
    m_widgets.speakerRawDetectionTable->setCurrentCell(0, 0);
    if (m_widgets.speakerRawDetectionDetailsEdit) {
        m_widgets.speakerRawDetectionDetailsEdit->setPlainText(
            QStringLiteral("Raw detections at %1 %2: %3\n\nSelect a row to inspect full JSON.")
                .arg(frameLabel)
                .arg(lookupFrame)
                .arg(detectionsForFrame.size()));
    }
    finalizeRefreshTiming();
}
