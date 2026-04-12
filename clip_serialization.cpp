#include "clip_serialization.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>

namespace editor
{

QJsonObject clipToJson(const TimelineClip &clip)
    {
        QJsonObject obj;
        obj[QStringLiteral("id")] = clip.id;
        obj[QStringLiteral("filePath")] = clip.filePath;
        obj[QStringLiteral("proxyPath")] = clip.proxyPath;
        obj[QStringLiteral("label")] = clip.label;
        obj[QStringLiteral("mediaType")] = clipMediaTypeToString(clip.mediaType);
        obj[QStringLiteral("sourceKind")] = mediaSourceKindToString(clip.sourceKind);
        obj[QStringLiteral("hasAudio")] = clip.hasAudio;
        obj[QStringLiteral("sourceFps")] = clip.sourceFps;
        obj[QStringLiteral("sourceDurationFrames")] = static_cast<qint64>(clip.sourceDurationFrames);
        obj[QStringLiteral("sourceInFrame")] = static_cast<qint64>(clip.sourceInFrame);
        obj[QStringLiteral("sourceInSubframeSamples")] = static_cast<qint64>(clip.sourceInSubframeSamples);
        obj[QStringLiteral("startFrame")] = static_cast<qint64>(clip.startFrame);
        obj[QStringLiteral("startSubframeSamples")] = static_cast<qint64>(clip.startSubframeSamples);
        obj[QStringLiteral("durationFrames")] = static_cast<qint64>(clip.durationFrames);
        obj[QStringLiteral("trackIndex")] = clip.trackIndex;
        obj[QStringLiteral("playbackRate")] = clip.playbackRate;
        obj[QStringLiteral("videoEnabled")] = clip.videoEnabled;
        obj[QStringLiteral("audioEnabled")] = clip.audioEnabled;
        obj[QStringLiteral("color")] = clip.color.name(QColor::HexArgb);
        obj[QStringLiteral("brightness")] = clip.brightness;
        obj[QStringLiteral("contrast")] = clip.contrast;
        obj[QStringLiteral("saturation")] = clip.saturation;
        obj[QStringLiteral("opacity")] = clip.opacity;
        obj[QStringLiteral("baseTranslationX")] = clip.baseTranslationX;
        obj[QStringLiteral("baseTranslationY")] = clip.baseTranslationY;
        obj[QStringLiteral("baseRotation")] = clip.baseRotation;
        obj[QStringLiteral("baseScaleX")] = clip.baseScaleX;
        obj[QStringLiteral("baseScaleY")] = clip.baseScaleY;
        obj[QStringLiteral("transformSkipAwareTiming")] = clip.transformSkipAwareTiming;
        QJsonArray keyframes;
        for (const TimelineClip::TransformKeyframe &keyframe : clip.transformKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("translationX")] = keyframe.translationX;
            keyframeObj[QStringLiteral("translationY")] = keyframe.translationY;
            keyframeObj[QStringLiteral("rotation")] = keyframe.rotation;
            keyframeObj[QStringLiteral("scaleX")] = keyframe.scaleX;
            keyframeObj[QStringLiteral("scaleY")] = keyframe.scaleY;
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            keyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("transformKeyframes")] = keyframes;
        QJsonArray gradingKeyframes;
        for (const TimelineClip::GradingKeyframe &keyframe : clip.gradingKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("brightness")] = keyframe.brightness;
            keyframeObj[QStringLiteral("contrast")] = keyframe.contrast;
            keyframeObj[QStringLiteral("saturation")] = keyframe.saturation;
            keyframeObj[QStringLiteral("shadowsR")] = keyframe.shadowsR;
            keyframeObj[QStringLiteral("shadowsG")] = keyframe.shadowsG;
            keyframeObj[QStringLiteral("shadowsB")] = keyframe.shadowsB;
            keyframeObj[QStringLiteral("midtonesR")] = keyframe.midtonesR;
            keyframeObj[QStringLiteral("midtonesG")] = keyframe.midtonesG;
            keyframeObj[QStringLiteral("midtonesB")] = keyframe.midtonesB;
            keyframeObj[QStringLiteral("highlightsR")] = keyframe.highlightsR;
            keyframeObj[QStringLiteral("highlightsG")] = keyframe.highlightsG;
            keyframeObj[QStringLiteral("highlightsB")] = keyframe.highlightsB;
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            gradingKeyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("gradingKeyframes")] = gradingKeyframes;
        QJsonArray opacityKeyframes;
        for (const TimelineClip::OpacityKeyframe &keyframe : clip.opacityKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("opacity")] = keyframe.opacity;
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            opacityKeyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("opacityKeyframes")] = opacityKeyframes;
        QJsonArray titleKeyframes;
        for (const TimelineClip::TitleKeyframe &keyframe : clip.titleKeyframes)
        {
            QJsonObject keyframeObj;
            keyframeObj[QStringLiteral("frame")] = static_cast<qint64>(keyframe.frame);
            keyframeObj[QStringLiteral("text")] = keyframe.text;
            keyframeObj[QStringLiteral("translationX")] = keyframe.translationX;
            keyframeObj[QStringLiteral("translationY")] = keyframe.translationY;
            keyframeObj[QStringLiteral("fontSize")] = keyframe.fontSize;
            keyframeObj[QStringLiteral("opacity")] = keyframe.opacity;
            keyframeObj[QStringLiteral("fontFamily")] = keyframe.fontFamily;
            keyframeObj[QStringLiteral("bold")] = keyframe.bold;
            keyframeObj[QStringLiteral("italic")] = keyframe.italic;
            keyframeObj[QStringLiteral("color")] = keyframe.color.name(QColor::HexArgb);
            keyframeObj[QStringLiteral("linearInterpolation")] = keyframe.linearInterpolation;
            titleKeyframes.push_back(keyframeObj);
        }
        obj[QStringLiteral("titleKeyframes")] = titleKeyframes;
        QJsonObject transcriptOverlayObj;
        transcriptOverlayObj[QStringLiteral("enabled")] = clip.transcriptOverlay.enabled;
        transcriptOverlayObj[QStringLiteral("autoScroll")] = clip.transcriptOverlay.autoScroll;
        transcriptOverlayObj[QStringLiteral("translationX")] = clip.transcriptOverlay.translationX;
        transcriptOverlayObj[QStringLiteral("translationY")] = clip.transcriptOverlay.translationY;
        transcriptOverlayObj[QStringLiteral("boxWidth")] = clip.transcriptOverlay.boxWidth;
        transcriptOverlayObj[QStringLiteral("boxHeight")] = clip.transcriptOverlay.boxHeight;
        transcriptOverlayObj[QStringLiteral("maxLines")] = clip.transcriptOverlay.maxLines;
        transcriptOverlayObj[QStringLiteral("maxCharsPerLine")] = clip.transcriptOverlay.maxCharsPerLine;
        transcriptOverlayObj[QStringLiteral("fontFamily")] = clip.transcriptOverlay.fontFamily;
        transcriptOverlayObj[QStringLiteral("fontPointSize")] = clip.transcriptOverlay.fontPointSize;
        transcriptOverlayObj[QStringLiteral("bold")] = clip.transcriptOverlay.bold;
        transcriptOverlayObj[QStringLiteral("italic")] = clip.transcriptOverlay.italic;
        transcriptOverlayObj[QStringLiteral("textColor")] =
            clip.transcriptOverlay.textColor.name(QColor::HexArgb);
        obj[QStringLiteral("transcriptOverlay")] = transcriptOverlayObj;
        obj[QStringLiteral("fadeSamples")] = clip.fadeSamples;
        obj[QStringLiteral("locked")] = clip.locked;
        obj[QStringLiteral("maskFeather")] = clip.maskFeather;
        obj[QStringLiteral("maskFeatherGamma")] = clip.maskFeatherGamma;
        QJsonArray correctionPolygons;
        for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
            if (polygon.pointsNormalized.size() < 3) {
                continue;
            }
            QJsonObject polygonObj;
            polygonObj[QStringLiteral("enabled")] = polygon.enabled;
            polygonObj[QStringLiteral("startFrame")] = static_cast<qint64>(qMax<int64_t>(0, polygon.startFrame));
            polygonObj[QStringLiteral("endFrame")] = static_cast<qint64>(polygon.endFrame);
            QJsonArray points;
            for (const QPointF& point : polygon.pointsNormalized) {
                QJsonObject pointObj;
                pointObj[QStringLiteral("x")] = point.x();
                pointObj[QStringLiteral("y")] = point.y();
                points.push_back(pointObj);
            }
            polygonObj[QStringLiteral("points")] = points;
            correctionPolygons.push_back(polygonObj);
        }
        obj[QStringLiteral("correctionPolygons")] = correctionPolygons;
        return obj;
    }

