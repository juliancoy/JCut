#pragma once

#include "timeline_fps.h"

#include <QColor>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>

enum class ClipMediaType {
    Unknown,
    Image,
    Video,
    Audio,
    Title,
};

enum class ProxyFormat {
    ImageSequence,
    H264,
    MJPEG,
};

enum class MediaSourceKind {
    File,
    ImageSequence,
};

enum class TrackVisualMode {
    Enabled,
    ForceOpaque,
    Hidden,
};

struct TimelineClip {
    static constexpr int kGradingCurveLutSize = 256;
    static constexpr int kSpeakerFramingSmoothingMaxFrames = 500;
    static constexpr int kSpeakerFramingGapHoldMaxFrames = 240;

    struct TransformKeyframe {
        int64_t frame = 0;
        QString title;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal rotation = 0.0;
        qreal scaleX = 1.0;
        qreal scaleY = 1.0;
        bool linearInterpolation = true;
    };

    struct GradingKeyframe {
        int64_t frame = 0;
        qreal brightness = 0.0;
        qreal contrast = 1.0;
        qreal saturation = 1.0;
        qreal opacity = 1.0;
        qreal shadowsR = 0.0, shadowsG = 0.0, shadowsB = 0.0;
        qreal midtonesR = 0.0, midtonesG = 0.0, midtonesB = 0.0;
        qreal highlightsR = 0.0, highlightsG = 0.0, highlightsB = 0.0;
        QVector<QPointF> curvePointsR;
        QVector<QPointF> curvePointsG;
        QVector<QPointF> curvePointsB;
        QVector<QPointF> curvePointsLuma;
        bool curveThreePointLock = false;
        bool curveSmoothingEnabled = true;
        bool linearInterpolation = true;
    };

    struct BoolKeyframe {
        int64_t frame = 0;
        bool enabled = false;
    };

    struct TitleKeyframe {
        int64_t frame = 0;
        QString text;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal fontSize = 48.0;
        qreal opacity = 1.0;
#ifdef __APPLE__
        QString fontFamily = QStringLiteral("Helvetica Neue");
#else
        QString fontFamily = QStringLiteral("DejaVu Sans");
#endif
        bool bold = true;
        bool italic = false;
        QColor color = QColor(QStringLiteral("#ffffff"));
        bool dropShadowEnabled = true;
        QColor dropShadowColor = QColor(QStringLiteral("#000000"));
        qreal dropShadowOpacity = 0.6;
        qreal dropShadowOffsetX = 2.0;
        qreal dropShadowOffsetY = 2.0;
        bool windowEnabled = false;
        QColor windowColor = QColor(QStringLiteral("#000000"));
        qreal windowOpacity = 0.35;
        qreal windowPadding = 16.0;
        bool windowFrameEnabled = false;
        QColor windowFrameColor = QColor(QStringLiteral("#ffffffff"));
        qreal windowFrameOpacity = 1.0;
        qreal windowFrameWidth = 2.0;
        qreal windowFrameGap = 4.0;
        bool linearInterpolation = true;
    };

    struct TranscriptOverlaySettings {
        static constexpr int kMinReadableCharsPerLine = 8;
        static constexpr qreal kMinReadableBoxWidth = 160.0;
        static constexpr qreal kMinReadableBoxHeight = 80.0;
        static constexpr int kMinReadableFontPointSize = 12;

        bool enabled = false;
        bool showBackground = true;
        qreal backgroundOpacity = 120.0 / 255.0;
        qreal backgroundCornerRadius = 14.0;
        bool showShadow = true;
        bool showSpeakerTitle = false;
        bool autoScroll = false;
        bool useManualPlacement = false;
        qreal translationX = 0.0;
        qreal translationY = 0.0;
        qreal boxWidth = 900.0;
        qreal boxHeight = 220.0;
        int maxLines = 2;
        int maxCharsPerLine = 28;
#ifdef __APPLE__
        QString fontFamily = QStringLiteral("Helvetica Neue");
#else
        QString fontFamily = QStringLiteral("DejaVu Sans");
#endif
        int fontPointSize = 42;
        bool bold = true;
        bool italic = false;
        QColor textColor = QColor(QStringLiteral("#ffffff"));
        QColor backgroundColor = QColor(QStringLiteral("#000000"));
        QColor highlightColor = QColor(QStringLiteral("#fff2a8"));
        QColor highlightTextColor = QColor(QStringLiteral("#181818"));

        void normalizeReadableBounds() {
            boxWidth = qMax<qreal>(kMinReadableBoxWidth, boxWidth);
            boxHeight = qMax<qreal>(kMinReadableBoxHeight, boxHeight);
            maxCharsPerLine = qMax(kMinReadableCharsPerLine, maxCharsPerLine);
            fontPointSize = qMax(kMinReadableFontPointSize, fontPointSize);
            backgroundOpacity = qBound<qreal>(0.0, backgroundOpacity, 1.0);
            backgroundCornerRadius = qBound<qreal>(0.0, backgroundCornerRadius, 128.0);
        }
    };

