#include "editor_shared_effects.h"
#include "core/image_file_decoder.h"
#include "mask_frame_map_core.h"
#include "editor_grading_core.h"
#include "frame_handle.h"
#include "editor_shared_keyframes.h"
#include "editor_shared_media.h"
#include "editor_shared_render_sync.h"
#include "editor_shared_transcript.h"
#include "mask_sidecar.h"
#include "transform_skip_aware_timing.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QtMath>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>

bool trackHasEffectPreset(const TimelineTrack& track)
{
    return track.effectPreset != ClipEffectPreset::None;
}

TimelineClip clipWithTrackEffectSettings(const TimelineClip& clip, const QVector<TimelineTrack>& tracks)
{
    Q_UNUSED(tracks);
    // Effects are clip-owned. A track/canvas may have its own effect state,
    // but that state must never be inherited by clips placed beneath it.
    return clip;
}

bool effectPresetSupportedForClipRole(ClipEffectPreset preset, ClipRole role)
{
    return role != ClipRole::MaskMatte ||
           (preset != ClipEffectPreset::DifferenceMatte &&
            preset != ClipEffectPreset::TemporalEcho);
}

TimelineClip clipWithRenderableEffectSettings(const TimelineClip& clip,
                                              const QVector<TimelineTrack>& tracks)
{
    TimelineClip renderable = clipWithTrackEffectSettings(clip, tracks);
    if (!effectPresetSupportedForClipRole(renderable.effectPreset, renderable.clipRole)) {
        renderable.effectPreset = ClipEffectPreset::None;
    }
    return renderable;
}

namespace {
qreal effectTimelinePositionForClip(
    const TimelineClip& clip,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers,
    const PlaybackTimingContext& timing);
}

TimelineClip evaluateClipEffectAnimationAtPosition(const TimelineClip& clip,
                                                   qreal timelineFramePosition)
{
    return evaluateClipEffectAnimationAtPosition(
        clip,
        timelineFramePosition,
        {},
        activePlaybackTimingContext());
}

TimelineClip evaluateClipEffectAnimationAtPosition(
    const TimelineClip& clip,
    qreal timelineFramePosition,
    const QVector<RenderSyncMarker>& markers,
    const PlaybackTimingContext& timing)
{
    TimelineClip evaluated = clip;
    const qreal localFrame = qBound<qreal>(
        0.0,
        timelineFramePosition - static_cast<qreal>(clip.startFrame),
        static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1)));
    bool enabled = clip.effectEnabled;
    QVector<TimelineClip::BoolKeyframe> keyframes = clip.effectEnabledKeyframes;
    std::sort(keyframes.begin(), keyframes.end(),
              [](const auto& left, const auto& right) {
                  return left.frame < right.frame;
              });
    for (const TimelineClip::BoolKeyframe& keyframe : keyframes) {
        if (static_cast<qreal>(keyframe.frame) > localFrame) break;
        enabled = keyframe.enabled;
    }
    if (!enabled) {
        evaluated.effectPreset = ClipEffectPreset::None;
        evaluated.edgeFillEffect = BackgroundFillEffect::None;
        return evaluated;
    }

    const QString mode = clip.effectModulationMode.trimmed().toLower();
    if (mode == QStringLiteral("none")) {
        return evaluated;
    }
    qreal modulationLocalFrame = localFrame;
    if (clip.effectSkipAwareTiming) {
        modulationLocalFrame = qBound<qreal>(
            0.0,
            effectTimelinePositionForClip(
                clip, timelineFramePosition, markers, timing) -
                static_cast<qreal>(clip.startFrame),
            static_cast<qreal>(qMax<int64_t>(
                0, clip.durationFrames - 1)));
    }
    const qreal seconds =
        modulationLocalFrame / static_cast<qreal>(kTimelineFps);
    qreal delta = 0.0;
    if (mode == QStringLiteral("lfo")) {
        constexpr qreal kTwoPi = 6.28318530717958647692;
        const qreal phase =
            qDegreesToRadians(clip.effectModulationPhaseDegrees);
        delta = clip.effectModulationAmount *
                std::sin(kTwoPi * clip.effectModulationRate * seconds + phase);
    } else if (mode == QStringLiteral("steady_increase")) {
        delta = clip.effectModulationAmount * seconds;
    }

    const QString target = clip.effectModulationTarget.trimmed().toLower();
    if (target == QStringLiteral("rows")) {
        evaluated.effectRows = qBound(
            1, qRound(static_cast<qreal>(clip.effectRows) + delta), 512);
    } else if (target == QStringLiteral("speed")) {
        evaluated.effectSpeed =
            qBound<qreal>(-8.0, clip.effectSpeed + delta, 8.0);
    } else if (target == QStringLiteral("spacing")) {
        evaluated.tilingSpacing =
            qBound<qreal>(0.1, clip.tilingSpacing + delta, 8.0);
    } else {
        evaluated.effectScale =
            qBound<qreal>(0.1, clip.effectScale + delta, 8.0);
    }
    return evaluated;
}

namespace {
std::vector<jcut::EditorPoint> editorCurveFromQt(
    const QVector<QPointF>& points)
{
    std::vector<jcut::EditorPoint> result;
    result.reserve(static_cast<std::size_t>(points.size()));
    for (const QPointF& point : points) {
        result.push_back({point.x(), point.y()});
    }
    return result;
}

QVector<QPointF> qtCurveFromEditor(
    const std::vector<jcut::EditorPoint>& points)
{
    QVector<QPointF> result;
    result.reserve(static_cast<qsizetype>(points.size()));
    for (const jcut::EditorPoint& point : points) {
        result.push_back(QPointF(point.x, point.y));
    }
    return result;
}

QVector<quint8> qtCurveLutFromEditor(
    const std::vector<std::uint8_t>& values)
{
    QVector<quint8> result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const std::uint8_t value : values) {
        result.push_back(static_cast<quint8>(value));
    }
    return result;
}