TimelineClip clipFromJson(const QJsonObject &obj)
    {
        TimelineClip clip;
        const bool hasSourceFps = obj.contains(QStringLiteral("sourceFps"));
        clip.id = obj.value(QStringLiteral("id")).toString();
        clip.filePath = obj.value(QStringLiteral("filePath")).toString();
        clip.proxyPath = obj.value(QStringLiteral("proxyPath")).toString();
        clip.label = obj.value(QStringLiteral("label")).toString(QFileInfo(clip.filePath).fileName());
        clip.mediaType = clipMediaTypeFromString(obj.value(QStringLiteral("mediaType")).toString());
        clip.sourceKind = mediaSourceKindFromString(obj.value(QStringLiteral("sourceKind")).toString());
        clip.hasAudio = obj.value(QStringLiteral("hasAudio")).toBool(false);
        clip.sourceFps = obj.value(QStringLiteral("sourceFps")).toDouble(30.0);
        clip.sourceDurationFrames = obj.value(QStringLiteral("sourceDurationFrames")).toVariant().toLongLong();
        clip.sourceInFrame = obj.value(QStringLiteral("sourceInFrame")).toVariant().toLongLong();
        clip.sourceInSubframeSamples = obj.value(QStringLiteral("sourceInSubframeSamples")).toVariant().toLongLong();
        clip.startFrame = obj.value(QStringLiteral("startFrame")).toVariant().toLongLong();
        clip.startSubframeSamples = obj.value(QStringLiteral("startSubframeSamples")).toVariant().toLongLong();
        clip.durationFrames = obj.value(QStringLiteral("durationFrames")).toVariant().toLongLong();
        clip.trackIndex = obj.value(QStringLiteral("trackIndex")).toInt(-1);
        clip.playbackRate = obj.value(QStringLiteral("playbackRate")).toDouble(1.0);
        clip.videoEnabled = obj.value(QStringLiteral("videoEnabled")).toBool(true);
        clip.audioEnabled = obj.value(QStringLiteral("audioEnabled")).toBool(true);
        if (clip.durationFrames == 0)
            clip.durationFrames = 120;
        if (clip.mediaType == ClipMediaType::Unknown && !clip.filePath.isEmpty())
        {
            const MediaProbeResult probe = probeMediaFile(clip.filePath, clip.durationFrames);
            clip.mediaType = probe.mediaType;
            clip.sourceKind = probe.sourceKind;
            clip.hasAudio = probe.hasAudio;
            clip.sourceFps = probe.fps;
            const qreal sourceFps = probe.fps > 0.001 ? probe.fps : static_cast<qreal>(kTimelineFps);
            clip.durationFrames = qMax<int64_t>(
                1,
                qRound64((static_cast<qreal>(qMax<int64_t>(1, probe.durationFrames)) / sourceFps) *
                         static_cast<qreal>(kTimelineFps)));
            clip.sourceDurationFrames = probe.durationFrames;
        }
        if (!hasSourceFps && !clip.filePath.isEmpty()) {
            const MediaProbeResult probe = probeMediaFile(clip.filePath, clip.durationFrames);
            if (probe.fps > 0.001) {
                clip.sourceFps = probe.fps;
            }
        }
        if (!clip.filePath.isEmpty() &&
            clip.mediaType != ClipMediaType::Image &&
            clip.mediaType != ClipMediaType::Title) {
            const MediaProbeResult probe = probeMediaFile(clip.filePath, clip.durationFrames);
            if (probe.fps > 0.001) {
                const bool suspiciousLegacySourceFps =
                    qAbs(clip.sourceFps - static_cast<qreal>(kTimelineFps)) <= 0.001 &&
                    qAbs(probe.fps - clip.sourceFps) > 0.01;
                if (suspiciousLegacySourceFps) {
                    clip.sourceFps = probe.fps;
                }
            }
            if (probe.durationFrames > 0 && clip.sourceDurationFrames <= 0) {
                clip.sourceDurationFrames = probe.durationFrames;
            }
            const bool looksLikeLegacyDuration =
                clip.sourceDurationFrames > 0 &&
                qAbs(clip.durationFrames - clip.sourceDurationFrames) <= 1;
            if (looksLikeLegacyDuration &&
                clip.sourceFps > 0.001 &&
                qAbs(clip.sourceFps - static_cast<qreal>(kTimelineFps)) > 0.001) {
                clip.durationFrames = qMax<int64_t>(
                    1,
                    qRound64((static_cast<qreal>(clip.sourceDurationFrames) / clip.sourceFps) *
                             static_cast<qreal>(kTimelineFps)));
            }
        }
        if (clip.sourceDurationFrames <= 0)
        {
            clip.sourceDurationFrames = clip.sourceInFrame + clip.durationFrames;
        }
        normalizeClipTiming(clip);
        clip.color = QColor(obj.value(QStringLiteral("color")).toString());
        if (!clip.color.isValid())
        {
            clip.color = QColor::fromHsv(static_cast<int>(qHash(clip.filePath) % 360), 160, 220, 220);
        }
        if (clip.mediaType == ClipMediaType::Audio)
        {
            clip.color = QColor(QStringLiteral("#2f7f93"));
        }
        clip.brightness = obj.value(QStringLiteral("brightness")).toDouble(0.0);
        clip.contrast = obj.value(QStringLiteral("contrast")).toDouble(1.0);
        clip.saturation = obj.value(QStringLiteral("saturation")).toDouble(1.0);
        clip.opacity = obj.value(QStringLiteral("opacity")).toDouble(1.0);
        clip.baseTranslationX = obj.value(QStringLiteral("baseTranslationX")).toDouble(0.0);
        clip.baseTranslationY = obj.value(QStringLiteral("baseTranslationY")).toDouble(0.0);
        clip.baseRotation = obj.value(QStringLiteral("baseRotation")).toDouble(0.0);
        clip.baseScaleX = obj.value(QStringLiteral("baseScaleX")).toDouble(1.0);
        clip.baseScaleY = obj.value(QStringLiteral("baseScaleY")).toDouble(1.0);
        clip.transformSkipAwareTiming = obj.value(QStringLiteral("transformSkipAwareTiming")).toBool(false);
        const QJsonArray keyframes = obj.value(QStringLiteral("transformKeyframes")).toArray();
        for (const QJsonValue &value : keyframes)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::TransformKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.translationX = keyframeObj.value(QStringLiteral("translationX")).toDouble(0.0);
            keyframe.translationY = keyframeObj.value(QStringLiteral("translationY")).toDouble(0.0);
            keyframe.rotation = keyframeObj.value(QStringLiteral("rotation")).toDouble(0.0);
            keyframe.scaleX = keyframeObj.value(QStringLiteral("scaleX")).toDouble(1.0);
            keyframe.scaleY = keyframeObj.value(QStringLiteral("scaleY")).toDouble(1.0);
            if (keyframeObj.contains(QStringLiteral("linearInterpolation"))) {
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            } else {
                // Backward compatibility with older saved projects.
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("interpolated")).toBool(true);
            }
            clip.transformKeyframes.push_back(keyframe);
        }
        const QJsonArray gradingKeyframes = obj.value(QStringLiteral("gradingKeyframes")).toArray();
        QVector<TimelineClip::OpacityKeyframe> migratedOpacityKeyframes;
        for (const QJsonValue &value : gradingKeyframes)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::GradingKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.brightness = keyframeObj.value(QStringLiteral("brightness")).toDouble(0.0);
            keyframe.contrast = keyframeObj.value(QStringLiteral("contrast")).toDouble(1.0);
            keyframe.saturation = keyframeObj.value(QStringLiteral("saturation")).toDouble(1.0);
            keyframe.shadowsR = keyframeObj.value(QStringLiteral("shadowsR")).toDouble(0.0);
            keyframe.shadowsG = keyframeObj.value(QStringLiteral("shadowsG")).toDouble(0.0);
            keyframe.shadowsB = keyframeObj.value(QStringLiteral("shadowsB")).toDouble(0.0);
            keyframe.midtonesR = keyframeObj.value(QStringLiteral("midtonesR")).toDouble(0.0);
            keyframe.midtonesG = keyframeObj.value(QStringLiteral("midtonesG")).toDouble(0.0);
            keyframe.midtonesB = keyframeObj.value(QStringLiteral("midtonesB")).toDouble(0.0);
            keyframe.highlightsR = keyframeObj.value(QStringLiteral("highlightsR")).toDouble(0.0);
            keyframe.highlightsG = keyframeObj.value(QStringLiteral("highlightsG")).toDouble(0.0);
            keyframe.highlightsB = keyframeObj.value(QStringLiteral("highlightsB")).toDouble(0.0);
            if (keyframeObj.contains(QStringLiteral("linearInterpolation"))) {
                keyframe.linearInterpolation =
                    keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            } else {
                keyframe.linearInterpolation = true;
            }
            clip.gradingKeyframes.push_back(keyframe);
            if (keyframeObj.contains(QStringLiteral("opacity"))) {
                TimelineClip::OpacityKeyframe opacityKeyframe;
                opacityKeyframe.frame = keyframe.frame;
                opacityKeyframe.opacity = keyframeObj.value(QStringLiteral("opacity")).toDouble(clip.opacity);
                opacityKeyframe.linearInterpolation = keyframe.linearInterpolation;
                migratedOpacityKeyframes.push_back(opacityKeyframe);
            }
        }
        const QJsonArray opacityKeyframes = obj.value(QStringLiteral("opacityKeyframes")).toArray();
        for (const QJsonValue &value : opacityKeyframes)
        {
            if (!value.isObject())
            {
                continue;
            }
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::OpacityKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.opacity = keyframeObj.value(QStringLiteral("opacity")).toDouble(clip.opacity);
            keyframe.linearInterpolation = keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            clip.opacityKeyframes.push_back(keyframe);
        }
        if (clip.opacityKeyframes.isEmpty() && !migratedOpacityKeyframes.isEmpty()) {
            clip.opacityKeyframes = migratedOpacityKeyframes;
        }
        const QJsonObject transcriptOverlayObj = obj.value(QStringLiteral("transcriptOverlay")).toObject();
        clip.transcriptOverlay.enabled = transcriptOverlayObj.value(QStringLiteral("enabled")).toBool(false);
        clip.transcriptOverlay.autoScroll = transcriptOverlayObj.value(QStringLiteral("autoScroll")).toBool(false);
        clip.transcriptOverlay.translationX = transcriptOverlayObj.value(QStringLiteral("translationX")).toDouble(0.0);
        clip.transcriptOverlay.translationY = transcriptOverlayObj.value(QStringLiteral("translationY")).toDouble(640.0);
        clip.transcriptOverlay.boxWidth = transcriptOverlayObj.value(QStringLiteral("boxWidth")).toDouble(900.0);
        clip.transcriptOverlay.boxHeight = transcriptOverlayObj.value(QStringLiteral("boxHeight")).toDouble(220.0);
        clip.transcriptOverlay.maxLines = qMax(1, transcriptOverlayObj.value(QStringLiteral("maxLines")).toInt(2));
        clip.transcriptOverlay.maxCharsPerLine =
            qMax(1, transcriptOverlayObj.value(QStringLiteral("maxCharsPerLine")).toInt(28));
        clip.transcriptOverlay.fontFamily =
            transcriptOverlayObj.value(QStringLiteral("fontFamily")).toString(kDefaultFontFamily);
        clip.transcriptOverlay.fontPointSize =
            qMax(8, transcriptOverlayObj.value(QStringLiteral("fontPointSize")).toInt(42));
        clip.transcriptOverlay.bold = transcriptOverlayObj.value(QStringLiteral("bold")).toBool(true);
        clip.transcriptOverlay.italic = transcriptOverlayObj.value(QStringLiteral("italic")).toBool(false);
        clip.transcriptOverlay.textColor =
            QColor(transcriptOverlayObj.value(QStringLiteral("textColor")).toString(QStringLiteral("#ffffffff")));
        clip.fadeSamples = qMax(0, obj.value(QStringLiteral("fadeSamples")).toInt(250));
        clip.locked = obj.value(QStringLiteral("locked")).toBool(false);
        clip.maskFeather = qMax(0.0, obj.value(QStringLiteral("maskFeather")).toDouble(0.0));
        clip.maskFeatherGamma = qBound(0.1, obj.value(QStringLiteral("maskFeatherGamma")).toDouble(2.0), 5.0);
        const QJsonArray correctionPolygons = obj.value(QStringLiteral("correctionPolygons")).toArray();
        for (const QJsonValue& polygonValue : correctionPolygons) {
            if (!polygonValue.isObject()) {
                continue;
            }
            const QJsonObject polygonObj = polygonValue.toObject();
            TimelineClip::CorrectionPolygon polygon;
            polygon.enabled = polygonObj.value(QStringLiteral("enabled")).toBool(true);
            polygon.startFrame = qMax<int64_t>(
                0,
                polygonObj.value(QStringLiteral("startFrame")).toVariant().toLongLong());
            polygon.endFrame = polygonObj.contains(QStringLiteral("endFrame"))
                ? polygonObj.value(QStringLiteral("endFrame")).toVariant().toLongLong()
                : -1;
            const QJsonArray points = polygonObj.value(QStringLiteral("points")).toArray();
            polygon.pointsNormalized.reserve(points.size());
            for (const QJsonValue& pointValue : points) {
                if (!pointValue.isObject()) {
                    continue;
                }
                const QJsonObject pointObj = pointValue.toObject();
                const qreal x = qBound<qreal>(0.0, pointObj.value(QStringLiteral("x")).toDouble(0.0), 1.0);
                const qreal y = qBound<qreal>(0.0, pointObj.value(QStringLiteral("y")).toDouble(0.0), 1.0);
                polygon.pointsNormalized.push_back(QPointF(x, y));
            }
            if (polygon.pointsNormalized.size() >= 3) {
                if (polygon.endFrame >= 0 && polygon.endFrame < polygon.startFrame) {
                    polygon.endFrame = polygon.startFrame;
                }
                clip.correctionPolygons.push_back(polygon);
            }
        }
        const QJsonArray titleKeyframesArr = obj.value(QStringLiteral("titleKeyframes")).toArray();
        for (const QJsonValue &value : titleKeyframesArr)
        {
            if (!value.isObject()) continue;
            const QJsonObject keyframeObj = value.toObject();
            TimelineClip::TitleKeyframe keyframe;
            keyframe.frame = keyframeObj.value(QStringLiteral("frame")).toVariant().toLongLong();
            keyframe.text = keyframeObj.value(QStringLiteral("text")).toString();
            keyframe.translationX = keyframeObj.value(QStringLiteral("translationX")).toDouble(0.0);
            keyframe.translationY = keyframeObj.value(QStringLiteral("translationY")).toDouble(0.0);
            keyframe.fontSize = keyframeObj.value(QStringLiteral("fontSize")).toDouble(48.0);
            keyframe.opacity = keyframeObj.value(QStringLiteral("opacity")).toDouble(1.0);
            keyframe.fontFamily = keyframeObj.value(QStringLiteral("fontFamily")).toString(kDefaultFontFamily);
            keyframe.bold = keyframeObj.value(QStringLiteral("bold")).toBool(true);
            keyframe.italic = keyframeObj.value(QStringLiteral("italic")).toBool(false);
            keyframe.color = QColor(keyframeObj.value(QStringLiteral("color")).toString(QStringLiteral("#ffffffff")));
            keyframe.linearInterpolation = keyframeObj.value(QStringLiteral("linearInterpolation")).toBool(true);
            clip.titleKeyframes.push_back(keyframe);
        }
        normalizeClipTransformKeyframes(clip);
        normalizeClipGradingKeyframes(clip);
        normalizeClipOpacityKeyframes(clip);
        normalizeClipTitleKeyframes(clip);
        return clip;
    }

} // namespace editor
