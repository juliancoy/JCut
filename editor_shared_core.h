#pragma once

#include "editor_action_result.h"
#include "editor_playback_types.h"
#include "editor_timeline_types.h"
#include "timeline_fps.h"

#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <memory>

struct MediaProbeResult {
    ClipMediaType mediaType = ClipMediaType::Unknown;
    MediaSourceKind sourceKind = MediaSourceKind::File;
    bool hasAudio = false;
    bool hasVideo = false;
    bool hasAlpha = false;
    int64_t durationFrames = 120;
    QString codecName;
    QSize frameSize;
    double fps = static_cast<double>(kTimelineFps);
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

struct TranscriptSentenceRun {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    QString text;
};

struct TranscriptRuntimeDocument {
    qint64 mtimeMs = -1;
    qint64 fileSize = -1;
    QVector<TranscriptSection> sections;
    QHash<QString, QVector<TranscriptSentenceRun>> sentenceRunsBySpeaker;
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

constexpr int kAudioSampleRate = 48000;
constexpr int64_t kSamplesPerFrame = kAudioSampleRate / kTimelineFps;
constexpr int64_t kAudioNudgeSamples = (kAudioSampleRate * 25) / 1000;