int clampChannel(int value) {
    return qBound(0, value, 255);
}

TimelineClip::GradingKeyframe maskGradeForClip(const TimelineClip& clip)
{
    TimelineClip::GradingKeyframe grade;
    grade.brightness = clip.maskGradeBrightness;
    grade.contrast = clip.maskGradeContrast;
    grade.saturation = clip.maskGradeSaturation;
    grade.curvePointsR = clip.maskGradeCurvePointsR;
    grade.curvePointsG = clip.maskGradeCurvePointsG;
    grade.curvePointsB = clip.maskGradeCurvePointsB;
    grade.curvePointsLuma = clip.maskGradeCurvePointsLuma;
    grade.curveSmoothingEnabled = clip.maskGradeCurveSmoothingEnabled;
    return grade;
}

QCache<QString, QImage>& preparedMaskCache()
{
    static QCache<QString, QImage> cache(256 * 1024);
    return cache;
}

QMutex& preparedMaskCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

struct RawMaskCacheCore {
    static constexpr std::size_t kMaximumBytes = 256ull * 1024ull * 1024ull;

    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<jcut::core::ImageBuffer>> images;
    std::unordered_set<std::string> loadsInFlight;
    std::deque<std::string> insertionOrder;
    std::size_t storedBytes = 0;
};

RawMaskCacheCore& rawMaskCacheCore()
{
    static RawMaskCacheCore cache;
    return cache;
}

class RawMaskDecodeExecutor {
public:
    RawMaskDecodeExecutor()
    {
        const unsigned reportedConcurrency = std::thread::hardware_concurrency();
        const unsigned workerCount =
            std::clamp(reportedConcurrency == 0 ? 2u : reportedConcurrency, 1u, 4u);
        workers_.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() { run(); });
        }
    }

    ~RawMaskDecodeExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

RawMaskDecodeExecutor& rawMaskDecodeExecutor()
{
    static RawMaskDecodeExecutor executor;
    return executor;
}

QImage qtImageFromCoreBuffer(
    const std::shared_ptr<const jcut::core::ImageBuffer>& buffer)
{
    if (!buffer || buffer->empty()) {
        return {};
    }
    auto* retained =
        new std::shared_ptr<const jcut::core::ImageBuffer>(buffer);
    const QImage::Format format =
        buffer->format == jcut::core::PixelFormat::Gray8
            ? QImage::Format_Grayscale8
            : QImage::Format_RGBA8888;
    return QImage(
        buffer->bytes.data(),
        buffer->size.width,
        buffer->size.height,
        buffer->strideBytes,
        format,
        [](void* value) {
            delete static_cast<
                std::shared_ptr<const jcut::core::ImageBuffer>*>(value);
        },
        retained);
}

struct FileVersionSignature {
    bool valid = false;
    qint64 size = -1;
    qint64 modifiedNanoseconds = -1;
    qint64 changedNanoseconds = -1;
    quint64 device = 0;
    quint64 inode = 0;

    bool operator==(const FileVersionSignature& other) const
    {
        return valid == other.valid && size == other.size &&
            modifiedNanoseconds == other.modifiedNanoseconds &&
            changedNanoseconds == other.changedNanoseconds &&
            device == other.device && inode == other.inode;
    }

    bool operator!=(const FileVersionSignature& other) const
    {
        return !(*this == other);
    }
};

FileVersionSignature fileVersionSignature(const QString& path)
{
    FileVersionSignature signature;
#if defined(Q_OS_WIN)
    struct _stat64 fileStat {};
    const std::wstring nativePath = QFileInfo(path).absoluteFilePath().toStdWString();
    if (::_wstat64(nativePath.c_str(), &fileStat) != 0 ||
        (fileStat.st_mode & _S_IFREG) == 0 || fileStat.st_size < 0) {
        return signature;
    }
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtime) * 1000000000;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctime) * 1000000000;
#else
    struct stat fileStat {};
    const QByteArray nativePath = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    if (::stat(nativePath.constData(), &fileStat) != 0 ||
        !S_ISREG(fileStat.st_mode) || fileStat.st_size < 0) {
        return signature;
    }
#if defined(Q_OS_DARWIN)
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtimespec.tv_sec) * 1000000000 +
        fileStat.st_mtimespec.tv_nsec;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctimespec.tv_sec) * 1000000000 +
        fileStat.st_ctimespec.tv_nsec;
#else
    signature.modifiedNanoseconds =
        static_cast<qint64>(fileStat.st_mtim.tv_sec) * 1000000000 +
        fileStat.st_mtim.tv_nsec;
    signature.changedNanoseconds =
        static_cast<qint64>(fileStat.st_ctim.tv_sec) * 1000000000 +
        fileStat.st_ctim.tv_nsec;
#endif
#endif
    signature.valid = true;
    signature.size = static_cast<qint64>(fileStat.st_size);
    signature.device = static_cast<quint64>(fileStat.st_dev);
    signature.inode = static_cast<quint64>(fileStat.st_ino);
    return signature;
}

std::optional<int64_t> mappedMaskFrameForSourceFrame(const TimelineClip& clip,
                                                     int64_t sourceFrame)
{
    return jcut::masks::mappedMaskFrameForSourceFrameCore(
        std::filesystem::path(clip.maskFramesDir.toStdString()),
        std::filesystem::path(clip.filePath.toStdString()),
        sourceFrame);
}

