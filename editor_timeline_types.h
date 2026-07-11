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

enum class ClipEffectPreset {
    None,
    NewsLogoTicker,
    PersonOrbit,
    AlternatingMotionBackground,
    FreezePattern,
    StepRepeat,
    DirectionalTrimTicker,
    SourceTile,
    Vulkan3DSynth,
};

enum class ClipTilingPattern {
    Grid,
    Encircle,
    SpiralXY,
    SpiralXZ,
    SpiralYZ,
    Diamond,
};

enum class ClipRole {
    Media,
    MaskMatte,
    EffectSynth,
    SpeakerTitle,
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
        qreal maskRepeatDeltaX = 0.0;
        qreal maskRepeatDeltaY = 0.0;
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
        enum class TextExtrudeMode {
            None = 0,
            StackedCopies = 1,
            ErodedSolid = 2,
        };
        enum class MaterialStyle {
            Solid = 0,
            Neon = 1,
            DiagonalStripes = 2,
            Grid = 3,
            ImagePattern = 4,
        };

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
        QString logoPath;
        MaterialStyle textMaterialStyle = MaterialStyle::Solid;
        QString textPatternImagePath;
        qreal textPatternScale = 1.0;
        bool dropShadowEnabled = true;
        QColor dropShadowColor = QColor(QStringLiteral("#000000"));
        qreal dropShadowOpacity = 0.6;
        qreal dropShadowOffsetX = 2.0;
        qreal dropShadowOffsetY = 2.0;
        bool windowEnabled = false;
        QColor windowColor = QColor(QStringLiteral("#000000"));
        qreal windowOpacity = 0.35;
        qreal windowPadding = 16.0;
        qreal windowWidth = 0.0;
        bool windowFrameEnabled = false;
        QColor windowFrameColor = QColor(QStringLiteral("#ffffffff"));
        qreal windowFrameOpacity = 1.0;
        qreal windowFrameWidth = 2.0;
        qreal windowFrameGap = 4.0;
        MaterialStyle windowFrameMaterialStyle = MaterialStyle::Solid;
        QString windowFramePatternImagePath;
        qreal windowFramePatternScale = 1.0;
        bool vulkan3DEnabled = false;
        bool vulkan3DExtrudeEnabled = false;
        TextExtrudeMode textExtrudeMode = TextExtrudeMode::None;
        qreal vulkan3DExtrudeDepth = 0.0;
        qreal vulkan3DBevelScale = 0.0;
        qreal vulkan3DYawDegrees = 0.0;
        qreal vulkan3DPitchDegrees = 0.0;
        qreal vulkan3DRollDegrees = 0.0;
        qreal vulkan3DDepth = 0.0;
        qreal vulkan3DScale = 1.0;
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
        qreal backgroundPadding = 16.0;
        bool backgroundFrameEnabled = false;
        QColor backgroundFrameColor = QColor(QStringLiteral("#ffffffff"));
        qreal backgroundFrameOpacity = 1.0;
        qreal backgroundFrameWidth = 2.0;
        qreal backgroundFrameGap = 4.0;
        bool showShadow = true;
        QColor shadowColor = QColor(QStringLiteral("#000000"));
        qreal shadowOpacity = 0.78;
        qreal shadowOffsetX = 5.0;
        qreal shadowOffsetY = 5.0;
        bool textOutlineEnabled = false;
        qreal textOutlineWidth = 0.0;
        QColor textOutlineColor = QColor(QStringLiteral("#000000"));
        qreal textOutlineOpacity = 0.80;
        TitleKeyframe::TextExtrudeMode textExtrudeMode =
            TitleKeyframe::TextExtrudeMode::None;
        qreal textExtrudeDepth = 0.16;
        qreal textExtrudeBevelScale = 0.7;
        bool showSpeakerTitle = false;
        bool highlightCurrentWord = true;
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
        qreal textOpacity = 1.0;
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
            backgroundPadding = qBound<qreal>(0.0, backgroundPadding, 400.0);
            backgroundFrameOpacity = qBound<qreal>(0.0, backgroundFrameOpacity, 1.0);
            backgroundFrameWidth = qBound<qreal>(0.0, backgroundFrameWidth, 120.0);
            backgroundFrameGap = qBound<qreal>(0.0, backgroundFrameGap, 200.0);
            textOpacity = qBound<qreal>(0.0, textOpacity, 1.0);
            shadowOpacity = qBound<qreal>(0.0, shadowOpacity, 1.0);
            shadowOffsetX = qBound<qreal>(-128.0, shadowOffsetX, 128.0);
            shadowOffsetY = qBound<qreal>(-128.0, shadowOffsetY, 128.0);
            textOutlineWidth = qBound<qreal>(0.0, textOutlineWidth, 24.0);
            textOutlineOpacity = qBound<qreal>(0.0, textOutlineOpacity, 1.0);
            textExtrudeDepth = qBound<qreal>(0.0, textExtrudeDepth, 2.0);
            textExtrudeBevelScale = qBound<qreal>(0.0, textExtrudeBevelScale, 2.0);
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
    ClipRole clipRole = ClipRole::Media;
    QString linkedSourceClipId;
    QString generatedFromMaskId;
    bool syncLockedToSource = false;
    bool sourceTransformLocked = false;
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
    bool effectSkipAwareTiming = true;
    QVector<TransformKeyframe> transformKeyframes;
    QVector<BoolKeyframe> speakerFramingEnabledKeyframes;
    QVector<TransformKeyframe> speakerFramingKeyframes;
    QVector<TransformKeyframe> speakerFramingTargetKeyframes;
    bool gradingPreviewEnabled = true;
    QVector<GradingKeyframe> gradingKeyframes;
    QVector<OpacityKeyframe> opacityKeyframes;
    QVector<TitleKeyframe> titleKeyframes;
    bool speakerTitleEngineActive = false;
    TranscriptOverlaySettings transcriptOverlay;
    int fadeSamples = 250;
    bool locked = false;
    bool maskEnabled = false;
    QString maskFramesDir;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    int maskFeatherFalloff = 0; // 0 power, 1 linear, 2 smoothstep, 3 smootherstep, 4 cosine, 5 gaussian
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
    bool maskForegroundLayerEnabled = false;
    bool maskRepeatEnabled = false;
    qreal maskRepeatDeltaX = 160.0;
    qreal maskRepeatDeltaY = 0.0;
    ClipEffectPreset effectPreset = ClipEffectPreset::None;
    int effectRows = 32;
    qreal effectSpeed = 1.0;
    qreal effectScale = 1.0;
    bool effectAlternateDirection = true;
    ClipTilingPattern tilingPattern = ClipTilingPattern::Grid;
    qreal tilingSpacing = 1.0;
    bool tilingWrap = true;
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
    ClipEffectPreset effectPreset = ClipEffectPreset::None;
    int effectRows = 32;
    qreal effectSpeed = 1.0;
    qreal effectScale = 1.0;
    bool effectAlternateDirection = true;
    ClipTilingPattern tilingPattern = ClipTilingPattern::Grid;
    qreal tilingSpacing = 1.0;
    bool tilingWrap = true;
};
