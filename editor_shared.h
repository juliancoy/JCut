#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QStringList>
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
    ImageSequence,  // JPEG frames in a directory (default — best compatibility)
    H264,           // H.264 in MOV/MP4 (small files, needs sequential decode)
    MJPEG,          // Motion JPEG in MOV (intra-frame, Linux only)
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
        // Basic grading
        qreal brightness = 0.0;
        qreal contrast = 1.0;
        qreal saturation = 1.0;
        // Derived during evaluation; serialized opacity lives in opacityKeyframes.
        qreal opacity = 1.0;
        // Shadows/Midtones/Highlights (Lift/Gamma/Gain style)
        qreal shadowsR = 0.0, shadowsG = 0.0, shadowsB = 0.0;
        qreal midtonesR = 0.0, midtonesG = 0.0, midtonesB = 0.0;
        qreal highlightsR = 0.0, highlightsG = 0.0, highlightsB = 0.0;
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
        bool autoScroll = false;
        bool useManualPlacement = false;
        qreal translationX = 0.0;
        qreal translationY = 640.0;
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
        int64_t startFrame = 0; // Clip-local frame (inclusive)
        int64_t endFrame = -1;  // Clip-local frame (inclusive), -1 means until clip end
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
    bool transformSkipAwareTiming = true;
    QVector<TransformKeyframe> transformKeyframes;
    QVector<GradingKeyframe> gradingKeyframes;
    QVector<OpacityKeyframe> opacityKeyframes;
    QVector<TitleKeyframe> titleKeyframes;
    TranscriptOverlaySettings transcriptOverlay;
    int fadeSamples = 250;  // Crossfade with previous audio clip (0 = no fade)
    bool locked = false;    // When true, prevents temporal adjustments
    qreal maskFeather = 0.0; // Mask feathering radius in pixels (0 = disabled, only applies to clips with alpha)
    qreal maskFeatherGamma = 1.0; // Feather curve power (1.0 = linear, <1.0 = sharper edges, >1.0 = softer edges)
    QVector<CorrectionPolygon> correctionPolygons; // Erase polygons in normalized source coordinates.
};

struct TimelineTrack {
    QString name;
    int height = 44;
    TrackVisualMode visualMode = TrackVisualMode::Enabled;
    bool audioEnabled = true;
};

struct ExportRangeSegment {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
};

enum class RenderSyncAction {
    DuplicateFrame,
    SkipFrame,
};

enum class PlaybackClockSource {
    Auto,
    Audio,
    Timeline,
};

enum class PlaybackAudioWarpMode {
    Disabled,
    Varispeed,
    TimeStretch,
};

struct RenderSyncMarker {
    QString clipId;
    int64_t frame = 0;
    RenderSyncAction action = RenderSyncAction::DuplicateFrame;
    int count = 1;
};

struct MediaProbeResult {
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    bool hasVideo = false;
    bool hasAlpha = false;
    int64_t durationFrames = 120;
    QString codecName;
    QSize frameSize;
    double fps = 30.0;
};

struct TranscriptWord {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QString speaker;
    QString text;
    bool skipped = false;
};

struct TranscriptSection {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QString text;
    QVector<TranscriptWord> words;
};

struct TranscriptOverlayLine {
    QStringList words;
    int activeWord = -1;
};

struct TranscriptOverlayLayout {
    QVector<TranscriptOverlayLine> lines;
    bool truncatedTop = false;
    bool truncatedBottom = false;
};

struct EffectiveVisualEffects {
    TimelineClip::GradingKeyframe grading;
    qreal maskFeather = 0.0;
    qreal maskFeatherGamma = 1.0;
    QVector<TimelineClip::CorrectionPolygon> correctionPolygons;
};

#ifdef __APPLE__
inline const QString kDefaultFontFamily = QStringLiteral("Helvetica Neue");
#else
inline const QString kDefaultFontFamily = QStringLiteral("DejaVu Sans");
#endif

constexpr int kTimelineFps = 30;
constexpr int kAudioSampleRate = 48000;
constexpr int64_t kSamplesPerFrame = kAudioSampleRate / kTimelineFps;
constexpr int64_t kAudioNudgeSamples = (kAudioSampleRate * 25) / 1000;