qreal effectTimelinePositionForClip(const TimelineClip& clip,
                                    qreal timelineFramePosition,
                                    const QVector<RenderSyncMarker>& markers,
                                    const PlaybackTimingContext& timing) {
    qreal adjustedTimelineFramePosition = timelineFramePosition;

    if (clip.effectSkipAwareTiming && !markers.isEmpty()) {
        const qreal maxLocalFrame = static_cast<qreal>(qMax<int64_t>(0, clip.durationFrames - 1));
        const qreal localTimelineFrame =
            qBound<qreal>(0.0, timelineFramePosition - static_cast<qreal>(clip.startFrame), maxLocalFrame);
        const int64_t steppedLocalTimelineFrame =
            qMax<int64_t>(0, static_cast<int64_t>(std::floor(localTimelineFrame)));
        const qreal fractional = localTimelineFrame - static_cast<qreal>(steppedLocalTimelineFrame);
        const int64_t adjustedLocalFrame =
            adjustedClipLocalFrameAtTimelineFrame(clip, steppedLocalTimelineFrame, markers);
        const qreal adjustedLocalFramePosition =
            qBound<qreal>(0.0, static_cast<qreal>(adjustedLocalFrame) + fractional, maxLocalFrame);
        adjustedTimelineFramePosition = static_cast<qreal>(clip.startFrame) + adjustedLocalFramePosition;
    }

    return static_cast<qreal>(clip.startFrame) +
           clipPlaybackFramePositionForTimelineFrame(clip, adjustedTimelineFramePosition, timing);
}
}  // namespace

QVector<QPointF> defaultGradingCurvePoints() {
    return {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
}

QVector<QPointF> sanitizeGradingCurvePoints(const QVector<QPointF>& points) {
    return qtCurveFromEditor(
        jcut::sanitizeEditorGradingCurve(editorCurveFromQt(points)));
}

qreal sampleGradingCurveAt(const QVector<QPointF>& points, qreal xNorm, bool smoothingEnabled) {
    return jcut::sampleEditorGradingCurveAt(
        editorCurveFromQt(points), xNorm, smoothingEnabled);
}

QVector<quint8> gradingCurveLut8(const QVector<QPointF>& points, int samples, bool smoothingEnabled) {
    return qtCurveLutFromEditor(jcut::editorGradingCurveLut8(
        editorCurveFromQt(points), samples, smoothingEnabled));
}

bool gradingCurveDiffersFromIdentity(const QVector<QPointF>& points, bool smoothingEnabled)
{
    const QVector<quint8> lut =
        gradingCurveLut8(points, TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    const QVector<quint8> identityLut =
        gradingCurveLut8(defaultGradingCurvePoints(), TimelineClip::kGradingCurveLutSize, smoothingEnabled);
    return !lut.isEmpty() && !identityLut.isEmpty() && lut != identityLut;
}

bool gradingUsesCurveLut(const TimelineClip::GradingKeyframe& grade)
{
    return gradingCurveDiffersFromIdentity(grade.curvePointsR, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsG, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsB, grade.curveSmoothingEnabled) ||
           gradingCurveDiffersFromIdentity(grade.curvePointsLuma, grade.curveSmoothingEnabled);
}

qreal evaluateEffectiveClipOpacityAtFrame(const TimelineClip& clip,
                                          const QVector<TimelineTrack>& tracks,
                                          int64_t timelineFrame) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtFrame(clip, timelineFrame);
}

qreal evaluateEffectiveClipOpacityAtPosition(const TimelineClip& clip,
                                             const QVector<TimelineTrack>& tracks,
                                             qreal timelineFramePosition) {
    if (trackVisualModeForClip(clip, tracks) == TrackVisualMode::ForceOpaque) {
        return 1.0;
    }
    return evaluateClipOpacityAtPosition(clip, timelineFramePosition);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip,
                                                                  const QVector<TimelineTrack>& tracks,
                                                                  int64_t timelineFrame) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtFrame(clip, timelineFrame);
    TimelineClip::GradingKeyframe speakerGrade;
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip, timelineFrame, {});
    if (clip.clipRole == ClipRole::Media &&
        clip.linkedSourceClipId.trimmed().isEmpty() &&
        transcriptSpeakerGradingForClipFileAtSourceFrame(clip.filePath, sourceFrame, &speakerGrade)) {
        grade = gradingWithSpeakerOverride(grade, speakerGrade);
    }
    grade.opacity = evaluateEffectiveClipOpacityAtFrame(clip, tracks, timelineFrame);
    return grade;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip,
                                                                     const QVector<TimelineTrack>& tracks,
                                                                     qreal timelineFramePosition) {
    TimelineClip::GradingKeyframe grade = evaluateClipGradingAtPosition(clip, timelineFramePosition);
    TimelineClip::GradingKeyframe speakerGrade;
    const int64_t sourceFrame = sourceFrameForClipAtTimelinePosition(clip, timelineFramePosition, {});
    if (clip.clipRole == ClipRole::Media &&
        clip.linkedSourceClipId.trimmed().isEmpty() &&
        transcriptSpeakerGradingForClipFileAtSourceFrame(clip.filePath, sourceFrame, &speakerGrade)) {
        grade = gradingWithSpeakerOverride(grade, speakerGrade);
    }
    grade.opacity = evaluateEffectiveClipOpacityAtPosition(clip, tracks, timelineFramePosition);
    return grade;
}

