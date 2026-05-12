#pragma once

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

    struct TransformKeyframe {
        int64_t frame = 0;
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
        bool enabled = false;
        bool showBackground = true;
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
    qint64 audioSourceLastVerifiedMs = 0;
    qreal sourceFps = 30.0;
    int64_t sourceDurationFrames = 0;
    QSize sourceFrameSize;
    int64_t sourceInFrame = 0;
    int64_t sourceInSubframeSamples = 0;
    int64_t startFrame = 0;
    int64_t startSubframeSamples = 0;
    int64_t durationFrames = 90;
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
    QString speakerFramingSpeakerId;
    qreal speakerFramingTargetXNorm = 0.5;
    qreal speakerFramingTargetYNorm = 0.35;
    qreal speakerFramingTargetBoxNorm = -1.0;
    qreal speakerFramingBakedTargetXNorm = 0.5;
    qreal speakerFramingBakedTargetYNorm = 0.35;
    qreal speakerFramingBakedTargetBoxNorm = -1.0;
    qreal speakerFramingMinConfidence = 0.08;
    bool transformSkipAwareTiming = true;
    QVector<TransformKeyframe> transformKeyframes;
    QVector<TransformKeyframe> speakerFramingKeyframes;
    QVector<GradingKeyframe> gradingKeyframes;
    QVector<OpacityKeyframe> opacityKeyframes;
    QVector<TitleKeyframe> titleKeyframes;
    TranscriptOverlaySettings transcriptOverlay;
    int fadeSamples = 250;
    bool locked = false;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    QVector<CorrectionPolygon> correctionPolygons;
};

struct TimelineTrack {
    QString name;
    int height = 44;
    TrackVisualMode visualMode = TrackVisualMode::Enabled;
    bool audioEnabled = true;
};
