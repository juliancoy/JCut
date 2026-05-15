#pragma once

#include "editor_action_result.h"
#include "editor_playback_types.h"
#include "editor_timeline_types.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <memory>

#include <cstdint>

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

struct TranscriptRuntimeDocument {
    qint64 mtimeMs = -1;
    qint64 fileSize = -1;
    QVector<TranscriptSection> sections;
};

struct SpeakerProfile {
    QString speakerId;
    QString name;
    QString organization;
    QString description;
    QString avatarPath;
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
QRect previewCanvasBaseRectForWidget(const QRect& widgetRect,
                                     const QSize& outputSize,
                                     int marginPx = 36);
QRectF previewCanvasBaseRectForWidgetF(const QRectF& widgetRect,
                                       const QSize& outputSize,
                                       qreal marginPx = 36.0);
QRect scaledPreviewCanvasRect(const QRect& baseRect,
                              qreal previewZoom,
                              const QPointF& previewPanOffset = QPointF());
QRectF scaledPreviewCanvasRectF(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset = QPointF());
QPointF clampedPreviewPanOffset(const QRectF& baseRect,
                                qreal previewZoom,
                                const QPointF& previewPanOffset);
QPointF previewCanvasScaleForTargetRect(const QRect& targetRect,
                                        const QSize& outputSize);
QPointF previewCanvasScaleForTargetRectF(const QRectF& targetRect,
                                         const QSize& outputSize);
QRect previewFitRectToBounds(const QSize& source, const QRect& bounds);
QRectF previewFitRectToBoundsF(const QSize& source, const QRectF& bounds);
qreal resolvedSourceFps(const TimelineClip& clip);
qreal timelineFrameToSeconds(int64_t timelineFrame);
QRectF normalizedCenterBoxRect(qreal xNorm, qreal yNorm, qreal boxSizeNorm, const QSizeF& frameSizePx);
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
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtFrame(const TimelineClip& clip,
                                                                  int64_t timelineFrame,
                                                                  const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipSpeakerFramingAtPosition(const TimelineClip& clip,
                                                                     qreal timelineFramePosition,
                                                                     const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe composeClipTransforms(const TimelineClip::TransformKeyframe& base,
                                                      const TimelineClip::TransformKeyframe& overlay);
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtFrame(const TimelineClip& clip,
                                                                   int64_t timelineFrame,
                                                                   const QSize& outputSize = QSize());
TimelineClip::TransformKeyframe evaluateClipRenderTransformAtPosition(const TimelineClip& clip,
                                                                      qreal timelineFramePosition,
                                                                      const QSize& outputSize = QSize());
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
QVector<QPointF> defaultGradingCurvePoints();
QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points);
qreal sampleGradingCurveAt(const QVector<QPointF>& points,
                           qreal xNorm,
                           bool smoothingEnabled = true);
QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points,
                                 int samples = TimelineClip::kGradingCurveLutSize,
                                 bool smoothingEnabled = true);
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
SpeakerProfile speakerProfileFromJson(const QString& speakerId, const QJsonObject& profileObj);
QJsonObject speakerProfileToJson(const SpeakerProfile& profile, const QJsonObject& base = QJsonObject());
QString transcriptEditablePathForClipFile(const QString& filePath);
QString transcriptWorkingPathForClipFile(const QString& filePath);
QStringList transcriptCutPathsForClipFile(const QString& filePath);
QString activeTranscriptPathForClipFile(const QString& filePath);
bool facestreamSidecarExistsForClipFile(const QString& filePath);
void setActiveTranscriptPathForClipFile(const QString& filePath, const QString& transcriptPath);
void clearActiveTranscriptPathForClipFile(const QString& filePath);
void clearAllActiveTranscriptPaths();
bool ensureEditableTranscriptForClipFile(const QString& filePath, QString* editablePathOut = nullptr);
bool loadTranscriptJsonCached(const QString& transcriptPath, QJsonDocument* documentOut);
std::shared_ptr<const TranscriptRuntimeDocument> loadTranscriptRuntimeDocument(const QString& transcriptPath);
QVector<TranscriptSection> loadTranscriptSections(const QString& transcriptPath);
QPointF transcriptSpeakerLocationForSourceFrame(const QString& transcriptPath,
                                                const QVector<TranscriptSection>& sections,
                                                int64_t sourceFrame,
                                                bool* okOut = nullptr);
bool transcriptSpeakerTrackingSampleForClipFileAtSourceFrame(const QString& clipFilePath,
                                                             const QString& speakerId,
                                                             int64_t sourceFrame,
                                                             qreal minConfidence,
                                                             QPointF* locationOut,
                                                             qreal* boxSizeOut);
void invalidateTranscriptSpeakerProfileCache(const QString& transcriptPath = QString());
void invalidateTranscriptJsonCache(const QString& transcriptPath = QString());
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
QString transcriptSpeakerTitleForSourceFrame(const QString& transcriptPath,
                                             const QVector<TranscriptSection>& sections,
                                             int64_t sourceFrame);
SpeakerProfile transcriptSpeakerProfileForSourceFrame(const QString& transcriptPath,
                                                      const QVector<TranscriptSection>& sections,
                                                      int64_t sourceFrame);
