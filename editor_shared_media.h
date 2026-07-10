#pragma once

#include "editor_shared_core.h"

QString clipMediaTypeToString(ClipMediaType type);
ClipMediaType clipMediaTypeFromString(const QString& value);
QString clipMediaTypeLabel(ClipMediaType type);
QString mediaSourceKindToString(MediaSourceKind kind);
MediaSourceKind mediaSourceKindFromString(const QString& value);
QString mediaSourceKindLabel(MediaSourceKind kind);
QString trackVisualModeToString(TrackVisualMode mode);
TrackVisualMode trackVisualModeFromString(const QString& value);
QString trackVisualModeLabel(TrackVisualMode mode);

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
bool clipProvidesMediaForVisibleMaskMatte(const TimelineClip& source,
                                          const QVector<TimelineClip>& clips,
                                          const QVector<TimelineTrack>& tracks);
bool clipAudioPlaybackEnabled(const TimelineClip& clip);
bool clipHasAlpha(const TimelineClip& clip);

MediaProbeResult probeMediaFile(const QString& filePath, qreal fallbackSeconds = 4.0);
qreal effectiveFpsForClip(const TimelineClip& clip);
QString playbackProxyPathForClip(const TimelineClip& clip);
QString playbackMediaPathForClip(const TimelineClip& clip);
void refreshClipAudioSource(TimelineClip& clip);
QString playbackAudioPathForClip(const TimelineClip& clip);
bool playbackUsesAlternateAudioSource(const TimelineClip& clip);
QString interactivePreviewMediaPathForClip(const TimelineClip& clip);
// True when this host has the Linux GPU interop (CUDA or VAAPI render node)
// that the end-to-end zero-copy decode contract requires. Callers that
// assert --require-zero-copy must consult this; on other hosts the explicit
// CPU-upload compatibility opt-in is the honest mode.
bool zeroCopyInteropEnvironmentDetected();
bool isVariableFrameRate(const QString& path);
bool isImageSequencePath(const QString& path);
QStringList imageSequenceFramePaths(const QString& path);
QString imageSequenceDisplayLabel(const QString& path);