TimelineClip::GradingKeyframe gradingWithSpeakerOverride(
    const TimelineClip::GradingKeyframe& clipGrade,
    const TimelineClip::GradingKeyframe& speakerGrade)
{
    TimelineClip::GradingKeyframe result = speakerGrade;
    // Per-person grading is an override layer, not an adjustment stacked on
    // the master or virtual-child grade. Preserve only temporal/compositing
    // metadata that is not part of the color transform.
    result.frame = clipGrade.frame;
    result.linearInterpolation = clipGrade.linearInterpolation;
    result.opacity = clipGrade.opacity;
    return result;
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtFrame(const TimelineClip& clip, int64_t timelineFrame) {
    return evaluateEffectiveClipGradingAtFrame(clip, {}, timelineFrame);
}

TimelineClip::GradingKeyframe evaluateEffectiveClipGradingAtPosition(const TimelineClip& clip, qreal timelineFramePosition) {
    return evaluateEffectiveClipGradingAtPosition(clip, {}, timelineFramePosition);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtFrame(clip, tracks, timelineFrame);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    effects.maskFeatherFalloff = clip.maskFeatherFalloff;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelineFrame(clip, polygon, timelineFrame)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition) {
    EffectiveVisualEffects effects;
    effects.grading = evaluateEffectiveClipGradingAtPosition(clip, tracks, timelineFramePosition);
    effects.maskFeather = clip.maskFeather;
    effects.maskFeatherGamma = clip.maskFeatherGamma;
    effects.maskFeatherFalloff = clip.maskFeatherFalloff;
    for (const TimelineClip::CorrectionPolygon& polygon : clip.correctionPolygons) {
        if (correctionPolygonActiveAtTimelinePosition(clip, polygon, timelineFramePosition)) {
            effects.correctionPolygons.push_back(polygon);
        }
    }
    return effects;
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtFrame(const TimelineClip& clip,
                                                             const QVector<TimelineTrack>& tracks,
                                                             int64_t timelineFrame,
                                                             const QVector<RenderSyncMarker>& markers) {
    return evaluateEffectiveVisualEffectsAtPosition(
        clip, tracks, static_cast<qreal>(timelineFrame), markers);
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers) {
    return evaluateEffectiveVisualEffectsAtPosition(
        clip, tracks, timelineFramePosition, markers, activePlaybackTimingContext());
}

EffectiveVisualEffects evaluateEffectiveVisualEffectsAtPosition(const TimelineClip& clip,
                                                                const QVector<TimelineTrack>& tracks,
                                                                qreal timelineFramePosition,
                                                                const QVector<RenderSyncMarker>& markers,
                                                                const PlaybackTimingContext& timing) {
    const qreal adjustedTimelinePosition =
        effectTimelinePositionForClip(clip, timelineFramePosition, markers, timing);
    return evaluateEffectiveVisualEffectsAtPosition(clip, tracks, adjustedTimelinePosition);
}
QImage applyClipGrade(const QImage& source, const TimelineClip::GradingKeyframe& grade) {
    const bool needsBasicGrade =
        !qFuzzyIsNull(grade.brightness) ||
        !qFuzzyCompare(grade.contrast, 1.0) ||
        !qFuzzyCompare(grade.saturation, 1.0) ||
        !qFuzzyCompare(grade.opacity, 1.0);
    const bool needsToneGrade =
        !qFuzzyIsNull(grade.shadowsR) || !qFuzzyIsNull(grade.shadowsG) || !qFuzzyIsNull(grade.shadowsB) ||
        !qFuzzyIsNull(grade.midtonesR) || !qFuzzyIsNull(grade.midtonesG) || !qFuzzyIsNull(grade.midtonesB) ||
        !qFuzzyIsNull(grade.highlightsR) || !qFuzzyIsNull(grade.highlightsG) || !qFuzzyIsNull(grade.highlightsB);

    const bool needsCurveGrade = gradingUsesCurveLut(grade);

    if (source.isNull() || (!needsBasicGrade && !needsToneGrade && !needsCurveGrade)) {
        return source;
    }

    QVector<quint8> curveLutR;
    QVector<quint8> curveLutG;
    QVector<quint8> curveLutB;
    QVector<quint8> curveLutL;
    if (needsCurveGrade) {
        curveLutR =
            gradingCurveLut8(grade.curvePointsR, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutG =
            gradingCurveLut8(grade.curvePointsG, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutB =
            gradingCurveLut8(grade.curvePointsB, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
        curveLutL =
            gradingCurveLut8(grade.curvePointsLuma, TimelineClip::kGradingCurveLutSize, grade.curveSmoothingEnabled);
    }

    auto smoothShadows = [](float luma) { return std::pow(1.0f - luma, 2.0f); };
    auto smoothMidtones = [](float luma) { return 1.0f - std::abs(luma - 0.5f) * 2.0f; };
    auto smoothHighlights = [](float luma) { return std::pow(luma, 2.0f); };

    QImage graded = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < graded.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(graded.scanLine(y));
        for (int x = 0; x < graded.width(); ++x) {
            QColor color = QColor::fromRgba(row[x]);
            float h = 0.0f, s = 0.0f, l = 0.0f, a = 0.0f;
            color.getHslF(&h, &s, &l, &a);

            float rf = color.redF();
            float gf = color.greenF();
            float bf = color.blueF();

            // Calculate luminance for tone-based grading
            float luminance = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;

            // Apply Shadows (Lift)
            if (needsToneGrade) {
                float shadowWeight = smoothShadows(luminance);
                rf *= (1.0f + grade.shadowsR * shadowWeight);
                gf *= (1.0f + grade.shadowsG * shadowWeight);
                bf *= (1.0f + grade.shadowsB * shadowWeight);

                // Apply Midtones (Gamma)
                float midtoneWeight = smoothMidtones(luminance);
                rf = std::pow(rf, 1.0f / (1.0f + grade.midtonesR * midtoneWeight));
                gf = std::pow(gf, 1.0f / (1.0f + grade.midtonesG * midtoneWeight));
                bf = std::pow(bf, 1.0f / (1.0f + grade.midtonesB * midtoneWeight));

                // Apply Highlights (Gain)
                float highlightWeight = smoothHighlights(luminance);
                rf += grade.highlightsR * highlightWeight;
                gf += grade.highlightsG * highlightWeight;
                bf += grade.highlightsB * highlightWeight;
            }

            if (!curveLutR.isEmpty() && !curveLutG.isEmpty() && !curveLutB.isEmpty()) {
                const int ri = qBound(0, qRound(rf * 255.0f), 255);
                const int gi = qBound(0, qRound(gf * 255.0f), 255);
                const int bi = qBound(0, qRound(bf * 255.0f), 255);
                rf = static_cast<float>(curveLutR[ri]) / 255.0f;
                gf = static_cast<float>(curveLutG[gi]) / 255.0f;
                bf = static_cast<float>(curveLutB[bi]) / 255.0f;
                if (!curveLutL.isEmpty()) {
                    const float curveLuma = (rf * 0.2126f) + (gf * 0.7152f) + (bf * 0.0722f);
                    const int lumaIndex = qBound(0, qRound(curveLuma * 255.0f), 255);
                    const float remappedLuma = static_cast<float>(curveLutL[lumaIndex]) / 255.0f;
                    if (curveLuma > 0.0001f) {
                        const float lumaScale = remappedLuma / curveLuma;
                        rf *= lumaScale;
                        gf *= lumaScale;
                        bf *= lumaScale;
                    } else {
                        rf = remappedLuma;
                        gf = remappedLuma;
                        bf = remappedLuma;
                    }
                }
            }

            // Basic grading
            rf = qBound(0.0f, rf, 1.0f);
            gf = qBound(0.0f, gf, 1.0f);
            bf = qBound(0.0f, bf, 1.0f);

            int r = clampChannel(qRound(((rf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int g = clampChannel(qRound(((gf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));
            int b = clampChannel(qRound(((bf * 255.0 - 127.5) * grade.contrast) + 127.5 + grade.brightness * 255.0));

            QColor adjusted(r, g, b, color.alpha());
            adjusted.getHslF(&h, &s, &l, &a);
            s = qBound(0.0f, static_cast<float>(s * grade.saturation), 1.0f);
            a = qBound(0.0f, static_cast<float>(a * grade.opacity), 1.0f);
            adjusted.setHslF(h, s, l, a);
            row[x] = adjusted.rgba();
        }
    }
    return graded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyClipGrade(const QImage& source, const TimelineClip& clip) {
    return applyClipGrade(source, evaluateEffectiveClipGradingAtFrame(clip, clip.startFrame));
}

namespace {
QString maskFramePathForSourceFrame(const TimelineClip& clip, int64_t sourceFrame)
{
    if (clip.maskFramesDir.trimmed().isEmpty()) {
        return QString();
    }
    const std::optional<int64_t> maskFrame = mappedMaskFrameForSourceFrame(
        clip, qMax<int64_t>(0, sourceFrame));
    if (!maskFrame.has_value()) {
        return QString();
    }
    const QString fileName =
        QStringLiteral("frame_%1.png")
            .arg(*maskFrame + 1, 6, 10, QLatin1Char('0'));
    return QDir(clip.maskFramesDir).absoluteFilePath(fileName);
}

QString maskFramePathForPresentedFrame(
    const TimelineClip& clip,
    const editor::FrameHandle& presentedFrame)
{
    if (clip.maskFramesDir.trimmed().isEmpty() || presentedFrame.isNull() ||
        presentedFrame.frameNumber() < 0) {
        return {};
    }
    const auto path = jcut::masks::maskFramePathForDecodedSampleCore(
        std::filesystem::path(clip.maskFramesDir.toStdString()),
        std::filesystem::path(clip.filePath.toStdString()),
        presentedFrame.frameNumber(),
        presentedFrame.sourcePresentationTimestamp());
    return path ? QString::fromStdString(path->string()) : QString{};
}

QImage morphMask(const QImage& source, int radius, bool dilate)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_Grayscale8);
    QImage output(input.size(), QImage::Format_Grayscale8);
    const int width = input.width();
    const int height = input.height();
    for (int y = 0; y < height; ++y) {
        uchar* dst = output.scanLine(y);
        for (int x = 0; x < width; ++x) {
            int value = dilate ? 0 : 255;
            for (int yy = qMax(0, y - radius); yy <= qMin(height - 1, y + radius); ++yy) {
                const uchar* src = input.constScanLine(yy);
                for (int xx = qMax(0, x - radius); xx <= qMin(width - 1, x + radius); ++xx) {
                    value = dilate ? qMax(value, static_cast<int>(src[xx]))
                                   : qMin(value, static_cast<int>(src[xx]));
                }
            }
            dst[x] = static_cast<uchar>(value);
        }
    }
    return output;
}

QImage blurMask(const QImage& source, int radius)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_Grayscale8);
    const int width = input.width();
    const int height = input.height();
    if (width <= 0 || height <= 0) {
        return input;
    }

    QVector<int> horizontal(width * height, 0);
    for (int y = 0; y < height; ++y) {
        const uchar* src = input.constScanLine(y);
        int sum = 0;
        int count = 0;
        for (int x = 0; x <= qMin(width - 1, radius); ++x) {
            sum += src[x];
            ++count;
        }
        for (int x = 0; x < width; ++x) {
            horizontal[(y * width) + x] = count > 0 ? sum / count : 0;
            const int removeX = x - radius;
            if (removeX >= 0) {
                sum -= src[removeX];
                --count;
            }
            const int addX = x + radius + 1;
            if (addX < width) {
                sum += src[addX];
                ++count;
            }
        }
    }

    QImage output(input.size(), QImage::Format_Grayscale8);
    QVector<int> columnSums(width, 0);
    QVector<int> columnCounts(width, 0);
    for (int y = 0; y <= qMin(height - 1, radius); ++y) {
        const int* row = horizontal.constData() + (y * width);
        for (int x = 0; x < width; ++x) {
            columnSums[x] += row[x];
            ++columnCounts[x];
        }
    }
    for (int y = 0; y < height; ++y) {
        uchar* dst = output.scanLine(y);
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<uchar>(columnCounts[x] > 0
                                            ? qBound(0, columnSums[x] / columnCounts[x], 255)
                                            : 0);
        }
        const int removeY = y - radius;
        if (removeY >= 0) {
            const int* row = horizontal.constData() + (removeY * width);
            for (int x = 0; x < width; ++x) {
                columnSums[x] -= row[x];
                --columnCounts[x];
            }
        }
        const int addY = y + radius + 1;
        if (addY < height) {
            const int* row = horizontal.constData() + (addY * width);
            for (int x = 0; x < width; ++x) {
                columnSums[x] += row[x];
                ++columnCounts[x];
            }
        }
    }
    return output;
}

QImage preparedClipMask(const TimelineClip& clip, int64_t sourceFrame, const QSize& size)
{
    const QString path = maskFramePathForSourceFrame(clip, sourceFrame);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        return QImage();
    }
    const QString cacheKey =
        QStringLiteral("%1|%2x%3|%4|%5|inv=%6|er=%7|di=%8|fe=%9|fg=%10|bl=%11")
            .arg(info.absoluteFilePath())
            .arg(size.width())
            .arg(size.height())
            .arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch())
            .arg(clip.maskInvert ? 1 : 0)
            .arg(clip.maskErode, 0, 'g', 8)
            .arg(clip.maskDilate, 0, 'g', 8)
            .arg(clip.maskFeather, 0, 'g', 8)
            .arg(clip.maskFeatherGamma, 0, 'g', 8)
            .arg(clip.maskBlur, 0, 'g', 8);
    {
        QMutexLocker lock(&preparedMaskCacheMutex());
        if (QImage* cached = preparedMaskCache().object(cacheKey)) {
            return *cached;
        }
    }
    QImage mask(path);
    if (mask.isNull()) {
        return QImage();
    }
    if (mask.size() != size) {
        mask = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    mask = mask.convertToFormat(QImage::Format_Grayscale8);
    if (clip.maskInvert) {
        mask.invertPixels();
    }
    if (clip.maskErode > 0.0) {
        mask = morphMask(mask, qRound(clip.maskErode), false);
    }
    if (clip.maskDilate > 0.0) {
        mask = morphMask(mask, qRound(clip.maskDilate), true);
    }
    const int blurRadius = qRound(qMax(clip.maskFeather, clip.maskBlur));
    if (blurRadius > 0) {
        mask = blurMask(mask, blurRadius);
    }
    {
        QMutexLocker lock(&preparedMaskCacheMutex());
        const int costKb = qMax(1, static_cast<int>((mask.sizeInBytes() + 1023) / 1024));
        preparedMaskCache().insert(cacheKey, new QImage(mask), costKb);
    }
    return mask;
}
}

QImage preparedClipMaskImage(const TimelineClip& clip, int64_t sourceFrame, const QSize& size)
{
    return preparedClipMask(clip, sourceFrame, size);
}

static std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBufferForPath(
    const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const FileVersionSignature version = fileVersionSignature(path);
    if (!version.valid || version.size <= 0) {
        return {};
    }
    const QString cacheKey =
        QStringLiteral("%1\x1f%2\x1f%3\x1f%4\x1f%5\x1f%6")
            .arg(info.absoluteFilePath())
            .arg(version.size)
            .arg(version.modifiedNanoseconds)
            .arg(version.changedNanoseconds)
            .arg(version.device)
            .arg(version.inode);
    const std::string coreCacheKey = cacheKey.toStdString();
    std::shared_ptr<const jcut::core::ImageBuffer> cached;
    {
        RawMaskCacheCore& cache = rawMaskCacheCore();
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto found = cache.images.find(coreCacheKey);
        if (found != cache.images.end()) {
            cached = found->second;
        } else if (!cache.loadsInFlight.insert(coreCacheKey).second) {
            return {};
        }
    }
    if (cached) {
        return cached;
    }

    const std::string corePath = info.absoluteFilePath().toStdString();
    rawMaskDecodeExecutor().enqueue([corePath, coreCacheKey]() {
        auto decoded = std::make_shared<jcut::core::ImageBuffer>(
            jcut::core::decodeImageFileGray(corePath));
        RawMaskCacheCore& cache = rawMaskCacheCore();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.loadsInFlight.erase(coreCacheKey);
        if (decoded->empty() ||
            decoded->bytes.size() > RawMaskCacheCore::kMaximumBytes) {
            return;
        }
        while (cache.storedBytes + decoded->bytes.size() >
                   RawMaskCacheCore::kMaximumBytes &&
               !cache.insertionOrder.empty()) {
            const std::string oldest = std::move(cache.insertionOrder.front());
            cache.insertionOrder.pop_front();
            const auto found = cache.images.find(oldest);
            if (found == cache.images.end()) {
                continue;
            }
            cache.storedBytes -= found->second->bytes.size();
            cache.images.erase(found);
        }
        const auto existing = cache.images.find(coreCacheKey);
        if (existing != cache.images.end()) {
            cache.storedBytes -= existing->second->bytes.size();
        } else {
            cache.insertionOrder.push_back(coreCacheKey);
        }
        cache.storedBytes += decoded->bytes.size();
        cache.images[coreCacheKey] = std::move(decoded);
    });
    return {};
}

std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBuffer(
    const TimelineClip& clip,
    int64_t sourceFrame)
{
    return rawClipMaskBufferForPath(
        maskFramePathForSourceFrame(clip, sourceFrame));
}

std::shared_ptr<const jcut::core::ImageBuffer> rawClipMaskBuffer(
    const TimelineClip& clip,
    const editor::FrameHandle& presentedFrame)
{
    return rawClipMaskBufferForPath(
        maskFramePathForPresentedFrame(clip, presentedFrame));
}

void prefetchClipMaskBuffers(const TimelineClip& clip, int64_t sourceFrame)
{
    if (clip.maskFramesDir.trimmed().isEmpty()) {
        return;
    }
    const auto paths =
        jcut::masks::maskFramePathsForSourceFramePrefetchCore(
            std::filesystem::path(clip.maskFramesDir.toStdString()),
            std::filesystem::path(clip.filePath.toStdString()),
            sourceFrame);
    for (const auto& path : paths) {
        rawClipMaskBufferForPath(QString::fromStdString(path.string()));
    }
}

QImage rawClipMaskImage(const TimelineClip& clip, int64_t sourceFrame)
{
    return qtImageFromCoreBuffer(rawClipMaskBuffer(clip, sourceFrame));
}

QImage rawClipMaskImage(const TimelineClip& clip,
                        const editor::FrameHandle& presentedFrame)
{
    return qtImageFromCoreBuffer(
        rawClipMaskBuffer(clip, presentedFrame));
}

QImage applyCorrectionPolygonsToMaskImage(
    const QImage& source,
    const QVector<TimelineClip::CorrectionPolygon>& activePolygons)
{
    if (source.isNull() || activePolygons.isEmpty()) {
        return source;
    }

    QImage corrected = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&corrected);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setBrush(Qt::black);
    painter.setPen(Qt::NoPen);
    const qreal width = qMax<qreal>(1.0, corrected.width());
    const qreal height = qMax<qreal>(1.0, corrected.height());
    for (const TimelineClip::CorrectionPolygon& polygon : activePolygons) {
        if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
            continue;
        }
        QPainterPath path;
        const auto imagePoint = [width, height](const QPointF& point) {
            return QPointF(qBound<qreal>(0.0, point.x(), 1.0) * width,
                           qBound<qreal>(0.0, point.y(), 1.0) * height);
        };
        path.moveTo(imagePoint(polygon.pointsNormalized.constFirst()));
        for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
            path.lineTo(imagePoint(polygon.pointsNormalized.at(i)));
        }
        path.closeSubpath();
        painter.drawPath(path);
    }
    painter.end();
    return corrected.convertToFormat(QImage::Format_Grayscale8);
}

QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame)
{
    TimelineClip::GradingKeyframe neutralGrade;
    return applyClipMaskEffectsToImage(source, clip, sourceFrame, neutralGrade);
}

QImage applyClipMaskEffectsToImage(const QImage& source,
                                   const TimelineClip& clip,
                                   int64_t sourceFrame,
                                   const TimelineClip::GradingKeyframe& clipGrade)
{
    if (source.isNull()) {
        return source;
    }
    auto failClosedMaskMatte = [&source, &clip]() {
        if (clip.clipRole != ClipRole::MaskMatte) {
            return source;
        }
        QImage transparent(source.size(), QImage::Format_ARGB32_Premultiplied);
        transparent.fill(Qt::transparent);
        return transparent;
    };
    if (!clip.maskEnabled || clip.maskFramesDir.trimmed().isEmpty()) {
        return failClosedMaskMatte();
    }
    const QImage mask = preparedClipMask(clip, sourceFrame, source.size());
    if (mask.isNull()) {
        return failClosedMaskMatte();
    }

    if (clip.maskShowOnly) {
        QImage output(mask.size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < output.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            QRgb* dstRow = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const int v = maskRow[x];
                dstRow[x] = qRgba(v, v, v, v);
            }
        }
        return output;
    }

    QImage original = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const bool generatedMaskMatte = clip.clipRole == ClipRole::MaskMatte;
    QImage base = applyClipGrade(original, clipGrade)
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage output(base.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);

    if (clip.maskDropShadowEnabled && clip.maskDropShadowOpacity > 0.0) {
        QImage shadowMask = clip.maskDropShadowRadius > 0.0
                                ? blurMask(mask, qRound(clip.maskDropShadowRadius))
                                : mask;
        QImage shadow(base.size(), QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        for (int y = 0; y < shadow.height(); ++y) {
            const uchar* maskRow = shadowMask.constScanLine(y);
            QRgb* dst = reinterpret_cast<QRgb*>(shadow.scanLine(y));
            for (int x = 0; x < shadow.width(); ++x) {
                const int alpha =
                    qBound(0, qRound(maskRow[x] * clip.maskDropShadowOpacity), 255);
                dst[x] = qRgba(0, 0, 0, alpha);
            }
        }
        QPainter painter(&output);
        painter.drawImage(QPointF(clip.maskDropShadowOffsetX, clip.maskDropShadowOffsetY), shadow);
        painter.end();
    }

    if (generatedMaskMatte) {
        QImage foreground = base.convertToFormat(QImage::Format_ARGB32);
        const qreal opacity = qBound<qreal>(0.0, clip.maskOpacity, 1.0);
        for (int y = 0; y < foreground.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            QRgb* dstRow = reinterpret_cast<QRgb*>(foreground.scanLine(y));
            for (int x = 0; x < foreground.width(); ++x) {
                dstRow[x] = qRgba(qRed(dstRow[x]),
                                  qGreen(dstRow[x]),
                                  qBlue(dstRow[x]),
                                  qBound(0, qRound(qAlpha(dstRow[x]) *
                                                  (maskRow[x] / 255.0) * opacity), 255));
            }
        }
        QPainter foregroundPainter(&output);
        foregroundPainter.drawImage(0, 0, foreground);
        foregroundPainter.end();
    } else {
        QPainter basePainter(&output);
        basePainter.drawImage(0, 0, base);
        basePainter.end();
    }
    if (!generatedMaskMatte && clip.maskGradeEnabled) {
        const QImage maskedGrade = applyClipGrade(original, maskGradeForClip(clip))
                                       .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const qreal opacity = qBound<qreal>(0.0, clip.maskOpacity, 1.0);
        for (int y = 0; y < output.height(); ++y) {
            const uchar* maskRow = mask.constScanLine(y);
            const QRgb* gradeRow = reinterpret_cast<const QRgb*>(maskedGrade.constScanLine(y));
            QRgb* dstRow = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = 0; x < output.width(); ++x) {
                const qreal amount = (maskRow[x] / 255.0) * opacity;
                if (amount <= 0.0) continue;
                const QRgb basePixel = dstRow[x];
                const QRgb gradePixel = gradeRow[x];
                auto mixChannel = [amount](int baseValue, int gradeValue) {
                    return qBound(0, qRound(baseValue + (gradeValue - baseValue) * amount), 255);
                };
                dstRow[x] = qRgba(mixChannel(qRed(basePixel), qRed(gradePixel)),
                                  mixChannel(qGreen(basePixel), qGreen(gradePixel)),
                                  mixChannel(qBlue(basePixel), qBlue(gradePixel)),
                                  mixChannel(qAlpha(basePixel), qAlpha(gradePixel)));
            }
        }
    }
    return output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyEffectiveClipVisualEffectsToImage(const QImage& source, const EffectiveVisualEffects& effects) {
    QImage output = applyClipGrade(source, effects.grading);
    if (!effects.correctionPolygons.isEmpty() && !output.isNull()) {
        output = output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.setBrush(Qt::black);
        painter.setPen(Qt::NoPen);
        const qreal width = qMax<qreal>(1.0, output.width());
        const qreal height = qMax<qreal>(1.0, output.height());
        for (const TimelineClip::CorrectionPolygon& polygon : effects.correctionPolygons) {
            if (!polygon.enabled || polygon.pointsNormalized.size() < 3) {
                continue;
            }
            QPainterPath path;
            const QPointF first(
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().x(), 1.0) * width,
                qBound<qreal>(0.0, polygon.pointsNormalized.constFirst().y(), 1.0) * height);
            path.moveTo(first);
            for (int i = 1; i < polygon.pointsNormalized.size(); ++i) {
                const QPointF point(
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].x(), 1.0) * width,
                    qBound<qreal>(0.0, polygon.pointsNormalized[i].y(), 1.0) * height);
                path.lineTo(point);
            }
            path.closeSubpath();
            painter.drawPath(path);
        }
        painter.end();
    }
    if (effects.maskFeather > 0.0) {
        output = applyMaskFeather(output, effects.maskFeather, effects.maskFeatherGamma,
                                  effects.maskFeatherFalloff);
    }
    return output;
}

