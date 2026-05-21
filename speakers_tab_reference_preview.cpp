#include "speakers_tab.h"
#include "speakers_tab_internal.h"

#include "decoder_context.h"

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

QPixmap SpeakersTab::placeholderSpeakerAvatar(const QString& speakerId) const
{
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#243447")));
    painter.drawEllipse(QRectF(1.0, 1.0, 30.0, 30.0));
    painter.setPen(QColor(QStringLiteral("#d8e6f5")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(10);
    painter.setFont(font);
    const QString fallback = speakerId.trimmed().isEmpty() ? QStringLiteral("?") : speakerId.left(1).toUpper();
    painter.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, fallback);
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::unsetSpeakerAvatar(int size) const
{
    const int iconSize = qMax(16, size);
    QImage image(iconSize, iconSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF border(1.0, 1.0, iconSize - 2.0, iconSize - 2.0);
    painter.setPen(QPen(QColor(QStringLiteral("#5a6d82")), 1.0, Qt::DashLine));
    painter.setBrush(QColor(QStringLiteral("#1a2533")));
    painter.drawRoundedRect(border, 4.0, 4.0);
    painter.setPen(QColor(QStringLiteral("#9fb3c8")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(qMax(7, iconSize / 4));
    painter.setFont(font);
    painter.drawText(QRectF(0.0, 0.0, iconSize, iconSize), Qt::AlignCenter, QStringLiteral("?"));
    return QPixmap::fromImage(image);
}

QPixmap SpeakersTab::speakerAvatarForRow(const TimelineClip& clip,
                                         const QJsonObject& transcriptRoot,
                                         const QJsonObject& profile,
                                         const QJsonArray& streams,
                                         const QString& speakerId,
                                         const QVector<int>& assignedTrackIds,
                                         editor::DecoderContext* decoderCtx,
                                         QHash<int64_t, QImage>* frameImageCache,
                                         qreal sourceFps)
{
    Q_UNUSED(transcriptRoot);
    const QJsonArray faceRefs = speakerFaceRefs(profile);
    for (int i = faceRefs.size() - 1; i >= 0; --i) {
        const QJsonObject previewKeyframe = previewKeyframeFromSpeakerFaceRef(faceRefs.at(i).toObject());
        if (!previewKeyframe.isEmpty()) {
            return faceStreamPreviewAvatarWithDecoder(
                clip, speakerId, previewKeyframe, 32, decoderCtx, frameImageCache, sourceFps);
        }
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
        return faceStreamPreviewAvatarWithDecoder(
            clip, speakerId, keyframes.first().toObject(), 32, decoderCtx, frameImageCache, sourceFps);
    }
    return unsetSpeakerAvatar(32);
}

QPixmap SpeakersTab::speakerReferenceAvatar(const TimelineClip& clip,
                                            const QString& speakerId,
                                            const QJsonObject& refObj,
                                            int size)
{
    if (refObj.isEmpty()) {
        Q_UNUSED(speakerId);
        return unsetSpeakerAvatar(size);
    }
    const int avatarSize = qMax(16, size);

    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    qreal locX = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    qreal locY = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);

    const QString cacheKey = QStringLiteral("ref|%1|%2|%3|%4|%5|%6")
        .arg(m_loadedTranscriptPath)
        .arg(speakerId)
        .arg(sourceFrame30)
        .arg(static_cast<int>(std::round(locX * 1000.0)))
        .arg(static_cast<int>(std::round(locY * 1000.0)))
        .arg(static_cast<int>(std::round(qMax<qreal>(0.0, boxSizeNorm) * 1000.0))) +
        QStringLiteral("|s=%1").arg(avatarSize);
    const auto cached = m_avatarCache.constFind(cacheKey);
    if (cached != m_avatarCache.cend()) {
        return cached.value();
    }

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));
    const QString mediaPath = interactivePreviewMediaPathForClip(clip);

    editor::DecoderContext ctx(mediaPath);
    QPixmap avatar = placeholderSpeakerAvatar(speakerId);
    if (ctx.initialize()) {
        const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
        const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
        if (!image.isNull() && image.width() > 0 && image.height() > 0) {
            const int width = image.width();
            const int height = image.height();
            const int minSide = qMin(width, height);
            int side = qMax(40, minSide / 3);
            if (boxSizeNorm > 0.0) {
                side = qBound(40, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
            }
            const int cx = static_cast<int>(std::round(locX * static_cast<qreal>(width)));
            const int cy = static_cast<int>(std::round(locY * static_cast<qreal>(height)));
            const int left = qBound(0, cx - (side / 2), qMax(0, width - side));
            const int top = qBound(0, cy - (side / 2), qMax(0, height - side));
            QImage crop = image.copy(QRect(left, top, qMin(side, width), qMin(side, height)))
                              .scaled(avatarSize, avatarSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (!crop.isNull()) {
                QImage rounded(avatarSize, avatarSize, QImage::Format_ARGB32_Premultiplied);
                rounded.fill(Qt::transparent);
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing, true);
                QPainterPath path;
                path.addEllipse(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0);
                painter.setClipPath(path);
                painter.drawImage(QRect(0, 0, avatarSize, avatarSize), crop);
                painter.setClipping(false);
                painter.setPen(QPen(QColor(QStringLiteral("#8dbbe4")), 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QRectF(1.0, 1.0, avatarSize - 2.0, avatarSize - 2.0));
                avatar = QPixmap::fromImage(rounded);
            }
        }
    }

    m_avatarCache.insert(cacheKey, avatar);
    return avatar;
}

QPixmap SpeakersTab::referenceFullFramePreview(const TimelineClip& clip,
                                               const QString& speakerId,
                                               const QJsonObject& refObj,
                                               QSize targetSize)
{
    Q_UNUSED(speakerId);
    const int outputW = qMax(640, targetSize.width());
    const int outputH = qMax(360, targetSize.height());
    QImage canvas(outputW, outputH, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(QStringLiteral("#111820")));

    const int64_t sourceFrame30 =
        qMax<int64_t>(0, refObj.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
    const qreal locX = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
    const qreal locY = qBound<qreal>(0.0, refObj.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
    const qreal boxSizeNorm = qBound<qreal>(
        -1.0, refObj.value(QString(kTranscriptSpeakerTrackingBoxSizeKey)).toDouble(-1.0), 1.0);

    const qreal sourceFps = resolvedSourceFps(clip);
    const int64_t decodeFrame = qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<qreal>(sourceFrame30) / kTimelineFps) * sourceFps)));

    const QString mediaPath = interactivePreviewMediaPathForClip(clip);
    editor::DecoderContext ctx(mediaPath);
    if (!ctx.initialize()) {
        return QPixmap::fromImage(canvas);
    }

    const editor::FrameHandle frame = ctx.decodeFrame(decodeFrame);
    const QImage image = frame.hasCpuImage() ? frame.cpuImage() : QImage();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QPixmap::fromImage(canvas);
    }

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QImage display = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize scaled = display.size().scaled(canvas.size(), Qt::KeepAspectRatio);
    const QRect drawRect(
        (canvas.width() - scaled.width()) / 2,
        (canvas.height() - scaled.height()) / 2,
        scaled.width(),
        scaled.height());
    painter.drawImage(drawRect, display);

    const qreal refPxX = drawRect.left() + (locX * drawRect.width());
    const qreal refPxY = drawRect.top() + (locY * drawRect.height());
    const int minSide = qMin(drawRect.width(), drawRect.height());
    int boxSide = qMax(48, minSide / 4);
    if (boxSizeNorm > 0.0) {
        boxSide = qBound(48, static_cast<int>(std::round(boxSizeNorm * minSide)), minSide);
    }
    const QRectF boxRect(refPxX - (boxSide / 2.0), refPxY - (boxSide / 2.0), boxSide, boxSide);

    painter.setPen(QPen(QColor(QStringLiteral("#ffb347")), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(boxRect);
    painter.setPen(QPen(QColor(QStringLiteral("#6fd6ff")), 3.0));
    painter.drawEllipse(QPointF(refPxX, refPxY), 5.0, 5.0);
    painter.setPen(QPen(QColor(QStringLiteral("#6fd6ff")), 1.0));
    painter.drawLine(QPointF(refPxX - 14.0, refPxY), QPointF(refPxX + 14.0, refPxY));
    painter.drawLine(QPointF(refPxX, refPxY - 14.0), QPointF(refPxX, refPxY + 14.0));

    return QPixmap::fromImage(canvas);
}

bool SpeakersTab::openReferencePreviewWindow(int referenceIndex)
{
    if (referenceIndex != 1 && referenceIndex != 2) {
        return false;
    }
    const TimelineClip* clip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
    if (!clip) {
        return false;
    }
    QString speakerId;
    QJsonObject refObj;
    if (!selectedSpeakerReferenceObject(referenceIndex, &speakerId, &refObj)) {
        if (activeCutMutable()) {
            onSpeakerPrecropFacesClicked();
            return true;
        }
        return false;
    }

    QDialog* dialog = new QDialog();
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(QStringLiteral("Ref %1 Source Preview").arg(referenceIndex));
    dialog->resize(980, 720);

    QVBoxLayout* root = new QVBoxLayout(dialog);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    QLabel* imageLabel = new QLabel(dialog);
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(640, 360);
    imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageLabel->setStyleSheet(QStringLiteral("background:#0d141d;border:1px solid #263748;border-radius:8px;"));
    root->addWidget(imageLabel, 1);

    QLabel* detailLabel = new QLabel(dialog);
    detailLabel->setWordWrap(true);
    detailLabel->setStyleSheet(QStringLiteral("color:#9eb6cf;"));
    root->addWidget(detailLabel);

    QFrame* controlsFrame = new QFrame(dialog);
    controlsFrame->setStyleSheet(QStringLiteral(
        "QFrame{background:#101924;border:1px solid #2a3f53;border-radius:8px;}"));
    QVBoxLayout* controlsLayout = new QVBoxLayout(controlsFrame);
    controlsLayout->setContentsMargins(8, 8, 8, 8);
    controlsLayout->setSpacing(6);

    QLabel* sentenceLabel = new QLabel(dialog);
    sentenceLabel->setWordWrap(true);
    sentenceLabel->setStyleSheet(QStringLiteral("color:#d5e5f6;"));
    controlsLayout->addWidget(sentenceLabel);

    QHBoxLayout* sentenceButtons = new QHBoxLayout();
    QPushButton* prevButton = new QPushButton(QStringLiteral("Prev Sentence"), dialog);
    QPushButton* nextButton = new QPushButton(QStringLiteral("Next Sentence"), dialog);
    QPushButton* randomButton = new QPushButton(QStringLiteral("Random Sentence"), dialog);
    sentenceButtons->addWidget(prevButton);
    sentenceButtons->addWidget(nextButton);
    sentenceButtons->addWidget(randomButton);
    controlsLayout->addLayout(sentenceButtons);

    QHBoxLayout* refButtons = new QHBoxLayout();
    QPushButton* pickButton = new QPushButton(QStringLiteral("Pick Ref %1 (Shift+Drag)").arg(referenceIndex), dialog);
    QPushButton* setButton = new QPushButton(QStringLiteral("Set Ref %1 @ Current Frame").arg(referenceIndex), dialog);
    refButtons->addWidget(pickButton);
    refButtons->addWidget(setButton);
    controlsLayout->addLayout(refButtons);
    root->addWidget(controlsFrame);

    auto refreshDialog = [this, imageLabel, detailLabel, sentenceLabel, referenceIndex]() {
        const TimelineClip* selectedClip = m_deps.getSelectedClip ? m_deps.getSelectedClip() : nullptr;
        QString refSpeakerId;
        QJsonObject latestRef;
        if (!selectedClip || !selectedSpeakerReferenceObject(referenceIndex, &refSpeakerId, &latestRef)) {
            imageLabel->setPixmap(QPixmap());
            detailLabel->setText(QStringLiteral("Reference unavailable."));
            sentenceLabel->setText(QStringLiteral("Sentence context unavailable."));
            return;
        }
        imageLabel->setPixmap(referenceFullFramePreview(*selectedClip, refSpeakerId, latestRef, QSize(1100, 620)));
        const int64_t frame30 =
            qMax<int64_t>(0, latestRef.value(QString(kTranscriptSpeakerTrackingFrameKey)).toVariant().toLongLong());
        const qreal xNorm =
            qBound<qreal>(0.0, latestRef.value(QString(kTranscriptSpeakerLocationXKey)).toDouble(0.5), 1.0);
        const qreal yNorm =
            qBound<qreal>(0.0, latestRef.value(QString(kTranscriptSpeakerLocationYKey)).toDouble(0.85), 1.0);
        detailLabel->setText(
            QStringLiteral("Untransformed clip frame %1 | Speaker %2 | x=%3 y=%4")
                .arg(frame30)
                .arg(refSpeakerId)
                .arg(QString::number(xNorm, 'f', 3))
                .arg(QString::number(yNorm, 'f', 3)));
        sentenceLabel->setText(currentSpeakerSentenceAtCurrentFrame(refSpeakerId));
    };

    connect(prevButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Previous);
        refreshDialog();
    });
    connect(nextButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Next);
        refreshDialog();
    });
    connect(randomButton, &QPushButton::clicked, dialog, [this, refreshDialog, speakerId]() {
        navigateSpeakerSentence(speakerId, SentenceNavAction::Random);
        refreshDialog();
    });
    connect(pickButton, &QPushButton::clicked, dialog, [this, referenceIndex]() {
        const QString id = selectedSpeakerId();
        if (!id.isEmpty()) {
            armReferencePickForSpeaker(id, referenceIndex);
        }
    });
    connect(setButton, &QPushButton::clicked, dialog, [this, refreshDialog, referenceIndex]() {
        const QString id = selectedSpeakerId();
        if (id.isEmpty()) {
            return;
        }
        saveSpeakerTrackingReference(id, referenceIndex);
        refreshDialog();
    });

    refreshDialog();
    dialog->show();
    return true;
}

QString SpeakersTab::speakerTrackingSummary(const QJsonObject& profile) const
{
    const QJsonObject tracking = speakerFramingObject(profile);
    if (tracking.isEmpty()) {
        return QStringLiteral("Speaker Tracking: Off");
    }
    const QString mode = tracking.value(QString(kTranscriptSpeakerTrackingModeKey)).toString(QStringLiteral("Manual"));
    const bool enabled = transcriptTrackingEnabled(tracking);
    const QString autoState = tracking.value(QString(kTranscriptSpeakerTrackingAutoStateKey)).toString();
    const int keyframeCount = tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().size();
    QString summary = QStringLiteral("Speaker Tracking: %1 (%2)")
        .arg(enabled ? QStringLiteral("On") : QStringLiteral("Off"))
        .arg(mode);
    if (keyframeCount > 0) {
        summary += QStringLiteral(" | Keys=%1").arg(keyframeCount);
    }
    if (!autoState.isEmpty()) {
        summary += QStringLiteral(" | Auto=%1").arg(autoState);
    }
    return summary;
}