    struct OpacityKeyframe {
        int64_t frame = 0;
        qreal opacity = 1.0;
        bool linearInterpolation = true;
    };

    struct CorrectionPolygon {
        QVector<QPointF> pointsNormalized;
        bool enabled = true;
        int64_t startFrame = 0;
        int64_t endFrame = -1;
    };

    QString id;
    QString filePath;
    QString proxyPath;
    bool useProxy = true;
    QString label;
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    QString audioSourceMode = QStringLiteral("embedded");
    QString audioSourcePath;
    QString audioSourceOriginalPath;
    QString audioSourceStatus = QStringLiteral("unknown");
    int audioStreamIndex = -1;
    QString audioBusId;
    qreal audioGain = 1.0;
    qreal audioPan = 0.0;
    bool audioSolo = false;
    bool audioLinkedToVideo = true;
    qint64 audioSourceLastVerifiedMs = 0;
    qreal sourceFps = static_cast<qreal>(kTimelineFps);
    int64_t sourceDurationFrames = 0;
    QSize sourceFrameSize;
    int64_t sourceInFrame = 0;
    int64_t sourceInSubframeSamples = 0;
    int64_t startFrame = 0;
    int64_t startSubframeSamples = 0;
    int64_t durationFrames = 90;
    int64_t durationSubframeSamples = 0;
    int trackIndex = 0;
    qreal playbackRate = 1.0;
    bool videoEnabled = true;
    bool audioEnabled = true;
    QColor color;
    qreal brightness = 0.0;
    qreal contrast = 1.0;
    qreal saturation = 1.0;
    qreal opacity = 1.0;
    qreal baseTranslationX = 0.0;
    qreal baseTranslationY = 0.0;
    qreal baseRotation = 0.0;
    qreal baseScaleX = 1.0;
    qreal baseScaleY = 1.0;
    bool speakerFramingEnabled = false;
    qreal speakerFramingBakedTargetXNorm = 0.5;
    qreal speakerFramingBakedTargetYNorm = 0.35;
    qreal speakerFramingBakedTargetBoxNorm = -1.0;
    qreal speakerFramingMinConfidence = 0.08;
    int speakerFramingManualTrackId = -1;
    QString speakerFramingManualStreamId;
    int speakerFramingCenterSmoothingFrames = 0;
    int speakerFramingZoomSmoothingFrames = 0;
    int speakerFramingSmoothingMode = 0;
    qreal speakerFramingCenterSmoothingStrength = 1.0;
    qreal speakerFramingZoomSmoothingStrength = 1.0;
    int speakerFramingGapHoldFrames = 0;
    int speakerSectionMinimumWords = 10;
    static constexpr qreal kSpeakerFramingSmoothingStrengthMax = 5.0;
    bool transformSkipAwareTiming = true;
    QVector<TransformKeyframe> transformKeyframes;
    QVector<BoolKeyframe> speakerFramingEnabledKeyframes;
    QVector<TransformKeyframe> speakerFramingKeyframes;
    QVector<TransformKeyframe> speakerFramingTargetKeyframes;
    QVector<GradingKeyframe> gradingKeyframes;
    QVector<OpacityKeyframe> opacityKeyframes;
    QVector<TitleKeyframe> titleKeyframes;
    TranscriptOverlaySettings transcriptOverlay;
    int fadeSamples = 250;
    bool locked = false;
    bool maskEnabled = false;
    QString maskFramesDir;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    qreal maskDilate = 0.0;
    qreal maskErode = 0.0;
    qreal maskBlur = 0.0;
    bool maskInvert = false;
    bool maskShowOnly = false;
    qreal maskOpacity = 1.0;
    bool maskGradeEnabled = false;
    qreal maskGradeBrightness = 0.0;
    qreal maskGradeContrast = 1.0;
    qreal maskGradeSaturation = 1.0;
    QVector<QPointF> maskGradeCurvePointsR;
    QVector<QPointF> maskGradeCurvePointsG;
    QVector<QPointF> maskGradeCurvePointsB;
    QVector<QPointF> maskGradeCurvePointsLuma;
    bool maskGradeCurveSmoothingEnabled = true;
    bool maskDropShadowEnabled = false;
    qreal maskDropShadowRadius = 12.0;
    qreal maskDropShadowOffsetX = 0.0;
    qreal maskDropShadowOffsetY = 4.0;
    qreal maskDropShadowOpacity = 0.45;
    QVector<CorrectionPolygon> correctionPolygons;
};

struct TimelineTrack {
    QString name;
    int height = 72;
    TrackVisualMode visualMode = TrackVisualMode::Enabled;
    bool audioEnabled = true;
    QString audioBusId;
    qreal audioGain = 1.0;
    bool audioMuted = false;
    bool audioSolo = false;
    bool audioWaveformVisible = true;
};