QString clipMediaTypeToString(ClipMediaType type);
ClipMediaType clipMediaTypeFromString(const QString& value);
QString clipMediaTypeLabel(ClipMediaType type);
QString mediaSourceKindToString(MediaSourceKind kind);
MediaSourceKind mediaSourceKindFromString(const QString& value);
QString mediaSourceKindLabel(MediaSourceKind kind);
QString trackVisualModeToString(TrackVisualMode mode);
TrackVisualMode trackVisualModeFromString(const QString& value);
QString trackVisualModeLabel(TrackVisualMode mode);

QString renderSyncActionToString(RenderSyncAction action);
RenderSyncAction renderSyncActionFromString(const QString& value);
QString renderSyncActionLabel(RenderSyncAction action);
QString playbackClockSourceToString(PlaybackClockSource source);
PlaybackClockSource playbackClockSourceFromString(const QString& value);
QString playbackClockSourceLabel(PlaybackClockSource source);
QString playbackAudioWarpModeToString(PlaybackAudioWarpMode mode);
PlaybackAudioWarpMode playbackAudioWarpModeFromString(const QString& value);
QString playbackAudioWarpModeLabel(PlaybackAudioWarpMode mode);
qreal normalizedPlaybackSpeed(qreal speed);
PlaybackAudioWarpMode normalizedPlaybackAudioWarpMode(qreal playbackSpeed, PlaybackAudioWarpMode mode);
qreal effectivePlaybackAudioWarpRate(qreal playbackSpeed, PlaybackAudioWarpMode mode);
bool shouldUseAudioMasterClock(PlaybackClockSource source,
                               PlaybackAudioWarpMode mode,
                               qreal playbackSpeed,
                               bool hasPlayableAudio);

bool clipHasVisuals(const TimelineClip& clip);
bool clipIsAudioOnly(const TimelineClip& clip);
bool clipHasCorrections(const TimelineClip& clip);
bool correctionPolygonActiveAtTimelineFrame(const TimelineClip& clip,
                                            const TimelineClip::CorrectionPolygon& polygon,
                                            int64_t timelineFrame);
bool correctionPolygonActiveAtTimelinePosition(const TimelineClip& clip,
                                               const TimelineClip::CorrectionPolygon& polygon,
                                               qreal timelineFramePosition);
bool clipVisualPlaybackEnabled(const TimelineClip& clip);
TrackVisualMode trackVisualModeForClip(const TimelineClip& clip, const QVector<TimelineTrack>& tracks);
bool clipVisualPlaybackEnabled(const TimelineClip& clip, const QVector<TimelineTrack>& tracks);
bool clipAudioPlaybackEnabled(const TimelineClip& clip);
bool clipHasAlpha(const TimelineClip& clip);

int64_t frameToSamples(int64_t frame);
qreal samplesToFramePosition(int64_t samples);
qreal resolvedSourceFps(const TimelineClip& clip);
int64_t sourceFramesToSamples(const TimelineClip& clip, qreal sourceFrames);
int64_t clipTimelineStartSamples(const TimelineClip& clip);
int64_t clipSourceInSamples(const TimelineClip& clip);
void normalizeSubframeTiming(int64_t& frame, int64_t& subframeSamples);
void normalizeClipTiming(TimelineClip& clip);

QString transformInterpolationLabel(bool linearInterpolation);
qreal sanitizeScaleValue(qreal value);
void normalizeClipTransformKeyframes(TimelineClip& clip);
void normalizeClipGradingKeyframes(TimelineClip& clip);
void normalizeClipOpacityKeyframes(TimelineClip& clip);
void normalizeClipTitleKeyframes(TimelineClip& clip);
TimelineClip::TransformKeyframe evaluateClipKeyframeOffsetAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::TransformKeyframe evaluateClipTransformAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
qreal evaluateClipOpacityAtFrame(const TimelineClip& clip, int64_t timelineFrame);
qreal evaluateClipOpacityAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
qreal evaluateEffectiveClipOpacityAtFrame(const TimelineClip& clip,
                                          const QVector<TimelineTrack>& tracks,
                                          int64_t timelineFrame);
qreal evaluateEffectiveClipOpacityAtPosition(const TimelineClip& clip,
                                             const QVector<TimelineTrack>& tracks,
                                             qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip,
                                                                  const QVector<TimelineTrack>& tracks,
                                                                  int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame);
TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame,
                                                             const QVector<RenderSyncMarker>& markers);
EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers);
int64_t adjustedClipLocalFrameAtTimelineFrame(const TimelineClip& clip,
                                              int64_t localTimelineFrame,
                                              const QVector<RenderSyncMarker>& markers);
int64_t sourceFrameForClipAtTimelinePosition(const TimelineClip& clip,
                                             qreal timelineFramePosition,
                                             const QVector<RenderSyncMarker>& markers);
int64_t sourceSampleForClipAtTimelineSample(const TimelineClip& clip,
                                            int64_t timelineSample,
                                            const QVector<RenderSyncMarker>& markers);
int64_t transcriptFrameForClipAtTimelineSample(const TimelineClip& clip,
                                               int64_t timelineSample,
                                               const QVector<RenderSyncMarker>& markers);

MediaProbeResult probeMediaFile(const QString& filePath, qreal fallbackSeconds = 4.0);
QImage applyClipGrade(const QImage& source, const TimelineClip& clip);
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade);
QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects);
QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma = 1.0);
qreal effectiveFpsForClip(const TimelineClip& clip);
QString playbackProxyPathForClip(const TimelineClip& clip);
QString playbackMediaPathForClip(const TimelineClip& clip);
void refreshClipAudioSource(TimelineClip& clip);
QString playbackAudioPathForClip(const TimelineClip& clip);
bool playbackUsesAlternateAudioSource(const TimelineClip& clip);
QString interactivePreviewMediaPathForClip(const TimelineClip& clip);
bool isVariableFrameRate(const QString& path);
bool isImageSequencePath(const QString& path);
QStringList imageSequenceFramePaths(const QString& path);
QString imageSequenceDisplayLabel(const QString& path);
QString transcriptPathForClipFile(const QString& filePath);
QString transcriptEditablePathForClipFile(const QString& filePath);
QString transcriptWorkingPathForClipFile(const QString& filePath);
QStringList transcriptCutPathsForClipFile(const QString& filePath);
QString activeTranscriptPathForClipFile(const QString& filePath);
void setActiveTranscriptPathForClipFile(const QString& filePath, const QString& transcriptPath);
void clearActiveTranscriptPathForClipFile(const QString& filePath);
void clearAllActiveTranscriptPaths();
bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut = nullptr);
QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath);
QPointF transcriptSpeakerLocationForSourceFrame(const QString& transcriptPath,
                                                const QVector<TranscriptSection>& sections,
                                                int64_t sourceFrame,
                                                bool* okOut = nullptr);
void invalidateTranscriptSpeakerProfileCache(const QString& transcriptPath = QString());
QJsonObject transcriptSpeakerTrackingConfigSnapshot();
bool applyTranscriptSpeakerTrackingConfigPatch(const QJsonObject& patch,
                                               QString* errorOut = nullptr);
QJsonObject transcriptSpeakerTrackingProfilingSnapshot();
void resetTranscriptSpeakerTrackingProfiling();
QPointF transcriptOverlayTranslationForOutput(const TimelineClip& clip,
                                              const QSize& outputSize,
                                              const QString& transcriptPath,
                                              const QVector<TranscriptSection>& sections,
                                              int64_t sourceFrame);
QRectF transcriptOverlayRectInOutputSpace(const TimelineClip& clip,
                                          const QSize& outputSize,
                                          const QString& transcriptPath,
                                          const QVector<TranscriptSection>& sections,
                                          int64_t sourceFrame);
int transcriptOverlayEffectiveLinesForBox(const TimelineClip& clip);
int transcriptOverlayEffectiveCharsForBox(const TimelineClip& clip);
TranscriptOverlayLayout transcriptOverlayLayoutAtSourceFrame(const TimelineClip& clip,
                                                            const QVector<TranscriptSection>& sections,
                                                            int64_t sourceFrame);
QString wrappedTranscriptSectionText(const QString& text, int maxCharsPerLine, int maxLines);
TranscriptOverlayLayout layoutTranscriptSection(const TranscriptSection& section,
                                               int64_t sourceFrame,
                                               int maxCharsPerLine,
                                               int maxLines,
                                               bool autoScroll);
QString transcriptOverlayHtml(const TranscriptOverlayLayout& layout,
                              const QColor& textColor,
                              const QColor& highlightTextColor,
                              const QColor& highlightFillColor);