QImage applyMaskFeather(const QImage& source, qreal featherRadius, qreal featherGamma,
                        int featherFalloff) {
    if (source.isNull() || featherRadius <= 0.0) {
        return source;
    }

    QImage feathered = source.convertToFormat(QImage::Format_ARGB32);
    const int radius = qRound(featherRadius);
    if (radius <= 0) {
        return source;
    }

    // Create a copy for reading
    const QImage sourceCopy = feathered.copy();
    const int width = feathered.width();
    const int height = feathered.height();

    // Box blur on the alpha channel with gamma curve
    const qreal gamma = qMax(0.01, featherGamma);
    for (int y = 0; y < height; ++y) {
        QRgb* destRow = reinterpret_cast<QRgb*>(feathered.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int alphaSum = 0;
            int pixelCount = 0;

            // Sample the box
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sampleY = qBound(0, y + dy, height - 1);
                const QRgb* srcRow = reinterpret_cast<const QRgb*>(sourceCopy.scanLine(sampleY));
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sampleX = qBound(0, x + dx, width - 1);
                    alphaSum += qAlpha(srcRow[sampleX]);
                    pixelCount++;
                }
            }

            // Box blur average
            const qreal blurredAlpha = static_cast<qreal>(alphaSum) / pixelCount / 255.0;
            const qreal t = qBound<qreal>(0.0, blurredAlpha, 1.0);
            qreal shapedAlpha = t;
            switch (qBound(0, featherFalloff, 5)) {
            case 1: // Linear
                break;
            case 2: // Smoothstep: C1-continuous, common compositing falloff.
                shapedAlpha = t * t * (3.0 - 2.0 * t);
                break;
            case 3: // Smootherstep: C2-continuous for especially clean motion.
                shapedAlpha = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
                break;
            case 4: // Raised cosine.
                shapedAlpha = 0.5 - 0.5 * std::cos(t * 3.14159265358979323846);
                break;
            case 5: { // Gaussian-style S curve, normalized to [0, 1].
                constexpr qreal k = 4.0;
                const qreal lo = std::exp(-k);
                shapedAlpha = (std::exp(-k * (1.0 - t) * (1.0 - t)) - lo) /
                              (1.0 - lo);
                break;
            }
            case 0:
            default:
                shapedAlpha = std::pow(t, 1.0 / gamma);
                break;
            }
            const int newAlpha = qBound(0, qRound(shapedAlpha * 255.0), 255);
            const QRgb original = destRow[x];
            destRow[x] = qRgba(qRed(original), qGreen(original), qBlue(original), newAlpha);
        }
    }

    return feathered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
