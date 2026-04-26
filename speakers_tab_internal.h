#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QStyledItemDelegate>

#include <cmath>
#include <limits>

namespace {
const QLatin1String kTranscriptWordSpeakerKey("speaker");
const QLatin1String kTranscriptSegmentSpeakerKey("speaker");
const QLatin1String kTranscriptSpeakerProfilesKey("speaker_profiles");
const QLatin1String kTranscriptSpeakerNameKey("name");
const QLatin1String kTranscriptSpeakerLocationKey("location");
const QLatin1String kTranscriptSpeakerLocationXKey("x");
const QLatin1String kTranscriptSpeakerLocationYKey("y");
const QLatin1String kTranscriptSpeakerFramingKey("framing");
const QLatin1String kTranscriptSpeakerTrackingKey("tracking");
const QLatin1String kTranscriptSpeakerTrackingModeKey("mode");
const QLatin1String kTranscriptSpeakerTrackingEnabledKey("enabled");
const QLatin1String kTranscriptSpeakerTrackingAutoStateKey("auto_state");
const QLatin1String kTranscriptSpeakerTrackingRef1Key("ref1");
const QLatin1String kTranscriptSpeakerTrackingRef2Key("ref2");
const QLatin1String kTranscriptSpeakerTrackingFrameKey("frame");
const QLatin1String kTranscriptSpeakerTrackingBoxSizeKey("box_size");
const QLatin1String kTranscriptSpeakerTrackingBoxLeftKey("box_left");
const QLatin1String kTranscriptSpeakerTrackingBoxTopKey("box_top");
const QLatin1String kTranscriptSpeakerTrackingBoxRightKey("box_right");
const QLatin1String kTranscriptSpeakerTrackingBoxBottomKey("box_bottom");
const QLatin1String kTranscriptSpeakerTrackingKeyframesKey("keyframes");
const QLatin1String kTranscriptSpeakerTrackingConfidenceKey("confidence");
const QLatin1String kTranscriptSpeakerTrackingSourceKey("source");

QJsonObject transcriptTrackingReferencePoint(const QJsonObject& tracking,
                                             const QLatin1String& key,
                                             bool* okOut)
{
    if (okOut) {
        *okOut = false;
    }
    const QJsonObject ref = tracking.value(QString(key)).toObject();
    if (ref.isEmpty()) {
        return {};
    }
    if (!ref.contains(QString(kTranscriptSpeakerTrackingFrameKey)) ||
        !ref.contains(QString(kTranscriptSpeakerLocationXKey)) ||
        !ref.contains(QString(kTranscriptSpeakerLocationYKey))) {
        return {};
    }
    if (okOut) {
        *okOut = true;
    }
    return ref;
}

bool transcriptTrackingHasPointstream(const QJsonObject& tracking);

bool transcriptTrackingEnabled(const QJsonObject& tracking)
{
    if (tracking.isEmpty()) {
        return false;
    }
    const bool hasPointstream = transcriptTrackingHasPointstream(tracking);
    const bool explicitlyEnabled =
        tracking.value(QString(kTranscriptSpeakerTrackingEnabledKey)).toBool(false);
    if (!hasPointstream || !explicitlyEnabled) {
        return false;
    }
    return true;
}

bool transcriptTrackingHasPointstream(const QJsonObject& tracking)
{
    return !tracking.value(QString(kTranscriptSpeakerTrackingKeyframesKey)).toArray().isEmpty();
}

QJsonObject speakerFramingObject(const QJsonObject& profile)
{
    return profile.value(QString(kTranscriptSpeakerFramingKey)).toObject();
}

void setSpeakerFramingObject(QJsonObject& profile, const QJsonObject& framing)
{
    profile[QString(kTranscriptSpeakerFramingKey)] = framing;
}

int64_t canonicalToSourceFrameForTracking(int64_t frame30, double sourceFps)
{
    const double fps = sourceFps > 0.0 ? sourceFps : 30.0;
    return qMax<int64_t>(
        0, static_cast<int64_t>(std::floor((static_cast<double>(frame30) / 30.0) * fps)));
}

QImage toGray8(const QImage& image)
{
    if (image.isNull()) {
        return {};
    }
    if (image.format() == QImage::Format_Grayscale8) {
        return image;
    }
    return image.convertToFormat(QImage::Format_Grayscale8);
}

bool cropSquareGray(const QImage& frameGray,
                    qreal xNorm,
                    qreal yNorm,
                    qreal boxNorm,
                    QImage* outCrop,
                    int* outLeft,
                    int* outTop,
                    int* outSide)
{
    if (!outCrop || frameGray.isNull()) {
        return false;
    }
    const int w = frameGray.width();
    const int h = frameGray.height();
    const int minSide = qMax(1, qMin(w, h));
    int side = static_cast<int>(std::round(qBound<qreal>(0.01, boxNorm, 1.0) * static_cast<qreal>(minSide)));
    side = qBound(20, side, minSide);
    const int cx = static_cast<int>(std::round(qBound<qreal>(0.0, xNorm, 1.0) * static_cast<qreal>(w)));
    const int cy = static_cast<int>(std::round(qBound<qreal>(0.0, yNorm, 1.0) * static_cast<qreal>(h)));
    const int left = qBound(0, cx - (side / 2), qMax(0, w - side));
    const int top = qBound(0, cy - (side / 2), qMax(0, h - side));
    *outCrop = frameGray.copy(left, top, side, side);
    if (outLeft) {
        *outLeft = left;
    }
    if (outTop) {
        *outTop = top;
    }
    if (outSide) {
        *outSide = side;
    }
    return !outCrop->isNull();
}

bool trackTemplateSad(const QImage& frameGray,
                      qreal predictedX,
                      qreal predictedY,
                      qreal predictedBox,
                      const QImage& tmplGray,
                      qreal searchScale,
                      int scanStrideCap,
                      qreal* outX,
                      qreal* outY,
                      qreal* outBox,
                      qreal* outConfidence)
{
    if (frameGray.isNull() || tmplGray.isNull() || tmplGray.width() < 8 || tmplGray.height() < 8) {
        return false;
    }

    const int fw = frameGray.width();
    const int fh = frameGray.height();
    const int minSide = qMax(1, qMin(fw, fh));
    const int tmplW = tmplGray.width();
    const int tmplH = tmplGray.height();
    const int cx = static_cast<int>(std::round(qBound<qreal>(0.0, predictedX, 1.0) * static_cast<qreal>(fw)));
    const int cy = static_cast<int>(std::round(qBound<qreal>(0.0, predictedY, 1.0) * static_cast<qreal>(fh)));

    int side = static_cast<int>(std::round(qBound<qreal>(0.01, predictedBox, 1.0) * static_cast<qreal>(minSide)));
    side = qBound(qMax(tmplW, tmplH), side, minSide);
    const qreal boundedSearchScale = qBound<qreal>(1.0, searchScale, 8.0);
    int searchSide = qBound(side, static_cast<int>(std::round(side * boundedSearchScale)), minSide);
    searchSide = qMax(searchSide, qMax(tmplW + 2, tmplH + 2));
    const int searchLeft = qBound(0, cx - (searchSide / 2), qMax(0, fw - searchSide));
    const int searchTop = qBound(0, cy - (searchSide / 2), qMax(0, fh - searchSide));
    const int searchRight = searchLeft + searchSide - tmplW;
    const int searchBottom = searchTop + searchSide - tmplH;
    if (searchRight < searchLeft || searchBottom < searchTop) {
        return false;
    }

    const int sampleStride = qBound(1, tmplW / 36, 3);
    const int scanStride = qBound(1, tmplW / 20, qMax(1, scanStrideCap));
    const uchar* frameBits = frameGray.constBits();
    const int frameStride = frameGray.bytesPerLine();
    const uchar* tmplBits = tmplGray.constBits();
    const int tmplStride = tmplGray.bytesPerLine();

    qint64 bestSad = std::numeric_limits<qint64>::max();
    int bestX = searchLeft;
    int bestY = searchTop;
    for (int y = searchTop; y <= searchBottom; y += scanStride) {
        for (int x = searchLeft; x <= searchRight; x += scanStride) {
            qint64 sad = 0;
            for (int ty = 0; ty < tmplH; ty += sampleStride) {
                const uchar* fRow = frameBits + ((y + ty) * frameStride) + x;
                const uchar* tRow = tmplBits + (ty * tmplStride);
                for (int tx = 0; tx < tmplW; tx += sampleStride) {
                    sad += std::llabs(static_cast<qint64>(fRow[tx]) - static_cast<qint64>(tRow[tx]));
                }
                if (sad >= bestSad) {
                    break;
                }
            }
            if (sad < bestSad) {
                bestSad = sad;
                bestX = x;
                bestY = y;
            }
        }
    }

    const int matchCx = bestX + (tmplW / 2);
    const int matchCy = bestY + (tmplH / 2);
    const qreal xNorm = qBound<qreal>(0.0, static_cast<qreal>(matchCx) / static_cast<qreal>(fw), 1.0);
    const qreal yNorm = qBound<qreal>(0.0, static_cast<qreal>(matchCy) / static_cast<qreal>(fh), 1.0);
    const qreal boxNorm =
        qBound<qreal>(0.01, static_cast<qreal>(qMax(tmplW, tmplH)) / static_cast<qreal>(minSide), 1.0);

    const qreal samplesX = static_cast<qreal>((tmplW + sampleStride - 1) / sampleStride);
    const qreal samplesY = static_cast<qreal>((tmplH + sampleStride - 1) / sampleStride);
    const qreal samples = qMax<qreal>(1.0, samplesX * samplesY);
    const qreal normalizedSad =
        qBound<qreal>(0.0, static_cast<qreal>(bestSad) / (255.0 * samples), 1.0);
    const qreal confidence = qBound<qreal>(0.0, 1.0 - normalizedSad, 1.0);

    if (outX) {
        *outX = xNorm;
    }
    if (outY) {
        *outY = yNorm;
    }
    if (outBox) {
        *outBox = boxNorm;
    }
    if (outConfidence) {
        *outConfidence = confidence;
    }
    return true;
}

QRect fitRectForSourceInOutput(const QSize& source, const QSize& output)
{
    const QSize safeOutput = output.isValid() ? output : QSize(1080, 1920);
    if (!source.isValid()) {
        return QRect(QPoint(0, 0), safeOutput);
    }
    QSize fitted = source;
    fitted.scale(safeOutput, Qt::KeepAspectRatio);
    const int x = (safeOutput.width() - fitted.width()) / 2;
    const int y = (safeOutput.height() - fitted.height()) / 2;
    return QRect(x, y, fitted.width(), fitted.height());
}

void applyGentleBidirectionalTransformSmoothing(
    QVector<TimelineClip::TransformKeyframe>& keyframes,
    bool smoothTranslation,
    bool smoothScale)
{
    if (keyframes.size() < 3 || (!smoothTranslation && !smoothScale)) {
        return;
    }

    // Centered bidirectional triangular window.
    constexpr int kRadius = 2; // window = 5, gentle smoothing
    QVector<TimelineClip::TransformKeyframe> smoothed = keyframes;
    for (int i = 0; i < keyframes.size(); ++i) {
        qreal sumW = 0.0;
        qreal sumTx = 0.0;
        qreal sumTy = 0.0;
        qreal sumLogSx = 0.0;
        qreal sumLogSy = 0.0;

        for (int d = -kRadius; d <= kRadius; ++d) {
            const int j = i + d;
            if (j < 0 || j >= keyframes.size()) {
                continue;
            }
            const qreal w = static_cast<qreal>((kRadius + 1) - std::abs(d)); // 3,2,1
            const TimelineClip::TransformKeyframe& kf = keyframes[j];

            sumW += w;
            if (smoothTranslation) {
                sumTx += w * kf.translationX;
                sumTy += w * kf.translationY;
            }
            if (smoothScale) {
                const qreal sx = qMax<qreal>(0.001, kf.scaleX);
                const qreal sy = qMax<qreal>(0.001, kf.scaleY);
                sumLogSx += w * std::log(sx);
                sumLogSy += w * std::log(sy);
            }
        }

        if (sumW <= 0.0) {
            continue;
        }
        TimelineClip::TransformKeyframe& out = smoothed[i];
        if (smoothTranslation) {
            out.translationX = sumTx / sumW;
            out.translationY = sumTy / sumW;
        } else {
            out.translationX = keyframes[i].translationX;
            out.translationY = keyframes[i].translationY;
        }
        if (smoothScale) {
            out.scaleX = std::exp(sumLogSx / sumW);
            out.scaleY = std::exp(sumLogSy / sumW);
        } else {
            out.scaleX = keyframes[i].scaleX;
            out.scaleY = keyframes[i].scaleY;
        }
    }

    keyframes.swap(smoothed);
}

void writeNormalizedFaceBox(QJsonObject& obj, qreal xNorm, qreal yNorm, qreal boxSizeNorm)
{
    const qreal x = qBound<qreal>(0.0, xNorm, 1.0);
    const qreal y = qBound<qreal>(0.0, yNorm, 1.0);
    const qreal side = qBound<qreal>(0.01, boxSizeNorm, 1.0);
    const qreal half = side * 0.5;
    obj[QString(kTranscriptSpeakerTrackingBoxLeftKey)] = qBound<qreal>(0.0, x - half, 1.0);
    obj[QString(kTranscriptSpeakerTrackingBoxTopKey)] = qBound<qreal>(0.0, y - half, 1.0);
    obj[QString(kTranscriptSpeakerTrackingBoxRightKey)] = qBound<qreal>(0.0, x + half, 1.0);
    obj[QString(kTranscriptSpeakerTrackingBoxBottomKey)] = qBound<qreal>(0.0, y + half, 1.0);
}

class SpeakerNameItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!index.data(Qt::UserRole + 10).toBool()) {
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect borderRect = option.rect.adjusted(4, 6, -4, -6);
        painter->setPen(QPen(QColor(QStringLiteral("#29c46a")), 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(borderRect, 4.0, 4.0);
        painter->restore();
    }
};
}
